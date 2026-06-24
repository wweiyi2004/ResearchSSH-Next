#include "CredentialStore.h"

#include <QtGlobal>

#include <string>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace researchssh {

namespace {

#ifdef Q_OS_WIN

std::wstring credentialTargetFor(const QString &key) {
    return (QStringLiteral("ResearchSSH-Next:") + key).toStdWString();
}

class WindowsCredentialStore final : public CredentialStore {
public:
    QString backendName() const override { return QStringLiteral("windows-credential-manager"); }

    bool store(const QString &key, const QByteArray &secret) override {
        if (key.isEmpty() || secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
            return false;

        const std::wstring target = credentialTargetFor(key);
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPWSTR>(target.c_str());
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
        credential.CredentialBlob = secret.isEmpty()
                                        ? nullptr
                                        : reinterpret_cast<LPBYTE>(
                                              const_cast<char *>(secret.constData()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

        return CredWriteW(&credential, 0) == TRUE;
    }

    QByteArray load(const QString &key) const override {
        if (key.isEmpty())
            return {};

        const std::wstring target = credentialTargetFor(key);
        PCREDENTIALW credential = nullptr;
        if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) != TRUE)
            return {};

        QByteArray secret;
        if (credential->CredentialBlobSize > 0 && credential->CredentialBlob) {
            secret = QByteArray(reinterpret_cast<const char *>(credential->CredentialBlob),
                                static_cast<int>(credential->CredentialBlobSize));
        }
        CredFree(credential);
        return secret;
    }

    bool remove(const QString &key) override {
        if (key.isEmpty())
            return false;

        const std::wstring target = credentialTargetFor(key);
        return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == TRUE;
    }

    bool contains(const QString &key) const override {
        if (key.isEmpty())
            return false;

        const std::wstring target = credentialTargetFor(key);
        PCREDENTIALW credential = nullptr;
        if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) != TRUE)
            return false;
        CredFree(credential);
        return true;
    }
};

#endif

} // namespace

bool MockCredentialStore::store(const QString &key, const QByteArray &secret) {
    // Wipe any previous secret for this key before dropping it, so an overwritten
    // password isn't left lingering on the heap.
    auto it = m_secrets.find(key);
    if (it != m_secrets.end())
        it.value().fill('\0');
    // Deep-copy into a buffer we exclusively own (detached from the caller's copy),
    // so the later fill('\0') in remove()/the destructor wipes these very bytes in
    // place rather than detaching from a shared copy and leaving the original.
    m_secrets.insert(key, QByteArray(secret.constData(), secret.size()));
    return true;
}

QByteArray MockCredentialStore::load(const QString &key) const {
    return m_secrets.value(key);
}

bool MockCredentialStore::remove(const QString &key) {
    auto it = m_secrets.find(key);
    if (it == m_secrets.end())
        return false;
    // Best-effort wipe of the in-memory copy before erasing.
    it.value().fill('\0');
    m_secrets.erase(it);
    return true;
}

bool MockCredentialStore::contains(const QString &key) const {
    return m_secrets.contains(key);
}

MockCredentialStore::~MockCredentialStore() {
    // Wipe all in-memory secrets on shutdown.
    for (auto it = m_secrets.begin(); it != m_secrets.end(); ++it)
        it.value().fill('\0');
    m_secrets.clear();
}

std::unique_ptr<CredentialStore> createDefaultCredentialStore() {
#ifdef Q_OS_WIN
    return std::make_unique<WindowsCredentialStore>();
#else
    // TODO(platform): return an AndroidKeystoreStore on Android once implemented.
    return std::make_unique<MockCredentialStore>();
#endif
}

} // namespace researchssh
