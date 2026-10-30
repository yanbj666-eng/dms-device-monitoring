#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <functional>

#include "CommandTracker.h" // CommandOutcome/CommandState
#include "DeviceSession.h"  // TelemetryBatch/SessionState

class QTimer;

namespace dms::domain {

// ---- 告警代码(LLD §6)----
// DB alarms.code 列、UI 显示、引擎内代码索引共用同一字符串。
// metricId=0 表示设备级告警(后两类),指标级告警 metricId∈1..4
namespace AlarmCode {
inline constexpr char ThresholdHigh[] = "THRESHOLD_HIGH"; // 越上界
inline constexpr char ThresholdLow[]  = "THRESHOLD_LOW";   // 越下界
inline constexpr char DataStale[]     = "DATA_STALE";      // 指标无有效值
inline constexpr char ProtocolCrc[]   = "PROTOCOL_CRC";    // 连续坏 CRC 帧
inline constexpr char CommLost[]      = "COMM_LOST";       // 通信失联
} // namespace AlarmCode

// ---- 告警等级(与 DB alarms.level 列一致,docs/06 §2)----
// 取值依据:阻断通信/业务越界 = ERROR,质量降级可继续 = WARN
namespace AlarmLevel {
inline constexpr quint8 Info  = 1;
inline constexpr quint8 Warn  = 2;
inline constexpr quint8 Error = 3;
} // namespace AlarmLevel

// 阈值规则(devices.json alarm_rules 项的 domain 形态;app 层负责映射与校验,
// 引擎信任 low<high、hysteresis≥0 已由 Config 层保证)
struct AlarmRule {
    quint16 metricId = 0;
    double low = 0.0;
    double high = 0.0;
    double hysteresis = 0.0;
};

// 告警生命周期事件(引擎唯一出口):UI 显示与 DB 落库共用同一事件流。
// 跨线程 queued 传值(HLD §2),atMs 由引擎在裁决点打(UTC Unix 毫秒),
// worker 照抄入库——单一打点源换来测试可确定性(注入假时钟断言时间列)
struct AlarmEvent {
    enum class Kind { Triggered, Cleared, Acknowledged };
    Kind kind = Kind::Triggered;
    QString deviceId;
    QString code;          // AlarmCode::*
    quint16 metricId = 0;  // 0=设备级
    quint8 level = AlarmLevel::Warn;
    QString message;       // Triggered 时的人读描述
    QString clearReason;   // Cleared 时:recovered / manual_disconnect
    qint64 atMs = 0;       // 引擎打点(UTC ms)
};

// 告警引擎(LLD §6,REQ-ALARM-001/002):IO 线程 worker-object,
// 消费 DeviceManager 转发的值对象信号,裁决四类告警的触发/恢复/确认。
//
// 设计要点:
//   - 内存活跃表是唯一裁决点:每 (code,metricId) 键只保留最近一条记录,
//     活跃期间持续异常不重复发射(DB 部分唯一索引只是二道防线);
//   - 记录恢复后仍留在"最近一条"槽位,支持先恢复后确认;同键再次越界
//     产生全新记录(不复用旧行,历史保留);
//   - 确认目标 = 该键最近一条记录,与 DB 侧 ORDER BY id DESC LIMIT 1 同语义;
//   - 规则可脱离 GUI/DB 测试:构造注入假时钟,测试手动调用 poll() 推进。
//
// 线程约定:与 DeviceManager 同线程(IO);GUI 经 queued acknowledge 槽请求确认,
// 结果异步经 alarmEvent 广播,不返回值
class AlarmEngine : public QObject
{
    Q_OBJECT

public:
    // UTC 毫秒墙钟:事件打点与陈旧间隔共用一个源。间隔用墙钟差而非
    // 单调钟,是因为打点值本身要入库(ts/cleared_at CHECK 要求单调不减);
    // NTP 微调毫秒级不影响 3000ms 判定,测试注入假时钟完全确定
    using Clock = std::function<qint64()>;

    // 评估参数(app.ini [connection] 同源:LLD §3/§6 引用同一组阈值,
    // 装配层从 SessionConfig 映射,避免第二套配置漂移)
    struct Settings {
        int pollIntervalMs = 100;      // DATA_STALE 评估周期(与会话 poll 同拍)
        int staleTimeoutMs = 3000;     // 指标无有效值 → DATA_STALE(LLD §6)
        int badCrcTrigger = 5;         // 连续坏 CRC → PROTOCOL_CRC
        int goodFramesToClearCrc = 3;  // 连续有效帧 → 解除 PROTOCOL_CRC
    };

    explicit AlarmEngine(Settings settings, Clock clock = defaultClock,
                         QObject *parent = nullptr);

    // 装配期(同线程)注入阈值规则;未配置阈值的指标不评估(LLD §6)
    void setRules(const QString &deviceId, const QList<AlarmRule> &rules);
    int activeAlarmCount() const; // 仅所属线程诊断/测试用

    static qint64 defaultClock();

public slots:
    // 建评估定时器:ioThread started 槽里调用(线程亲和,同 DeviceSession 纪律)
    void start();
    void onTelemetry(QString deviceId, dms::domain::TelemetryBatch batch);
    void onSessionState(QString deviceId, dms::domain::SessionState state);
    // 会话每解码块的协议健康(先计坏帧突发后计有效帧,与会话自身 streak
    // 更新顺序一致,见 DeviceSession::onBytesReceived)
    void onProtocolHealth(QString deviceId, quint32 badCrcFrames, quint32 goodFrames);
    // 只认设备确认过的 Start/Stop/Reset(Succeeded = ACK Success):驱动采集启停
    // (Reset 按协议 §4.2 会启动遥测,与 Start 同样恢复检测)
    void onCommandFinished(QString deviceId, dms::domain::CommandOutcome outcome);
    // 确认该键最近一条未确认记录(活跃或已恢复均可,LLD §6)
    void acknowledge(QString deviceId, QString code, quint16 metricId);
    // 定时器驱动;测试注入假时钟后手动调用推进
    void poll();

signals:
    void alarmEvent(dms::domain::AlarmEvent event);

private:
    // 每设备聚合状态:records 键 = codeIdx*8+metricId(代码≤5、指标≤4,
    // quint32 紧凑键避免 QString 拼接)
    struct Record {
        AlarmEvent triggered;      // 触发时的完整事件(恢复/确认补状态)
        bool active = false;        // 活跃 = 未恢复
        bool acknowledged = false; // 确认独立于恢复(LLD §6)
    };
    struct DeviceState {
        QHash<quint32, Record> records;
        QHash<quint16, AlarmRule> rules;  // metricId -> 规则
        qint64 metricSeenMs[4] = {0, 0, 0, 0}; // 指标 1..4 最近有效值时刻
        // 会话处于 Online/Degraded:只有链路已建立才评估 DATA_STALE(LLD §3
        // "Online 下"),链路未建立的问题由 COMM_LOST/人工断开语义表达
        bool linkUp = false;
        bool acquisitionActive = true;         // Stop ACK 后暂停陈旧检测
        int crcBadStreak = 0;                  // 连续坏 CRC 帧
        int crcGoodStreak = 0;                 // 连续有效帧(解除 PROTOCOL_CRC)
    };

    static quint32 recordKey(quint8 codeIdx, quint16 metricId)
    {
        return quint32(codeIdx) * 8 + metricId;
    }

    DeviceState &deviceState(const QString &deviceId); // 首见即建档
    Record *findRecord(DeviceState &st, quint8 codeIdx, quint16 metricId);
    void evaluateThresholds(DeviceState &st, const QString &deviceId,
                            quint16 metricId, double value, qint64 now);
    void trigger(DeviceState &st, const QString &deviceId, quint8 codeIdx,
                 quint16 metricId, const QString &code, quint8 level,
                 const QString &message, qint64 now);
    void clear(DeviceState &st, const QString &deviceId, quint8 codeIdx,
               quint16 metricId, const QString &code, const QString &reason,
               qint64 now);
    void retimeMetrics(DeviceState &st, qint64 now); // 新一轮连接/Start 后重置陈旧计时

    Settings settings_;
    Clock clock_;
    QTimer *pollTimer_ = nullptr; // start() 时在所属线程懒创建
    QHash<QString, DeviceState> devices_;
};

} // namespace dms::domain

Q_DECLARE_METATYPE(dms::domain::AlarmEvent)
