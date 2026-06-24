#include "TerminalViewModel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace researchssh {

namespace {

constexpr qsizetype kMaxPendingControlChars = 4096;

bool isCsiFinal(const QChar c) {
    const ushort u = c.unicode();
    return u >= 0x40 && u <= 0x7e;
}

bool isEscIntermediate(const QChar c) {
    const ushort u = c.unicode();
    return u >= 0x20 && u <= 0x2f;
}

bool isEscFinal(const QChar c) {
    const ushort u = c.unicode();
    return u >= 0x30 && u <= 0x7e;
}

} // namespace

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
    const QString chunk = sanitiseTerminalChunk(m_utf8.decode(bytes));

    QString filtered;
    filtered.reserve(chunk.size());
    for (qsizetype i = 0; i < chunk.size(); ++i) {
        const QChar c = chunk.at(i);
        const ushort u = c.unicode();
        if (u == '\r') {
            if (i + 1 < chunk.size() && chunk.at(i + 1) == QChar('\n'))
                ++i;
            filtered.append(QChar('\n'));
        } else if (u == '\b') {
            if (!filtered.isEmpty() && filtered.back() != QChar('\n'))
                filtered.chop(1);
        } else if (u == '\n' || u == '\t' || u >= 0x20) {
            filtered.append(c);
        }
    }
    appendText(filtered);
}

QString TerminalViewModel::sanitiseTerminalChunk(const QString &chunk) {
    const QString input = m_pendingControl + chunk;
    m_pendingControl.clear();

    auto holdPending = [&](const QString &text, qsizetype start) {
        m_pendingControl = text.mid(start);
        if (m_pendingControl.size() > kMaxPendingControlChars)
            m_pendingControl.clear();
    };

    QString out;
    out.reserve(input.size());
    for (qsizetype i = 0; i < input.size();) {
        const QChar c = input.at(i);
        const ushort u = c.unicode();

        if (u == 0x1b) {
            if (i + 1 >= input.size()) {
                holdPending(input, i);
                break;
            }

            const QChar next = input.at(i + 1);
            const ushort n = next.unicode();

            if (n == '[') {
                qsizetype j = i + 2;
                bool complete = false;
                while (j < input.size()) {
                    if (isCsiFinal(input.at(j))) {
                        ++j;
                        complete = true;
                        break;
                    }
                    ++j;
                }
                if (!complete) {
                    holdPending(input, i);
                    break;
                }
                i = j;
                continue;
            }

            if (n == ']') {
                qsizetype j = i + 2;
                bool complete = false;
                while (j < input.size()) {
                    const ushort cur = input.at(j).unicode();
                    if (cur == 0x07) {
                        ++j;
                        complete = true;
                        break;
                    }
                    if (cur == 0x1b) {
                        if (j + 1 >= input.size())
                            break;
                        if (input.at(j + 1) == QChar('\\')) {
                            j += 2;
                            complete = true;
                            break;
                        }
                    }
                    ++j;
                }
                if (!complete) {
                    holdPending(input, i);
                    break;
                }
                i = j;
                continue;
            }

            if (n == 'P' || n == 'X' || n == '^' || n == '_') {
                qsizetype j = i + 2;
                bool complete = false;
                while (j < input.size()) {
                    if (input.at(j).unicode() == 0x1b) {
                        if (j + 1 >= input.size())
                            break;
                        if (input.at(j + 1) == QChar('\\')) {
                            j += 2;
                            complete = true;
                            break;
                        }
                    }
                    ++j;
                }
                if (!complete) {
                    holdPending(input, i);
                    break;
                }
                i = j;
                continue;
            }

            if (isEscIntermediate(next)) {
                qsizetype j = i + 2;
                bool complete = false;
                while (j < input.size()) {
                    if (isEscFinal(input.at(j))) {
                        ++j;
                        complete = true;
                        break;
                    }
                    ++j;
                }
                if (!complete) {
                    holdPending(input, i);
                    break;
                }
                i = j;
                continue;
            }

            i += 2;
            continue;
        }

        if (u == 0x9b) {
            qsizetype j = i + 1;
            bool complete = false;
            while (j < input.size()) {
                if (isCsiFinal(input.at(j))) {
                    ++j;
                    complete = true;
                    break;
                }
                ++j;
            }
            if (!complete) {
                holdPending(input, i);
                break;
            }
            i = j;
            continue;
        }

        out.append(c);
        ++i;
    }
    return out;
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
