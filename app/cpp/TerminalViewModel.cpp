#include "TerminalViewModel.h"

namespace researchssh {

TerminalViewModel::TerminalViewModel(QObject *parent) : QObject(parent) {}

void TerminalViewModel::appendBytes(const QByteArray &bytes) {
    QString chunk = QString::fromUtf8(bytes);
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

} // namespace researchssh
