// CredentialStore — platform-abstracted secret storage.
//
// Secrets (passwords, key passphrases) never travel through QML. They live in the
// C++ layer and are handed to the Rust core only via the dedicated FFI entry
// point (rscore_session_set_password), which copies them into a zeroizing buffer.
//
// This interface abstracts the OS-specific backends:
//   * Windows  -> Windows Credential Manager (wincred.h: CredWriteW/CredReadW)
//   * Android  -> Android Keystore (via JNI)
//   * Fallback -> in-memory MockCredentialStore (framework stage / tests)
//
// createDefaultCredentialStore returns the best available backend for the
// current platform and falls back to Mock where a native backend is not present.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <memory>

namespace researchssh {

// Abstract credential store. Keys are opaque identifiers chosen by the caller
// (e.g. "<user>@<host>:<port>").
class CredentialStore {
public:
    virtual ~CredentialStore() = default;

    // Backend name for diagnostics ("mock", "windows-credential-manager", ...).
    virtual QString backendName() const = 0;

    // Store / overwrite a secret. Returns true on success.
    virtual bool store(const QString &key, const QByteArray &secret) = 0;

    // Load a secret. Returns an empty QByteArray if not found.
    virtual QByteArray load(const QString &key) const = 0;

    // Remove a secret. Returns true if something was removed.
    virtual bool remove(const QString &key) = 0;

    // Whether a secret exists for the key.
    virtual bool contains(const QString &key) const = 0;
};

// In-memory mock used at the framework stage. Secrets live only for the process
// lifetime and are cleared on destruction. NOT secure storage — a placeholder.
class MockCredentialStore final : public CredentialStore {
public:
    QString backendName() const override { return QStringLiteral("mock"); }
    bool store(const QString &key, const QByteArray &secret) override;
    QByteArray load(const QString &key) const override;
    bool remove(const QString &key) override;
    bool contains(const QString &key) const override;

    ~MockCredentialStore() override;

private:
    QHash<QString, QByteArray> m_secrets;
};

// Factory: returns the best available backend for the current platform.
std::unique_ptr<CredentialStore> createDefaultCredentialStore();

} // namespace researchssh
