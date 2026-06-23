// EditorViewModel — backing state for the in-app text editor.
//
// Holds the currently open remote file's path and load state. The actual editable
// text lives in the QML TextArea; on open the loaded content is delivered via the
// contentLoaded() signal, and on save the controller reads the TextArea's text.

#pragma once

#include <QObject>
#include <QString>

namespace researchssh {

class EditorViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(bool isOpen READ isOpen NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool saving READ saving NOTIFY changed)

public:
    explicit EditorViewModel(QObject *parent = nullptr);

    QString path() const { return m_path; }
    QString fileName() const;
    bool isOpen() const { return m_isOpen; }
    bool busy() const { return m_busy; }
    bool saving() const { return m_saving; }

    void beginOpen(const QString &path); // busy=true, remember path
    void setContent(const QString &path, const QByteArray &data); // -> contentLoaded
    void failOpen();
    void activatePath(const QString &path);
    void beginSave();
    void finishSave(bool ok, const QString &message = {});
    void close();

signals:
    void changed();
    // Carries freshly loaded text for the QML editor to display.
    void contentLoaded(const QString &path, const QString &text);
    void saveSucceeded();
    void saveFailed(const QString &message);

private:
    QString m_path;
    bool m_isOpen = false;
    bool m_busy = false;
    bool m_saving = false;
};

} // namespace researchssh
