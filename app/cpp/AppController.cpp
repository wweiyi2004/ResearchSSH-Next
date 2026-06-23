#include "AppController.h"

#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

namespace researchssh {

namespace {

constexpr quint64 kEditorReadLimit = 2 * 1024 * 1024;

QString baseNameOf(const QString &path) {
    const QString trimmed = path.endsWith('/') && path.size() > 1 ? path.left(path.size() - 1)
                                                                  : path;
    const int slash = trimmed.lastIndexOf('/');
    return slash >= 0 ? trimmed.mid(slash + 1) : trimmed;
}

QString joinRemotePath(const QString &dir, const QString &name) {
    if (name.isEmpty())
        return dir;
    if (dir.isEmpty() || dir == QStringLiteral("."))
        return name;
    if (dir == QStringLiteral("/"))
        return QStringLiteral("/%1").arg(name);
    QString base = dir;
    while (base.endsWith('/') && base.size() > 1)
        base.chop(1);
    return QStringLiteral("%1/%2").arg(base, name);
}

bool looksLikeTextBuffer(const QByteArray &data) {
    if (data.isEmpty())
        return true;
    if (data.contains('\0'))
        return false;

    int suspiciousControls = 0;
    for (const char ch : data) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
            ++suspiciousControls;
    }
    if (suspiciousControls > data.size() / 100)
        return false;

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(data);
    (void)decoded;
    return !decoder.hasError();
}

bool isValidRemoteName(const QString &name) {
    const QString trimmed = name.trimmed();
    return !trimmed.isEmpty() && !trimmed.contains('/') && !trimmed.contains('\\');
}

} // namespace

AppController::AppController(QObject *parent) : QObject(parent) {
    m_servers = new ServerListModel(this);
    m_terminal = new TerminalViewModel(this);
    m_fileTree = new RemoteFileTreeModel(this);
    m_editor = new EditorViewModel(this);
    m_credentials = createDefaultCredentialStore();

    connect(m_fileTree, &RemoteFileTreeModel::directoryExpandRequested, this,
            &AppController::expandDir);
}

AppController::~AppController() {
    // Tear down the session (joins the Rust driver) BEFORE m_bridge destroys the
    // core. m_bridge is a member, so its destructor runs after this body.
    teardownSession();
}

bool AppController::initialize(QString *errorOut) {
    if (!m_bridge.initialize(errorOut))
        return false;
    m_servers->seedDemoServers();
    if (m_servers->count() > 0)
        selectServer(0);
    m_terminal->appendNotice(
        QStringLiteral("ResearchSSH-Next 核心 %1 就绪(凭据后端：%2)。")
            .arg(RustCoreBridge::version(), credentialBackend()));
    m_terminal->appendNotice(QStringLiteral(
        "请选择左侧服务器并点击“连接”。当前为模拟 Provider；主机名以 “fail” 开头的条目"
        "用于演示连接失败的错误流程。"));
    return true;
}

void AppController::selectServer(int index) {
    if (!m_servers->isValidIndex(index) || index == m_selectedIndex)
        return;
    m_selectedIndex = index;
    emit selectedIndexChanged();
}

void AppController::connectToServer(int index) {
    if (!m_servers->isValidIndex(index))
        return;
    selectServer(index);

    // Switching to a different server: tear down the previous session first.
    if (m_session && m_currentIndex != index)
        teardownSession();

    if (!m_session) {
        const ServerItem &item = m_servers->itemAt(index);
        QString err;
        RsSession *session = m_bridge.createSession(item.host, item.port, item.username,
                                                    item.provider, this, &err);
        if (!session) {
            m_terminal->appendNotice(QStringLiteral("创建会话失败：%1").arg(err));
            m_servers->setStatus(index, RsSessionState_Failed);
            return;
        }
        m_session = session;
        m_currentIndex = index;
        m_currentEndpoint = endpointFor(index);
        emit currentEndpointChanged();

        const RsErrorCode fsRc = m_bridge.setFileCallback(m_session, this);
        if (fsRc != RsErrorCode_Ok)
            m_terminal->appendNotice(
                QStringLiteral("注册文件回调失败：%1").arg(RustCoreBridge::describe(fsRc)));

        // Demonstrate the secure credential path: a stored secret (if any) is sent
        // straight to the core's zeroizing buffer — it never passes through QML.
        // The mock provider discards it; this is the hook the real provider uses.
        const QByteArray secret = m_credentials->load(m_currentEndpoint);
        if (!secret.isEmpty())
            m_bridge.setSessionPassword(m_session, secret);

        // Public-key auth: hand the (optional) key path + passphrase to the core.
        // An empty path means auto-discover ~/.ssh defaults; the mock ignores this.
        const QByteArray keyPass =
            m_credentials->load(m_currentEndpoint + QStringLiteral("#keypass"));
        m_bridge.setSessionPrivateKey(m_session, item.keyPath, keyPass);
    }

    m_terminal->appendNotice(QStringLiteral("正在连接 %1 …").arg(m_currentEndpoint));
    const RsErrorCode rc = m_bridge.connectSession(m_session);
    if (rc != RsErrorCode_Ok)
        m_terminal->appendNotice(
            QStringLiteral("连接请求被拒绝：%1").arg(RustCoreBridge::describe(rc)));
}

void AppController::connectToHost(const QString &host, int port, const QString &username,
                                  const QString &password, const QString &name,
                                  const QString &keyPath, const QString &keyPassphrase) {
    if (host.isEmpty() || username.isEmpty()) {
        m_terminal->appendNotice(QStringLiteral("主机和用户名不能为空。"));
        return;
    }
    const quint16 p = port > 0 ? static_cast<quint16>(port) : 22;
    const int index = m_servers->addServer(name, host, p, username,
                                           static_cast<int>(RsProviderKind_Russh), keyPath);

    // Stash secrets in the credential store keyed by endpoint; connectToServer loads
    // them from there and hands them to the core. They never round-trip through QML.
    const QString endpoint = QStringLiteral("%1@%2:%3").arg(username, host).arg(p);
    if (!password.isEmpty())
        m_credentials->store(endpoint, password.toUtf8());
    if (!keyPassphrase.isEmpty())
        m_credentials->store(endpoint + QStringLiteral("#keypass"), keyPassphrase.toUtf8());

    connectToServer(index);
}

void AppController::confirmHostKey(bool accept) {
    if (!m_session)
        return;
    m_terminal->appendNotice(accept ? QStringLiteral("已接受主机密钥。")
                                    : QStringLiteral("已拒绝主机密钥,连接中止。"));
    m_bridge.confirmHostKey(m_session, accept);
}

void AppController::disconnectCurrent() {
    if (!m_session)
        return;
    const RsErrorCode rc = m_bridge.disconnectSession(m_session);
    if (rc != RsErrorCode_Ok)
        m_terminal->appendNotice(
            QStringLiteral("断开请求被拒绝：%1").arg(RustCoreBridge::describe(rc)));
}

void AppController::cancel() {
    if (m_session)
        m_bridge.cancelSession(m_session);
}

void AppController::sendCommand(const QString &text) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法发送命令。"));
        return;
    }
    QByteArray payload = text.toUtf8();
    payload.append('\n');
    const RsErrorCode rc = m_bridge.sendToSession(m_session, payload);
    if (rc != RsErrorCode_Ok)
        m_terminal->appendNotice(
            QStringLiteral("发送失败：%1").arg(RustCoreBridge::describe(rc)));
}

void AppController::runQuickCommand(const QString &command) {
    sendCommand(command);
}

void AppController::clearTerminal() {
    m_terminal->clear();
}

QString AppController::clipboardName() const {
    return baseNameOf(m_clipboardPath);
}

void AppController::ingestRustEvents(const RustEventBatch &batch) {
    for (const RustEvent &ev : batch) {
        switch (ev.kind) {
        case RsEventKind_StateChanged:
            setConnectionState(ev.state);
            break;
        case RsEventKind_Data:
            m_terminal->appendBytes(ev.data);
            break;
        case RsEventKind_Error: {
            const QString msg =
                ev.message.isEmpty() ? RustCoreBridge::describe(ev.code) : ev.message;
            m_terminal->appendNotice(QStringLiteral("错误：%1").arg(msg));
            break;
        }
        case RsEventKind_HostKeyPrompt:
            m_terminal->appendNotice(QStringLiteral("服务器主机密钥指纹：%1").arg(ev.message));
            emit hostKeyPromptRequested(ev.message);
            break;
        }
    }
}

void AppController::ingestFileResults(const FsResultBatch &batch) {
    for (const FsResult &result : batch) {
        const auto it = m_fsPending.find(result.requestId);
        if (it == m_fsPending.end())
            continue;

        const PendingFs pending = it.value();
        m_fsPending.erase(it);
        emit fileActivityChanged();

        if (result.kind == RsFsResultKind_Error) {
            if (pending.kind == PendingFs::Listing) {
                m_fileTree->markLoadFailed(pending.path);
                if (pending.path.isEmpty()) {
                    const auto code = static_cast<RsErrorCode>(result.errorCode);
                    const QString msg =
                        result.message.isEmpty() ? RustCoreBridge::describe(code) : result.message;
                    if (code == RsErrorCode_ProviderUnavailable) {
                        setFileStatus(false, QStringLiteral(
                                                 "文件传输不可用：当前 SSH 会话没有打开 SFTP 子系统。"));
                    } else {
                        setFileStatus(false, QStringLiteral("远端文件加载失败：%1").arg(msg));
                    }
                }
            } else if (pending.kind == PendingFs::EditorOpen) {
                m_editor->failOpen();
            }

            const QString msg =
                result.message.isEmpty()
                    ? RustCoreBridge::describe(static_cast<RsErrorCode>(result.errorCode))
                    : result.message;
            if (pending.kind == PendingFs::EditorSave)
                m_editor->finishSave(false, msg);
            m_terminal->appendNotice(QStringLiteral("文件操作失败：%1").arg(msg));
            continue;
        }

        switch (pending.kind) {
        case PendingFs::Listing:
            if (result.kind == RsFsResultKind_Listing) {
                if (pending.path.isEmpty()) {
                    rememberHomeFromRootListing(result.entries);
                    setFileStatus(true,
                                  result.entries.isEmpty()
                                      ? QStringLiteral("远端文件已就绪：当前目录为空")
                                      : QStringLiteral("远端文件已就绪"));
                }
                m_fileTree->applyListing(pending.path, result.entries);
            } else {
                if (pending.path.isEmpty())
                    setFileStatus(false, QStringLiteral("远端文件加载失败：返回结果类型不匹配。"));
                m_fileTree->markLoadFailed(pending.path);
            }
            break;
        case PendingFs::EditorOpen:
            if (result.kind == RsFsResultKind_Content) {
                if (static_cast<quint64>(result.data.size()) >= kEditorReadLimit) {
                    m_editor->failOpen();
                    m_terminal->appendNotice(QStringLiteral(
                        "拒绝打开：文件达到编辑器读取上限 %1 MB。")
                                                .arg(kEditorReadLimit / 1024 / 1024));
                    break;
                }
                if (!looksLikeTextBuffer(result.data)) {
                    m_editor->failOpen();
                    m_terminal->appendNotice(
                        QStringLiteral("拒绝打开：该文件看起来不是 UTF-8 文本。"));
                    break;
                }
                m_editor->setContent(pending.path, result.data);
                m_terminal->appendNotice(QStringLiteral("已打开远端文件：%1").arg(pending.path));
            } else {
                m_editor->failOpen();
                m_terminal->appendNotice(QStringLiteral("打开文件失败：返回结果类型不匹配。"));
            }
            break;
        case PendingFs::EditorSave:
            if (result.kind == RsFsResultKind_Ok) {
                m_editor->finishSave(true);
                m_terminal->appendNotice(QStringLiteral("已保存远端文件：%1").arg(pending.path));
                if (!pending.refreshDir.isNull())
                    expandDir(pending.refreshDir);
                if (!pending.extraRefreshDir.isNull() && pending.extraRefreshDir != pending.refreshDir)
                    expandDir(pending.extraRefreshDir);
            } else {
                // Any non-Ok, non-Error result is unexpected for a write; fail the
                // save so the editor doesn't stay stuck in the "saving" state.
                m_editor->finishSave(false, QStringLiteral("保存失败：返回结果类型不匹配。"));
                m_terminal->appendNotice(QStringLiteral("保存失败：返回结果类型不匹配。"));
            }
            break;
        case PendingFs::Mutate:
            if (result.kind == RsFsResultKind_Ok) {
                m_terminal->appendNotice(QStringLiteral("文件操作完成。"));
                if (pending.path == m_editor->path())
                    m_editor->close();
                if (pending.clearClipboard) {
                    m_clipboardPath.clear();
                    m_clipboardCut = false;
                    emit clipboardChanged();
                }
                if (!pending.refreshDir.isNull())
                    expandDir(pending.refreshDir);
                if (!pending.extraRefreshDir.isNull() && pending.extraRefreshDir != pending.refreshDir)
                    expandDir(pending.extraRefreshDir);
            }
            break;
        }
    }
}

void AppController::openPath(const QString &path) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法打开远端文件。"));
        return;
    }
    if (path.isEmpty())
        return;

    m_editor->beginOpen(path);
    const quint64 id = m_bridge.fsRead(m_session, path, kEditorReadLimit);
    if (id == 0) {
        m_editor->failOpen();
        m_terminal->appendNotice(QStringLiteral("无法提交读取请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::EditorOpen, path, {}});
}

void AppController::reloadFiles() {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_fileTree->clearTree();
        setFileStatus(false, QStringLiteral("连接后显示远端文件"));
        return;
    }
    m_fileTree->clearTree();
    setFileStatus(false, QStringLiteral("正在加载远端文件..."));
    m_fileTree->loadRoot();
}

void AppController::copyPath(const QString &path) {
    if (path.isEmpty())
        return;
    m_clipboardPath = path;
    m_clipboardCut = false;
    emit clipboardChanged();
    m_terminal->appendNotice(QStringLiteral("已复制远端路径：%1").arg(path));
}

void AppController::cutPath(const QString &path) {
    if (path.isEmpty())
        return;
    m_clipboardPath = path;
    m_clipboardCut = true;
    emit clipboardChanged();
    m_terminal->appendNotice(QStringLiteral("已剪切远端路径：%1").arg(path));
}

void AppController::paste(const QString &destDir) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法粘贴远端文件。"));
        return;
    }
    if (m_clipboardPath.isEmpty()) {
        m_terminal->appendNotice(QStringLiteral("没有可粘贴的远端路径。"));
        return;
    }

    const QString source = m_clipboardPath;
    const QString dest = joinRemotePath(targetDirForWrite(destDir), baseNameOf(source));
    const QString sourceParent = parentDirOf(source);
    const quint64 id = m_clipboardCut ? m_bridge.fsRename(m_session, source, dest)
                                      : m_bridge.fsCopy(m_session, source, dest);
    if (id == 0) {
        m_terminal->appendNotice(m_clipboardCut ? QStringLiteral("无法提交剪切请求。")
                                                : QStringLiteral("无法提交复制请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::Mutate, m_clipboardCut ? source : dest, destDir,
                                 m_clipboardCut ? sourceParent : QString(), m_clipboardCut});
}

void AppController::renamePath(const QString &path, const QString &newName) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法重命名远端路径。"));
        return;
    }
    if (path.isEmpty())
        return;
    if (!isValidRemoteName(newName)) {
        m_terminal->appendNotice(QStringLiteral("名称无效：不能包含路径分隔符。"));
        return;
    }

    const QString parent = parentDirOf(path);
    const QString dest = joinRemotePath(parent, newName.trimmed());
    const quint64 id = m_bridge.fsRename(m_session, path, dest);
    if (id == 0) {
        m_terminal->appendNotice(QStringLiteral("无法提交重命名请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parent});
}

void AppController::deletePath(const QString &path, bool isDir) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法删除远端路径。"));
        return;
    }
    if (path.isEmpty())
        return;

    const QString parent = parentDirOf(path);
    const quint64 id = m_bridge.fsRemove(m_session, path, isDir);
    if (id == 0) {
        m_terminal->appendNotice(QStringLiteral("无法提交删除请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parent});
}

void AppController::makeDir(const QString &parentDir, const QString &name) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法创建远端目录。"));
        return;
    }
    if (!isValidRemoteName(name)) {
        m_terminal->appendNotice(QStringLiteral("目录名称无效：不能包含路径分隔符。"));
        return;
    }

    const QString path = joinRemotePath(targetDirForWrite(parentDir), name.trimmed());
    const quint64 id = m_bridge.fsMkdir(m_session, path);
    if (id == 0) {
        m_terminal->appendNotice(QStringLiteral("无法提交新建目录请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parentDir});
}

void AppController::makeFile(const QString &parentDir, const QString &name) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法创建远端文件。"));
        return;
    }
    if (!isValidRemoteName(name)) {
        m_terminal->appendNotice(QStringLiteral("文件名称无效：不能包含路径分隔符。"));
        return;
    }

    const QString path = joinRemotePath(targetDirForWrite(parentDir), name.trimmed());
    const quint64 id = m_bridge.fsWrite(m_session, path, QByteArray());
    if (id == 0) {
        m_terminal->appendNotice(QStringLiteral("无法提交新建文件请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parentDir});
}

void AppController::uploadFile(const QString &destDir, const QUrl &localFile) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法上传文件。"));
        return;
    }

    const QString localPath = localFile.toLocalFile();
    QFile file(localPath);
    if (localPath.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        m_terminal->appendNotice(QStringLiteral("无法读取本地文件：%1").arg(localPath));
        return;
    }

    const QString dest = joinRemotePath(targetDirForWrite(destDir), QFileInfo(file).fileName());
    const QByteArray data = file.readAll();
    const quint64 id = m_bridge.fsWrite(m_session, dest, data);
    if (id == 0) {
        m_terminal->appendNotice(QStringLiteral("无法提交上传请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::Mutate, dest, destDir});
}

void AppController::saveEditor(const QString &text) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法保存远端文件。"));
        return;
    }
    if (!m_editor->isOpen() || m_editor->path().isEmpty())
        return;

    const QString path = m_editor->path();
    m_editor->beginSave();
    const quint64 id = m_bridge.fsWrite(m_session, path, text.toUtf8());
    if (id == 0) {
        m_editor->finishSave(false, QStringLiteral("无法提交保存请求。"));
        m_terminal->appendNotice(QStringLiteral("无法提交保存请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::EditorSave, path, parentDirOf(path)});
}

void AppController::setConnectionState(RsSessionState state) {
    m_connectionState = state;
    if (m_currentIndex >= 0)
        m_servers->setStatus(m_currentIndex, state);
    emit connectionStateChanged();

    if (state == RsSessionState_Connected) {
        m_remoteHomePath.clear();
        reloadFiles();
    } else if (state == RsSessionState_Disconnected || state == RsSessionState_Failed) {
        clearPendingFs();
        m_remoteHomePath.clear();
        setFileStatus(false, QStringLiteral("连接后显示远端文件"));
        m_fileTree->clearTree();
        m_editor->close();
    }
}

void AppController::teardownSession() {
    if (m_session) {
        m_bridge.destroySession(m_session); // joins the driver; no more callbacks
        m_session = nullptr;
    }
    clearPendingFs();
    m_clipboardPath.clear();
    m_clipboardCut = false;
    m_remoteHomePath.clear();
    setFileStatus(false, QStringLiteral("连接后显示远端文件"));
    emit clipboardChanged();
    m_fileTree->clearTree();
    m_editor->close();
    m_currentIndex = -1;
}

QString AppController::endpointFor(int index) const {
    const ServerItem &item = m_servers->itemAt(index);
    return QStringLiteral("%1@%2:%3").arg(item.username, item.host).arg(item.port);
}

void AppController::expandDir(const QString &path) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_fileTree->markLoadFailed(path);
        return;
    }

    const quint64 id = m_bridge.fsList(m_session, path);
    if (id == 0) {
        m_fileTree->markLoadFailed(path);
        m_terminal->appendNotice(QStringLiteral("无法提交目录加载请求。"));
        return;
    }
    trackFsRequest(id, PendingFs{PendingFs::Listing, path, {}});
}

QString AppController::parentDirOf(const QString &path) {
    if (path.isEmpty())
        return {};
    QString trimmed = path;
    while (trimmed.endsWith('/') && trimmed.size() > 1)
        trimmed.chop(1);
    const int slash = trimmed.lastIndexOf('/');
    if (slash < 0)
        return {};
    if (slash == 0)
        return QStringLiteral("/");
    return trimmed.left(slash);
}

QString AppController::targetDirForWrite(const QString &dir) const {
    if ((dir.isEmpty() || dir == QStringLiteral(".")) && !m_remoteHomePath.isEmpty())
        return m_remoteHomePath;
    return dir;
}

void AppController::rememberHomeFromRootListing(const QVector<FsEntry> &entries) {
    for (const FsEntry &entry : entries) {
        const QString parent = parentDirOf(entry.path);
        if (!parent.isEmpty()) {
            m_remoteHomePath = parent;
            return;
        }
    }
}

void AppController::setFileStatus(bool available, const QString &text) {
    if (m_fileAvailable == available && m_fileStatusText == text)
        return;
    m_fileAvailable = available;
    m_fileStatusText = text;
    emit fileStatusChanged();
}

void AppController::trackFsRequest(quint64 requestId, const PendingFs &pending) {
    if (requestId == 0)
        return;
    m_fsPending.insert(requestId, pending);
    emit fileActivityChanged();
}

void AppController::clearPendingFs() {
    if (m_fsPending.isEmpty())
        return;
    m_fsPending.clear();
    emit fileActivityChanged();
}

} // namespace researchssh
