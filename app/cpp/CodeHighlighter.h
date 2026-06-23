// CodeHighlighter - lightweight syntax highlighting for the QML editor.

#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QTextDocument;

namespace researchssh {

class SyntaxHighlighter;

class CodeHighlighter : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY darkThemeChanged)

public:
    explicit CodeHighlighter(QObject *parent = nullptr);
    ~CodeHighlighter() override;

    QObject *textDocument() const { return m_textDocumentObject; }
    void setTextDocument(QObject *document);

    QString language() const { return m_language; }
    void setLanguage(const QString &language);

    bool darkTheme() const { return m_darkTheme; }
    void setDarkTheme(bool dark);

signals:
    void textDocumentChanged();
    void languageChanged();
    void darkThemeChanged();

private:
    void rebuild();

    QObject *m_textDocumentObject = nullptr;
    QTextDocument *m_document = nullptr;
    QString m_language;
    bool m_darkTheme = true;
    std::unique_ptr<SyntaxHighlighter> m_highlighter;
};

} // namespace researchssh
