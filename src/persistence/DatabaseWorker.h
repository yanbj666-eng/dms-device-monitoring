#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUuid>

#include <deque>
#include <functional>
#include <variant>

#include "Database.h"
#include "domain/AlarmEngine.h"
#include "domain/CommandTracker.h"
#include "domain/DeviceManager.h"
#include "domain/DeviceSession.h"

class QTimer;

namespace dms::persistence {

// 批量写入参数(AppConfig.db 映射;装配层已把 path 解析成绝对路径,
// 范围校验在 Config 层完成,docs/15 §4)
struct DbWorkerSettings {
    QString databasePath;
    int batchSamples = 500;      // 触发阈值:样本数先到即提交
    int flushIntervalMs = 1000;  // 时间触发与失败重试节奏
    int queueSamples = 50000;    // 遥测内存队列上限,满丢新计数
    int eventQueueSize = 1000;   // 命令/告警事件 FIFO 上限
    int busyTimeoutMs = 1000;
    int shutdownFlushMs = 5000;  // 退出排空预算
};

// DB 线程 worker-object(LLD §7,与 DeviceManager 同模式):
//   - 遥测有界批队列:batch_samples 或 flush_interval_ms 先到者触发,
//     队列满丢新样本并累计 droppedSamples(不丢旧、不静默);
//   - 每批一个 BEGIN IMMEDIATE 事务,任一失败整批回滚保留,按节奏重试;
//     重复键 ON CONFLICT(唯一键) DO NOTHING,同键异值日志报告不覆盖;
//   - 命令/告警事件共用一条 FIFO(容量 event_queue_size):受理(Sent 插入)、
//     终态(更新)与告警触发/恢复/确认保持到达顺序;空闲时直写低延迟,
//     失败积压重试,满时丢新并明确报告未持久化;
//   - received_at/created_at/finished_at 在本层入队时打点(UTC 墙钟):
//     跨线程 queued 延迟毫秒级,换来单一时钟源与可注入可测性。
//     告警列例外:atMs 由引擎在裁决点打(测试注入假时钟可断言时间列),
//     本层照抄,满足 cleared_at ≥ ts 的 CHECK 只依赖 FIFO 顺序。
// 线程约定:无 parent → moveToThread;started 槽里 initialize;
// GUI/IO 只经 queued 信号喂数据,结果经 flushCompleted/closed/dbFatal 回报
class DatabaseWorker : public QObject
{
    Q_OBJECT

public:
    // UTC 毫秒墙钟(区别于会话层的 steady 单调钟:入库列是 Unix 毫秒)
    using Clock = std::function<qint64()>;
    using Sleeper = std::function<void(int)>; // 排空重试节奏,测试注入假睡眠

    explicit DatabaseWorker(DbWorkerSettings settings, Clock clock = defaultClock,
                            Sleeper sleeper = defaultSleeper, QObject *parent = nullptr);

    // 诊断计数(所属线程读;跨线程经 closed/flushCompleted 信号回报)
    quint64 droppedSamples() const { return droppedSamples_; }
    quint64 droppedEvents() const { return droppedEvents_; }
    int pendingSamples() const { return int(pending_.size()); }
    int pendingEvents() const { return int(events_.size()); }

    static qint64 defaultClock();
    static void defaultSleeper(int ms);

public slots:
    // 建连接/建库/恢复/upsert 设备;失败发 dbFatal 后拒绝一切写入
    void initialize(QList<dms::persistence::DeviceRow> devices);
    void onTelemetry(QString deviceId, dms::domain::TelemetryBatch batch);
    void onCommandSubmitted(dms::domain::CommandSubmission submission);
    void onCommandFinished(QString deviceId, dms::domain::CommandOutcome outcome);
    // 告警生命周期事件:Triggered=INSERT, Cleared/Acknowledged=UPDATE;
    // atMs 用引擎打点(见类注释),本层不重打
    void onAlarmEvent(dms::domain::AlarmEvent event);
    // 定时器驱动的时间触发;测试注入假时钟后手动调用推进
    void pollFlush();
    // 退出排空(BlockingQueued 调用):预算内重试,完成发 closed 并关连接;
    // stopped 后到达的数据只计数丢弃
    void drainAndClose();

signals:
    // 每轮写入尝试后的同步点(测试等待/诊断)
    void flushCompleted(bool ok, int pendingSamples, int pendingEvents);
    void dbFatal(dms::ErrorCode code, QString message);
    // 排空结束:未提交数显式上报,不伪报成功(LLD §7)
    void closed(qint64 uncommittedSamples, qint64 uncommittedEvents,
                quint64 droppedSamples, quint64 droppedEvents);

private:
    struct PendingSample {
        QString deviceId;
        QUuid sessionId;
        quint32 seq = 0;
        quint16 metricId = 0;
        double value = 0.0;
        qint64 ts = 0;         // 设备时间,0=未知
        qint64 receivedAt = 0; // 本层入队时刻(UTC ms)
    };

    // 命令事件:FIFO 保序。submitted=true 是 commands 表 Sent 插入,
    // false 是终态更新(result/ack_status/finished_at)
    struct CommandEvent {
        bool submitted = true;
        QString deviceId;
        QUuid sessionId;
        quint32 seq = 0;
        qint64 atMs = 0; // 受理/终态到达时刻(UTC ms),重试不改写
        quint16 commandId = 0;
        QList<dms::protocol::CommandParam> params;
        dms::domain::CommandState finalState = dms::domain::CommandState::Sent;
        quint16 ackStatus = 0;
    };

    // 命令/告警事件:FIFO 保序(单一队列保证同键事件不乱序)。
    // 命令:submitted=true 是 commands 表 Sent 插入,false 是终态更新;
    // 告警:AlarmEvent 原样落库(atMs 已是引擎打点)
    using Event = std::variant<CommandEvent, dms::domain::AlarmEvent>;

    bool flushTelemetryChunk(); // 一批一事务(batch_samples 上限,LLD §7)
    bool flushEvents();         // 事件 FIFO 一事务清空(命令+告警)
    // 两者都试,刷新时间锚点并发 flushCompleted;返回本轮是否全部成功
    bool flushOnce();
    void reportDuplicate(const PendingSample &sample); // 同键异值诊断
    void tryFlushEventsIfIdle(); // 事件直写低延迟路径
    void dropSample(const QString &deviceId, int count, dms::ErrorCode code,
                    const QString &reason);
    void closeDatabase();

    DbWorkerSettings settings_;
    Clock clock_;
    Sleeper sleeper_;
    Database database_;
    QTimer *flushTimer_ = nullptr; // initialize 时在本线程创建(线程亲和)
    std::deque<PendingSample> pending_;
    std::deque<Event> events_;
    QSet<QString> knownDevices_; // 已 upsert 设备:防 FK 毒丸堵死队列
    qint64 lastFlushAtMs_ = 0;
    bool stopped_ = false; // drainAndClose 之后
    bool fatal_ = false;   // initialize 失败,拒绝一切写入
    quint64 droppedSamples_ = 0;
    quint64 droppedEvents_ = 0;
};

} // namespace dms::persistence
