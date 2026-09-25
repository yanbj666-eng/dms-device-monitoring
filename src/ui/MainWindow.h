#pragma once

#include <QHash>
#include <QMainWindow>
#include <QString>

#include "DeviceListModel.h"
#include "domain/DeviceSession.h"
#include "protocol/MessageCodec.h"

class QLabel;
class QListView;
class QSpinBox;
class QTimer;

namespace dms::ui {

class CurveWidget;
class TelemetryBuffer;

// 主窗口(S4 最小 UI,LLD §8 页面集的 Devices+Telemetry 子集):
//   - 左侧设备列表(状态色/最近遥测),右侧选中设备四指标曲线;
//   - 命令演示:Reset / SetParameter(采样周期),结果走命令生命周期信号;
//   - 100ms 合并刷新:遥测信号只入缓冲,定时器统一喂曲线(LLD §5)。
// 全部 QWidget 只在 GUI 线程;IO 数据经 queued 槽进入
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // 只收原始参数与设备清单,不依赖 app 层类型(依赖方向 ui -> domain)
    MainWindow(int refreshIntervalMs, int curvePoints,
               const QVector<QPair<QString, QString>> &deviceIdAndNames,
               QWidget *parent = nullptr);

signals:
    // 经 auto(queued)连接送达 IO 线程的 DeviceManager
    void startAllRequested();
    void stopAllRequested();
    void sendCommandRequested(QString deviceId, quint16 commandId,
                              QList<dms::protocol::CommandParam> params);

public slots:
    void onDeviceStateChanged(QString deviceId, dms::domain::SessionState state);
    void onDeviceTelemetry(QString deviceId, dms::domain::TelemetryBatch batch);
    void onDeviceCommandFinished(QString deviceId, dms::domain::CommandOutcome outcome);
    void onCommandRejected(QString deviceId, quint16 commandId, QString reason);

private slots:
    void refreshCurves(); // 100ms 定时器
    void onSelectionChanged();
    void sendReset();
    void sendSetPeriod();

private:
    void updateStatusBar();

    DeviceListModel *model_ = nullptr;
    QListView *deviceList_ = nullptr;
    QSpinBox *periodSpin_ = nullptr;
    QTimer *refreshTimer_ = nullptr;
    QLabel *onlineLabel_ = nullptr;
    CurveWidget *curves_[4] = {nullptr, nullptr, nullptr, nullptr}; // metricId 1..4
    QHash<QString, TelemetryBuffer *> buffers_; // 每设备一份,随窗口生命周期
    QString selectedDevice_;
    int onlineCount_ = 0;
};

} // namespace dms::ui
