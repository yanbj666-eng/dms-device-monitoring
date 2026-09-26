#include "AlarmEngine.h"

#include <QDateTime>
#include <QTimer>

#include "common/Logging.h"
#include "protocol/MessageCodec.h"

namespace dms::domain {

namespace {

// 代码表:recordKey 里的 codeIdx 即此表下标,等级归属见 AlarmLevel 注释
enum CodeIdx : quint8 {
    IdxThresholdHigh = 0,
    IdxThresholdLow,
    IdxDataStale,
    IdxProtocolCrc,
    IdxCommLost,
    CodeCount
};

struct CodeInfo {
    const char *name;
    quint8 level;
};

constexpr CodeInfo kCodes[CodeCount] = {
    {AlarmCode::ThresholdHigh, AlarmLevel::Error},
    {AlarmCode::ThresholdLow, AlarmLevel::Error},
    {AlarmCode::DataStale, AlarmLevel::Warn},
    {AlarmCode::ProtocolCrc, AlarmLevel::Warn},
    {AlarmCode::CommLost, AlarmLevel::Error},
};

// 遥测 uint32/float32 统一取数值:double 精确覆盖 float32 与 uint32 全值域
double sampleValue(const dms::protocol::TelemetrySample &sample)
{
    return sample.valueType == dms::protocol::kValueTypeUint32
               ? double(sample.asUint)
               : double(sample.asFloat);
}

// 告警消息里的指标名(协议 §4.1);UI/日志共用同一措辞
QString metricName(quint16 metricId)
{
    switch (metricId) {
    case 1: return QStringLiteral("温度");
    case 2: return QStringLiteral("压力");
    case 3: return QStringLiteral("电压");
    case 4: return QStringLiteral("转速");
    case 0: return QStringLiteral("设备级");
    }
    return QStringLiteral("指标%1").arg(metricId);
}

QString numberText(double value)
{
    return QString::number(value, 'f', 2);
}

dms::LogContext logCtx(const QString &deviceId, dms::ErrorCode code = dms::ErrorCode::None)
{
    dms::LogContext ctx;
    ctx.deviceId = deviceId;
    ctx.code = code;
    return ctx;
}

} // namespace

AlarmEngine::AlarmEngine(Settings settings, Clock clock, QObject *parent)
    : QObject(parent),
      settings_(settings),
      clock_(std::move(clock))
{
    // queued 连接(UI/DB 跨线程消费)需要值对象已注册
    qRegisterMetaType<dms::domain::AlarmEvent>("dms::domain::AlarmEvent");
}

qint64 AlarmEngine::defaultClock()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void AlarmEngine::setRules(const QString &deviceId, const QList<AlarmRule> &rules)
{
    DeviceState &st = deviceState(deviceId);
    st.rules.clear();
    for (const AlarmRule &rule : rules)
        st.rules.insert(rule.metricId, rule);
}

int AlarmEngine::activeAlarmCount() const
{
    int count = 0;
    for (const DeviceState &st : devices_)
        for (const Record &rec : st.records)
            if (rec.active)
                ++count;
    return count;
}

void AlarmEngine::start()
{
    if (!pollTimer_) {
        // start() 总在所属线程被调(ioThread started 槽),timer 与引擎同线程
        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(settings_.pollIntervalMs);
        connect(pollTimer_, &QTimer::timeout, this, &AlarmEngine::poll);
    }
    pollTimer_->start();
}

void AlarmEngine::onTelemetry(QString deviceId, dms::domain::TelemetryBatch batch)
{
    DeviceState &st = deviceState(deviceId);
    const qint64 now = clock_();
    for (const dms::protocol::TelemetrySample &sample : batch.samples) {
        if (sample.metricId < 1 || sample.metricId > 4)
            continue; // 契约外指标不参与陈旧与阈值
        st.metricSeenMs[sample.metricId - 1] = now;
        evaluateThresholds(st, deviceId, sample.metricId, sampleValue(sample), now);
    }
}

void AlarmEngine::onSessionState(QString deviceId, dms::domain::SessionState state)
{
    DeviceState &st = deviceState(deviceId);
    const qint64 now = clock_();
    switch (state) {
    case SessionState::Connecting:
        // 每一轮连接(含失联后的重试)都重置陈旧计时:与会话层
        // onTransportOpened 重置 metricLastMs_ 同拍。失联期间不叠加
        // DATA_STALE 噪声,链路问题由 COMM_LOST 单独表达(TC-ALARM-003)
        retimeMetrics(st, now);
        break;
    case SessionState::Online:
    case SessionState::Degraded:
        // 有效通信恢复(首条有效消息后只可能是这两个态)→ 解除 COMM_LOST
        clear(st, deviceId, IdxCommLost, 0, AlarmCode::CommLost,
              QStringLiteral("recovered"), now);
        break;
    case SessionState::Reconnecting:
        // 失联:重复进入不重复告警(活跃期间 dedup)
        trigger(st, deviceId, IdxCommLost, 0, AlarmCode::CommLost, AlarmLevel::Error,
                QStringLiteral("无有效接收,通信失联"), now);
        break;
    case SessionState::Disconnected:
        // 人工断开/停用:关闭该设备全部活跃告警,不新建失联告警(LLD §3/§6)。
        // 采集标志回到默认:新一轮(新会话实例)从"采集活跃"起步
        for (auto it = st.records.begin(); it != st.records.end(); ++it) {
            if (!it->active)
                continue;
            const quint8 codeIdx = quint8(it.key() / 8);
            const quint16 metricId = quint16(it.key() % 8);
            clear(st, deviceId, codeIdx, metricId,
                  QString::fromLatin1(kCodes[codeIdx].name),
                  QStringLiteral("manual_disconnect"), now);
        }
        st.acquisitionActive = true;
        break;
    case SessionState::Fault:
        break; // 本地不可恢复错误:告警动作未定义,保持现状
    }
}

void AlarmEngine::onProtocolHealth(QString deviceId, quint32 badCrcFrames,
                                   quint32 goodFrames)
{
    DeviceState &st = deviceState(deviceId);
    const qint64 now = clock_();
    if (badCrcFrames > 0) {
        // 坏帧先计(与会话自身 streak 更新顺序一致):连续计数累加,
        // 一个坏帧即打断"连续有效帧"的解除计数
        st.crcGoodStreak = 0;
        st.crcBadStreak += int(badCrcFrames);
        if (st.crcBadStreak >= settings_.badCrcTrigger)
            trigger(st, deviceId, IdxProtocolCrc, 0, AlarmCode::ProtocolCrc,
                    AlarmLevel::Warn,
                    QStringLiteral("连续 %1 个 CRC 错误帧")
                        .arg(st.crcBadStreak),
                    now);
    }
    if (goodFrames > 0) {
        st.crcBadStreak = 0;
        st.crcGoodStreak += int(goodFrames);
        if (st.crcGoodStreak >= settings_.goodFramesToClearCrc)
            clear(st, deviceId, IdxProtocolCrc, 0, AlarmCode::ProtocolCrc,
                  QStringLiteral("recovered"), now);
    }
}

void AlarmEngine::onCommandFinished(QString deviceId, dms::domain::CommandOutcome outcome)
{
    // 只有设备确认执行(ACK Success)才改变采集状态;Rejected/超时结果未知,
    // 不臆断设备已停/已启(LLD §6"Stop 已 ACK 后")
    if (outcome.state != CommandState::Succeeded)
        return;
    if (outcome.commandId != dms::protocol::CommandId::Stop
        && outcome.commandId != dms::protocol::CommandId::Start)
        return;

    DeviceState &st = deviceState(deviceId);
    if (outcome.commandId == dms::protocol::CommandId::Stop) {
        st.acquisitionActive = false;
    } else {
        st.acquisitionActive = true;
        retimeMetrics(st, clock_()); // Start 后重新计时(LLD §6)
    }
}

void AlarmEngine::acknowledge(QString deviceId, QString code, quint16 metricId)
{
    // 最近一条记录(活跃或已恢复)才可确认:与 DB 侧 UPDATE ... ORDER BY id
    // DESC LIMIT 1 同语义,保证两边对"确认了哪条"理解一致
    const auto it = devices_.find(deviceId);
    if (it == devices_.end()) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App,
                        logCtx(deviceId),
                        QStringLiteral("告警确认未命中:未知设备 code=%1").arg(code));
        return;
    }
    quint8 codeIdx = CodeCount;
    for (quint8 i = 0; i < CodeCount; ++i) {
        if (code == QLatin1String(kCodes[i].name)) {
            codeIdx = i;
            break;
        }
    }
    if (codeIdx == CodeCount)
        return; // 未知代码:静默丢弃比半确认更安全
    Record *rec = findRecord(it.value(), codeIdx, metricId);
    if (!rec || rec->acknowledged)
        return; // 无记录或已确认:幂等,不重复发事件

    rec->acknowledged = true;
    AlarmEvent event = rec->triggered;
    event.kind = AlarmEvent::Kind::Acknowledged;
    event.clearReason.clear();
    event.atMs = clock_();
    emit alarmEvent(event);
}

void AlarmEngine::poll()
{
    const qint64 now = clock_();
    for (auto deviceIt = devices_.begin(); deviceIt != devices_.end(); ++deviceIt) {
        const QString deviceId = deviceIt.key();
        DeviceState &st = deviceIt.value();
        // Stop 已 ACK 后暂停整个陈旧检测:不触发新告警,也不清已有的
        // (暂停不等于数据变新鲜;Start ACK 后重置计时,下轮 poll 自然恢复)
        if (!st.acquisitionActive)
            continue;
        for (quint16 metricId = 1; metricId <= 4; ++metricId) {
            const bool stale = now - st.metricSeenMs[metricId - 1]
                               >= settings_.staleTimeoutMs;
            if (stale)
                trigger(st, deviceId, IdxDataStale, metricId, AlarmCode::DataStale,
                        AlarmLevel::Warn,
                        QStringLiteral("%1 超过 %2ms 无有效值")
                            .arg(metricName(metricId))
                            .arg(settings_.staleTimeoutMs),
                        now);
            else
                clear(st, deviceId, IdxDataStale, metricId, AlarmCode::DataStale,
                      QStringLiteral("recovered"), now);
        }
    }
}

AlarmEngine::DeviceState &AlarmEngine::deviceState(const QString &deviceId)
{
    // 首见即建档:metricSeenMs=0,若该设备始终无遥测,3s 后如实判陈旧
    // (会话层同样从打开时刻起算"未收到首个指标也从打开时刻计时")
    return devices_[deviceId];
}

AlarmEngine::Record *AlarmEngine::findRecord(DeviceState &st, quint8 codeIdx,
                                             quint16 metricId)
{
    const quint32 key = recordKey(codeIdx, metricId);
    const auto it = st.records.find(key);
    return it == st.records.end() ? nullptr : &it.value();
}

void AlarmEngine::evaluateThresholds(DeviceState &st, const QString &deviceId,
                                     quint16 metricId, double value, qint64 now)
{
    const auto ruleIt = st.rules.constFind(metricId);
    if (ruleIt == st.rules.constEnd())
        return; // 未配置阈值的指标不触发阈值告警(LLD §6)
    const AlarmRule &rule = ruleIt.value();
    const QString name = metricName(metricId);

    // 高值方向:严格大于才触发(等值不触发);恢复含等值(≤ high-h)。
    // 迟滞带 (high-h, high] 内不触发也不恢复,防边界抖动反复开合
    if (value > rule.high)
        trigger(st, deviceId, IdxThresholdHigh, metricId, AlarmCode::ThresholdHigh,
                AlarmLevel::Error,
                QStringLiteral("%1 %2 高于上界 %3")
                    .arg(name, numberText(value), numberText(rule.high)),
                now);
    else {
        const Record *rec = findRecord(st, IdxThresholdHigh, metricId);
        if (rec && rec->active && value <= rule.high - rule.hysteresis)
            clear(st, deviceId, IdxThresholdHigh, metricId, AlarmCode::ThresholdHigh,
                  QStringLiteral("recovered"), now);
    }

    // 低值方向独立评估:值可从高界直接摆到低界,两个方向同帧各判各的
    if (value < rule.low)
        trigger(st, deviceId, IdxThresholdLow, metricId, AlarmCode::ThresholdLow,
                AlarmLevel::Error,
                QStringLiteral("%1 %2 低于下界 %3")
                    .arg(name, numberText(value), numberText(rule.low)),
                now);
    else {
        const Record *rec = findRecord(st, IdxThresholdLow, metricId);
        if (rec && rec->active && value >= rule.low + rule.hysteresis)
            clear(st, deviceId, IdxThresholdLow, metricId, AlarmCode::ThresholdLow,
                  QStringLiteral("recovered"), now);
    }
}

void AlarmEngine::trigger(DeviceState &st, const QString &deviceId, quint8 codeIdx,
                          quint16 metricId, const QString &code, quint8 level,
                          const QString &message, qint64 now)
{
    const quint32 key = recordKey(codeIdx, metricId);
    Record &rec = st.records[key]; // 不存在则新建
    if (rec.active)
        return; // 持续异常不重复:活跃期间逐帧调用到此为止(LLD §6)

    // 再次越界(此前已恢复)不复用旧记录:新事件新状态,历史保留
    rec.triggered = AlarmEvent{};
    rec.triggered.kind = AlarmEvent::Kind::Triggered;
    rec.triggered.deviceId = deviceId;
    rec.triggered.code = code;
    rec.triggered.metricId = metricId;
    rec.triggered.level = level;
    rec.triggered.message = message;
    rec.triggered.atMs = now;
    rec.active = true;
    rec.acknowledged = false;

    emit alarmEvent(rec.triggered);
    dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, logCtx(deviceId),
                    QStringLiteral("告警触发 %1 [%2] %3")
                        .arg(code)
                        .arg(metricId)
                        .arg(message));
}

void AlarmEngine::clear(DeviceState &st, const QString &deviceId, quint8 codeIdx,
                        quint16 metricId, const QString &code, const QString &reason,
                        qint64 now)
{
    Record *rec = findRecord(st, codeIdx, metricId);
    if (!rec || !rec->active)
        return;

    rec->active = false; // 记录留在"最近一条"槽位:先恢复仍可确认(LLD §6)
    AlarmEvent event = rec->triggered;
    event.kind = AlarmEvent::Kind::Cleared;
    event.clearReason = reason;
    event.atMs = now;
    emit alarmEvent(event);
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(deviceId),
                    QStringLiteral("告警恢复 %1 [%2] 原因 %3")
                        .arg(code)
                        .arg(metricId)
                        .arg(reason));
}

void AlarmEngine::retimeMetrics(DeviceState &st, qint64 now)
{
    for (qint64 &seen : st.metricSeenMs)
        seen = now;
}

} // namespace dms::domain
