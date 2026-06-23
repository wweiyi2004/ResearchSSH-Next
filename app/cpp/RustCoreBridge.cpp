#include "RustCoreBridge.h"

#include "AppController.h"

#include <QtGlobal>

namespace researchssh {

RustCoreBridge::RustCoreBridge() = default;

RustCoreBridge::~RustCoreBridge() {
    if (m_core) {
        rscore_destroy(m_core);
        m_core = nullptr;
    }
}

bool RustCoreBridge::initialize(QString *errorOut) {
    if (m_core)
        return true;
    RsErrorCode err = RsErrorCode_Internal;
    m_core = rscore_create(&err);
    if (!m_core) {
        if (errorOut)
            *errorOut = describe(err);
        return false;
    }
    return true;
}

QString RustCoreBridge::version() {
    const char *v = rscore_version();
    return v ? QString::fromUtf8(v) : QStringLiteral("unknown");
}

QString RustCoreBridge::describe(RsErrorCode code) {
    const char *m = rscore_error_message(code);
    return m ? QString::fromUtf8(m) : QStringLiteral("unknown error");
}

RsSession *RustCoreBridge::createSession(const QString &host, quint16 port,
                                         const QString &username, RsProviderKind provider,
                                         QObject *receiver, QString *errorOut) {
    if (!m_core) {
        if (errorOut)
            *errorOut = QStringLiteral("core not initialized");
        return nullptr;
    }

    // UTF-8 buffers must stay alive for the duration of the call (Rust copies them).
    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray userUtf8 = username.toUtf8();

    RsSessionConfig config;
    config.host = hostUtf8.constData();
    config.port = port;
    config.username = userUtf8.constData();
    config.provider = provider;

    RsErrorCode err = RsErrorCode_Internal;
    RsSession *session =
        rscore_session_create(m_core, &config, &RustCoreBridge::onRustEvents,
                              static_cast<void *>(receiver), &err);
    if (!session && errorOut)
        *errorOut = describe(err);
    return session;
}

void RustCoreBridge::destroySession(RsSession *session) {
    if (session)
        rscore_session_destroy(session);
}

RsErrorCode RustCoreBridge::connectSession(RsSession *session) {
    return rscore_session_connect(session);
}

RsErrorCode RustCoreBridge::disconnectSession(RsSession *session) {
    return rscore_session_disconnect(session);
}

RsErrorCode RustCoreBridge::sendToSession(RsSession *session, const QByteArray &data) {
    return rscore_session_send(session, reinterpret_cast<const uint8_t *>(data.constData()),
                               static_cast<uintptr_t>(data.size()));
}

RsErrorCode RustCoreBridge::cancelSession(RsSession *session) {
    return rscore_session_cancel(session);
}

RsErrorCode RustCoreBridge::setSessionPassword(RsSession *session, const QByteArray &secret) {
    return rscore_session_set_password(session,
                                       reinterpret_cast<const uint8_t *>(secret.constData()),
                                       static_cast<uintptr_t>(secret.size()));
}

RsErrorCode RustCoreBridge::setSessionPrivateKey(RsSession *session, const QString &keyPath,
                                                 const QByteArray &passphrase) {
    const QByteArray pathUtf8 = keyPath.toUtf8();
    return rscore_session_set_private_key(
        session, pathUtf8.constData(),
        reinterpret_cast<const uint8_t *>(passphrase.constData()),
        static_cast<uintptr_t>(passphrase.size()));
}

RsErrorCode RustCoreBridge::confirmHostKey(RsSession *session, bool accept) {
    return rscore_session_confirm_host_key(session, accept);
}

RsErrorCode RustCoreBridge::setFileCallback(RsSession *session, QObject *receiver) {
    return rscore_session_set_file_callback(session, &RustCoreBridge::onFileResults,
                                            static_cast<void *>(receiver));
}

quint64 RustCoreBridge::fsList(RsSession *session, const QString &path) {
    return rscore_session_fs_list(session, path.toUtf8().constData());
}

quint64 RustCoreBridge::fsRead(RsSession *session, const QString &path, quint64 maxLen) {
    return rscore_session_fs_read(session, path.toUtf8().constData(), maxLen);
}

quint64 RustCoreBridge::fsWrite(RsSession *session, const QString &path, const QByteArray &data) {
    return rscore_session_fs_write(session, path.toUtf8().constData(),
                                   reinterpret_cast<const uint8_t *>(data.constData()),
                                   static_cast<uintptr_t>(data.size()));
}

quint64 RustCoreBridge::fsRename(RsSession *session, const QString &from, const QString &to) {
    return rscore_session_fs_rename(session, from.toUtf8().constData(), to.toUtf8().constData());
}

quint64 RustCoreBridge::fsRemove(RsSession *session, const QString &path, bool recursive) {
    return rscore_session_fs_remove(session, path.toUtf8().constData(), recursive);
}

quint64 RustCoreBridge::fsMkdir(RsSession *session, const QString &path) {
    return rscore_session_fs_mkdir(session, path.toUtf8().constData());
}

quint64 RustCoreBridge::fsCopy(RsSession *session, const QString &from, const QString &to) {
    return rscore_session_fs_copy(session, from.toUtf8().constData(), to.toUtf8().constData());
}

void RustCoreBridge::onFileResults(void *user_data, const RsFsResult *results, uintptr_t count) {
    auto *receiver = static_cast<AppController *>(user_data);
    if (!receiver || !results)
        return;

    FsResultBatch batch;
    batch.reserve(static_cast<qsizetype>(count));
    for (uintptr_t i = 0; i < count; ++i) {
        const RsFsResult &r = results[i];
        FsResult out;
        out.requestId = r.request_id;
        out.kind = static_cast<int>(r.kind);
        out.errorCode = static_cast<int>(r.error_code);
        if (r.kind == RsFsResultKind_Listing && r.entries) {
            out.entries.reserve(static_cast<qsizetype>(r.entry_count));
            for (uintptr_t j = 0; j < r.entry_count; ++j) {
                const RsFileEntry &e = r.entries[j];
                FsEntry fe;
                fe.name = e.name ? QString::fromUtf8(e.name) : QString();
                fe.path = e.path ? QString::fromUtf8(e.path) : QString();
                fe.kind = static_cast<int>(e.kind);
                fe.size = e.size;
                fe.modified = e.modified_unix;
                fe.editable = e.editable_text;
                out.entries.push_back(std::move(fe));
            }
        } else if (r.kind == RsFsResultKind_Content && r.data && r.data_len > 0) {
            out.data = QByteArray(reinterpret_cast<const char *>(r.data),
                                  static_cast<qsizetype>(r.data_len));
        } else if (r.kind == RsFsResultKind_Error && r.message) {
            out.message = QString::fromUtf8(r.message);
        }
        batch.push_back(std::move(out));
    }

    QMetaObject::invokeMethod(
        receiver, [receiver, batch]() { receiver->ingestFileResults(batch); },
        Qt::QueuedConnection);
}

void RustCoreBridge::onRustEvents(void *user_data, const RsEvent *events, uintptr_t count) {
    auto *receiver = static_cast<AppController *>(user_data);
    if (!receiver || !events)
        return;

    // Copy out of the transient Rust buffers (valid only during this call).
    RustEventBatch batch;
    batch.reserve(static_cast<qsizetype>(count));
    for (uintptr_t i = 0; i < count; ++i) {
        const RsEvent &e = events[i];
        RustEvent ev;
        ev.kind = e.kind;
        ev.state = e.state;
        ev.code = e.error_code;
        if (e.kind == RsEventKind_Data && e.data && e.data_len > 0) {
            ev.data = QByteArray(reinterpret_cast<const char *>(e.data),
                                 static_cast<qsizetype>(e.data_len));
        } else if ((e.kind == RsEventKind_Error || e.kind == RsEventKind_HostKeyPrompt) &&
                   e.message) {
            ev.message = QString::fromUtf8(e.message);
        }
        batch.push_back(std::move(ev));
    }

    // Marshal onto the Qt UI thread: the callback runs on a Rust runtime thread,
    // but all UI/model mutation must happen on the thread `receiver` lives in.
    QMetaObject::invokeMethod(
        receiver, [receiver, batch]() { receiver->ingestRustEvents(batch); },
        Qt::QueuedConnection);
}

} // namespace researchssh
