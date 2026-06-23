#include "ServerListModel.h"

namespace researchssh {

ServerListModel::ServerListModel(QObject *parent) : QAbstractListModel(parent) {}

int ServerListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_items.size());
}

QVariant ServerListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const ServerItem &item = m_items.at(index.row());
    switch (role) {
    case NameRole:
        return item.name;
    case HostRole:
        return item.host;
    case PortRole:
        return item.port;
    case UsernameRole:
        return item.username;
    case ProviderRole:
        return item.provider == RsProviderKind_Mock ? QStringLiteral("mock")
                                                     : QStringLiteral("russh");
    case StatusRole:
        return item.status;
    case StatusTextRole:
        return statusText(item.status);
    case EndpointRole:
        return QStringLiteral("%1@%2:%3").arg(item.username, item.host).arg(item.port);
    default:
        return {};
    }
}

QHash<int, QByteArray> ServerListModel::roleNames() const {
    return {
        {NameRole, "name"},         {HostRole, "host"},
        {PortRole, "port"},         {UsernameRole, "username"},
        {ProviderRole, "provider"}, {StatusRole, "status"},
        {StatusTextRole, "statusText"}, {EndpointRole, "endpoint"},
    };
}

void ServerListModel::seedDemoServers() {
    beginResetModel();
    m_items = {
        ServerItem{QStringLiteral("HPC 登录节点"), QStringLiteral("login.hpc.example.edu"), 22,
                   QStringLiteral("researcher"), RsProviderKind_Mock, RsSessionState_Idle},
        ServerItem{QStringLiteral("GPU 集群"), QStringLiteral("gpu.cluster.example.edu"), 22,
                   QStringLiteral("alice"), RsProviderKind_Mock, RsSessionState_Idle},
        ServerItem{QStringLiteral("实验室工作站"), QStringLiteral("ws-lab-07.example.edu"),
                   2222, QStringLiteral("bob"), RsProviderKind_Mock, RsSessionState_Idle},
        // Hosts starting with "fail" are rejected by the mock provider — this row
        // exists to demonstrate the error state in the UI.
        ServerItem{QStringLiteral("无法连接(错误演示)"),
                   QStringLiteral("fail.offline.example.edu"), 22, QStringLiteral("nobody"),
                   RsProviderKind_Mock, RsSessionState_Idle},
    };
    endResetModel();
}

int ServerListModel::addServer(const QString &name, const QString &host, int port,
                               const QString &username, int provider, const QString &keyPath) {
    const int row = static_cast<int>(m_items.size());
    beginInsertRows(QModelIndex(), row, row);
    m_items.push_back(ServerItem{
        name.isEmpty() ? host : name,
        host,
        static_cast<quint16>(port),
        username,
        provider == static_cast<int>(RsProviderKind_Russh) ? RsProviderKind_Russh
                                                           : RsProviderKind_Mock,
        RsSessionState_Idle,
        keyPath,
    });
    endInsertRows();
    return row;
}

void ServerListModel::setStatus(int row, RsSessionState status) {
    if (!isValidIndex(row))
        return;
    m_items[row].status = status;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {StatusRole, StatusTextRole});
}

QString ServerListModel::statusText(int status) {
    switch (static_cast<RsSessionState>(status)) {
    case RsSessionState_Idle:
        return QStringLiteral("空闲");
    case RsSessionState_Connecting:
        return QStringLiteral("连接中…");
    case RsSessionState_Connected:
        return QStringLiteral("已连接");
    case RsSessionState_Disconnecting:
        return QStringLiteral("断开中…");
    case RsSessionState_Disconnected:
        return QStringLiteral("已断开");
    case RsSessionState_Failed:
        return QStringLiteral("失败");
    }
    return QStringLiteral("未知");
}

} // namespace researchssh
