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
#include <QObject>
#include <QUrl>
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
    Q_PROPERTY(QString coreVersion READ coreVersion CONSTANT)
    Q_PROPERTY(QString credentialBackend READ credentialBackend CONSTANT)
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

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    // Initialise the Rust core + seed demo data. Returns false on failure.
    bool initialize(QString *errorOut = nullptr);

    // Called by RustCoreBridge on the UI thread with a batch of session events.
    void ingestRustEvents(const RustEventBatch &batch);
    // Called by RustCoreBridge on the UI thread with a batch of file results.
    void ingestFileResults(const FsResultBatch &batch);

    ServerListModel *serverModel() { return m_servers; }
    TerminalViewModel *terminal() { return m_terminal; }
    RemoteFileTreeModel *fileTree() { return m_fileTree; }
    EditorViewModel *editor() { return m_editor; }
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

public slots:
    void selectServer(int index);
    void connectToServer(int index);
    // Add a real (russh) server from the connection dialog and connect to it. The
    // password is stored in the CredentialStore and handed to the core; it never
    // lives in QML state beyond the input field.
    void connectToHost(const QString &host, int port, const QString &username,
                       const QString &password, const QString &name,
                       const QString &keyPath, const QString &keyPassphrase);
    void disconnectCurrent();
    void cancel();
    // Deliver the user's host-key confirmation decision.
    void confirmHostKey(bool accept);
    void sendCommand(const QString &text);
    void runQuickCommand(const QString &command);
    void clearTerminal();

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

signals:
    void selectedIndexChanged();
    void connectionStateChanged();
    void currentEndpointChanged();
    void fileActivityChanged();
    void fileStatusChanged();
    void clipboardChanged();
    // Emitted when the server presents an unknown host key needing confirmation.
    void hostKeyPromptRequested(const QString &fingerprint);

private:
    void setConnectionState(RsSessionState state);
    void teardownSession();
    QString endpointFor(int index) const;
    void expandDir(const QString &path); // RemoteFileTreeModel::directoryExpandRequested
    static QString parentDirOf(const QString &path);
    QString targetDirForWrite(const QString &dir) const;
    void rememberHomeFromRootListing(const QVector<FsEntry> &entries);
    void setFileStatus(bool available, const QString &text);

    // Tracks outstanding file requests so results can be dispatched by id.
    struct PendingFs {
        enum Kind { Listing, EditorOpen, EditorSave, Mutate } kind = Listing;
        QString path;       // for Listing / EditorOpen / EditorSave
        QString refreshDir;      // for Mutate: directory to re-list on success
        QString extraRefreshDir; // optional second directory for moves
        bool clearClipboard = false;
    };
    void trackFsRequest(quint64 requestId, const PendingFs &pending);
    void clearPendingFs();

    QHash<quint64, PendingFs> m_fsPending;
    QString m_clipboardPath; // copy/cut source
    bool m_clipboardCut = false;
    QString m_remoteHomePath;
    QString m_fileStatusText = QStringLiteral("连接后显示远端文件");
    bool m_fileAvailable = false;

    RustCoreBridge m_bridge;
    ServerListModel *m_servers = nullptr;
    TerminalViewModel *m_terminal = nullptr;
    RemoteFileTreeModel *m_fileTree = nullptr;
    EditorViewModel *m_editor = nullptr;
    std::unique_ptr<CredentialStore> m_credentials;

    RsSession *m_session = nullptr;
    int m_currentIndex = -1; // server bound to the active session
    int m_selectedIndex = -1;
    int m_connectionState = RsSessionState_Idle;
    QString m_currentEndpoint;
};

} // namespace researchssh
