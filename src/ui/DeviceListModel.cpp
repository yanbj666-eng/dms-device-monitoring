#include "DeviceListModel.h"

#include <QDateTime>

namespace dms::ui {

QColor stateColor(dms::domain::SessionState state)
{
    using dms::domain::SessionState;
    switch (state) {
    case SessionState::Online:       return QColor(0x2e, 0xa0, 0x43);   // 绿
    case SessionState::Degraded:     return QColor(0xd9, 0x9b, 0x1e);   // 黄
    case SessionState::Connecting:   return QColor(0x35, 0x74, 0xc4);   // 蓝
    case SessionState::Reconnecting: return QColor(0xd9, 0x6b, 0x1e);   // 橙
    case SessionState::Fault:        return QColor(0xc0, 0x39, 0x2b);   // 红
    case SessionState::Disconnected: return QColor(0x8a, 0x8a, 0x8a);   // 灰
    }
    return QColor(0x8a, 0x8a, 0x8a);
}

DeviceListModel::DeviceListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DeviceListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

QVariant DeviceListModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= rows_.size())
        return {};
    const Row &row = rows_[index.row()];
    switch (role) {
    case DeviceIdRole:
        return row.id;
    case NameRole:
        return row.name;
    case Qt::DisplayRole: // 默认委托只画 Display/Decoration:最近遥测并入文字
        return QStringLiteral("%1  [%2]  %3")
            .arg(row.name, sessionStateName(row.state), lastUpdateText(row));
    case StateTextRole:
        return QStringLiteral("%1  [%2]").arg(row.name, sessionStateName(row.state));
    case Qt::DecorationRole: // QColor 装饰由默认委托画成色块,即列表的状态色点
    case StateColorRole:
        return stateColor(row.state);
    case LastUpdateRole:
        return lastUpdateText(row);
    default:
        return {};
    }
}

QString DeviceListModel::lastUpdateText(const Row &row)
{
    if (row.lastTelemetryMs == 0)
        return QStringLiteral("无遥测");
    const qint64 delta = QDateTime::currentMSecsSinceEpoch() - row.lastTelemetryMs;
    return delta < 1500 ? QStringLiteral("刚刚") : QStringLiteral("%1s 前").arg(delta / 1000);
}

QHash<int, QByteArray> DeviceListModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles[DeviceIdRole] = "deviceId";
    roles[NameRole] = "name";
    roles[StateTextRole] = "stateText";
    roles[StateColorRole] = "stateColor";
    roles[LastUpdateRole] = "lastUpdate";
    return roles;
}

void DeviceListModel::setDevices(const QVector<QPair<QString, QString>> &idAndNames)
{
    beginResetModel();
    rows_.clear();
    rowById_.clear();
    for (const auto &pair : idAndNames) {
        if (rowById_.contains(pair.first))
            continue;
        rowById_.insert(pair.first, rows_.size());
        rows_.append(Row{pair.first, pair.second});
    }
    endResetModel();
}

int DeviceListModel::rowOf(const QString &deviceId) const
{
    return rowById_.value(deviceId, -1);
}

int DeviceListModel::onlineCount() const
{
    int online = 0;
    for (const Row &row : rows_) {
        if (row.state == dms::domain::SessionState::Online
            || row.state == dms::domain::SessionState::Degraded)
            ++online;
    }
    return online;
}

QString DeviceListModel::deviceIdAt(int row) const
{
    return (row >= 0 && row < rows_.size()) ? rows_[row].id : QString();
}

void DeviceListModel::updateState(const QString &deviceId, dms::domain::SessionState state)
{
    const int row = rowOf(deviceId);
    if (row < 0)
        return;
    rows_[row].state = state;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx,
                     {Qt::DisplayRole, Qt::DecorationRole, StateTextRole, StateColorRole});
}

void DeviceListModel::touchTelemetry(const QString &deviceId)
{
    const int row = rowOf(deviceId);
    if (row < 0)
        return;
    rows_[row].lastTelemetryMs = QDateTime::currentMSecsSinceEpoch();
}

void DeviceListModel::refreshTimes()
{
    if (rows_.isEmpty())
        return;
    const QModelIndex first = index(0);
    const QModelIndex last = index(rows_.size() - 1);
    emit dataChanged(first, last, {Qt::DisplayRole, LastUpdateRole});
}

} // namespace dms::ui
