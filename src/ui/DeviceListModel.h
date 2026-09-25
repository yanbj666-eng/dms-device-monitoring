#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

#include "domain/DeviceSession.h"

namespace dms::ui {

// 状态 → 显示色(列表色点与状态栏共用)
QColor stateColor(dms::domain::SessionState state);

// 设备列表 Model(Widgets Model-View):
// 行数据只由 GUI 线程经 deviceStateChanged/遥测到达时间更新,
// IO 线程的数据流全部经 MainWindow 的 queued 槽进来
class DeviceListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        DeviceIdRole = Qt::UserRole + 1,
        NameRole,
        StateTextRole,
        StateColorRole,
        LastUpdateRole, // 距最近一次遥测的秒数文本
    };

    explicit DeviceListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 初始化设备行(配置装配一次);重复 id 忽略
    void setDevices(const QVector<QPair<QString, QString>> &idAndNames);
    int rowOf(const QString &deviceId) const;
    QString deviceIdAt(int row) const;
    int onlineCount() const; // Online+Degraded(都在传数据)

    void updateState(const QString &deviceId, dms::domain::SessionState state);
    void touchTelemetry(const QString &deviceId); // 记录最近遥测到达时刻
    void refreshTimes(); // 100ms 定时器驱动:刷新"Xs 前"文本

private:
    struct Row {
        QString id;
        QString name;
        dms::domain::SessionState state = dms::domain::SessionState::Disconnected;
        qint64 lastTelemetryMs = 0; // 0=从未收到
    };

    QVector<Row> rows_;
    QHash<QString, int> rowById_;
};

} // namespace dms::ui
