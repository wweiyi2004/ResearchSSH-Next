#include "CodeHighlighter.h"

#include <QQuickTextDocument>
#include <QFont>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextDocument>

namespace researchssh {

namespace {

QTextCharFormat fmt(const QColor &color, bool bold = false, bool italic = false) {
    QTextCharFormat f;
    f.setForeground(color);
    f.setFontWeight(bold ? QFont::DemiBold : QFont::Normal);
    f.setFontItalic(italic);
    return f;
}

struct Rule {
    QRegularExpression pattern;
    QTextCharFormat format;
};

} // namespace

class SyntaxHighlighter final : public QSyntaxHighlighter {
public:
    SyntaxHighlighter(QTextDocument *document, QString language, bool darkTheme)
        : QSyntaxHighlighter(document), m_language(std::move(language)), m_darkTheme(darkTheme) {
        buildRules();
    }

protected:
    void highlightBlock(const QString &text) override {
        for (const Rule &rule : m_rules) {
            auto it = rule.pattern.globalMatch(text);
            while (it.hasNext()) {
                const auto match = it.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }

        if (m_language == QStringLiteral("python") || m_language == QStringLiteral("shell")) {
            const int hash = text.indexOf('#');
            if (hash >= 0)
                setFormat(hash, text.size() - hash, m_comment);
        }
    }

private:
    void addWords(const QStringList &words, const QTextCharFormat &format) {
        if (words.isEmpty())
            return;
        m_rules.push_back({QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(words.join('|'))),
                           format});
    }

    void buildRules() {
        m_keyword = fmt(m_darkTheme ? QColor(86, 156, 214) : QColor(0, 92, 197), true);
        m_builtin = fmt(m_darkTheme ? QColor(78, 201, 176) : QColor(38, 127, 153));
        m_string = fmt(m_darkTheme ? QColor(206, 145, 120) : QColor(163, 21, 21));
        m_number = fmt(m_darkTheme ? QColor(181, 206, 168) : QColor(9, 134, 88));
        m_comment = fmt(m_darkTheme ? QColor(106, 153, 85) : QColor(0, 128, 0), false, true);
        const QTextCharFormat typeFmt =
            fmt(m_darkTheme ? QColor(78, 201, 176) : QColor(38, 127, 153), true);
        const QTextCharFormat preprocFmt =
            fmt(m_darkTheme ? QColor(197, 134, 192) : QColor(175, 0, 219), true);

        if (m_language == QStringLiteral("python")) {
            addWords({"False", "None", "True", "and", "as", "assert", "async", "await",
                      "break", "class", "continue", "def", "del", "elif", "else", "except",
                      "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
                      "nonlocal", "not", "or", "pass", "raise", "return", "try", "while",
                      "with", "yield"},
                     m_keyword);
            addWords({"abs", "bool", "dict", "enumerate", "float", "int", "len", "list", "map",
                      "max", "min", "open", "print", "range", "set", "str", "sum", "tuple",
                      "zip", "torch", "numpy", "pandas"},
                     m_builtin);
            m_rules.push_back({QRegularExpression(QStringLiteral("@[A-Za-z_][A-Za-z0-9_]*")),
                               preprocFmt});
        } else if (m_language == QStringLiteral("cpp")) {
            addWords({"alignas", "auto", "bool", "break", "case", "catch", "char", "class",
                      "const", "constexpr", "continue", "double", "else", "enum", "explicit",
                      "false", "float", "for", "if", "int", "long", "namespace", "nullptr",
                      "private", "protected", "public", "return", "short", "static", "struct",
                      "switch", "template", "this", "throw", "true", "try", "using", "virtual",
                      "void", "while"},
                     m_keyword);
            addWords({"std", "QString", "QObject", "QVector", "QHash", "QByteArray", "auto"},
                     typeFmt);
            m_rules.push_back({QRegularExpression(QStringLiteral("^\\s*#\\s*[A-Za-z_]+.*$")),
                               preprocFmt});
            m_rules.push_back({QRegularExpression(QStringLiteral("//.*$")), m_comment});
        } else if (m_language == QStringLiteral("shell")) {
            addWords({"case", "do", "done", "elif", "else", "esac", "export", "fi", "for",
                      "function", "if", "in", "local", "then", "while"},
                     m_keyword);
            addWords({"awk", "cat", "cd", "chmod", "conda", "cp", "grep", "head", "ls", "mkdir",
                      "nvidia-smi", "pip", "python", "rm", "rsync", "squeue", "tail", "tar",
                      "watch"},
                     m_builtin);
        } else if (m_language == QStringLiteral("markdown")) {
            m_rules.push_back({QRegularExpression(QStringLiteral("^#{1,6}\\s+.*$")), m_keyword});
            m_rules.push_back({QRegularExpression(QStringLiteral("`[^`]*`")), m_string});
        }

        m_rules.push_back({QRegularExpression(QStringLiteral("\"([^\"\\\\]|\\\\.)*\"")), m_string});
        m_rules.push_back({QRegularExpression(QStringLiteral("'([^'\\\\]|\\\\.)*'")), m_string});
        m_rules.push_back({QRegularExpression(QStringLiteral("\\b[0-9]+(\\.[0-9]+)?\\b")),
                           m_number});
    }

    QString m_language;
    bool m_darkTheme = true;
    QVector<Rule> m_rules;
    QTextCharFormat m_keyword;
    QTextCharFormat m_builtin;
    QTextCharFormat m_string;
    QTextCharFormat m_number;
    QTextCharFormat m_comment;
};

CodeHighlighter::CodeHighlighter(QObject *parent) : QObject(parent) {}

CodeHighlighter::~CodeHighlighter() = default;

void CodeHighlighter::setTextDocument(QObject *document) {
    if (m_textDocumentObject == document)
        return;
    m_textDocumentObject = document;
    m_document = nullptr;
    if (auto *quickDoc = qobject_cast<QQuickTextDocument *>(document))
        m_document = quickDoc->textDocument();
    rebuild();
    emit textDocumentChanged();
}

void CodeHighlighter::setLanguage(const QString &language) {
    const QString normalised = language.trimmed().toLower();
    if (m_language == normalised)
        return;
    m_language = normalised;
    rebuild();
    emit languageChanged();
}

void CodeHighlighter::setDarkTheme(bool dark) {
    if (m_darkTheme == dark)
        return;
    m_darkTheme = dark;
    rebuild();
    emit darkThemeChanged();
}

void CodeHighlighter::rebuild() {
    m_highlighter.reset();
    if (!m_document || m_language.isEmpty())
        return;
    m_highlighter = std::make_unique<SyntaxHighlighter>(m_document, m_language, m_darkTheme);
    m_highlighter->rehighlight();
}

} // namespace researchssh
