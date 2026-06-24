#include "AppController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QStringDecoder>
#include <QStringList>
#include <QVersionNumber>
#include <QVariantMap>

namespace researchssh {

#ifndef RESEARCHSSH_APP_VERSION
#define RESEARCHSSH_APP_VERSION "0.0.0"
#endif

#ifndef RESEARCHSSH_UPDATE_REPO
#define RESEARCHSSH_UPDATE_REPO "wweiyi2004/ResearchSSH-Next"
#endif

namespace {

constexpr quint64 kEditorReadLimit = 2 * 1024 * 1024;
constexpr auto kResourceBegin = "__RSSH_RESOURCE_BEGIN__";
constexpr auto kResourceEnd = "__RSSH_RESOURCE_END__";

QString versionWithoutPrefix(QString version) {
    version = version.trimmed();
    while (version.startsWith(QLatin1Char('v')) || version.startsWith(QLatin1Char('V')))
        version.remove(0, 1);
    return version;
}

QString preferredReleaseDownloadUrl(const QJsonArray &assets, const QString &htmlUrl) {
    QString firstUrl;
    QString zipUrl;
    QString installerUrl;

    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QString url = asset.value(QStringLiteral("browser_download_url")).toString();
        if (url.isEmpty())
            continue;
        if (firstUrl.isEmpty())
            firstUrl = url;
        const QString lower = name.toLower();
        if (zipUrl.isEmpty() && lower.endsWith(QStringLiteral(".zip")))
            zipUrl = url;
        if (installerUrl.isEmpty() &&
            (lower.contains(QStringLiteral("setup")) ||
             lower.contains(QStringLiteral("installer")) || lower.endsWith(QStringLiteral(".msi")) ||
             lower.endsWith(QStringLiteral(".exe")))) {
            installerUrl = url;
        }
    }

    if (!installerUrl.isEmpty())
        return installerUrl;
    if (!zipUrl.isEmpty())
        return zipUrl;
    if (!firstUrl.isEmpty())
        return firstUrl;
    return htmlUrl;
}

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

enum class PackageAction { Install, Update, Remove };

QString packageActionName(PackageAction action) {
    switch (action) {
    case PackageAction::Install:
        return QStringLiteral("install");
    case PackageAction::Update:
        return QStringLiteral("update");
    case PackageAction::Remove:
        return QStringLiteral("remove");
    }
    return QStringLiteral("install");
}

bool isPipManager(const QString &manager) {
    return manager == QStringLiteral("pip") || manager == QStringLiteral("pip3");
}

bool isCondaManager(const QString &manager) {
    return manager == QStringLiteral("conda") || manager == QStringLiteral("mamba");
}

bool isSystemPackageManager(const QString &manager) {
    return manager == QStringLiteral("apt") || manager == QStringLiteral("dnf") ||
           manager == QStringLiteral("yum") || manager == QStringLiteral("pacman") ||
           manager == QStringLiteral("zypper");
}

bool isNpmManager(const QString &manager) {
    return manager == QStringLiteral("npm");
}

bool canInstallPackageManager(const QString &manager) {
    return isPipManager(manager) || manager == QStringLiteral("uv") || isCondaManager(manager) ||
           isSystemPackageManager(manager) || isNpmManager(manager);
}

bool canUpdatePackageManager(const QString &manager) {
    return canInstallPackageManager(manager);
}

bool canRemovePackageManager(const QString &manager) {
    return isPipManager(manager) || manager == QStringLiteral("uv") || isCondaManager(manager) ||
           isSystemPackageManager(manager) || isNpmManager(manager);
}

QString packageListCommand() {
    return QStringLiteral(
        "printf '__RSSH_PKG_LIST__\\n'; "
        "if command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1; then "
        "PY=python3; command -v python3 >/dev/null 2>&1 || PY=python; "
        "\"$PY\" -m pip list --format=freeze 2>/dev/null | awk -F'==' 'NF>=2 {print \"pip|\" $1 \"|\" $2}' | head -n 160; "
        "fi; "
        "if command -v conda >/dev/null 2>&1; then conda list 2>/dev/null | awk 'NF>=2 && $1 !~ /^#/ {print \"conda|\" $1 \"|\" $2}' | head -n 160; fi; "
        "if command -v mamba >/dev/null 2>&1 && ! command -v conda >/dev/null 2>&1; then mamba list 2>/dev/null | awk 'NF>=2 && $1 !~ /^#/ {print \"mamba|\" $1 \"|\" $2}' | head -n 160; fi; "
        "if command -v apt >/dev/null 2>&1; then apt list --installed 2>/dev/null | awk -F'[ /]' 'NR>1 {print \"apt|\" $1 \"|\" $3}' | head -n 160; fi; "
        "if command -v dnf >/dev/null 2>&1; then dnf -q list installed 2>/dev/null | awk 'NF>=2 {name=$1; sub(/\\.[^.]+$/, \"\", name); print \"dnf|\" name \"|\" $2}' | head -n 160; fi; "
        "if command -v yum >/dev/null 2>&1 && ! command -v dnf >/dev/null 2>&1; then yum -q list installed 2>/dev/null | awk 'NF>=2 {name=$1; sub(/\\.[^.]+$/, \"\", name); print \"yum|\" name \"|\" $2}' | head -n 160; fi; "
        "if command -v pacman >/dev/null 2>&1; then pacman -Q 2>/dev/null | awk 'NF>=2 {print \"pacman|\" $1 \"|\" $2}' | head -n 160; fi; "
        "if command -v zypper >/dev/null 2>&1; then zypper --non-interactive search --installed-only 2>/dev/null | awk -F'|' '$2 ~ /[A-Za-z0-9_.+-]/ {gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); gsub(/^[ \\t]+|[ \\t]+$/, \"\", $4); print \"zypper|\" $2 \"|\" $4}' | head -n 160; fi; "
        "if command -v npm >/dev/null 2>&1; then npm list -g --depth=0 --parseable 2>/dev/null | awk -F'/node_modules/' 'NF>=2 {print \"npm|\" $2 \"|global\"}' | head -n 120; fi; "
        "if command -v module >/dev/null 2>&1; then module list 2>&1 | sed 's/^[[:space:]]*//' | grep -E '^[0-9]+\\)' | sed 's/^[0-9]*)[[:space:]]*/module|/; s/$/|loaded/' | head -n 80; fi; "
        "printf '__RSSH_PKG_DONE__\\n'");
}

QString packageSearchCommand(const QString &query) {
    const QString quoted = shellQuote(query);
    return QStringLiteral(
               "printf '__RSSH_PKG_SEARCH__ %s\\n' %1; "
               "if command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1; then "
               "PY=python3; command -v python3 >/dev/null 2>&1 || PY=python; "
               "\"$PY\" -m pip index versions %1 2>/dev/null | awk -v q=%1 'NR==1 {ver=$0; sub(/^[^(]*\\(/, \"\", ver); sub(/\\).*/, \"\", ver); if (ver == \"\") ver=\"latest\"; print \"pip|\" q \"|\" ver \"|PyPI\"; exit}'; "
               "fi; "
               "if command -v conda >/dev/null 2>&1; then conda search %1 2>/dev/null | awk 'NF>=2 && $1 !~ /^#/ {print \"conda|\" $1 \"|\" $2 \"|Conda\"; count++; if (count>=12) exit}'; fi; "
               "if command -v mamba >/dev/null 2>&1; then mamba search %1 2>/dev/null | awk 'NF>=2 && $1 !~ /^#/ {print \"mamba|\" $1 \"|\" $2 \"|Mamba\"; count++; if (count>=12) exit}'; fi; "
               "if command -v apt-cache >/dev/null 2>&1; then apt-cache search --names-only %1 2>/dev/null | awk -F' - ' 'NF>=1 {print \"apt|\" $1 \"|available|\" $2}' | head -n 12; fi; "
               "if command -v dnf >/dev/null 2>&1; then dnf -q search %1 2>/dev/null | awk 'NF>=1 && $1 !~ /:$/ {name=$1; sub(/\\.[^.]+$/, \"\", name); print \"dnf|\" name \"|available|\" $0; count++; if (count>=12) exit}'; fi; "
               "if command -v yum >/dev/null 2>&1 && ! command -v dnf >/dev/null 2>&1; then yum -q search %1 2>/dev/null | awk 'NF>=1 && $1 !~ /:$/ {name=$1; sub(/\\.[^.]+$/, \"\", name); print \"yum|\" name \"|available|\" $0; count++; if (count>=12) exit}'; fi; "
               "if command -v pacman >/dev/null 2>&1; then pacman -Ss %1 2>/dev/null | awk '/^[^ ]/ {split($1,a,\"/\"); print \"pacman|\" a[2] \"|\" $2 \"|\" $1; count++; if (count>=12) exit}'; fi; "
               "if command -v zypper >/dev/null 2>&1; then zypper --non-interactive search %1 2>/dev/null | awk -F'|' '$2 ~ /[A-Za-z0-9_.+-]/ {gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); gsub(/^[ \\t]+|[ \\t]+$/, \"\", $4); print \"zypper|\" $2 \"|\" $4 \"|zypper\"; count++; if (count>=12) exit}'; fi; "
               "if command -v npm >/dev/null 2>&1; then npm search --parseable %1 2>/dev/null | awk -F'\\t' 'NF>=1 {print \"npm|\" $1 \"|\" $4 \"|\" $2; count++; if (count>=12) exit}'; fi; "
               "if command -v module >/dev/null 2>&1; then module avail %1 2>&1 | sed 's/^[[:space:]]*//' | grep -E '.+' | head -n 8 | sed 's/^/module|/'; fi; "
               "printf '__RSSH_PKG_SEARCH_DONE__\\n'")
        .arg(quoted);
}

QString packageMutationCommand(PackageAction action, const QString &manager,
                               const QString &packageName) {
    const QString quoted = shellQuote(packageName);
    QString operation;
    if (isPipManager(manager)) {
        if (action == PackageAction::Remove) {
            operation = QStringLiteral(
                            "PY=python3; command -v python3 >/dev/null 2>&1 || PY=python; "
                            "\"$PY\" -m pip uninstall -y %1 2>&1")
                            .arg(quoted);
        } else {
            operation = QStringLiteral(
                            "PY=python3; command -v python3 >/dev/null 2>&1 || PY=python; "
                            "\"$PY\" -m pip install --user --upgrade %1 2>&1")
                            .arg(quoted);
        }
    } else if (manager == QStringLiteral("uv")) {
        operation = action == PackageAction::Remove
                        ? QStringLiteral("uv pip uninstall %1 2>&1").arg(quoted)
                        : QStringLiteral("uv pip install --upgrade %1 2>&1").arg(quoted);
    } else if (isCondaManager(manager)) {
        const QString verb = action == PackageAction::Install
                                 ? QStringLiteral("install")
                                 : (action == PackageAction::Update ? QStringLiteral("update")
                                                                    : QStringLiteral("remove"));
        operation = QStringLiteral("%1 %2 -y %3 2>&1").arg(manager, verb, quoted);
    } else if (manager == QStringLiteral("apt")) {
        const QString verb = action == PackageAction::Install
                                 ? QStringLiteral("install -y")
                                 : (action == PackageAction::Update
                                        ? QStringLiteral("install -y --only-upgrade")
                                        : QStringLiteral("remove -y"));
        operation = QStringLiteral(
                        "[ \"$(id -u 2>/dev/null || echo 1)\" -eq 0 ] || "
                        "{ echo 'system package changes require a root session' >&2; exit 126; }; "
                        "env DEBIAN_FRONTEND=noninteractive apt-get %1 %2 2>&1")
                        .arg(verb, quoted);
    } else if (manager == QStringLiteral("dnf") || manager == QStringLiteral("yum")) {
        const QString verb = action == PackageAction::Install
                                 ? QStringLiteral("install")
                                 : (action == PackageAction::Update ? QStringLiteral("upgrade")
                                                                    : QStringLiteral("remove"));
        operation = QStringLiteral(
                        "[ \"$(id -u 2>/dev/null || echo 1)\" -eq 0 ] || "
                        "{ echo 'system package changes require a root session' >&2; exit 126; }; "
                        "%1 -y %2 %3 2>&1")
                        .arg(manager, verb, quoted);
    } else if (manager == QStringLiteral("pacman")) {
        const QString verb = action == PackageAction::Remove ? QStringLiteral("-R")
                                                             : QStringLiteral("-S");
        operation = QStringLiteral(
                        "[ \"$(id -u 2>/dev/null || echo 1)\" -eq 0 ] || "
                        "{ echo 'system package changes require a root session' >&2; exit 126; }; "
                        "pacman %1 --noconfirm %2 2>&1")
                        .arg(verb, quoted);
    } else if (manager == QStringLiteral("zypper")) {
        const QString verb = action == PackageAction::Install
                                 ? QStringLiteral("install")
                                 : (action == PackageAction::Update ? QStringLiteral("update")
                                                                    : QStringLiteral("remove"));
        operation = QStringLiteral(
                        "[ \"$(id -u 2>/dev/null || echo 1)\" -eq 0 ] || "
                        "{ echo 'system package changes require a root session' >&2; exit 126; }; "
                        "zypper --non-interactive %1 %2 2>&1")
                        .arg(verb, quoted);
    } else if (manager == QStringLiteral("npm")) {
        const QString verb = action == PackageAction::Install
                                 ? QStringLiteral("install")
                                 : (action == PackageAction::Update ? QStringLiteral("update")
                                                                    : QStringLiteral("uninstall"));
        operation = QStringLiteral("npm %1 -g %2 2>&1").arg(verb, quoted);
    }
    if (operation.isEmpty())
        return {};

    return QStringLiteral(
               "printf '__RSSH_PKG_ACTION__ %s %s %s\\n' %1 %2 %3; "
               "%4; rc=$?; "
               "printf '__RSSH_PKG_ACTION_DONE__\\n'; exit $rc")
        .arg(shellQuote(packageActionName(action)), shellQuote(manager), quoted, operation);
}

QString tailForUi(const QString &text, qsizetype limit = 1800) {
    QString trimmed = text.trimmed();
    if (trimmed.size() <= limit)
        return trimmed;
    return QStringLiteral("...\n%1").arg(trimmed.right(limit));
}

bool packageAlreadyInstalled(const QVariantList &packages, const QString &manager,
                             const QString &name) {
    for (const QVariant &value : packages) {
        const QVariantMap pkg = value.toMap();
        if (pkg.value(QStringLiteral("manager")).toString().compare(manager, Qt::CaseInsensitive) ==
                0 &&
            pkg.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
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
    loadServers();
    if (m_servers->count() > 0)
        selectServer(0);
    m_terminal->appendNotice(
        QStringLiteral("ResearchSSH-Next 核心 %1 就绪(凭据后端：%2)。")
            .arg(RustCoreBridge::version(), credentialBackend()));
    m_terminal->appendNotice(
        m_servers->count() > 0
            ? QStringLiteral("已加载 %1 个 SSH 服务器。").arg(m_servers->count())
            : QStringLiteral("点击左侧“添加服务器”添加 SSH 服务器。"));
    return true;
}

QString AppController::appVersion() const {
    return QStringLiteral(RESEARCHSSH_APP_VERSION);
}

void AppController::checkForUpdates() {
    if (m_updateBusy)
        return;

    if (!m_updateNetwork)
        m_updateNetwork = new QNetworkAccessManager(this);

    m_updateBusy = true;
    m_updateAvailable = false;
    m_updateDownloadUrl.clear();
    m_updateStatusText = QStringLiteral("正在检查热更新...");
    emit updateStateChanged();

    const QString repo = QStringLiteral(RESEARCHSSH_UPDATE_REPO);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest")
                                    .arg(repo)));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("ResearchSSH-Next/%1").arg(appVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");

    QNetworkReply *reply = m_updateNetwork->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QNetworkReply::NetworkError error = reply->error();
        const QByteArray payload = reply->readAll();
        const QString message = error == QNetworkReply::NoError ? QString() : reply->errorString();
        reply->deleteLater();
        finishUpdateCheck(payload, message);
    });
}

void AppController::openUpdateDownload() {
    if (m_updateDownloadUrl.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl(m_updateDownloadUrl));
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
        const RsErrorCode execRc = m_bridge.setExecCallback(m_session, this);
        if (execRc != RsErrorCode_Ok)
            m_terminal->appendNotice(
                QStringLiteral("注册任务回调失败：%1").arg(RustCoreBridge::describe(execRc)));

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

    saveServers();
    connectToServer(index);
}

void AppController::editServer(int index, const QString &host, int port, const QString &username,
                               const QString &password, const QString &name,
                               const QString &keyPath, const QString &keyPassphrase) {
    if (!m_servers->isValidIndex(index))
        return;
    const QString cleanHost = host.trimmed();
    const QString cleanUser = username.trimmed();
    const quint16 p = port > 0 ? static_cast<quint16>(port) : 22;
    if (cleanHost.isEmpty() || cleanUser.isEmpty() || port <= 0 || port > 65535) {
        m_terminal->appendNotice(QStringLiteral("服务器配置无效：主机、用户名和端口不能为空。"));
        return;
    }

    const QString oldEndpoint = endpointFor(index);
    const QString newEndpoint = QStringLiteral("%1@%2:%3").arg(cleanUser, cleanHost).arg(p);
    const bool endpointChanged = oldEndpoint != newEndpoint;
    const bool editingCurrentSession = index == m_currentIndex;

    const bool hadPassword = m_credentials->contains(oldEndpoint);
    const QByteArray oldPassword = hadPassword ? m_credentials->load(oldEndpoint) : QByteArray();
    const QString oldKeyPassKey = oldEndpoint + QStringLiteral("#keypass");
    const QString newKeyPassKey = newEndpoint + QStringLiteral("#keypass");
    const bool hadKeyPass = m_credentials->contains(oldKeyPassKey);
    const QByteArray oldKeyPass = hadKeyPass ? m_credentials->load(oldKeyPassKey) : QByteArray();

    if (editingCurrentSession) {
        teardownSession();
        m_connectionState = RsSessionState_Idle;
        emit connectionStateChanged();
        if (!m_currentEndpoint.isEmpty()) {
            m_currentEndpoint.clear();
            emit currentEndpointChanged();
        }
    }

    if (!m_servers->updateServer(index, name, cleanHost, p, cleanUser,
                                 static_cast<int>(RsProviderKind_Russh), keyPath)) {
        m_terminal->appendNotice(QStringLiteral("服务器配置更新失败。"));
        return;
    }
    m_servers->setStatus(index, RsSessionState_Idle);

    if (!password.isEmpty()) {
        m_credentials->store(newEndpoint, password.toUtf8());
        if (endpointChanged)
            m_credentials->remove(oldEndpoint);
    } else if (endpointChanged && hadPassword) {
        m_credentials->store(newEndpoint, oldPassword);
        m_credentials->remove(oldEndpoint);
    }

    if (!keyPassphrase.isEmpty()) {
        m_credentials->store(newKeyPassKey, keyPassphrase.toUtf8());
        if (endpointChanged)
            m_credentials->remove(oldKeyPassKey);
    } else if (endpointChanged && hadKeyPass) {
        m_credentials->store(newKeyPassKey, oldKeyPass);
        m_credentials->remove(oldKeyPassKey);
    }

    saveServers();
    selectServer(index);
    m_terminal->appendNotice(QStringLiteral("已更新服务器：%1").arg(newEndpoint));
}

void AppController::deleteServer(int index) {
    if (!m_servers->isValidIndex(index))
        return;

    const QString endpoint = endpointFor(index);
    const bool deletingCurrentSession = index == m_currentIndex;

    if (deletingCurrentSession) {
        teardownSession();
        m_connectionState = RsSessionState_Idle;
        emit connectionStateChanged();
        if (!m_currentEndpoint.isEmpty()) {
            m_currentEndpoint.clear();
            emit currentEndpointChanged();
        }
    } else if (m_currentIndex > index) {
        --m_currentIndex;
    }

    m_credentials->remove(endpoint);
    m_credentials->remove(endpoint + QStringLiteral("#keypass"));

    if (!m_servers->removeServer(index))
        return;
    saveServers();

    int nextSelected = m_selectedIndex;
    if (m_selectedIndex == index)
        nextSelected = m_servers->count() == 0 ? -1 : qMin(index, m_servers->count() - 1);
    else if (m_selectedIndex > index)
        --nextSelected;
    if (nextSelected != m_selectedIndex || m_selectedIndex == index) {
        m_selectedIndex = nextSelected;
        emit selectedIndexChanged();
    }

    m_terminal->appendNotice(QStringLiteral("已删除服务器：%1").arg(endpoint));
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

bool AppController::sendTerminalBytes(const QByteArray &payload,
                                      const QString &failureContext) {
    const RsErrorCode rc = m_bridge.sendToSession(m_session, payload);
    if (rc != RsErrorCode_Ok) {
        m_terminal->appendNotice(
            QStringLiteral("%1：%2").arg(failureContext, RustCoreBridge::describe(rc)));
        return false;
    }
    return true;
}

bool AppController::sendShellLine(const QString &line, const QString &failureContext) {
    QByteArray payload = line.toUtf8();
    payload.append('\r');
    return sendTerminalBytes(payload, failureContext);
}

void AppController::sendCommand(const QString &text) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法发送命令。"));
        return;
    }
    if (text.trimmed().isEmpty())
        return;
    sendShellLine(text, QStringLiteral("发送失败"));
}

void AppController::sendInterrupt() {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法中止程序。"));
        return;
    }
    const QByteArray payload(1, '\x03');
    if (sendTerminalBytes(payload, QStringLiteral("中止信号发送失败")))
        m_terminal->appendNotice(QStringLiteral("已发送 Ctrl+C。"));
}

void AppController::runQuickCommand(const QString &command) {
    sendCommand(command);
}

void AppController::clearTerminal() {
    m_terminal->clear();
}

quint64 AppController::submitExec(const QString &command, quint64 timeoutMs, int kind,
                                  const QString &label) {
    if (!m_session || m_connectionState != RsSessionState_Connected)
        return 0;
    const quint64 id = m_bridge.exec(m_session, command, timeoutMs);
    if (id == 0)
        return 0;
    m_execPending.insert(id, PendingExec{static_cast<PendingExec::Kind>(kind), label});
    return id;
}

void AppController::refreshEnvironment() {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_packageStatusText = QStringLiteral("未连接，无法扫描环境");
        emit packageStateChanged();
        return;
    }
    if (m_packageBusy)
        return;

    m_packageBusy = true;
    m_packageStatusText = QStringLiteral("正在扫描服务器环境...");
    emit packageStateChanged();

    const QString probe = QStringLiteral(
        "printf '__RSSH_ENV_PROBE__\\n'; "
        "if [ -r /etc/os-release ]; then . /etc/os-release; printf 'os=%s\\n' \"${PRETTY_NAME:-$NAME}\"; else uname -s | sed 's/^/os=/'; fi; "
        "printf 'kernel=%s\\n' \"$(uname -r 2>/dev/null || true)\"; "
        "printf 'arch=%s\\n' \"$(uname -m 2>/dev/null || true)\"; "
        "for tool in apt dnf yum pacman zypper python3 python pip pip3 uv conda mamba npm module ml spack; do "
        "p=$(command -v \"$tool\" 2>/dev/null || true); [ -n \"$p\" ] && printf 'tool:%s=%s\\n' \"$tool\" \"$p\"; "
        "done; "
        "(python3 --version 2>&1 || python --version 2>&1 || true) | head -n 1 | sed 's/^/version:python=/'; "
        "(gcc --version 2>/dev/null || true) | head -n 1 | sed 's/^/version:gcc=/'; "
        "(nvcc --version 2>/dev/null | grep release || true) | sed 's/^/version:cuda=/'; "
        "printf '__RSSH_ENV_DONE__\\n'");
    if (submitExec(probe, 8000, PendingExec::EnvironmentProbe, QStringLiteral("环境扫描")) == 0) {
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("环境扫描请求提交失败");
        emit packageStateChanged();
    }
}

void AppController::searchPackages(const QString &query) {
    const QString q = query.trimmed();
    if (q.isEmpty())
        return;
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_packageStatusText = QStringLiteral("未连接，无法搜索包");
        emit packageStateChanged();
        return;
    }
    if (m_packageBusy)
        return;

    m_packageBusy = true;
    m_packageStatusText = QStringLiteral("正在搜索：%1").arg(q);
    m_packageSearchResults.clear();
    emit packageStateChanged();

    const QString command = packageSearchCommand(q);
    if (submitExec(command, 12000, PendingExec::PackageSearch, q) == 0) {
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("搜索请求提交失败");
        emit packageStateChanged();
    }
}

void AppController::installPackage(const QString &manager, const QString &name) {
    const QString mgr = manager.trimmed().toLower();
    const QString pkg = name.trimmed();
    if (pkg.isEmpty())
        return;
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_packageStatusText = QStringLiteral("未连接，无法安装包");
        emit packageStateChanged();
        return;
    }
    if (m_packageBusy)
        return;

    if (!canInstallPackageManager(mgr)) {
        m_packageStatusText = QStringLiteral("%1 来源暂不支持一键安装").arg(manager);
        emit packageStateChanged();
        return;
    }
    const QString command = packageMutationCommand(PackageAction::Install, mgr, pkg);
    if (command.isEmpty()) {
        m_packageStatusText = QStringLiteral("%1 来源暂不支持一键安装").arg(manager);
        emit packageStateChanged();
        return;
    }

    m_packageBusy = true;
    m_packageLogText.clear();
    m_packageStatusText = QStringLiteral("正在安装 %1（%2）...").arg(pkg, manager);
    emit packageStateChanged();
    if (submitExec(command, 300000, PendingExec::PackageInstall, pkg) == 0) {
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("安装请求提交失败");
        emit packageStateChanged();
    }
}

void AppController::updatePackage(const QString &manager, const QString &name) {
    const QString mgr = manager.trimmed().toLower();
    const QString pkg = name.trimmed();
    if (pkg.isEmpty())
        return;
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_packageStatusText = QStringLiteral("未连接，无法更新包");
        emit packageStateChanged();
        return;
    }
    if (m_packageBusy)
        return;
    if (!canUpdatePackageManager(mgr)) {
        m_packageStatusText = QStringLiteral("%1 来源暂不支持一键更新").arg(manager);
        emit packageStateChanged();
        return;
    }
    const QString command = packageMutationCommand(PackageAction::Update, mgr, pkg);
    if (command.isEmpty()) {
        m_packageStatusText = QStringLiteral("%1 来源暂不支持一键更新").arg(manager);
        emit packageStateChanged();
        return;
    }

    m_packageBusy = true;
    m_packageLogText.clear();
    m_packageStatusText = QStringLiteral("正在更新 %1（%2）...").arg(pkg, manager);
    emit packageStateChanged();
    if (submitExec(command, 300000, PendingExec::PackageUpdate, pkg) == 0) {
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("更新请求提交失败");
        emit packageStateChanged();
    }
}

void AppController::removePackage(const QString &manager, const QString &name) {
    const QString mgr = manager.trimmed().toLower();
    const QString pkg = name.trimmed();
    if (pkg.isEmpty())
        return;
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_packageStatusText = QStringLiteral("未连接，无法卸载包");
        emit packageStateChanged();
        return;
    }
    if (m_packageBusy)
        return;
    if (!canRemovePackageManager(mgr)) {
        m_packageStatusText = QStringLiteral("%1 来源暂不支持一键卸载").arg(manager);
        emit packageStateChanged();
        return;
    }
    const QString command = packageMutationCommand(PackageAction::Remove, mgr, pkg);
    if (command.isEmpty()) {
        m_packageStatusText = QStringLiteral("%1 来源暂不支持一键卸载").arg(manager);
        emit packageStateChanged();
        return;
    }

    m_packageBusy = true;
    m_packageLogText.clear();
    m_packageStatusText = QStringLiteral("正在卸载 %1（%2）...").arg(pkg, manager);
    emit packageStateChanged();
    if (submitExec(command, 300000, PendingExec::PackageRemove, pkg) == 0) {
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("卸载请求提交失败");
        emit packageStateChanged();
    }
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
        const QRegularExpression gpuIndexPattern(QStringLiteral("^\\d+$"));
        if (!gpuIndexPattern.match(gpu).hasMatch()) {
            m_terminal->appendNotice(QStringLiteral("GPU 编号无效：%1").arg(device));
            return;
        }
        envPrefix = QStringLiteral("CUDA_VISIBLE_DEVICES=%1 ").arg(gpu);
        label = QStringLiteral("GPU %1").arg(gpu);
    }

    m_terminal->appendNotice(QStringLiteral("运行 %1，目标：%2").arg(path, label));
    const QString command =
        QStringLiteral("%1PYTHON_BIN=$(command -v python3 2>/dev/null || command -v python 2>/dev/null); "
                       "if [ -z \"$PYTHON_BIN\" ]; then echo 'python3/python not found' >&2; exit 127; fi; "
                       "\"$PYTHON_BIN\" %2")
            .arg(envPrefix, shellQuote(path));
    sendShellLine(command, QStringLiteral("运行 Python 失败"));
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
    if (submitExec(command, 8000, PendingExec::ResourceSnapshot, QStringLiteral("资源快照")) == 0) {
        m_resourceSnapshotBusy = false;
        m_resourceCapture.clear();
        m_resourceMarkerTail.clear();
        m_resourceSnapshotText = QStringLiteral("资源快照命令发送失败");
        emit resourceSnapshotChanged();
    }
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
            if (!captureResourceOutput(ev.data))
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
                if (pending.mutation == PendingFs::DeleteLike) {
                    m_editor->removePath(pending.path, pending.recursive);
                } else if (pending.mutation == PendingFs::MoveLike) {
                    m_editor->movePath(pending.path, pending.destinationPath);
                }
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
    trackFsRequest(id, PendingFs{PendingFs::Mutate,
                                 m_clipboardCut ? source : dest,
                                 destDir,
                                 m_clipboardCut ? sourceParent : QString(),
                                 m_clipboardCut,
                                 m_clipboardCut ? PendingFs::MoveLike : PendingFs::CopyLike,
                                 dest,
                                 false});
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
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parent, {}, false,
                                 PendingFs::MoveLike, dest, false});
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
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parent, {}, false,
                                 PendingFs::DeleteLike, {}, isDir});
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
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parentDir, {}, false,
                                 PendingFs::CreateLike, path, false});
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
    trackFsRequest(id, PendingFs{PendingFs::Mutate, path, parentDir, {}, false,
                                 PendingFs::CreateLike, path, false});
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
    trackFsRequest(id, PendingFs{PendingFs::Mutate, dest, destDir, {}, false,
                                 PendingFs::CreateLike, dest, false});
}

void AppController::saveEditor(const QString &text) {
    saveEditorPath(m_editor->path(), text);
}

void AppController::saveEditorPath(const QString &path, const QString &text) {
    if (!m_session || m_connectionState != RsSessionState_Connected) {
        m_terminal->appendNotice(QStringLiteral("未连接,无法保存远端文件。"));
        return;
    }
    if (path.isEmpty())
        return;

    m_editor->activatePath(path);
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
        m_resourceSnapshotBusy = false;
        m_resourceCapture.clear();
        m_resourceMarkerTail.clear();
        m_execPending.clear();
        m_packageTools.clear();
        m_installedPackages.clear();
        m_packageSearchResults.clear();
        m_packageLogText.clear();
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("连接后可扫描环境");
        emit packageStateChanged();
        emit resourceSnapshotChanged();
        setFileStatus(false, QStringLiteral("连接后显示远端文件"));
        m_fileTree->clearTree();
        m_editor->removePath({}, true);
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
    m_resourceSnapshotBusy = false;
    m_resourceCapture.clear();
    m_resourceMarkerTail.clear();
    m_execPending.clear();
    m_packageTools.clear();
    m_installedPackages.clear();
    m_packageSearchResults.clear();
    m_packageLogText.clear();
    m_packageBusy = false;
    m_packageStatusText = QStringLiteral("连接后可扫描环境");
    emit packageStateChanged();
    emit resourceSnapshotChanged();
    setFileStatus(false, QStringLiteral("连接后显示远端文件"));
    emit clipboardChanged();
    m_fileTree->clearTree();
    m_editor->removePath({}, true);
    m_currentIndex = -1;
}

void AppController::loadServers() {
    QSettings settings;
    const int size = settings.beginReadArray(QStringLiteral("servers"));
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        const QString host = settings.value(QStringLiteral("host")).toString().trimmed();
        const QString username =
            settings.value(QStringLiteral("username")).toString().trimmed();
        if (host.isEmpty() || username.isEmpty())
            continue;
        const QString name = settings.value(QStringLiteral("name")).toString();
        const int port = settings.value(QStringLiteral("port"), 22).toInt();
        const QString keyPath = settings.value(QStringLiteral("keyPath")).toString();
        m_servers->addServer(name, host, port, username,
                             static_cast<int>(RsProviderKind_Russh), keyPath);
    }
    settings.endArray();
}

void AppController::saveServers() const {
    QSettings settings;
    settings.beginWriteArray(QStringLiteral("servers"));
    for (int i = 0; i < m_servers->count(); ++i) {
        const ServerItem &item = m_servers->itemAt(i);
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("name"), item.name);
        settings.setValue(QStringLiteral("host"), item.host);
        settings.setValue(QStringLiteral("port"), item.port);
        settings.setValue(QStringLiteral("username"), item.username);
        settings.setValue(QStringLiteral("keyPath"), item.keyPath);
    }
    settings.endArray();
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

void AppController::ingestExecResults(const ExecResultBatch &batch) {
    for (const ExecResult &result : batch) {
        const auto it = m_execPending.find(result.requestId);
        if (it == m_execPending.end())
            continue;

        const PendingExec pending = it.value();
        m_execPending.erase(it);

        const bool isError = result.kind == static_cast<int>(RsExecResultKind_Error);
        const QString stdoutText = QString::fromUtf8(result.stdoutData);
        const QString stderrText =
            result.message.isEmpty() ? QString::fromUtf8(result.stderrData) : result.message;

        switch (pending.kind) {
        case PendingExec::ResourceSnapshot:
            m_resourceSnapshotBusy = false;
            if (isError || result.exitStatus != 0) {
                m_resourceSnapshotText = stderrText.isEmpty()
                                             ? QStringLiteral("资源快照采集失败")
                                             : QStringLiteral("资源快照采集失败：%1").arg(stderrText);
                emit resourceSnapshotChanged();
            } else {
                parseResourceSnapshot(stdoutText);
            }
            break;
        case PendingExec::EnvironmentProbe:
            m_packageBusy = false;
            if (isError || result.exitStatus != 0) {
                m_packageStatusText = stderrText.isEmpty()
                                          ? QStringLiteral("环境扫描失败")
                                          : QStringLiteral("环境扫描失败：%1").arg(stderrText);
            } else {
                parseEnvironmentProbe(stdoutText);
            }
            emit packageStateChanged();
            break;
        case PendingExec::PackageList:
            m_packageBusy = false;
            if (isError || result.exitStatus != 0) {
                m_packageStatusText = stderrText.isEmpty()
                                          ? QStringLiteral("已安装清单读取失败")
                                          : QStringLiteral("已安装清单读取失败：%1").arg(stderrText);
            } else {
                parseInstalledPackages(stdoutText);
            }
            emit packageStateChanged();
            break;
        case PendingExec::PackageSearch:
            m_packageBusy = false;
            if (isError || result.exitStatus != 0) {
                m_packageStatusText = stderrText.isEmpty()
                                          ? QStringLiteral("包搜索失败")
                                          : QStringLiteral("包搜索失败：%1").arg(stderrText);
            } else {
                parsePackageSearch(stdoutText);
            }
            emit packageStateChanged();
            break;
        case PendingExec::PackageInstall:
            m_packageBusy = false;
            finishPackageMutation(result, QStringLiteral("安装完成"), QStringLiteral("安装失败"),
                                  QStringLiteral("安装后刷新包列表"));
            emit packageStateChanged();
            break;
        case PendingExec::PackageUpdate:
            m_packageBusy = false;
            finishPackageMutation(result, QStringLiteral("更新完成"), QStringLiteral("更新失败"),
                                  QStringLiteral("更新后刷新包列表"));
            emit packageStateChanged();
            break;
        case PendingExec::PackageRemove:
            m_packageBusy = false;
            finishPackageMutation(result, QStringLiteral("卸载完成"), QStringLiteral("卸载失败"),
                                  QStringLiteral("卸载后刷新包列表"));
            emit packageStateChanged();
            break;
        }
    }
}

void AppController::parseEnvironmentProbe(const QString &text) {
    QVariantList tools;
    QString osText;
    QString pythonText;
    QString cudaText;

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("os="))) {
            osText = line.mid(3);
        } else if (line.startsWith(QStringLiteral("version:python="))) {
            pythonText = line.mid(QStringLiteral("version:python=").size());
        } else if (line.startsWith(QStringLiteral("version:cuda="))) {
            cudaText = line.mid(QStringLiteral("version:cuda=").size());
        } else if (line.startsWith(QStringLiteral("tool:"))) {
            const int eq = line.indexOf('=');
            if (eq <= 5)
                continue;
            QVariantMap tool;
            tool.insert(QStringLiteral("name"), line.mid(5, eq - 5));
            tool.insert(QStringLiteral("path"), line.mid(eq + 1));
            tools.push_back(tool);
        }
    }

    m_packageTools = tools;
    QStringList summary;
    if (!osText.isEmpty())
        summary.push_back(osText);
    if (!pythonText.isEmpty())
        summary.push_back(pythonText);
    if (!cudaText.isEmpty())
        summary.push_back(QStringLiteral("CUDA: %1").arg(cudaText));
    m_packageStatusText =
        summary.isEmpty() ? QStringLiteral("环境扫描完成") : summary.join(QStringLiteral(" · "));

    const QString listCommand = packageListCommand();
    m_packageBusy = true;
    if (submitExec(listCommand, 12000, PendingExec::PackageList, QStringLiteral("已安装包列表")) ==
        0) {
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("环境扫描完成，但已安装清单请求提交失败");
    }
}

void AppController::parseInstalledPackages(const QString &text) {
    QVariantList packages;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("__RSSH_")))
            continue;
        const QStringList parts = line.split('|');
        if (parts.size() < 3)
            continue;
        QVariantMap pkg;
        const QString manager = parts.at(0).trimmed();
        const QString name = parts.at(1).trimmed();
        pkg.insert(QStringLiteral("manager"), manager);
        pkg.insert(QStringLiteral("name"), name);
        pkg.insert(QStringLiteral("version"), parts.at(2).trimmed());
        const QString normalizedManager = manager.toLower();
        pkg.insert(QStringLiteral("canUpdate"), canUpdatePackageManager(normalizedManager));
        pkg.insert(QStringLiteral("canRemove"), canRemovePackageManager(normalizedManager));
        packages.push_back(pkg);
    }
    m_installedPackages = packages;
    m_packageStatusText = QStringLiteral("已安装包：%1 项").arg(packages.size());
}

void AppController::parsePackageSearch(const QString &text) {
    QVariantList results;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("__RSSH_")))
            continue;
        QVariantMap item;
        const QStringList parts = line.split('|');
        if (parts.size() >= 4) {
            const QString manager = parts.at(0).trimmed();
            const QString name = parts.at(1).trimmed();
            item.insert(QStringLiteral("manager"), manager);
            item.insert(QStringLiteral("name"), name);
            item.insert(QStringLiteral("version"), parts.at(2).trimmed());
            item.insert(QStringLiteral("detail"), parts.mid(3).join(QStringLiteral(" | ")).trimmed());
            const QString normalizedManager = manager.toLower();
            item.insert(QStringLiteral("canInstall"), canInstallPackageManager(normalizedManager));
            item.insert(QStringLiteral("installed"),
                        packageAlreadyInstalled(m_installedPackages, normalizedManager, name));
        } else if (parts.size() >= 2) {
            const QString manager = parts.at(0).trimmed();
            const QString name = parts.at(1).trimmed();
            item.insert(QStringLiteral("manager"), manager);
            item.insert(QStringLiteral("name"), name);
            item.insert(QStringLiteral("version"), QStringLiteral("latest"));
            item.insert(QStringLiteral("detail"), line);
            const QString normalizedManager = manager.toLower();
            item.insert(QStringLiteral("canInstall"), canInstallPackageManager(normalizedManager));
            item.insert(QStringLiteral("installed"),
                        packageAlreadyInstalled(m_installedPackages, normalizedManager, name));
        } else {
            continue;
        }
        results.push_back(item);
    }
    m_packageSearchResults = results;
    m_packageStatusText = QStringLiteral("搜索结果：%1 项").arg(results.size());
}

void AppController::finishUpdateCheck(const QByteArray &payload, const QString &networkError) {
    m_updateBusy = false;
    m_updateAvailable = false;
    m_updateDownloadUrl.clear();

    if (!networkError.isEmpty()) {
        m_updateStatusText = QStringLiteral("热更新检查失败：%1").arg(networkError);
        emit updateStateChanged();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_updateStatusText = QStringLiteral("热更新检查失败：响应格式无效");
        emit updateStateChanged();
        return;
    }

    const QJsonObject release = document.object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    const QString latestText = versionWithoutPrefix(tag);
    const QVersionNumber currentVersion = QVersionNumber::fromString(appVersion());
    const QVersionNumber latestVersion = QVersionNumber::fromString(latestText);
    const QString htmlUrl = release.value(QStringLiteral("html_url")).toString();
    const QString downloadUrl =
        preferredReleaseDownloadUrl(release.value(QStringLiteral("assets")).toArray(), htmlUrl);

    if (tag.isEmpty() || latestVersion.isNull()) {
        m_updateStatusText = QStringLiteral("热更新检查失败：未识别最新版本");
        m_updateDownloadUrl = htmlUrl;
        emit updateStateChanged();
        return;
    }

    if (!currentVersion.isNull() && QVersionNumber::compare(latestVersion, currentVersion) > 0) {
        m_updateAvailable = true;
        m_updateDownloadUrl = downloadUrl;
        m_updateStatusText = QStringLiteral("发现新版本 %1").arg(tag);
    } else {
        m_updateDownloadUrl = htmlUrl;
        m_updateStatusText = QStringLiteral("当前已是最新版本（%1）").arg(appVersion());
    }

    emit updateStateChanged();
}

void AppController::finishPackageMutation(const ExecResult &result, const QString &successText,
                                          const QString &failureText,
                                          const QString &refreshText) {
    const bool isError = result.kind == static_cast<int>(RsExecResultKind_Error);
    const QString output = QString::fromUtf8(result.stdoutData + result.stderrData).trimmed();
    m_packageLogText = tailForUi(output);
    if (isError || result.exitStatus != 0) {
        const QString detail =
            result.message.isEmpty() ? tailForUi(output, 700) : result.message.trimmed();
        m_packageStatusText = detail.isEmpty()
                                  ? failureText
                                  : QStringLiteral("%1，详情见操作日志").arg(failureText);
        if (m_packageLogText.isEmpty())
            m_packageLogText = detail;
        return;
    }
    m_packageStatusText = successText;
    const QString listCommand = packageListCommand();
    m_packageBusy = true;
    if (submitExec(listCommand, 12000, PendingExec::PackageList, refreshText) == 0) {
        m_packageBusy = false;
        m_packageStatusText = QStringLiteral("%1，但刷新包列表失败").arg(successText);
    }
}

bool AppController::captureResourceOutput(const QByteArray &data) {
    if (!m_resourceSnapshotBusy)
        return false;

    const QString chunk = QString::fromUtf8(data);
    const QString beginMarker = QString::fromLatin1(kResourceBegin);
    const QString endMarker = QString::fromLatin1(kResourceEnd);

    if (m_resourceCapture.isEmpty()) {
        const QString probe = m_resourceMarkerTail + chunk;
        const qsizetype begin = probe.indexOf(beginMarker);
        if (begin < 0) {
            m_resourceMarkerTail = probe.right(beginMarker.size() - 1);
            return false;
        }
        const qsizetype chunkStart = qMax<qsizetype>(0, begin - m_resourceMarkerTail.size());
        if (chunkStart > 0)
            m_terminal->appendBytes(data.left(static_cast<qsizetype>(chunkStart)));
        m_resourceCapture = probe.mid(begin);
        m_resourceMarkerTail.clear();
    } else {
        m_resourceCapture.append(chunk);
    }

    if (m_resourceCapture.size() > 50000) {
        m_resourceSnapshotBusy = false;
        m_resourceSnapshotText = QStringLiteral("资源快照过长，已停止解析");
        m_resourceCapture.clear();
        m_resourceMarkerTail.clear();
        emit resourceSnapshotChanged();
        return true;
    }
    const qsizetype end = m_resourceCapture.indexOf(endMarker);
    if (end >= 0) {
        const qsizetype capturedEnd = end + endMarker.size();
        const QString captured = m_resourceCapture.left(capturedEnd);
        const QString trailing = m_resourceCapture.mid(capturedEnd);
        m_resourceCapture.clear();
        m_resourceMarkerTail.clear();
        parseResourceSnapshot(captured);
        if (!trailing.isEmpty())
            m_terminal->appendBytes(trailing.toUtf8());
    }
    return true;
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
