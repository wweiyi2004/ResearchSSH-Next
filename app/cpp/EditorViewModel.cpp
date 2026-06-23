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
    m_isOpen = true;
    m_busy = true;
    emit changed();
}

void EditorViewModel::setContent(const QString &path, const QByteArray &data) {
    if (m_path == path)
        m_busy = false;
    m_isOpen = true;
    emit changed();
    emit contentLoaded(path, QString::fromUtf8(data));
}

void EditorViewModel::failOpen(const QString &path, const QString &message) {
    if (path.isEmpty() || m_path == path) {
        m_busy = false;
        emit changed();
    }
    emit openFailed(path, message);
}

void EditorViewModel::activatePath(const QString &path) {
    if (path.isEmpty())
        return;
    m_path = path;
    m_isOpen = true;
    m_busy = false;
    emit changed();
}

void EditorViewModel::closePath(const QString &path) {
    if (path.isEmpty())
        return;
    emit pathClosed(path);
}

void EditorViewModel::beginSave() {
    if (!m_isOpen)
        return;
    m_saving = true;
    emit changed();
}

void EditorViewModel::finishSave(bool ok, const QString &message, const QString &path) {
    m_saving = false;
    emit changed();
    const QString target = path.isEmpty() ? m_path : path;
    if (ok)
        emit saveSucceeded(target);
    else
        emit saveFailed(target, message);
}

void EditorViewModel::close() {
    m_path.clear();
    m_isOpen = false;
    m_busy = false;
    m_saving = false;
    emit changed();
}

} // namespace researchssh
