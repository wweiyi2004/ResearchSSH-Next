// TerminalViewModel — backing state for the center terminal pane.
//
// Holds the accumulated terminal text and exposes it to QML. At the framework
// stage this is a plain text sink (no VT/ANSI interpretation yet — that lives in
// the Rust core's future `vt` module). Control bytes are stripped defensively so
// the placeholder view stays readable.

#pragma once

#include <QObject>
#include <QString>
#include <QStringDecoder>

namespace researchssh {

class TerminalViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    Q_PROPERTY(int byteCount READ byteCount NOTIFY textChanged)
    Q_PROPERTY(QString logPath READ logPath CONSTANT)

public:
    explicit TerminalViewModel(QObject *parent = nullptr);

    QString text() const { return m_text; }
    int byteCount() const { return static_cast<int>(m_text.size()); }
    QString logPath() const { return m_logPath; }

    // Append raw terminal bytes (UTF-8). Sanitises control sequences for now.
    void appendBytes(const QByteArray &bytes);
    // Append a framework/diagnostic line (rendered distinctly by convention: "» ").
    void appendNotice(const QString &line);

public slots:
    void clear();

signals:
    void textChanged();

private:
    QString sanitiseTerminalChunk(const QString &chunk);
    void appendText(const QString &chunk);
    void appendLog(const QString &chunk);

    QString m_text;
    QString m_logPath;
    // Retains an incomplete multi-byte UTF-8 sequence across chunk boundaries, so a
    // character split between two reads from the core is decoded correctly instead
    // of turning into replacement characters.
    QStringDecoder m_utf8{QStringDecoder::Utf8};
    // Retains a partially received ANSI/VT control sequence until the next chunk.
    QString m_pendingControl;
    // Cap the buffer so a long-running session can't grow it without bound.
    static constexpr int kMaxChars = 200000;
};

} // namespace researchssh
