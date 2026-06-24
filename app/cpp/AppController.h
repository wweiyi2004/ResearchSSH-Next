// AppController — the top-level object QML talks to.
//
// It orchestrates the UI-facing models and the Rust core via RustCoreBridge. QML
// only ever calls into this controller and reads the exposed models/properties;
// it never sees a secret, a raw handle, or the FFI.
//
// All methods here run on the Qt UI thread. Rust events arrive via
// ingestRustEvents(), which RustCoreBridge marshals onto this thread.

#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <memory>

#include "CredentialStore.h"
#include "EditorViewModel.h"
#include "RemoteFileTreeModel.h"
#include "RustCoreBridge.h"
#include "ServerListModel.h"
#include "TerminalViewModel.h"

namespace researchssh {

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(ServerListModel *serverModel READ serverModel CONSTANT)
    Q_PROPERTY(TerminalViewModel *terminal READ terminal CONSTANT)
    Q_PROPERTY(RemoteFileTreeModel *fileTree READ fileTree CONSTANT)
    Q_PROPERTY(EditorViewModel *editor READ editor CONSTANT)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString coreVersion READ coreVersion CONSTANT)
    Q_PROPERTY(QString credentialBackend READ credentialBackend CONSTANT)
    Q_PROPERTY(QString updateStatusText READ updateStatusText NOTIFY updateStateChanged)
    Q_PROPERTY(bool updateBusy READ updateBusy NOTIFY updateStateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateStateChanged)
    Q_PROPERTY(QString updateDownloadUrl READ updateDownloadUrl NOTIFY updateStateChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedIndexChanged)
    Q_PROPERTY(int connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString connectionStateText READ connectionStateText NOTIFY connectionStateChanged)
    Q_PROPERTY(QString currentEndpoint READ currentEndpoint NOTIFY currentEndpointChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectionStateChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY connectionStateChanged)
    Q_PROPERTY(bool fileBusy READ fileBusy NOTIFY fileActivityChanged)
    Q_PROPERTY(int pendingFileOperations READ pendingFileOperations NOTIFY fileActivityChanged)
    Q_PROPERTY(bool fileAvailable READ fileAvailable NOTIFY fileStatusChanged)
    Q_PROPERTY(QString fileStatusText READ fileStatusText NOTIFY fileStatusChanged)
    Q_PROPERTY(QString clipboardPath READ clipboardPath NOTIFY clipboardChanged)
    Q_PROPERTY(QString clipboardName READ clipboardName NOTIFY clipboardChanged)
    Q_PROPERTY(bool clipboardCut READ clipboardCut NOTIFY clipboardChanged)
    Q_PROPERTY(QVariantList resourceDevices READ resourceDevices NOTIFY resourceSnapshotChanged)
    Q_PROPERTY(QVariantList resourceProcesses READ resourceProcesses NOTIFY resourceSnapshotChanged)
    Q_PROPERTY(QVariantList resourceProcessGroups READ resourceProcessGroups NOTIFY resourceSnapshotChanged)
    Q_PROPERTY(QVariantList resourceJobs READ resourceJobs NOTIFY resourceSnapshotChanged)
    Q_PROPERTY(QVariantList resourceDisks READ resourceDisks NOTIFY resourceSnapshotChanged)
    Q_PROPERTY(QString resourceSnapshotText READ resourceSnapshotText NOTIFY resourceSnapshotChanged)
    Q_PROPERTY(bool resourceSnapshotBusy READ resourceSnapshotBusy NOTIFY resourceSnapshotChanged)
    Q_PROPERTY(QVariantList packageTools READ packageTools NOTIFY packageStateChanged)
    Q_PROPERTY(QVariantList installedPackages READ installedPackages NOTIFY packageStateChanged)
    Q_PROPERTY(QVariantList packageSearchResults READ packageSearchResults NOTIFY packageStateChanged)
    Q_PROPERTY(QString packageStatusText READ packageStatusText NOTIFY packageStateChanged)
    Q_PROPERTY(QString packageLogText READ packageLogText NOTIFY packageStateChanged)
    Q_PROPERTY(bool packageBusy READ packageBusy NOTIFY packageStateChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    // Initialise the Rust core + seed demo data. Returns false on failure.
    bool initialize(QString *errorOut = nullptr);

    // Called by RustCoreBridge on the UI thread with a batch of session events.
    void ingestRustEvents(const RustEventBatch &batch);
    // Called by RustCoreBridge on the UI thread with a batch of file results.
    void ingestFileResults(const FsResultBatch &batch);
    // Called by RustCoreBridge on the UI thread with side-channel command results.
    void ingestExecResults(const ExecResultBatch &batch);

    ServerListModel *serverModel() { return m_servers; }
    TerminalViewModel *terminal() { return m_terminal; }
    RemoteFileTreeModel *fileTree() { return m_fileTree; }
    EditorViewModel *editor() { return m_editor; }
    QString appVersion() const;
    QString coreVersion() const { return RustCoreBridge::version(); }
    QString credentialBackend() const {
        return m_credentials ? m_credentials->backendName() : QStringLiteral("none");
    }
    int selectedIndex() const { return m_selectedIndex; }
    int connectionState() const { return m_connectionState; }
    QString connectionStateText() const { return ServerListModel::statusText(m_connectionState); }
    QString currentEndpoint() const { return m_currentEndpoint; }
    bool isConnected() const { return m_connectionState == RsSessionState_Connected; }
    bool isBusy() const {
        return m_connectionState == RsSessionState_Connecting ||
               m_connectionState == RsSessionState_Disconnecting;
    }
    bool fileBusy() const { return !m_fsPending.isEmpty(); }
    int pendingFileOperations() const { return static_cast<int>(m_fsPending.size()); }
    bool fileAvailable() const { return m_fileAvailable; }
    QString fileStatusText() const { return m_fileStatusText; }
    QString clipboardPath() const { return m_clipboardPath; }
    QString clipboardName() const;
    bool clipboardCut() const { return m_clipboardCut; }
    QVariantList resourceDevices() const { return m_resourceDevices; }
    QVariantList resourceProcesses() const { return m_resourceProcesses; }
    QVariantList resourceProcessGroups() const { return m_resourceProcessGroups; }
    QVariantList resourceJobs() const { return m_resourceJobs; }
    QVariantList resourceDisks() const { return m_resourceDisks; }
    QString resourceSnapshotText() const { return m_resourceSnapshotText; }
    bool resourceSnapshotBusy() const { return m_resourceSnapshotBusy; }
    QVariantList packageTools() const { return m_packageTools; }
    QVariantList installedPackages() const { return m_installedPackages; }
    QVariantList packageSearchResults() const { return m_packageSearchResults; }
    QString packageStatusText() const { return m_packageStatusText; }
    QString packageLogText() const { return m_packageLogText; }
    bool packageBusy() const { return m_packageBusy; }
    QString updateStatusText() const { return m_updateStatusText; }
    bool updateBusy() const { return m_updateBusy; }
    bool updateAvailable() const { return m_updateAvailable; }
    QString updateDownloadUrl() const { return m_updateDownloadUrl; }

public slots:
    void checkForUpdates();
    void openUpdateDownload();
    void selectServer(int index);
    void connectToServer(int index);
    // Add a real (russh) server from the connection dialog and connect to it. The
    // password is stored in the CredentialStore and handed to the core; it never
    // lives in QML state beyond the input field.
    void connectToHost(const QString &host, int port, const QString &username,
                       const QString &password, const QString &name,
                       const QString &keyPath, const QString &keyPassphrase);
    void editServer(int index, const QString &host, int port, const QString &username,
                    const QString &password, const QString &name, const QString &keyPath,
                    const QString &keyPassphrase);
    void deleteServer(int index);
    void disconnectCurrent();
    void cancel();
    // Deliver the user's host-key confirmation decision.
    void confirmHostKey(bool accept);
    void sendCommand(const QString &text);
    void sendInterrupt();
    void runQuickCommand(const QString &command);
    void clearTerminal();
    void runPythonFile(const QString &path, const QString &device);
    void refreshResourceSnapshot();
    void refreshEnvironment();
    void searchPackages(const QString &query);
    void installPackage(const QString &manager, const QString &name);
    void updatePackage(const QString &manager, const QString &name);
    void removePackage(const QString &manager, const QString &name);
    void activateEditorPath(const QString &path);
    void closeEditor();

    // File-management actions (used by the file tree / editor UI).
    void openPath(const QString &path);
    void reloadFiles();
    void copyPath(const QString &path);
    void cutPath(const QString &path);
    void paste(const QString &destDir);
    void renamePath(const QString &path, const QString &newName);
    void deletePath(const QString &path, bool isDir);
    void makeDir(const QString &parentDir, const QString &name);
    void makeFile(const QString &parentDir, const QString &name);
    void uploadFile(const QString &destDir, const QUrl &localFile);
    void saveEditor(const QString &text);
    void saveEditorPath(const QString &path, const QString &text);

signals:
    void selectedIndexChanged();
    void connectionStateChanged();
    void currentEndpointChanged();
    void fileActivityChanged();
    void fileStatusChanged();
    void clipboardChanged();
    void resourceSnapshotChanged();
    void packageStateChanged();
    void updateStateChanged();
    // Emitted when the server presents an unknown host key needing confirmation.
    void hostKeyPromptRequested(const QString &fingerprint);

private:
    void setConnectionState(RsSessionState state);
    void teardownSession();
    void loadServers();
    void saveServers() const;
    QString endpointFor(int index) const;
    void expandDir(const QString &path); // RemoteFileTreeModel::directoryExpandRequested
    static QString parentDirOf(const QString &path);
    QString targetDirForWrite(const QString &dir) const;
    void rememberHomeFromRootListing(const QVector<FsEntry> &entries);
    void setFileStatus(bool available, const QString &text);
    void seedResourceSnapshot(const QString &statusText);
    void parseResourceSnapshot(const QString &text);
    void parseEnvironmentProbe(const QString &text);
    void parseInstalledPackages(const QString &text);
    void parsePackageSearch(const QString &text);
    void finishUpdateCheck(const QByteArray &payload, const QString &networkError);
    void finishPackageMutation(const ExecResult &result, const QString &successText,
                               const QString &failureText, const QString &refreshText);
    bool sendTerminalBytes(const QByteArray &payload, const QString &failureContext);
    bool sendShellLine(const QString &line, const QString &failureContext);
    bool captureResourceOutput(const QByteArray &data);
    void rebuildResourceProcessGroups();
    quint64 submitExec(const QString &command, quint64 timeoutMs, int kind, const QString &label);

    // Tracks outstanding file requests so results can be dispatched by id.
    struct PendingFs {
        enum Kind { Listing, EditorOpen, EditorSave, Mutate } kind = Listing;
        QString path;       // for Listing / EditorOpen / EditorSave
        QString refreshDir;      // for Mutate: directory to re-list on success
        QString extraRefreshDir; // optional second directory for moves
        bool clearClipboard = false;
        enum Mutation { NoMutation, CreateLike, CopyLike, MoveLike, DeleteLike } mutation =
            NoMutation;
        QString destinationPath;
        bool recursive = false;
    };
    void trackFsRequest(quint64 requestId, const PendingFs &pending);
    void clearPendingFs();

    QHash<quint64, PendingFs> m_fsPending;
    QString m_clipboardPath; // copy/cut source
    bool m_clipboardCut = false;
    QString m_remoteHomePath;
    QString m_fileStatusText = QStringLiteral("连接后显示远端文件");
    bool m_fileAvailable = false;
    QVariantList m_resourceDevices;
    QVariantList m_resourceProcesses;
    QVariantList m_resourceProcessGroups;
    QVariantList m_resourceJobs;
    QVariantList m_resourceDisks;
    QString m_resourceSnapshotText = QStringLiteral("尚未采集资源快照");
    bool m_resourceSnapshotBusy = false;
    QString m_resourceCapture;
    QString m_resourceMarkerTail;
    QVariantList m_packageTools;
    QVariantList m_installedPackages;
    QVariantList m_packageSearchResults;
    QString m_packageStatusText = QStringLiteral("连接后可扫描环境");
    QString m_packageLogText;
    bool m_packageBusy = false;
    QString m_updateStatusText = QStringLiteral("尚未检查热更新");
    QString m_updateDownloadUrl;
    bool m_updateBusy = false;
    bool m_updateAvailable = false;
    struct PendingExec {
        enum Kind {
            ResourceSnapshot,
            EnvironmentProbe,
            PackageList,
            PackageSearch,
            PackageInstall,
            PackageUpdate,
            PackageRemove
        }
            kind = EnvironmentProbe;
        QString label;
    };
    QHash<quint64, PendingExec> m_execPending;

    RustCoreBridge m_bridge;
    ServerListModel *m_servers = nullptr;
    TerminalViewModel *m_terminal = nullptr;
    RemoteFileTreeModel *m_fileTree = nullptr;
    EditorViewModel *m_editor = nullptr;
    std::unique_ptr<CredentialStore> m_credentials;
    QNetworkAccessManager *m_updateNetwork = nullptr;

    RsSession *m_session = nullptr;
    int m_currentIndex = -1; // server bound to the active session
    int m_selectedIndex = -1;
    int m_connectionState = RsSessionState_Idle;
    QString m_currentEndpoint;
};

} // namespace researchssh
