#pragma once

#include <QHash>
#include <QMainWindow>
#include <QString>

#include "DeviceListModel.h"
#include "domain/AlarmEngine.h"
#include "domain/DeviceSession.h"
#include "protocol/MessageCodec.h"

class QLabel;
class QListView;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace dms::ui {

class CurveWidget;
class TelemetryBuffer;

// 主窗口(S4 最小 UI + S6 告警面板):
//   - 左侧设备列表(状态色/最近遥测),右侧选中设备四指标曲线;
//   - 底部告警面板(dock):触发/恢复/确认事件流,新事件插顶,确认按钮经
//     queued 信号请求引擎裁决(不返回值,结果经 onAlarmEvent 回流);
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
    // 经 auto(queued)连接送达 IO 线程的 DeviceManager / AlarmEngine
    void startAllRequested();
    void stopAllRequested();
    void sendCommandRequested(QString deviceId, quint16 commandId,
                              QList<dms::protocol::CommandParam> params);
    void alarmAckRequested(QString deviceId, QString code, quint16 metricId);

public slots:
    void onDeviceStateChanged(QString deviceId, dms::domain::SessionState state);
    void onDeviceTelemetry(QString deviceId, dms::domain::TelemetryBatch batch);
    void onDeviceCommandFinished(QString deviceId, dms::domain::CommandOutcome outcome);
    void onCommandRejected(QString deviceId, quint16 commandId, QString reason);
    // 引擎唯一出口的消费端:触发插行,恢复/确认更新对应行状态
    void onAlarmEvent(dms::domain::AlarmEvent event);

private slots:
    void refreshCurves(); // 100ms 定时器
    void onSelectionChanged();
    void sendReset();
    void sendSetPeriod();
    void acknowledgeSelectedAlarm();

private:
    void updateStatusBar();
    void refreshAlarmCount();
    void upsertAlarmRow(const dms::domain::AlarmEvent &event);
    // 自顶(最近)向下找 key 命中且 anchor 列文本等于 expected 的行;
    // expected 为空 = 键命中的首行
    int findAlarmRow(const QString &key, int anchorColumn,
                     const QString &expected) const;

    DeviceListModel *model_ = nullptr;
    QListView *deviceList_ = nullptr;
    QSpinBox *periodSpin_ = nullptr;
    QTimer *refreshTimer_ = nullptr;
    QLabel *onlineLabel_ = nullptr;
    QLabel *alarmLabel_ = nullptr;
    QTableWidget *alarmTable_ = nullptr;
    CurveWidget *curves_[4] = {nullptr, nullptr, nullptr, nullptr}; // metricId 1..4
    QHash<QString, TelemetryBuffer *> buffers_; // 每设备一份,随窗口生命周期
    QString selectedDevice_;
    int onlineCount_ = 0;
};

} // namespace dms::ui
