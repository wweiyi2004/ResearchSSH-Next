#include "RemoteFileTreeModel.h"

#include <QSet>

namespace researchssh {

RemoteFileTreeModel::RemoteFileTreeModel(QObject *parent) : QAbstractItemModel(parent) {
    m_root = new Node();
    m_root->isDir = true;
    m_root->path = QString(); // "" = home
    m_byPath.insert(QString(), m_root);
}

RemoteFileTreeModel::~RemoteFileTreeModel() {
    delete m_root;
}

RemoteFileTreeModel::Node *RemoteFileTreeModel::nodeFromIndex(const QModelIndex &index) const {
    if (!index.isValid())
        return m_root;
    return static_cast<Node *>(index.internalPointer());
}

QString RemoteFileTreeModel::parentPathOf(QString path) {
    while (path.endsWith('/') && path.size() > 1)
        path.chop(1);
    const int slash = path.lastIndexOf('/');
    if (slash < 0)
        return {};
    if (slash == 0)
        return QStringLiteral("/");
    return path.left(slash);
}

bool RemoteFileTreeModel::isRootAliasPath(const QString &path) const {
    if (path.isEmpty() || path == QStringLiteral("."))
        return true;
    if (m_root->children.isEmpty())
        return false;
    return parentPathOf(m_root->children.constFirst()->path) == path;
}

RemoteFileTreeModel::Node *RemoteFileTreeModel::nodeForPath(const QString &path) const {
    if (Node *node = m_byPath.value(path, nullptr))
        return node;
    return isRootAliasPath(path) ? m_root : nullptr;
}

void RemoteFileTreeModel::removePathIndex(Node *node) {
    if (!node)
        return;
    for (Node *child : node->children)
        removePathIndex(child);
    if (!node->path.isEmpty())
        m_byPath.remove(node->path);
}

QModelIndex RemoteFileTreeModel::indexForNode(Node *node) const {
    if (!node || node == m_root || !node->parent)
        return QModelIndex();
    const int row = static_cast<int>(node->parent->children.indexOf(node));
    if (row < 0)
        return QModelIndex();
    return createIndex(row, 0, node);
}

QModelIndex RemoteFileTreeModel::index(int row, int column, const QModelIndex &parent) const {
    if (column != 0)
        return QModelIndex();
    Node *parentNode = nodeFromIndex(parent);
    if (row < 0 || row >= parentNode->children.size())
        return QModelIndex();
    return createIndex(row, 0, parentNode->children.at(row));
}

QModelIndex RemoteFileTreeModel::parent(const QModelIndex &child) const {
    if (!child.isValid())
        return QModelIndex();
    Node *node = static_cast<Node *>(child.internalPointer());
    return indexForNode(node->parent);
}

int RemoteFileTreeModel::rowCount(const QModelIndex &parent) const {
    if (parent.column() > 0)
        return 0;
    return static_cast<int>(nodeFromIndex(parent)->children.size());
}

int RemoteFileTreeModel::columnCount(const QModelIndex &) const {
    return 1;
}

QVariant RemoteFileTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    const Node *node = static_cast<Node *>(index.internalPointer());
    switch (role) {
    case NameRole:
    case FileNameRole:
    case Qt::DisplayRole:
        return node->name;
    case PathRole:
        return node->path;
    case IsDirRole:
        return node->isDir;
    case SizeRole:
        return static_cast<qulonglong>(node->size);
    case EditableRole:
        return node->editable;
    default:
        return {};
    }
}

QHash<int, QByteArray> RemoteFileTreeModel::roleNames() const {
    return {
        {NameRole, "name"},   {PathRole, "path"},         {IsDirRole, "isDir"},
        {SizeRole, "size"},   {EditableRole, "editable"}, {FileNameRole, "fileName"},
    };
}

bool RemoteFileTreeModel::hasChildren(const QModelIndex &parent) const {
    return nodeFromIndex(parent)->isDir;
}

bool RemoteFileTreeModel::canFetchMore(const QModelIndex &parent) const {
    Node *node = nodeFromIndex(parent);
    return node->isDir && !node->loaded && !node->loading;
}

void RemoteFileTreeModel::fetchMore(const QModelIndex &parent) {
    Node *node = nodeFromIndex(parent);
    if (!node->isDir || node->loaded || node->loading)
        return;
    node->loading = true;
    emit directoryExpandRequested(node->path);
}

void RemoteFileTreeModel::loadRoot() {
    // Force a (re)load of the home directory.
    m_root->loaded = false;
    m_root->loading = true;
    emit directoryExpandRequested(QString());
}

bool RemoteFileTreeModel::childExists(const QString &parentPath, const QString &name) const {
    const QString needle = name.trimmed();
    if (needle.isEmpty())
        return false;
    const Node *parentNode = nodeForPath(parentPath);
    if (!parentNode || !parentNode->loaded)
        return false;
    for (const Node *child : parentNode->children) {
        if (QString::compare(child->name, needle, Qt::CaseSensitive) == 0)
            return true;
    }
    return false;
}

void RemoteFileTreeModel::clearTree() {
    beginResetModel();
    qDeleteAll(m_root->children);
    m_root->children.clear();
    m_root->loaded = false;
    m_root->loading = false;
    m_byPath.clear();
    m_byPath.insert(QString(), m_root);
    endResetModel();
}

void RemoteFileTreeModel::markLoadFailed(const QString &path) {
    if (Node *node = nodeForPath(path)) {
        node->loading = false;
    }
}

void RemoteFileTreeModel::applyListing(const QString &path, const QVector<FsEntry> &entries) {
    Node *parentNode = nodeForPath(path);
    if (!parentNode)
        return;
    const QModelIndex parentIndex = indexForNode(parentNode);

    QSet<QString> incomingPaths;
    for (const FsEntry &entry : entries)
        incomingPaths.insert(entry.path);

    for (int row = parentNode->children.size() - 1; row >= 0; --row) {
        Node *child = parentNode->children.at(row);
        if (incomingPaths.contains(child->path))
            continue;
        beginRemoveRows(parentIndex, row, row);
        parentNode->children.removeAt(row);
        removePathIndex(child);
        delete child;
        endRemoveRows();
    }

    for (const FsEntry &e : entries) {
        if (Node *existing = nodeForPath(e.path)) {
            existing->name = e.name;
            existing->isDir = (e.kind == 1); // RsFileKind_Directory
            existing->size = e.size;
            existing->editable = e.editable;
            const QModelIndex changed = indexForNode(existing);
            if (changed.isValid())
                emit dataChanged(changed, changed,
                                 {NameRole, FileNameRole, IsDirRole, SizeRole, EditableRole});
            continue;
        }

        const int row = parentNode->children.size();
        beginInsertRows(parentIndex, row, row);
        Node *child = new Node();
        child->name = e.name;
        child->path = e.path;
        child->isDir = (e.kind == 1); // RsFileKind_Directory
        child->size = e.size;
        child->editable = e.editable;
        child->parent = parentNode;
        parentNode->children.push_back(child);
        if (!child->path.isEmpty())
            m_byPath.insert(child->path, child);
        endInsertRows();
    }

    parentNode->loaded = true;
    parentNode->loading = false;
}

} // namespace researchssh
