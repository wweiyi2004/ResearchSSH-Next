// ServerListModel — the left-pane list of configured servers.
//
// A plain QAbstractListModel exposing server metadata + a per-row connection
// status to QML. It holds NO secrets and never touches the FFI; it is pure UI
// state owned by the C++ layer.

#pragma once

#include <QAbstractListModel>
#include <QVector>

#include "research_ssh_core.h"

namespace researchssh {

struct ServerItem {
    QString name;
    QString host;
    quint16 port = 22;
    QString username;
    RsProviderKind provider = RsProviderKind_Mock;
    int status = RsSessionState_Idle; // mirrors RsSessionState
    // Optional private-key path for public-key auth ("" = auto-discover ~/.ssh).
    // Kept last so existing aggregate initialisations stay valid.
    QString keyPath;
};

class ServerListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        HostRole,
        PortRole,
        UsernameRole,
        ProviderRole,
        StatusRole,
        StatusTextRole,
        EndpointRole, // "user@host:port"
    };
    Q_ENUM(Roles)

    explicit ServerListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Seed the model with a few demo servers (incl. one that fails to connect).
    void seedDemoServers();

    // Access for the controller.
    bool isValidIndex(int row) const { return row >= 0 && row < m_items.size(); }
    const ServerItem &itemAt(int row) const { return m_items.at(row); }
    void setStatus(int row, RsSessionState status);

    Q_INVOKABLE int count() const { return static_cast<int>(m_items.size()); }

    // Append a user-defined server (e.g. from the connection dialog). Returns the
    // new row index.
    Q_INVOKABLE int addServer(const QString &name, const QString &host, int port,
                              const QString &username, int provider,
                              const QString &keyPath = QString());

    static QString statusText(int status);

private:
    QVector<ServerItem> m_items;
};

} // namespace researchssh
