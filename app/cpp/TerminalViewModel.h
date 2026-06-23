// TerminalViewModel — backing state for the center terminal pane.
//
// Holds the accumulated terminal text and exposes it to QML. At the framework
// stage this is a plain text sink (no VT/ANSI interpretation yet — that lives in
// the Rust core's future `vt` module). Control bytes are stripped defensively so
// the placeholder view stays readable.

#pragma once

#include <QObject>
#include <QString>

namespace researchssh {

class TerminalViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    Q_PROPERTY(int byteCount READ byteCount NOTIFY textChanged)

public:
    explicit TerminalViewModel(QObject *parent = nullptr);

    QString text() const { return m_text; }
    int byteCount() const { return static_cast<int>(m_text.size()); }

    // Append raw terminal bytes (UTF-8). Sanitises control sequences for now.
    void appendBytes(const QByteArray &bytes);
    // Append a framework/diagnostic line (rendered distinctly by convention: "» ").
    void appendNotice(const QString &line);

public slots:
    void clear();

signals:
    void textChanged();

private:
    void appendText(const QString &chunk);

    QString m_text;
    // Cap the buffer so a long-running session can't grow it without bound.
    static constexpr int kMaxChars = 200000;
};

} // namespace researchssh
