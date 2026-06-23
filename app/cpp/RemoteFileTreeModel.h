// RemoteFileTreeModel — lazy tree of the remote filesystem.
//
// A QAbstractItemModel for QML TreeView. Directory contents are loaded on demand:
// expanding a directory emits directoryExpandRequested(path); the controller issues
// an fsList and feeds the result back via applyListing(). The model holds NO secrets
// and never calls the FFI — it only describes UI state.

#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QVector>

#include "RustCoreBridge.h" // FsEntry

namespace researchssh {

class RemoteFileTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        SizeRole,
        EditableRole,
        FileNameRole,
    };
    Q_ENUM(Roles)

    explicit RemoteFileTreeModel(QObject *parent = nullptr);
    ~RemoteFileTreeModel() override;

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    // Controller API.
    void clearTree();                         // reset to empty (on disconnect)
    void applyListing(const QString &path, const QVector<FsEntry> &entries);
    void markLoadFailed(const QString &path); // allow retry
    // Path used to load the top level (home). "" means the connection's home dir.
    Q_INVOKABLE void loadRoot();
    Q_INVOKABLE bool childExists(const QString &parentPath, const QString &name) const;

signals:
    void directoryExpandRequested(const QString &path);

private:
    struct Node {
        QString name;
        QString path;
        bool isDir = false;
        quint64 size = 0;
        bool editable = false;
        bool loaded = false;
        bool loading = false;
        Node *parent = nullptr;
        QVector<Node *> children;
        ~Node() { qDeleteAll(children); }
    };

    Node *nodeFromIndex(const QModelIndex &index) const;
    Node *nodeForPath(const QString &path) const;
    QModelIndex indexForNode(Node *node) const;
    void removePathIndex(Node *node);
    bool isRootAliasPath(const QString &path) const;
    static QString parentPathOf(QString path);

    Node *m_root; // invisible root; its children are the home dir contents
    QHash<QString, Node *> m_byPath; // path -> node ("" = root/home)
};

} // namespace researchssh
