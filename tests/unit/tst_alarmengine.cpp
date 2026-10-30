#include <QtTest>

#include "domain/AlarmEngine.h"
#include "protocol/MessageCodec.h"

// AlarmEngine 测试(LLD §6,TC-ALARM-001..004 的规则面):
// 阈值/迟滞/去重/独立确认 + DATA_STALE + PROTOCOL_CRC + COMM_LOST 生命周期。
// 全部同线程直驱 + 假时钟,规则可脱离 GUI/DB 验证
using namespace dms::domain;
using dms::protocol::TelemetrySample;

namespace {

// 假 UTC 时钟:tick() 推进,事件打点与间隔判定共用它,断言可确定
class FakeClock
{
public:
    qint64 operator()() const { return nowMs_; }
    void advance(qint64 ms) { nowMs_ += ms; }
    qint64 nowMs_ = 1000000; // 非 0 起点:暴露"忘记初始化时间锚"类缺陷
};

struct AlarmSpy
{
    QList<AlarmEvent> events;
    int count(AlarmEvent::Kind kind, const char *code, quint16 metricId) const
    {
        int n = 0;
        for (const AlarmEvent &e : events)
            if (e.kind == kind && e.code == QLatin1String(code) && e.metricId == metricId)
                ++n;
        return n;
    }
    const AlarmEvent *last(AlarmEvent::Kind kind, const char *code,
                           quint16 metricId = 0) const
    {
        for (int i = events.size() - 1; i >= 0; --i)
            if (events[i].kind == kind && events[i].code == QLatin1String(code)
                && events[i].metricId == metricId)
                return &events[i];
        return nullptr;
    }
};

struct EngineHarness
{
    FakeClock clock;
    AlarmEngine::Settings settings; // 默认值:陈旧 3000ms、5 坏 CRC、3 好帧
    AlarmEngine engine;
    AlarmSpy spy;

    EngineHarness()
        : engine(AlarmEngine::Settings{},
                 [c = &clock]() -> qint64 { return (*c)(); })
    {
        QObject::connect(&engine, &AlarmEngine::alarmEvent,
                         [this](const AlarmEvent &e) { spy.events.append(e); });
    }

    void setTempRule(double low, double high, double hysteresis)
    {
        QList<AlarmRule> rules;
        rules.append(AlarmRule{1, low, high, hysteresis});
        engine.setRules(QStringLiteral("dev1"), rules);
    }

    void feed(float temp)
    {
        TelemetryBatch batch;
        TelemetrySample s;
        s.metricId = 1;
        s.valueType = dms::protocol::kValueTypeFloat32;
        s.asFloat = temp;
        batch.samples.append(s);
        engine.onTelemetry(QStringLiteral("dev1"), batch);
    }

    // 四指标全喂:陈旧恢复用(缺哪个指标哪个就永远陈旧)
    void feedAll()
    {
        TelemetryBatch batch;
        for (quint16 metricId = 1; metricId <= 4; ++metricId) {
            TelemetrySample s;
            s.metricId = metricId;
            s.valueType = dms::protocol::kValueTypeFloat32;
            s.asFloat = 25.0f;
            batch.samples.append(s);
        }
        engine.onTelemetry(QStringLiteral("dev1"), batch);
    }
};

} // namespace

class TstAlarmEngine : public QObject
{
    Q_OBJECT

private slots:
    // ---- TC-ALARM-001:阈值触发/等值/迟滞恢复 ----
    void threshold_triggersAboveHighAndBelowLow();
    void threshold_equalityAtBoundDoesNotTrigger();
    void threshold_persistentViolationEmitsOnce();
    void threshold_hysteresisHoldsUntilBandPassed();
    void threshold_noRulesNoAlarm();
    void threshold_uint32MetricComparesNumerically();
    void threshold_retriggerAfterRecoveryIsNewRecord();

    // ---- TC-ALARM-002:确认与恢复独立 ----
    void ack_marksAcknowledgedOnce();
    void ack_worksAfterCleared();
    void ack_targetsMostRecentRecord();
    void ack_unknownDeviceOrCodeIsIgnored();

    // ---- TC-ALARM-003:陈旧/失联/协议可区分 ----
    void stale_triggersAfterTimeoutAndClearsOnData();
    void stale_pausedByStopAckResumedByStartAck();
    void stale_retimesOnFreshConnectingRound();
    void crc_fiveConsecutiveBadTriggerThreeGoodClear();
    void crc_interleavedGoodFrameResetsStreak();
    void commLost_triggersOnReconnectingClearsOnRecovery();
    void commLost_manualDisconnectClosesActiveAlarms();
    // ---- 审查回归:DATA_STALE 只在链路已建立时评估 ----
    void stale_notReevaluatedAfterManualDisconnect();
    void stale_suppressedWhileLinkDown();
    void stale_countsFromLinkUpNotFromConnecting();
    void stale_resumedByResetAck();

    // ---- 混合 ----
    void telemetryFreshnessFeedsStaleDetection();
};

void TstAlarmEngine::threshold_triggersAboveHighAndBelowLow()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 2.0);

    h.feed(85.0);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdHigh, 1), 1);
    QCOMPARE(h.spy.events.last().level, quint8(AlarmLevel::Error));

    h.feed(5.0);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdLow, 1), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdHigh, 1), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ThresholdHigh, 1), 1);
}

void TstAlarmEngine::threshold_equalityAtBoundDoesNotTrigger()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 0.0);

    h.feed(80.0); // 等于上界:不触发
    h.feed(10.0); // 等于下界:不触发
    QCOMPARE(h.spy.events.size(), 0);
    QCOMPARE(h.engine.activeAlarmCount(), 0);
}

void TstAlarmEngine::threshold_persistentViolationEmitsOnce()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 2.0);

    for (int i = 0; i < 50; ++i)
        h.feed(95.0); // 10Hz 持续越界半分钟
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdHigh, 1), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ThresholdHigh, 1), 0);
    QCOMPARE(h.engine.activeAlarmCount(), 1);
}

void TstAlarmEngine::threshold_hysteresisHoldsUntilBandPassed()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 5.0);

    h.feed(90.0);
    h.feed(78.0); // 落入迟滞带 (75,80]:既不触发新高,也不恢复旧告警
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ThresholdHigh, 1), 0);
    QCOMPARE(h.engine.activeAlarmCount(), 1);

    h.feed(75.0); // 恰到 high-h:恢复含等值
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ThresholdHigh, 1), 1);
    const AlarmEvent *cleared =
        h.spy.last(AlarmEvent::Kind::Cleared, AlarmCode::ThresholdHigh, 1);
    QVERIFY(cleared);
    QCOMPARE(cleared->clearReason, QStringLiteral("recovered"));

    h.feed(78.0); // 带内不重触发
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdHigh, 1), 1);
}

void TstAlarmEngine::threshold_noRulesNoAlarm()
{
    EngineHarness h; // 不设规则
    h.feed(99999.0);
    QCOMPARE(h.spy.events.size(), 0);
}

void TstAlarmEngine::threshold_uint32MetricComparesNumerically()
{
    EngineHarness h;
    QList<AlarmRule> rules;
    rules.append(AlarmRule{4, 500.0, 3500.0, 0.0}); // 转速
    h.engine.setRules(QStringLiteral("dev1"), rules);

    TelemetryBatch batch;
    TelemetrySample s;
    s.metricId = 4;
    s.valueType = dms::protocol::kValueTypeUint32;
    s.asUint = 4000;
    batch.samples.append(s);
    h.engine.onTelemetry(QStringLiteral("dev1"), batch);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdHigh, 4), 1);
}

void TstAlarmEngine::threshold_retriggerAfterRecoveryIsNewRecord()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 0.0);

    h.feed(90.0);
    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::ThresholdHigh, 1);
    h.feed(70.0); // 恢复
    h.clock.advance(100);
    h.feed(90.0); // 再次越界:全新记录,确认状态归零

    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdHigh, 1), 2);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Acknowledged, AlarmCode::ThresholdHigh, 1), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ThresholdHigh, 1), 1);
    QCOMPARE(h.engine.activeAlarmCount(), 1);
    // 新记录未确认:可再次确认
    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::ThresholdHigh, 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Acknowledged, AlarmCode::ThresholdHigh, 1), 2);
}

void TstAlarmEngine::ack_marksAcknowledgedOnce()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 0.0);
    h.feed(90.0);

    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::ThresholdHigh, 1);
    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::ThresholdHigh, 1); // 幂等
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Acknowledged, AlarmCode::ThresholdHigh, 1), 1);
    const AlarmEvent *ack =
        h.spy.last(AlarmEvent::Kind::Acknowledged, AlarmCode::ThresholdHigh, 1);
    QVERIFY(ack);
    QCOMPARE(ack->atMs, h.clock.nowMs_); // 引擎裁决点打点
}

void TstAlarmEngine::ack_worksAfterCleared()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 0.0);
    h.feed(90.0);
    h.feed(70.0); // 先恢复

    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::ThresholdHigh, 1); // 后确认
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Acknowledged, AlarmCode::ThresholdHigh, 1), 1);
}

void TstAlarmEngine::ack_targetsMostRecentRecord()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 0.0);
    h.feed(90.0);
    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::ThresholdHigh, 1);
    h.feed(70.0);
    h.feed(90.0); // 第二次触发
    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::ThresholdHigh, 1);

    // 两次确认,分别落在各自记录的裁决时刻:确认针对"最近一条"
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Acknowledged, AlarmCode::ThresholdHigh, 1), 2);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ThresholdHigh, 1), 2);
}

void TstAlarmEngine::ack_unknownDeviceOrCodeIsIgnored()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 0.0);
    h.feed(90.0);

    h.engine.acknowledge(QStringLiteral("no-such-dev"), AlarmCode::ThresholdHigh, 1);
    h.engine.acknowledge(QStringLiteral("dev1"), "NOT_A_CODE", 1);
    h.engine.acknowledge(QStringLiteral("dev1"), AlarmCode::DataStale, 2); // 无该记录
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Acknowledged, AlarmCode::ThresholdHigh, 1), 0);
}

void TstAlarmEngine::stale_triggersAfterTimeoutAndClearsOnData()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting); // 建档+重置计时
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online); // 链路建立:起算陈旧

    h.clock.advance(2999);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);

    h.clock.advance(1); // 恰好 3000ms
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 2), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 3), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 4), 1);
    QCOMPARE(h.engine.activeAlarmCount(), 4);

    h.feedAll(); // 数据回来了,下轮 poll 恢复
    h.clock.advance(100);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::DataStale, 1), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::DataStale, 2), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::DataStale, 3), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::DataStale, 4), 1);
    QCOMPARE(h.engine.activeAlarmCount(), 0);
}

void TstAlarmEngine::stale_pausedByStopAckResumedByStartAck()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online); // 链路建立:起算陈旧

    CommandOutcome stop;
    stop.commandId = dms::protocol::CommandId::Stop;
    stop.state = CommandState::Succeeded;
    h.engine.onCommandFinished(QStringLiteral("dev1"), stop);

    h.clock.advance(10000); // Stop 已确认:静默多倍超时也不该有陈旧告警
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);

    CommandOutcome start;
    start.commandId = dms::protocol::CommandId::Start;
    start.state = CommandState::Succeeded;
    h.engine.onCommandFinished(QStringLiteral("dev1"), start); // 重新计时
    h.clock.advance(2999);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);
    h.clock.advance(1);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 1);
}

void TstAlarmEngine::stale_retimesOnFreshConnectingRound()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.clock.advance(2000);

    // 失联 → 重连:重试轮也重置陈旧计时,失联期间不叠加 DATA_STALE 噪声
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Reconnecting);
    h.clock.advance(2500);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.clock.advance(500); // 距建档 5000ms,但距重连 500ms
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::CommLost, 0), 1);

    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::CommLost, 0), 1);
}

void TstAlarmEngine::crc_fiveConsecutiveBadTriggerThreeGoodClear()
{
    EngineHarness h;

    for (int i = 0; i < 4; ++i)
        h.engine.onProtocolHealth(QStringLiteral("dev1"), 1, 0);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ProtocolCrc, 0), 0);

    h.engine.onProtocolHealth(QStringLiteral("dev1"), 1, 0); // 第 5 个
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ProtocolCrc, 0), 1);

    h.engine.onProtocolHealth(QStringLiteral("dev1"), 0, 2); // 2 个好帧不够
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ProtocolCrc, 0), 0);
    h.engine.onProtocolHealth(QStringLiteral("dev1"), 0, 1); // 第 3 个解除
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ProtocolCrc, 0), 1);
}

void TstAlarmEngine::crc_interleavedGoodFrameResetsStreak()
{
    EngineHarness h;
    for (int round = 0; round < 5; ++round) {
        h.engine.onProtocolHealth(QStringLiteral("dev1"), 3, 0); // 3 坏
        h.engine.onProtocolHealth(QStringLiteral("dev1"), 0, 1); // 1 好清零
    }
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::ProtocolCrc, 0), 0);
}

void TstAlarmEngine::commLost_triggersOnReconnectingClearsOnRecovery()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);

    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Reconnecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Reconnecting); // 重复进入
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::CommLost, 0), 1);

    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Degraded); // 恢复但降级
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::CommLost, 0), 1);
    const AlarmEvent *cleared = h.spy.last(AlarmEvent::Kind::Cleared, AlarmCode::CommLost);
    QVERIFY(cleared);
    QCOMPARE(cleared->clearReason, QStringLiteral("recovered"));
}

void TstAlarmEngine::commLost_manualDisconnectClosesActiveAlarms()
{
    EngineHarness h;
    h.setTempRule(10.0, 80.0, 0.0);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online); // 链路建立:起算陈旧
    h.feed(90.0); // 阈值告警活跃
    h.clock.advance(3000);
    h.engine.poll(); // 四指标陈旧告警活跃

    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Disconnected);
    QCOMPARE(h.engine.activeAlarmCount(), 0);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::ThresholdHigh, 1), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Cleared, AlarmCode::DataStale, 1), 1);
    const AlarmEvent *closed =
        h.spy.last(AlarmEvent::Kind::Cleared, AlarmCode::DataStale, 1);
    QVERIFY(closed);
    QCOMPARE(closed->clearReason, QStringLiteral("manual_disconnect"));
    // 人工断开不产生失联告警(LLD §3)
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::CommLost, 0), 0);
}

void TstAlarmEngine::telemetryFreshnessFeedsStaleDetection()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online); // 链路建立:起算陈旧
    h.clock.advance(2000);
    h.feed(25.0); // 只喂指标 1:其余指标陈旧,指标 1 新鲜
    h.clock.advance(1001);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 2), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 3), 1);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 4), 1);
}

// 人工断开关闭全部活跃告警后,陈旧检测不得在 3s 后又对已断开设备重新触发
void TstAlarmEngine::stale_notReevaluatedAfterManualDisconnect()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online);
    h.clock.advance(3000);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 1);

    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Disconnected);
    QCOMPARE(h.engine.activeAlarmCount(), 0);
    h.clock.advance(10000);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 1);
    QCOMPARE(h.engine.activeAlarmCount(), 0);
}

// 失联/Fault 期间链路问题由 COMM_LOST(或 Fault 状态)表达:退避 ≥4s 时
// 也不得叠加 DATA_STALE(重试轮之间没有 Connecting 重置计时)
void TstAlarmEngine::stale_suppressedWhileLinkDown()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online);
    h.feedAll();
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Reconnecting);
    h.clock.advance(8000); // 第 4 档退避
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::CommLost, 0), 1);

    h.engine.onSessionState(QStringLiteral("dev2"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev2"), SessionState::Online);
    h.engine.onSessionState(QStringLiteral("dev2"), SessionState::Fault);
    h.clock.advance(8000);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);
}

// 建链本身可耗时数秒(连接超时 3000ms):陈旧窗口从链路建立起算,
// 否则首条有效消息一到就因"Connecting 期间的等待"误报
void TstAlarmEngine::stale_countsFromLinkUpNotFromConnecting()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.clock.advance(2500);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online); // 首条是心跳
    h.clock.advance(600);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);
    h.clock.advance(2400); // 距链路建立 3000ms
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 1);
}

// 协议 §4.2:Reset ACK 后设备恢复默认周期并启动遥测——与 Start 同样恢复
// 陈旧检测并重新计时(TC-ALARM-004 Stop/Start/Reset)
void TstAlarmEngine::stale_resumedByResetAck()
{
    EngineHarness h;
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Connecting);
    h.engine.onSessionState(QStringLiteral("dev1"), SessionState::Online);

    CommandOutcome stop;
    stop.commandId = dms::protocol::CommandId::Stop;
    stop.state = CommandState::Succeeded;
    h.engine.onCommandFinished(QStringLiteral("dev1"), stop);
    h.clock.advance(5000);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);

    CommandOutcome reset;
    reset.commandId = dms::protocol::CommandId::Reset;
    reset.state = CommandState::Succeeded;
    h.engine.onCommandFinished(QStringLiteral("dev1"), reset);
    h.clock.advance(2999);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 0);
    h.clock.advance(1);
    h.engine.poll();
    QCOMPARE(h.spy.count(AlarmEvent::Kind::Triggered, AlarmCode::DataStale, 1), 1);
}

QTEST_MAIN(TstAlarmEngine)
#include "tst_alarmengine.moc"
