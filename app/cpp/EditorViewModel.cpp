#include "EditorViewModel.h"

namespace researchssh {

EditorViewModel::EditorViewModel(QObject *parent) : QObject(parent) {}

QString EditorViewModel::fileName() const {
    if (m_path.isEmpty())
        return {};
    const int slash = m_path.lastIndexOf('/');
    return slash >= 0 ? m_path.mid(slash + 1) : m_path;
}

void EditorViewModel::beginOpen(const QString &path) {
    m_path = path;
    m_busy = true;
    m_saving = false;
    emit changed();
}

void EditorViewModel::setContent(const QString &path, const QByteArray &data) {
    m_path = path;
    m_busy = false;
    m_isOpen = true;
    m_saving = false;
    emit changed();
    emit contentLoaded(path, QString::fromUtf8(data));
}

void EditorViewModel::failOpen() {
    m_path.clear();
    m_isOpen = false;
    m_busy = false;
    m_saving = false;
    emit changed();
}

void EditorViewModel::activatePath(const QString &path) {
    if (path.isEmpty())
        return;
    m_path = path;
    m_isOpen = true;
    m_busy = false;
    m_saving = false;
    emit changed();
}

void EditorViewModel::beginSave() {
    if (!m_isOpen)
        return;
    m_saving = true;
    emit changed();
}

void EditorViewModel::finishSave(bool ok, const QString &message) {
    if (!m_saving && ok)
        return;
    m_saving = false;
    emit changed();
    if (ok)
        emit saveSucceeded();
    else
        emit saveFailed(message);
}

void EditorViewModel::close() {
    m_path.clear();
    m_isOpen = false;
    m_busy = false;
    m_saving = false;
    emit changed();
}

} // namespace researchssh
