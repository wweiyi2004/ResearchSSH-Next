#include "TerminalViewModel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace researchssh {

TerminalViewModel::TerminalViewModel(QObject *parent) : QObject(parent) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath() + QStringLiteral("/ResearchSSH-Next");
    QDir().mkpath(dir);
    m_logPath = dir + QStringLiteral("/terminal.log");

    QFile file(m_logPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QTextStream out(&file);
        out << "ResearchSSH-Next terminal log "
            << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
    }
}

void TerminalViewModel::appendBytes(const QByteArray &bytes) {
    // Stateful decode: a multi-byte sequence straddling two chunks is held over and
    // completed on the next call rather than emitting a replacement character.
    QString chunk = m_utf8.decode(bytes);
    // Framework stage: no terminal emulator yet. Normalise CRLF and drop other
    // C0 control characters (except tab/newline) so the placeholder view is clean.
    chunk.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    chunk.replace(QChar('\r'), QChar('\n'));
    QString filtered;
    filtered.reserve(chunk.size());
    for (const QChar c : chunk) {
        const ushort u = c.unicode();
        if (u == '\n' || u == '\t' || u >= 0x20)
            filtered.append(c);
    }
    appendText(filtered);
}

void TerminalViewModel::appendNotice(const QString &line) {
    appendText(QStringLiteral("» %1\n").arg(line));
}

void TerminalViewModel::appendText(const QString &chunk) {
    if (chunk.isEmpty())
        return;
    appendLog(chunk);
    m_text.append(chunk);
    if (m_text.size() > kMaxChars)
        m_text.remove(0, m_text.size() - kMaxChars);
    emit textChanged();
}

void TerminalViewModel::clear() {
    if (m_text.isEmpty())
        return;
    m_text.clear();
    emit textChanged();
}

void TerminalViewModel::appendLog(const QString &chunk) {
    if (m_logPath.isEmpty())
        return;
    QFile file(m_logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream out(&file);
    out << chunk;
}

} // namespace researchssh
