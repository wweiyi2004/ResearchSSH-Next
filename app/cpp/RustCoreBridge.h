// RustCoreBridge — the thin C++ wrapper around the Rust core's C ABI.
//
// This is the ONLY C++ file that talks to the FFI. It:
//   * owns the RsCore* handle,
//   * translates between Qt types and the C ABI (QString<->UTF-8, QByteArray<->
//     byte buffers),
//   * receives event batches on arbitrary Rust threads and marshals them onto the
//     Qt UI thread with Qt::QueuedConnection.
//
// Ownership rules (mirrored from the FFI header):
//   * Handles are owned by Rust; we store the pointer and call the matching
//     *_destroy exactly once.
//   * RsEvent.data / RsEvent.message are valid only during the callback, so we
//     copy them into QByteArray / QString immediately.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include "research_ssh_core.h"

namespace researchssh {

// A single event after it has been copied out of the (transient) RsEvent buffers.
struct RustEvent {
    RsEventKind kind = RsEventKind_StateChanged;
    RsSessionState state = RsSessionState_Idle;
    RsErrorCode code = RsErrorCode_Ok;
    QByteArray data;   // terminal bytes (kind == Data)
    QString message;   // error text (kind == Error)
};
using RustEventBatch = QVector<RustEvent>;

// One directory entry, copied out of a transient RsFileEntry.
struct FsEntry {
    QString name;
    QString path;
    int kind = 0;       // RsFileKind
    quint64 size = 0;
    qint64 modified = -1;
    bool editable = false;
};

// One file-operation result, copied out of a transient RsFsResult.
struct FsResult {
    quint64 requestId = 0;
    int kind = 0;       // RsFsResultKind (Listing/Content/Ok/Error)
    QVector<FsEntry> entries;
    QByteArray data;
    int errorCode = 0;
    QString message;
};
using FsResultBatch = QVector<FsResult>;

class RustCoreBridge {
public:
    RustCoreBridge();
    ~RustCoreBridge();

    RustCoreBridge(const RustCoreBridge &) = delete;
    RustCoreBridge &operator=(const RustCoreBridge &) = delete;

    // Create the core + async runtime. Returns false on failure.
    bool initialize(QString *errorOut = nullptr);
    bool isReady() const { return m_core != nullptr; }

    // Core version string (from the Rust core).
    static QString version();

    // Human-readable description of an error code.
    static QString describe(RsErrorCode code);

    // Create a session. Events are delivered to `receiver->ingestRustEvents(...)`
    // on the Qt UI thread. `receiver` must outlive the session. Returns nullptr on
    // failure (and fills errorOut).
    RsSession *createSession(const QString &host, quint16 port, const QString &username,
                             RsProviderKind provider, QObject *receiver, QString *errorOut);

    // Joins the driver and frees the session. Safe with nullptr.
    void destroySession(RsSession *session);

    RsErrorCode connectSession(RsSession *session);
    RsErrorCode disconnectSession(RsSession *session);
    RsErrorCode sendToSession(RsSession *session, const QByteArray &data);
    RsErrorCode cancelSession(RsSession *session);

    // Hand a secret to the session's provider (copied into a zeroizing buffer in
    // Rust). The mock discards it; the russh provider uses it for password auth.
    RsErrorCode setSessionPassword(RsSession *session, const QByteArray &secret);

    // Deliver the user's decision for a pending host-key prompt.
    RsErrorCode confirmHostKey(RsSession *session, bool accept);

    // Register the file-result callback (file results go to receiver->ingestFileResults).
    RsErrorCode setFileCallback(RsSession *session, QObject *receiver);

    // File operations. Each returns a request id (0 = could not queue); the result
    // arrives later via the file callback.
    quint64 fsList(RsSession *session, const QString &path);
    quint64 fsRead(RsSession *session, const QString &path, quint64 maxLen);
    quint64 fsWrite(RsSession *session, const QString &path, const QByteArray &data);
    quint64 fsRename(RsSession *session, const QString &from, const QString &to);
    quint64 fsRemove(RsSession *session, const QString &path, bool recursive);
    quint64 fsMkdir(RsSession *session, const QString &path);
    quint64 fsCopy(RsSession *session, const QString &from, const QString &to);

private:
    // C ABI callbacks. Invoked on arbitrary Rust threads; copy + queue to UI.
    static void onRustEvents(void *user_data, const RsEvent *events, uintptr_t count);
    static void onFileResults(void *user_data, const RsFsResult *results, uintptr_t count);

    RsCore *m_core = nullptr;
};

} // namespace researchssh
