#include "CredentialStore.h"

namespace researchssh {

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
    // TODO(platform): return a WindowsCredentialStore on Windows and an
    // AndroidKeystoreStore on Android once those backends are implemented.
    return std::make_unique<MockCredentialStore>();
}

} // namespace researchssh
