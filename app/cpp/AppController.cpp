#include "AppController.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QVariantMap>

namespace researchssh {

namespace {

constexpr quint64 kEditorReadLimit = 2 * 1024 * 1024;
constexpr auto kResourceBegin = "__RSSH_RESOURCE_BEGIN__";
constexpr auto kResourceEnd = "__RSSH_RESOURCE_END__";

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

QString shellQuote(const QString &value) {
    QString quoted = value;
    quoted.replace('\'', QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(quoted);
}

int clampPercent(double value) {
    if (value < 0.0)
        return 0;
    if (value > 100.0)
        return 100;
    return static_cast<int>(value + 0.5);
}

bool lineEqualsMarker(const QString &line, const char *marker) {
    return line.trimmed() == QString::fromLatin1(marker);
}

bool containsMarkerLine(const QString &text, const char *marker) {
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")));
    for (const QString &line : lines) {
        if (lineEqualsMarker(line, marker))
            return true;
    }
    return false;
}

bool extractMarkedBody(const QString &text, QString *body) {
    QStringList bodyLines;
    bool inBody = false;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::KeepEmptyParts);
    for (const QString &line : lines) {
        if (!inBody) {
            if (lineEqualsMarker(line, kResourceBegin)) {
                inBody = true;
                bodyLines.clear();
            }
            continue;
        }
        if (lineEqualsMarker(line, kResourceEnd)) {
            if (body)
                *body = bodyLines.join(QLatin1Char('\n'));
            return true;
        }
        bodyLines.push_back(line);
    }
    return false;
}

} // namespace

AppController::AppController(QObject *parent) : QObject(parent) {
    m_servers = new ServerListModel(this);
    m_terminal = new TerminalViewModel(this);
    m_fileTree = new RemoteFileTreeModel(this);
    m_editor = new EditorViewModel(this);
    m_credentials = createDefaultCredentialStore();
    seedResourceSnapshot(QStringLiteral("示例资源快照，连接后可手动刷新"));

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

void AppController::sendInterrupt() {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法中止程序。"));
        return;
    }
    const QByteArray payload(1, '\x03');
    const RsErrorCode rc = m_bridge.sendToSession(m_session, payload);
    if (rc != RsErrorCode_Ok)
        m_terminal->appendNotice(
            QStringLiteral("中止信号发送失败：%1").arg(RustCoreBridge::describe(rc)));
    else
        m_terminal->appendNotice(QStringLiteral("已发送 Ctrl+C。"));
}

void AppController::runQuickCommand(const QString &command) {
    sendCommand(command);
}

void AppController::clearTerminal() {
    m_terminal->clear();
}

void AppController::runPythonFile(const QString &path, const QString &device) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法运行 Python 文件。"));
        return;
    }
    if (path.isEmpty() || !path.endsWith(QStringLiteral(".py"), Qt::CaseInsensitive)) {
        m_terminal->appendNotice(QStringLiteral("请选择一个 .py 文件再运行。"));
        return;
    }

    const QString target = device.trimmed().toLower();
    QString envPrefix;
    QString label;
    if (target == QStringLiteral("cpu")) {
        envPrefix = QStringLiteral("CUDA_VISIBLE_DEVICES='' ");
        label = QStringLiteral("CPU");
    } else {
        QString gpu = target;
        if (gpu.startsWith(QStringLiteral("gpu")))
            gpu = gpu.mid(3).trimmed();
        if (gpu.isEmpty())
            gpu = QStringLiteral("0");
        envPrefix = QStringLiteral("CUDA_VISIBLE_DEVICES=%1 ").arg(gpu);
        label = QStringLiteral("GPU %1").arg(gpu);
    }

    m_terminal->appendNotice(QStringLiteral("运行 %1，目标：%2").arg(path, label));
    sendCommand(QStringLiteral("%1python %2").arg(envPrefix, shellQuote(path)));
}

void AppController::refreshResourceSnapshot() {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        seedResourceSnapshot(QStringLiteral("未连接，显示示例资源快照"));
        return;
    }
    if (m_resourceSnapshotBusy)
        return;

    m_resourceSnapshotBusy = true;
    m_resourceCapture.clear();
    m_resourceMarkerTail.clear();
    m_resourceSnapshotText = QStringLiteral("正在采集远端资源快照...");
    emit resourceSnapshotChanged();

    const QString command = QStringLiteral(
        "printf '__RSSH_RESOURCE_BEGIN__\\n'; "
        "if command -v nvidia-smi >/dev/null 2>&1; then "
        "nvidia-smi --query-gpu=index,uuid,name,utilization.gpu,memory.used,memory.total "
        "--format=csv,noheader,nounits 2>/dev/null || true; "
        "printf '__RSSH_RESOURCE_PROCESSES__\\n'; "
        "nvidia-smi --query-compute-apps=gpu_uuid,pid,process_name,used_memory "
        "--format=csv,noheader,nounits 2>/dev/null || true; "
        "else printf '__RSSH_RESOURCE_PROCESSES__\\n'; fi; "
        "printf '__RSSH_RESOURCE_CPU__\\n'; "
        "ps -eo pid,user,comm,pcpu,pmem --sort=-pcpu 2>/dev/null | head -n 8 || true; "
        "printf '__RSSH_RESOURCE_JOBS__\\n'; "
        "if command -v squeue >/dev/null 2>&1; then "
        "squeue -h -o '%i|%P|%j|%u|%T|%M|%D|%R' 2>/dev/null | head -n 8 || true; fi; "
        "printf '__RSSH_RESOURCE_DISK__\\n'; "
        "df -hP 2>/dev/null | awk 'NR>1 {print $1 \"|\" $2 \"|\" $3 \"|\" $4 \"|\" $5 \"|\" $6}' | head -n 8 || true; "
        "printf '__RSSH_RESOURCE_END__\\n'");
    sendCommand(command);
}

void AppController::activateEditorPath(const QString &path) {
    if (!path.isEmpty())
        m_editor->activatePath(path);
}

void AppController::closeEditor() {
    m_editor->close();
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
            captureResourceOutput(ev.data);
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
                m_editor->failOpen(pending.path, result.message);
            }

            const QString msg =
                result.message.isEmpty()
                    ? RustCoreBridge::describe(static_cast<RsErrorCode>(result.errorCode))
                    : result.message;
            if (pending.kind == PendingFs::EditorSave)
                m_editor->finishSave(false, msg, pending.path);
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
                    m_editor->failOpen(pending.path, QStringLiteral("文件达到编辑器读取上限。"));
                    m_terminal->appendNotice(QStringLiteral(
                        "拒绝打开：文件达到编辑器读取上限 %1 MB。")
                                                .arg(kEditorReadLimit / 1024 / 1024));
                    break;
                }
                if (!looksLikeTextBuffer(result.data)) {
                    m_editor->failOpen(pending.path, QStringLiteral("该文件看起来不是 UTF-8 文本。"));
                    m_terminal->appendNotice(
                        QStringLiteral("拒绝打开：该文件看起来不是 UTF-8 文本。"));
                    break;
                }
                m_editor->setContent(pending.path, result.data);
                m_terminal->appendNotice(QStringLiteral("已打开远端文件：%1").arg(pending.path));
            } else {
                m_editor->failOpen(pending.path, QStringLiteral("返回结果类型不匹配。"));
                m_terminal->appendNotice(QStringLiteral("打开文件失败：返回结果类型不匹配。"));
            }
            break;
        case PendingFs::EditorSave:
            if (result.kind == RsFsResultKind_Ok) {
                m_editor->finishSave(true, {}, pending.path);
                m_terminal->appendNotice(QStringLiteral("已保存远端文件：%1").arg(pending.path));
                if (!pending.refreshDir.isNull())
                    expandDir(pending.refreshDir);
                if (!pending.extraRefreshDir.isNull() && pending.extraRefreshDir != pending.refreshDir)
                    expandDir(pending.extraRefreshDir);
            } else {
                // Any non-Ok, non-Error result is unexpected for a write; fail the
                // save so the editor doesn't stay stuck in the "saving" state.
                m_editor->finishSave(false, QStringLiteral("保存失败：返回结果类型不匹配。"),
                                     pending.path);
                m_terminal->appendNotice(QStringLiteral("保存失败：返回结果类型不匹配。"));
            }
            break;
        case PendingFs::Mutate:
            if (result.kind == RsFsResultKind_Ok) {
                m_terminal->appendNotice(QStringLiteral("文件操作完成。"));
                if (pending.path == m_editor->path())
                    m_editor->closePath(pending.path);
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
        m_editor->failOpen(path, QStringLiteral("无法提交读取请求。"));
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
        m_editor->finishSave(false, QStringLiteral("无法提交保存请求。"), path);
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

void AppController::seedResourceSnapshot(const QString &statusText) {
    QVariantMap cpu;
    cpu.insert(QStringLiteral("id"), QStringLiteral("CPU"));
    cpu.insert(QStringLiteral("kind"), QStringLiteral("CPU"));
    cpu.insert(QStringLiteral("name"), QStringLiteral("32 vCPU node"));
    cpu.insert(QStringLiteral("utilization"), 42);
    cpu.insert(QStringLiteral("memoryUsed"), 128);
    cpu.insert(QStringLiteral("memoryTotal"), 256);

    QVariantMap gpu0;
    gpu0.insert(QStringLiteral("id"), QStringLiteral("GPU 0"));
    gpu0.insert(QStringLiteral("kind"), QStringLiteral("GPU"));
    gpu0.insert(QStringLiteral("name"), QStringLiteral("NVIDIA A100-SXM4-80GB"));
    gpu0.insert(QStringLiteral("utilization"), 37);
    gpu0.insert(QStringLiteral("memoryUsed"), 12345);
    gpu0.insert(QStringLiteral("memoryTotal"), 81920);

    QVariantMap gpu1;
    gpu1.insert(QStringLiteral("id"), QStringLiteral("GPU 1"));
    gpu1.insert(QStringLiteral("kind"), QStringLiteral("GPU"));
    gpu1.insert(QStringLiteral("name"), QStringLiteral("NVIDIA A100-SXM4-80GB"));
    gpu1.insert(QStringLiteral("utilization"), 5);
    gpu1.insert(QStringLiteral("memoryUsed"), 2048);
    gpu1.insert(QStringLiteral("memoryTotal"), 81920);

    QVariantMap p0;
    p0.insert(QStringLiteral("pid"), QStringLiteral("29418"));
    p0.insert(QStringLiteral("user"), QStringLiteral("alice"));
    p0.insert(QStringLiteral("name"), QStringLiteral("python train.py"));
    p0.insert(QStringLiteral("device"), QStringLiteral("GPU 0"));
    p0.insert(QStringLiteral("cpu"), 66);
    p0.insert(QStringLiteral("memory"), 18);
    p0.insert(QStringLiteral("gpuMemory"), 11840);

    QVariantMap p1;
    p1.insert(QStringLiteral("pid"), QStringLiteral("18302"));
    p1.insert(QStringLiteral("user"), QStringLiteral("researcher"));
    p1.insert(QStringLiteral("name"), QStringLiteral("python eval.py"));
    p1.insert(QStringLiteral("device"), QStringLiteral("GPU 1"));
    p1.insert(QStringLiteral("cpu"), 21);
    p1.insert(QStringLiteral("memory"), 7);
    p1.insert(QStringLiteral("gpuMemory"), 2048);

    QVariantMap p2;
    p2.insert(QStringLiteral("pid"), QStringLiteral("911"));
    p2.insert(QStringLiteral("user"), QStringLiteral("root"));
    p2.insert(QStringLiteral("name"), QStringLiteral("systemd"));
    p2.insert(QStringLiteral("device"), QStringLiteral("CPU"));
    p2.insert(QStringLiteral("cpu"), 4);
    p2.insert(QStringLiteral("memory"), 1);
    p2.insert(QStringLiteral("gpuMemory"), 0);

    QVariantMap job0;
    job0.insert(QStringLiteral("id"), QStringLiteral("102934"));
    job0.insert(QStringLiteral("partition"), QStringLiteral("gpu"));
    job0.insert(QStringLiteral("name"), QStringLiteral("train.sh"));
    job0.insert(QStringLiteral("user"), QStringLiteral("alice"));
    job0.insert(QStringLiteral("state"), QStringLiteral("RUNNING"));
    job0.insert(QStringLiteral("time"), QStringLiteral("2:13:05"));
    job0.insert(QStringLiteral("nodes"), QStringLiteral("4"));
    job0.insert(QStringLiteral("where"), QStringLiteral("gpu[01-04]"));

    QVariantMap disk0;
    disk0.insert(QStringLiteral("filesystem"), QStringLiteral("/dev/nvme0n1"));
    disk0.insert(QStringLiteral("size"), QStringLiteral("1.8T"));
    disk0.insert(QStringLiteral("used"), QStringLiteral("0.9T"));
    disk0.insert(QStringLiteral("available"), QStringLiteral("0.8T"));
    disk0.insert(QStringLiteral("percent"), 53);
    disk0.insert(QStringLiteral("mount"), QStringLiteral("/home"));

    QVariantMap disk1;
    disk1.insert(QStringLiteral("filesystem"), QStringLiteral("scratch"));
    disk1.insert(QStringLiteral("size"), QStringLiteral("50T"));
    disk1.insert(QStringLiteral("used"), QStringLiteral("31T"));
    disk1.insert(QStringLiteral("available"), QStringLiteral("19T"));
    disk1.insert(QStringLiteral("percent"), 62);
    disk1.insert(QStringLiteral("mount"), QStringLiteral("/scratch"));

    m_resourceDevices = {cpu, gpu0, gpu1};
    m_resourceProcesses = {p0, p1, p2};
    m_resourceJobs = {job0};
    m_resourceDisks = {disk0, disk1};
    rebuildResourceProcessGroups();
    m_resourceSnapshotText = statusText;
    m_resourceSnapshotBusy = false;
    emit resourceSnapshotChanged();
}

void AppController::parseResourceSnapshot(const QString &text) {
    QString body;
    if (!extractMarkedBody(text, &body)) {
        m_resourceSnapshotBusy = false;
        m_resourceSnapshotText = QStringLiteral("资源快照解析失败：未找到完整标记");
        emit resourceSnapshotChanged();
        return;
    }

    enum Section { GpuDevices, GpuProcesses, CpuProcesses, Jobs, Disks };
    Section section = GpuDevices;
    QVariantList devices;
    QVariantList processes;
    QVariantList jobs;
    QVariantList disks;
    double cpuTotal = 0.0;
    double memTotal = 0.0;
    QHash<QString, QString> gpuUuidToLabel;

    const QStringList lines = body.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    const QRegularExpression cpuLine(
        QStringLiteral("^\\s*(\\d+)\\s+(\\S+)\\s+(\\S+)\\s+([0-9.]+)\\s+([0-9.]+)\\s*$"));

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line == QStringLiteral("__RSSH_RESOURCE_BEGIN__"))
            continue;
        if (line == QStringLiteral("__RSSH_RESOURCE_PROCESSES__")) {
            section = GpuProcesses;
            continue;
        }
        if (line == QStringLiteral("__RSSH_RESOURCE_CPU__")) {
            section = CpuProcesses;
            continue;
        }
        if (line == QStringLiteral("__RSSH_RESOURCE_JOBS__")) {
            section = Jobs;
            continue;
        }
        if (line == QStringLiteral("__RSSH_RESOURCE_DISK__")) {
            section = Disks;
            continue;
        }
        if (line.startsWith(QStringLiteral("PID ")))
            continue;

        if (section == GpuDevices) {
            const QStringList parts = line.split(',', Qt::KeepEmptyParts);
            if (parts.size() < 5)
                continue;
            bool okUtil = false;
            bool okUsed = false;
            bool okTotal = false;
            const bool hasUuid =
                parts.size() >= 6 && parts.at(1).trimmed().startsWith(QStringLiteral("GPU-"));
            const int size = parts.size();
            const QString index = parts.at(0).trimmed();
            const QString uuid = hasUuid ? parts.at(1).trimmed() : QString();
            const int nameStart = hasUuid ? 2 : 1;
            const int nameEndExclusive = size - 3;
            QStringList nameParts;
            for (int i = nameStart; i < nameEndExclusive; ++i)
                nameParts.push_back(parts.at(i).trimmed());
            const QString name = nameParts.join(QStringLiteral(", "));
            const int util = parts.at(size - 3).trimmed().toInt(&okUtil);
            const int used = parts.at(size - 2).trimmed().toInt(&okUsed);
            const int total = parts.at(size - 1).trimmed().toInt(&okTotal);
            if (!okUtil || !okUsed || !okTotal)
                continue;
            const QString label = QStringLiteral("GPU %1").arg(index);
            if (!uuid.isEmpty())
                gpuUuidToLabel.insert(uuid, label);
            QVariantMap gpu;
            gpu.insert(QStringLiteral("id"), label);
            gpu.insert(QStringLiteral("kind"), QStringLiteral("GPU"));
            gpu.insert(QStringLiteral("name"), name);
            gpu.insert(QStringLiteral("utilization"), clampPercent(util));
            gpu.insert(QStringLiteral("memoryUsed"), used);
            gpu.insert(QStringLiteral("memoryTotal"), total);
            devices.push_back(gpu);
        } else if (section == GpuProcesses) {
            const QStringList parts = line.split(',', Qt::KeepEmptyParts);
            if (parts.size() < 3)
                continue;
            bool okMem = false;
            const bool hasDevice = parts.size() >= 4;
            const int size = parts.size();
            QString device = hasDevice ? parts.at(0).trimmed() : QStringLiteral("GPU");
            device = gpuUuidToLabel.value(device, device);
            const QString pid = hasDevice ? parts.at(1).trimmed() : parts.at(0).trimmed();
            const int nameStart = hasDevice ? 2 : 1;
            const int nameEndExclusive = size - 1;
            QStringList nameParts;
            for (int i = nameStart; i < nameEndExclusive; ++i)
                nameParts.push_back(parts.at(i).trimmed());
            const QString name = nameParts.join(QStringLiteral(", "));
            const int gpuMem = parts.at(size - 1).trimmed().toInt(&okMem);
            QVariantMap proc;
            proc.insert(QStringLiteral("pid"), pid);
            proc.insert(QStringLiteral("user"), QStringLiteral("-"));
            proc.insert(QStringLiteral("name"), name);
            proc.insert(QStringLiteral("device"), device);
            proc.insert(QStringLiteral("cpu"), 0);
            proc.insert(QStringLiteral("memory"), 0);
            proc.insert(QStringLiteral("gpuMemory"), okMem ? gpuMem : 0);
            processes.push_back(proc);
        } else {
            if (section == Jobs) {
                const QStringList parts = line.split('|', Qt::KeepEmptyParts);
                if (parts.size() < 8)
                    continue;
                QVariantMap job;
                job.insert(QStringLiteral("id"), parts.at(0).trimmed());
                job.insert(QStringLiteral("partition"), parts.at(1).trimmed());
                job.insert(QStringLiteral("name"), parts.at(2).trimmed());
                job.insert(QStringLiteral("user"), parts.at(3).trimmed());
                job.insert(QStringLiteral("state"), parts.at(4).trimmed());
                job.insert(QStringLiteral("time"), parts.at(5).trimmed());
                job.insert(QStringLiteral("nodes"), parts.at(6).trimmed());
                job.insert(QStringLiteral("where"), parts.at(7).trimmed());
                jobs.push_back(job);
                continue;
            }
            if (section == Disks) {
                const QStringList parts = line.split('|', Qt::KeepEmptyParts);
                if (parts.size() < 6)
                    continue;
                bool okPct = false;
                QString pctText = parts.at(4).trimmed();
                pctText.chop(pctText.endsWith('%') ? 1 : 0);
                const int percent = pctText.toInt(&okPct);
                QVariantMap disk;
                disk.insert(QStringLiteral("filesystem"), parts.at(0).trimmed());
                disk.insert(QStringLiteral("size"), parts.at(1).trimmed());
                disk.insert(QStringLiteral("used"), parts.at(2).trimmed());
                disk.insert(QStringLiteral("available"), parts.at(3).trimmed());
                disk.insert(QStringLiteral("percent"), okPct ? clampPercent(percent) : 0);
                disk.insert(QStringLiteral("mount"), parts.at(5).trimmed());
                disks.push_back(disk);
                continue;
            }
            const auto match = cpuLine.match(line);
            if (!match.hasMatch())
                continue;
            const double cpu = match.captured(4).toDouble();
            const double mem = match.captured(5).toDouble();
            cpuTotal += cpu;
            memTotal += mem;
            QVariantMap proc;
            proc.insert(QStringLiteral("pid"), match.captured(1));
            proc.insert(QStringLiteral("user"), match.captured(2));
            proc.insert(QStringLiteral("name"), match.captured(3));
            proc.insert(QStringLiteral("device"), QStringLiteral("CPU"));
            proc.insert(QStringLiteral("cpu"), clampPercent(cpu));
            proc.insert(QStringLiteral("memory"), clampPercent(mem));
            proc.insert(QStringLiteral("gpuMemory"), 0);
            processes.push_back(proc);
        }
    }

    QVariantMap cpu;
    cpu.insert(QStringLiteral("id"), QStringLiteral("CPU"));
    cpu.insert(QStringLiteral("kind"), QStringLiteral("CPU"));
    cpu.insert(QStringLiteral("name"), QStringLiteral("Top process load"));
    cpu.insert(QStringLiteral("utilization"), clampPercent(cpuTotal));
    cpu.insert(QStringLiteral("memoryUsed"), clampPercent(memTotal));
    cpu.insert(QStringLiteral("memoryTotal"), 100);
    devices.push_front(cpu);

    if (devices.isEmpty())
        seedResourceSnapshot(QStringLiteral("未解析到远端资源，显示示例快照"));
    else {
        m_resourceDevices = devices;
        m_resourceProcesses = processes;
        m_resourceJobs = jobs;
        m_resourceDisks = disks;
        rebuildResourceProcessGroups();
        m_resourceSnapshotText =
            QStringLiteral("快照：%1").arg(QDateTime::currentDateTime().toString("HH:mm:ss"));
        m_resourceSnapshotBusy = false;
        emit resourceSnapshotChanged();
    }
}

void AppController::captureResourceOutput(const QByteArray &data) {
    const QString chunk = QString::fromUtf8(data);
    QString captureChunk = chunk;
    if (!m_resourceSnapshotBusy) {
        const QString probe = m_resourceMarkerTail + chunk;
        if (!containsMarkerLine(probe, kResourceBegin)) {
            m_resourceMarkerTail = probe.right(QString::fromLatin1(kResourceBegin).size() + 2);
            return;
        }
        captureChunk = probe;
        m_resourceMarkerTail.clear();
        m_resourceSnapshotBusy = true;
    }

    m_resourceCapture.append(captureChunk);
    if (!containsMarkerLine(m_resourceCapture, kResourceBegin)) {
        m_resourceCapture = m_resourceCapture.right(QString::fromLatin1(kResourceBegin).size() + 2);
        return;
    }

    if (m_resourceCapture.size() > 50000) {
        m_resourceSnapshotBusy = false;
        m_resourceSnapshotText = QStringLiteral("资源快照过长，已停止解析");
        m_resourceCapture.clear();
        m_resourceMarkerTail.clear();
        emit resourceSnapshotChanged();
        return;
    }
    if (extractMarkedBody(m_resourceCapture, nullptr)) {
        const QString captured = m_resourceCapture;
        m_resourceCapture.clear();
        m_resourceMarkerTail.clear();
        parseResourceSnapshot(captured);
    }
}

void AppController::rebuildResourceProcessGroups() {
    QHash<QString, QVariantList> buckets;
    QHash<QString, double> cpuTotals;
    QHash<QString, int> memoryTotals;

    for (const QVariant &value : m_resourceProcesses) {
        const QVariantMap process = value.toMap();
        QString device = process.value(QStringLiteral("device")).toString();
        if (device.isEmpty())
            device = QStringLiteral("CPU");
        buckets[device].push_back(process);
        cpuTotals[device] += process.value(QStringLiteral("cpu")).toDouble();
        memoryTotals[device] += process.value(QStringLiteral("gpuMemory")).toInt();
    }

    QVariantList groups;
    auto appendGroup = [&](const QString &device) {
        if (!buckets.contains(device))
            return;
        QVariantMap group;
        group.insert(QStringLiteral("device"), device);
        group.insert(QStringLiteral("processes"), buckets.value(device));
        group.insert(QStringLiteral("count"), buckets.value(device).size());
        group.insert(QStringLiteral("cpu"), clampPercent(cpuTotals.value(device)));
        group.insert(QStringLiteral("gpuMemory"), memoryTotals.value(device));
        groups.push_back(group);
    };

    appendGroup(QStringLiteral("CPU"));
    const auto keys = buckets.keys();
    for (const QString &device : keys) {
        if (device != QStringLiteral("CPU"))
            appendGroup(device);
    }
    m_resourceProcessGroups = groups;
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
