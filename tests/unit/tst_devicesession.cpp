// tst_devicesession:S2 会话雏形 + S3 状态机/命令生命周期测试
// FakeTransport 注入字节流:固定向量/半包/坏CRC/载荷拒绝/ACK/错误转发/日志集成;
// S3 用可注入假时钟驱动超时/退避,不真实等待(LLD §3"所有时间间隔用可注入时钟")
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <memory>

#include "Frame.h"
#include "MessageCodec.h"
#include "domain/DeviceSession.h"

using dms::domain::CommandOutcome;
using dms::domain::CommandState;
using dms::domain::DeviceSession;
using dms::domain::SessionConfig;
using dms::domain::SessionState;
using dms::domain::TelemetryBatch;
using dms::protocol::AckStatus;
using dms::protocol::CommandParam;
using dms::protocol::kValueTypeFloat32;
using dms::protocol::kValueTypeUint32;
using dms::protocol::MessageType;
using dms::protocol::TelemetrySample;

namespace {

// 测试替身:不发真字节,由测试直接喂 bytesReceived / 注入错误;
// deferOpen 模拟"连接中",acceptWrites=false 模拟入队失败(无 writeAccepted)
class FakeTransport : public dms::transport::ITransport
{
public:
    using ITransport::ITransport;

    bool deferOpen = false;    // true: open() 不发 opened(停在 Connecting)
    bool acceptWrites = true;  // false: write() 不发 writeAccepted
    int openCount = 0;
    QList<QByteArray> written;

    void open() override
    {
        ++openCount;
        if (!deferOpen)
            emit opened();
    }
    void close() override { emit closed(); }
    void write(const QByteArray &bytes, quint64 requestId) override
    {
        written.append(bytes);
        if (acceptWrites)
            emit writeAccepted(requestId);
    }
    bool isOpen() const override { return true; }

    void feed(const QByteArray &bytes) { emit bytesReceived(bytes); }
    void injectError(dms::transport::TransportError e) { emit errorOccurred(e); }
    void completeOpen() { emit opened(); } // deferOpen 后异步补发"握手完成"
};

QByteArray telemetryFrame(quint32 seq, quint64 ts, const QList<TelemetrySample> &samples,
                          const QByteArray &extraTlv = {})
{
    return dms::protocol::encodeFrame(quint8(MessageType::Telemetry), seq, ts,
                                      dms::protocol::MessageCodec::encodeTelemetry(samples)
                                          + extraTlv);
}

QByteArray heartbeatFrame(quint32 seq, quint64 ts = 0)
{
    return dms::protocol::encodeFrame(quint8(MessageType::Heartbeat), seq, ts, {});
}

QByteArray ackFrame(quint32 seq, quint16 commandId, AckStatus status)
{
    return dms::protocol::encodeFrame(quint8(MessageType::CommandAck), seq, 0,
                                      dms::protocol::MessageCodec::encodeCommandAck(commandId,
                                                                                    status));
}

QList<TelemetrySample> fourSamples()
{
    return {
        {quint16(dms::protocol::MetricId::Temperature), kValueTypeFloat32, 25.5f, 0},
        {quint16(dms::protocol::MetricId::Pressure), kValueTypeFloat32, 101.3f, 0},
        {quint16(dms::protocol::MetricId::Voltage), kValueTypeFloat32, 12.0f, 0},
        {quint16(dms::protocol::MetricId::Rpm), kValueTypeUint32, 0.0f, 1500},
    };
}

// param_id=1:sample_period_ms(与模拟器/集成测试同口径)
CommandParam periodParam(quint32 ms)
{
    QByteArray raw(4, 0);
    qToBigEndian(ms, raw.data());
    return CommandParam{1, kValueTypeUint32, raw};
}

// 从 commandFinished spy 里取指定 seq 的结果;未终结返回 false
bool findOutcome(const QSignalSpy &spy, quint32 seq, CommandOutcome &out)
{
    for (const QList<QVariant> &args : spy) {
        if (args.at(0).value<CommandOutcome>().seq == seq) {
            out = args.at(0).value<CommandOutcome>();
            return true;
        }
    }
    return false;
}

// S3 脚手架:假时钟 + 确定性抖动(jitter=0,退避序列可精确断言)
struct Rig {
    qint64 now = 5000;
    FakeTransport *transport = nullptr; // 会话接管所有权,必须堆分配
    std::unique_ptr<DeviceSession> session;

    explicit Rig(SessionConfig config = SessionConfig(), int jitterPct = 0)
    {
        transport = new FakeTransport;
        session = std::make_unique<DeviceSession>(
            QStringLiteral("sim-tcp-01"), transport, config, [this] { return now; },
            [jitterPct] { return jitterPct; });
    }

    // 推进假时钟并驱动一轮超时检查
    void advance(qint64 ms) { now += ms; session->checkTimeouts(); }

    // start + 首条四指标遥测 → Online(首条有效消息路径)
    void goOnline(quint32 seq = 100)
    {
        session->start();
        QCOMPARE(session->state(), SessionState::Connecting);
        transport->feed(telemetryFrame(seq, quint64(now), fourSamples()));
        QCOMPARE(session->state(), SessionState::Online);
    }
};

} // namespace

class TestDeviceSession : public QObject
{
    Q_OBJECT

private slots:
    // ---- S2:解码与分发 ----
    void telemetryFixedVector();
    void telemetryFourMetricsAndUnknown();
    void heartbeatFixedVector();
    void halfPacketReassembly();
    void badCrcCountedAndRecovered();
    void badHeaderCounted();
    void payloadRejectedWholeFrame();
    void ackDecoded();
    void unknownTypeCounted();
    void transportErrorForwarded();
    void startStopLifecycle();
    void loggingIntegration();

    // ---- S3:状态机 ----
    void firstTelemetryGoesOnline();
    void heartbeatOnlyGraceThenDegraded();
    void heartbeatDoesNotRefreshMetrics();
    void offlineTimeoutReconnects();
    void degradedRecoversAfterThreeGoodFrames();
    void badCrcFiveFramesDegraded();
    void connectTimeoutReconnects();
    void peerCloseReconnects();
    void backoffSequenceAndCap();
    void jitterShiftsBackoff();
    void stableOnlineResetsBackoff();
    void configInvalidGoesFaultAndNoRetry();
    void frameAssemblyTimeoutDiscardsStaleHalf();
    void seqContinuesAcrossReconnect();
    void reconnectAttemptNotKilledByStaleOpenTimestamp();

    // ---- S3:命令生命周期 ----
    void commandRoundTripSucceeded();
    void commandRejectedStatus();
    void commandTimesOutAndLateAckIgnored();
    void commandSinglePendingSlot();
    void commandRejectedOutsideOnlineStates();
    void commandCancelledOnOffline();
    void commandCancelledOnManualStop();
    void commandFailedOnLocalWriteError();
    void commandFailedOnSendQueueFull();

    // ---- S6:协议健康信号 + 采集启停 ----
    void protocolHealthCountsBadAndGoodFrames();
    void protocolHealthSkipsPayloadRejectedAndBadHeader();
    void stopAckExemptsStaleFromHealthCheck();
    void startAckResumesStaleWindow();

private:
    QTemporaryDir tempRoot_;
    QString freshDir();
};

QString TestDeviceSession::freshDir()
{
    static int counter = 0;
    const QString sub = QDir(tempRoot_.path()).filePath(QString::number(++counter));
    QDir().mkpath(sub);
    return sub;
}

// 协议规范 §5 向量2:TELEMETRY temperature=25.0,Seq=2,Ts=1000
void TestDeviceSession::telemetryFixedVector()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::telemetryReceived);

    transport->feed(QByteArray::fromHex(
        "aa55010200000002000800000000000003e80001010441c800003fe22715"));

    QCOMPARE(spy.size(), 1);
    const TelemetryBatch batch = spy.first().at(0).value<TelemetryBatch>();
    QCOMPARE(batch.samples.size(), 1);
    QCOMPARE(batch.samples[0].metricId, quint16(dms::protocol::MetricId::Temperature));
    QCOMPARE(batch.samples[0].asFloat, 25.0f);
    QCOMPARE(batch.seq, 2u);
    QCOMPARE(batch.timestampMs, 1000ull);
    QCOMPARE(batch.unknownMetrics, 0u);
    QCOMPARE(session.framesReceived(), 1ull);
    QVERIFY(!session.sessionId().isNull()); // 每会话 UUID 已生成
}

// 四指标 + 一个结构合法的未知指标:known 交付,unknown 计数
void TestDeviceSession::telemetryFourMetricsAndUnknown()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::telemetryReceived);

    // 未知指标 999=0x03E7,float32,len4(encodeTelemetry 只编已知指标,手工拼)
    const QByteArray unknownTlv = QByteArray::fromHex("03e7010400000000");
    transport->feed(telemetryFrame(5, 12345, fourSamples(), unknownTlv));

    QCOMPARE(spy.size(), 1);
    const TelemetryBatch batch = spy.first().at(0).value<TelemetryBatch>();
    QCOMPARE(batch.samples.size(), 4);
    QCOMPARE(batch.unknownMetrics, 1u);
    QCOMPARE(batch.seq, 5u);
}

// 协议规范 §5 向量1:空 HEARTBEAT,Seq=1,Ts=0
void TestDeviceSession::heartbeatFixedVector()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::heartbeatReceived);

    transport->feed(QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf"));

    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(0).toUInt(), 1u);        // seq
    QCOMPARE(spy.first().at(1).toULongLong(), 0ull); // ts
}

// 半包:同一帧拆 10/5/15 三块喂入,只交付一次
void TestDeviceSession::halfPacketReassembly()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::telemetryReceived);

    const QByteArray frame = telemetryFrame(9, 777, fourSamples().mid(0, 1));
    transport->feed(frame.left(10));
    QCOMPARE(spy.size(), 0); // 未到齐不交付
    transport->feed(frame.mid(10, 5));
    QCOMPARE(spy.size(), 0);
    transport->feed(frame.mid(15));
    QCOMPARE(spy.size(), 1);
}

// 坏 CRC:计数、不交付;后续好帧照常恢复
void TestDeviceSession::badCrcCountedAndRecovered()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy hbSpy(&session, &DeviceSession::heartbeatReceived);

    QByteArray bad = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    bad[21] = char(uchar(bad[21]) ^ 0xFF); // 破坏 CRC 末字节
    transport->feed(bad);
    const QByteArray good = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    transport->feed(good);

    QCOMPARE(session.badCrcFrames(), 1u);
    QCOMPARE(session.framesReceived(), 1ull);
    QCOMPARE(hbSpy.size(), 1);
}

// 非法 Version:头非法计数(FRAME_LENGTH 口径)
void TestDeviceSession::badHeaderCounted()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);

    QByteArray bad = dms::protocol::encodeFrame(quint8(MessageType::Heartbeat), 7, 100, {});
    bad[2] = 0x02; // Version 篡为 0x02
    transport->feed(bad);

    QCOMPARE(session.badHeaderFrames(), 1u);
    QCOMPARE(session.framesReceived(), 0ull);
}

// NaN 遥测:载荷整帧拒绝,不交付部分指标(LLD §2)
void TestDeviceSession::payloadRejectedWholeFrame()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::telemetryReceived);

    QList<TelemetrySample> samples = fourSamples();
    samples[0].asFloat = std::numeric_limits<float>::quiet_NaN();
    transport->feed(telemetryFrame(11, 1000, samples));

    QCOMPARE(spy.size(), 0);
    QCOMPARE(session.payloadErrors(), 1u);
    QCOMPARE(session.framesReceived(), 0ull); // 未成功交付不计
}

// ACK:载荷 4B,seq 回显、commandId/status 正确解码
void TestDeviceSession::ackDecoded()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::commandAckReceived);

    const QByteArray frame = dms::protocol::encodeFrame(
        quint8(MessageType::CommandAck), 77, 5000,
        dms::protocol::MessageCodec::encodeCommandAck(4, AckStatus::Busy));
    transport->feed(frame);

    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(0).toUInt(), 77u);   // seq 回显
    QCOMPARE(spy.first().at(1).toUInt(), 4u);    // SetParameter
    QCOMPARE(spy.first().at(2).toUInt(), 3u);    // Busy
}

// 未知类型(CRC 正确):只计数不交付
void TestDeviceSession::unknownTypeCounted()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy hbSpy(&session, &DeviceSession::heartbeatReceived);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);

    transport->feed(dms::protocol::encodeFrame(0x20, 3, 100, {})); // v1 保留类型

    QCOMPARE(session.unknownTypeFrames(), 1u);
    QCOMPARE(session.framesReceived(), 0ull);
    QCOMPARE(hbSpy.size(), 0);
    QCOMPARE(telSpy.size(), 0);
}

// transport 错误:结构化转发,不在会话层吞掉
void TestDeviceSession::transportErrorForwarded()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::sessionError);

    transport->injectError({dms::ErrorCode::ConnectionLost, QStringLiteral("boom")});

    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(0).value<dms::ErrorCode>(), dms::ErrorCode::ConnectionLost);
    QCOMPARE(spy.first().at(1).toString(), QStringLiteral("boom"));
}

// start/stop:透传 open/close,生命周期信号转发
void TestDeviceSession::startStopLifecycle()
{
    // 会话按 parent-child 接管 transport 所有权,必须堆分配(栈对象会双重释放)
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy closedSpy(&session, &DeviceSession::sessionClosed);

    session.start();
    QCOMPARE(openedSpy.size(), 1);
    QCOMPARE(session.state(), SessionState::Connecting); // 打开后等首条有效消息
    session.stop();
    QCOMPARE(closedSpy.size(), 1);
    QCOMPARE(session.state(), SessionState::Disconnected);
}

// 日志集成:会话日志带 device_id + session_id + 错误码(LLD §8 字段)
void TestDeviceSession::loggingIntegration()
{
    const QString dir = freshDir();
    dms::Logger::Settings settings;
    settings.level = dms::LogLevel::Info;
    dms::initializeLogging(settings, dir);
    QUuid sessionId;
    {
        auto *transport = new FakeTransport; // 同上:parent-child 要求堆分配
        DeviceSession session(QStringLiteral("sim-tcp-07"), transport);
        sessionId = session.sessionId();
        QByteArray bad = dms::protocol::encodeFrame(quint8(MessageType::Heartbeat), 1, 0, {});
        bad[bad.size() - 1] = char(uchar(bad[bad.size() - 1]) ^ 0xFF);
        transport->feed(bad);
    }
    dms::shutdownLogging();

    QFile f(QDir(dir).filePath(QStringLiteral("dms.log")));
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    QVERIFY(content.contains(QStringLiteral("FRAME_CRC")));
    QVERIFY(content.contains(QStringLiteral("sim-tcp-07")));
    QVERIFY(content.contains(sessionId.toString(QUuid::WithoutBraces)));
}

// ---- S3:状态机 ----

// 首条有效消息 = 四指标遥测 → 直接 Online(LLD §3)
void TestDeviceSession::firstTelemetryGoesOnline()
{
    Rig rig;
    QSignalSpy stateSpy(rig.session.get(), &DeviceSession::stateChanged);

    rig.goOnline();

    QCOMPARE(stateSpy.size(), 2);
    QCOMPARE(stateSpy.at(0).at(0).value<SessionState>(), SessionState::Connecting);
    QCOMPARE(stateSpy.at(1).at(0).value<SessionState>(), SessionState::Online);
}

// 只有心跳:指标未到也"从打开时刻计时",短暂 Online 后降级(LLD §3;
// 未收到首个指标也从打开时刻计时 → 有宽限,超 3000ms 才 Degraded)
void TestDeviceSession::heartbeatOnlyGraceThenDegraded()
{
    Rig rig;
    rig.session->start();
    rig.transport->feed(heartbeatFrame(1));
    QCOMPARE(rig.session->state(), SessionState::Online); // 宽限期内指标视为未超时

    rig.advance(3100); // 无任何遥测 → 指标过期
    QCOMPARE(rig.session->state(), SessionState::Degraded);

    // 心跳持续刷存活:不失联,但也不能凭心跳回 Online(不刷新遥测时间)
    rig.transport->feed(heartbeatFrame(2));
    QCOMPARE(rig.session->state(), SessionState::Degraded);
    rig.advance(3100); // 距上次心跳 3100ms < 4000ms
    rig.transport->feed(heartbeatFrame(3));
    QCOMPARE(rig.session->state(), SessionState::Degraded);
}

// 心跳保活但不刷新遥测时间:指标 3000ms 无更新只降级,链路不失联(LLD §3)
void TestDeviceSession::heartbeatDoesNotRefreshMetrics()
{
    Rig rig;
    rig.goOnline(); // t=5000 指标刷新

    rig.advance(1500);
    rig.transport->feed(heartbeatFrame(1)); // t=6500 心跳
    rig.advance(1601);                      // t=8101:指标 3101ms 无更新,存活仅 1601ms

    QCOMPARE(rig.session->state(), SessionState::Degraded); // 降级不失联
}

// 4000ms 无有效接收 → Reconnecting(无 transport 错误,纯超时路径)
void TestDeviceSession::offlineTimeoutReconnects()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy stateSpy(rig.session.get(), &DeviceSession::stateChanged);
    QSignalSpy errSpy(rig.session.get(), &DeviceSession::sessionError);

    rig.advance(4100);

    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
    QCOMPARE(stateSpy.size(), 2); // Degraded(指标先过期)→ Reconnecting
    QCOMPARE(stateSpy.at(0).at(0).value<SessionState>(), SessionState::Degraded);
    QCOMPARE(stateSpy.at(1).at(0).value<SessionState>(), SessionState::Reconnecting);
    QCOMPARE(errSpy.size(), 0); // 超时路径不冒充 transport 错误
}

// Degraded → Online:四指标新鲜且连续 3 个有效帧(LLD §3)
void TestDeviceSession::degradedRecoversAfterThreeGoodFrames()
{
    Rig rig;
    rig.goOnline();
    rig.advance(3100); // 指标过期 → Degraded
    QCOMPARE(rig.session->state(), SessionState::Degraded);

    rig.transport->feed(telemetryFrame(101, quint64(rig.now), fourSamples()));
    QCOMPARE(rig.session->state(), SessionState::Degraded); // 第 1 帧不够
    rig.advance(10);
    rig.transport->feed(telemetryFrame(102, quint64(rig.now), fourSamples()));
    QCOMPARE(rig.session->state(), SessionState::Degraded); // 第 2 帧不够
    rig.advance(10);
    rig.transport->feed(telemetryFrame(103, quint64(rig.now), fourSamples()));
    QCOMPARE(rig.session->state(), SessionState::Online); // 第 3 帧恢复
}

// 连续 5 个 CRC 错 → Degraded(通信继续);计数被有效帧清零
void TestDeviceSession::badCrcFiveFramesDegraded()
{
    Rig rig;
    rig.goOnline();

    // 4 个坏帧 + 1 个好帧(清零)+ 4 个坏帧:始终不足 5 连,不降级
    for (int i = 0; i < 4; ++i) {
        QByteArray bad = heartbeatFrame(quint32(i + 1));
        bad[bad.size() - 1] = char(uchar(bad[bad.size() - 1]) ^ 0xFF);
        rig.transport->feed(bad);
    }
    rig.transport->feed(heartbeatFrame(9));
    QCOMPARE(rig.session->state(), SessionState::Online); // 计数已清零
    for (int i = 0; i < 4; ++i) {
        QByteArray bad = heartbeatFrame(quint32(i + 10));
        bad[bad.size() - 1] = char(uchar(bad[bad.size() - 1]) ^ 0xFF);
        rig.transport->feed(bad);
    }
    QCOMPARE(rig.session->state(), SessionState::Online);

    // 再来 1 个坏帧凑足 5 连 → Degraded
    QByteArray bad = heartbeatFrame(20);
    bad[bad.size() - 1] = char(uchar(bad[bad.size() - 1]) ^ 0xFF);
    rig.transport->feed(bad);
    QCOMPARE(rig.session->state(), SessionState::Degraded);
    QCOMPARE(rig.session->badCrcFrames(), 9u);
}

// 建链超时(transport 上报 CONNECT_TIMEOUT)→ Reconnecting + 退避后重试
void TestDeviceSession::connectTimeoutReconnects()
{
    Rig rig;
    rig.transport->deferOpen = true; // open() 不发 opened:停在 Connecting
    rig.session->start();
    QCOMPARE(rig.session->state(), SessionState::Connecting);

    rig.transport->injectError({dms::ErrorCode::ConnectTimeout, QStringLiteral("timed out")});
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);

    rig.advance(999); // 首档退避 1000ms
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
    rig.advance(1);
    QCOMPARE(rig.session->state(), SessionState::Connecting); // 重试点火
    QCOMPARE(rig.transport->openCount, 2);                    // 只有一个在途尝试
}

// 对端关闭(closed 而非 error):同样进入重连
void TestDeviceSession::peerCloseReconnects()
{
    Rig rig;
    rig.goOnline();

    rig.transport->close(); // 非主动拆链(closeByUs_ 为 false)

    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
}

// S4 冒烟发现:重连点火时若不清上一轮 openedAtMs_,异步建链(打开回调
// 未到)期间首个 poll tick 就拿陈旧时间戳误判"无首帧超时",把真实连接
// 错误(如连接拒绝)掩盖成 FRAME_TIMEOUT
void TestDeviceSession::reconnectAttemptNotKilledByStaleOpenTimestamp()
{
    Rig rig;
    rig.goOnline(); // 本轮 openedAtMs_ 已置位

    rig.advance(4100); // 失联 → Reconnecting(首档退避 1s)
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);

    rig.transport->deferOpen = true; // 模拟异步建链:open 后 opened 迟迟不来
    rig.advance(1000);               // 重试点火 → Connecting
    QCOMPARE(rig.session->state(), SessionState::Connecting);

    rig.advance(100); // 一个 poll tick:不得被陈旧时间戳误杀
    QCOMPARE(rig.session->state(), SessionState::Connecting);

    rig.transport->completeOpen(); // 握手完成,首帧窗口从现在起算
    rig.advance(3999);
    QCOMPARE(rig.session->state(), SessionState::Connecting);
    rig.advance(1); // 打开后 4000ms 无首帧:真实超时仍然生效
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
}

// 退避序列 1,2,4,8,10,10…s(LLD §3),jitter=0 精确断言
void TestDeviceSession::backoffSequenceAndCap()
{
    SessionConfig cfg;
    cfg.firstFrameTimeoutMs = 100; // 快速走完"打开但无首帧"循环
    Rig rig(cfg);
    rig.goOnline();
    rig.advance(4100); // → Reconnecting,首档退避排定
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);

    const qint64 expected[] = {1000, 2000, 4000, 8000, 10000, 10000};
    int expectedOpens = 1; // goOnline 的首次 open
    for (const qint64 delay : expected) {
        rig.advance(delay - 1);
        QCOMPARE(rig.session->state(), SessionState::Reconnecting); // 未到期不重试
        rig.advance(1);
        QCOMPARE(rig.session->state(), SessionState::Connecting); // 重试点火
        QCOMPARE(rig.transport->openCount, ++expectedOpens);
        rig.advance(cfg.firstFrameTimeoutMs); // 打开后无首帧 → 再次失联
        QCOMPARE(rig.session->state(), SessionState::Reconnecting);
    }
}

// ±10% 抖动落在边界内(LLD §3:抖动后限制在 0.1–10s)
void TestDeviceSession::jitterShiftsBackoff()
{
    SessionConfig cfg;
    cfg.firstFrameTimeoutMs = 100;

    Rig plus10(cfg, +10);
    plus10.goOnline();
    plus10.advance(4100);
    plus10.advance(1099); // 1000 * 1.1 - 1
    QCOMPARE(plus10.session->state(), SessionState::Reconnecting);
    plus10.advance(1);
    QCOMPARE(plus10.session->state(), SessionState::Connecting);

    Rig minus10(cfg, -10);
    minus10.goOnline();
    minus10.advance(4100);
    minus10.advance(899); // 1000 * 0.9 - 1
    QCOMPARE(minus10.session->state(), SessionState::Reconnecting);
    minus10.advance(1);
    QCOMPARE(minus10.session->state(), SessionState::Connecting);
}

// 稳定 Online 10s 才清零退避计数:下一次失联回到 1s 档(LLD §3)
void TestDeviceSession::stableOnlineResetsBackoff()
{
    Rig rig;
    rig.goOnline();
    rig.advance(4100); // 第一次失联:1s 档
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
    rig.advance(1000);
    QCOMPARE(rig.session->state(), SessionState::Connecting);
    rig.transport->feed(telemetryFrame(201, quint64(rig.now), fourSamples()));
    QCOMPARE(rig.session->state(), SessionState::Online);

    // 稳定 10s:期间持续喂帧,不触发失联
    for (int i = 0; i < 10; ++i) {
        rig.advance(1000);
        rig.transport->feed(telemetryFrame(quint32(300 + i), quint64(rig.now), fourSamples()));
    }

    rig.advance(4100); // 第二次失联
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
    rig.advance(999); // 若计数未清零,此档应为 2000ms
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
    rig.advance(1);
    QCOMPARE(rig.session->state(), SessionState::Connecting); // 1s 档:已清零
}

// 配置错误 → Fault:停止重试,人工 start 才重启(LLD §3)
void TestDeviceSession::configInvalidGoesFaultAndNoRetry()
{
    Rig rig;
    rig.goOnline();

    rig.transport->injectError({dms::ErrorCode::ConfigInvalid, QStringLiteral("bad config")});
    QCOMPARE(rig.session->state(), SessionState::Fault);

    rig.advance(100000); // Fault 不重试
    QCOMPARE(rig.session->state(), SessionState::Fault);
    QCOMPARE(rig.transport->openCount, 1);

    rig.session->start(); // 修正后人工重启
    QCOMPARE(rig.session->state(), SessionState::Connecting);
    QCOMPARE(rig.transport->openCount, 2);
}

// 组帧 2000ms 超时:滞留半包被清,补齐的后半不再拼出旧帧(LLD §2/§3)
void TestDeviceSession::frameAssemblyTimeoutDiscardsStaleHalf()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy telSpy(rig.session.get(), &DeviceSession::telemetryReceived);

    const QByteArray frame = telemetryFrame(400, 1, fourSamples());
    rig.transport->feed(frame.left(10)); // 半包滞留
    rig.advance(2000);                   // 组帧超时:缓存清空

    rig.transport->feed(frame.mid(10)); // 迟到的后半:前半已丢,拼不出帧
    QCOMPARE(telSpy.size(), 0);

    rig.transport->feed(frame); // 新帧完整到达,正常恢复
    QCOMPARE(telSpy.size(), 1);
}

// Seq 跨重连不复位:旧 ACK 无法关联新命令(LLD §4)
void TestDeviceSession::seqContinuesAcrossReconnect()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);

    QCOMPARE(rig.session->sendCommand(2), 1u);
    rig.advance(4100); // 失联前命令已超 2000ms:TimedOut 先终结
    CommandOutcome out;
    QVERIFY(findOutcome(finSpy, 1, out));
    QCOMPARE(out.state, CommandState::TimedOut);
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);

    rig.advance(1000); // 退避后重试
    QCOMPARE(rig.session->state(), SessionState::Connecting);
    rig.transport->feed(telemetryFrame(500, quint64(rig.now), fourSamples()));
    QCOMPARE(rig.session->state(), SessionState::Online);

    QCOMPARE(rig.session->sendCommand(2), 2u); // Seq 继续递增
}

// ---- S3:命令生命周期 ----

// 全链路:sendCommand 组帧入队 → ACK Success → Succeeded
void TestDeviceSession::commandRoundTripSucceeded()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy sentSpy(rig.session.get(), &DeviceSession::commandSent);
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);

    const quint32 seq = rig.session->sendCommand(4, {periodParam(200)});
    QCOMPARE(seq, 1u);
    QCOMPARE(sentSpy.size(), 1);
    QCOMPARE(sentSpy.first().at(0).toUInt(), 1u);
    QCOMPARE(sentSpy.first().at(1).toUInt(), 4u);
    QCOMPARE(rig.transport->written.size(), 1);

    // 写出去的帧回环解码:COMMAND/SetParameter/param1=200
    dms::protocol::FrameDecoder decoder;
    const auto r = decoder.decode(rig.transport->written.at(0));
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames.at(0).type, quint8(MessageType::Command));
    dms::protocol::CommandMessage msg;
    QCOMPARE(dms::protocol::MessageCodec::decodeCommand(r.frames.at(0).payload, msg),
             dms::protocol::DecodeError::None);
    QCOMPARE(msg.commandId, quint16(4));
    QCOMPARE(msg.params.size(), 1);
    QCOMPARE(msg.params.at(0).id, quint16(1));
    QCOMPARE(qFromBigEndian<quint32>(msg.params.at(0).value.constData()), 200u);

    rig.transport->feed(ackFrame(seq, 4, AckStatus::Success));

    QCOMPARE(finSpy.size(), 1);
    CommandOutcome out = finSpy.first().at(0).value<CommandOutcome>();
    QCOMPARE(out.seq, 1u);
    QCOMPARE(out.commandId, quint16(4));
    QCOMPARE(out.state, CommandState::Succeeded);
    QCOMPARE(out.ackStatus, quint16(AckStatus::Success));
    // 回归:ACK 终态也必须带会话 ID(S5 终态 UPDATE 按它定位行,曾因绕过 finishPending 漏打标)
    QCOMPARE(out.sessionId, rig.session->sessionId());
}

// ACK 非 Success → Rejected(设备拒绝,不是本地失败)
void TestDeviceSession::commandRejectedStatus()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);

    const quint32 seq = rig.session->sendCommand(4, {periodParam(50)}); // 越界值
    rig.transport->feed(ackFrame(seq, 4, AckStatus::BadParams));

    CommandOutcome out;
    QVERIFY(findOutcome(finSpy, seq, out));
    QCOMPARE(out.state, CommandState::Rejected);
    QCOMPARE(out.ackStatus, quint16(AckStatus::BadParams));
    QCOMPARE(out.sessionId, rig.session->sessionId()); // 同上:ACK 终态带会话 ID
}

// 2000ms 无 ACK → TimedOut;迟到 ACK 只记日志不改结果(LLD §4)
void TestDeviceSession::commandTimesOutAndLateAckIgnored()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);
    QSignalSpy ackSpy(rig.session.get(), &DeviceSession::commandAckReceived);

    const quint32 seq = rig.session->sendCommand(2);
    rig.advance(2000);

    CommandOutcome out;
    QVERIFY(findOutcome(finSpy, seq, out));
    QCOMPARE(out.state, CommandState::TimedOut);
    QVERIFY(out.detail.contains(QStringLiteral("unknown")));

    rig.transport->feed(ackFrame(seq, 2, AckStatus::Success)); // 迟到 ACK
    QCOMPARE(finSpy.size(), 1);       // 结果不翻转
    QCOMPARE(ackSpy.size(), 1);       // ACK 事件照常上报
}

// 每设备最多一个未决命令:第二个直接拒绝(LLD §4)
void TestDeviceSession::commandSinglePendingSlot()
{
    Rig rig;
    rig.goOnline();

    QCOMPARE(rig.session->sendCommand(2), 1u);
    QCOMPARE(rig.session->sendCommand(1), 0u); // 未决中,拒绝

    rig.advance(2000); // 首个超时终结后,槽位释放
    QCOMPARE(rig.session->sendCommand(1), 2u);
}

// Connecting/Disconnected 等状态下命令直接拒绝(LLD §4:仅 Online/Degraded)
void TestDeviceSession::commandRejectedOutsideOnlineStates()
{
    Rig rig;
    QCOMPARE(rig.session->sendCommand(2), 0u); // Disconnected

    rig.goOnline();
    rig.session->stop();
    QCOMPARE(rig.session->sendCommand(2), 0u); // 人工断开后
}

// 链路失联:未决命令 Cancelled(执行结果未知,不自动重发)。
// 可达路径:命令在失联线(4000ms)前不久才发出,ACK 超时(2000ms)未到
// 先被失联取消——静默开始早于发命令,命令年龄 < ACK 超时
void TestDeviceSession::commandCancelledOnOffline()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);

    rig.advance(3500); // 距最后有效帧 3500ms:已 Degraded,未失联
    QCOMPARE(rig.session->state(), SessionState::Degraded);
    rig.session->sendCommand(2); // Degraded 下允许发命令
    rig.advance(600);            // 4000ms 失联线到达,命令仅 600ms 大

    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
    CommandOutcome out;
    QVERIFY(findOutcome(finSpy, 1u, out));
    QCOMPARE(out.state, CommandState::Cancelled); // 取消而非超时
    QVERIFY(out.detail.contains(QStringLiteral("connection lost")));
}

// 人工断开:命令 Cancelled 且记录原因;之后不再自动重试
void TestDeviceSession::commandCancelledOnManualStop()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);

    rig.session->sendCommand(2);
    rig.session->stop();

    QCOMPARE(rig.session->state(), SessionState::Disconnected);
    CommandOutcome out;
    QVERIFY(findOutcome(finSpy, 1u, out));
    QCOMPARE(out.state, CommandState::Cancelled);
    QVERIFY(out.detail.contains(QStringLiteral("manual")));

    rig.advance(10000);
    QCOMPARE(rig.transport->openCount, 1); // 无任何自动重连
    QCOMPARE(rig.session->state(), SessionState::Disconnected);
}

// 本地发送失败(未入队即断链)→ Failed,同时状态机进重连
void TestDeviceSession::commandFailedOnLocalWriteError()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);

    rig.transport->acceptWrites = false; // write 不发 writeAccepted
    const quint32 seq = rig.session->sendCommand(2);
    rig.transport->injectError(
        {dms::ErrorCode::ConnectionLost,
         QStringLiteral("write on non-connected transport (request 1)")});

    CommandOutcome out;
    QVERIFY(findOutcome(finSpy, seq, out));
    QCOMPARE(out.state, CommandState::Failed); // 本地失败 ≠ 失联取消
    QCOMPARE(rig.session->state(), SessionState::Reconnecting);
}

// 发送队列满 → Failed,但链路仍健康:状态机不动(SEND_QUEUE_FULL 不是链路故障)
void TestDeviceSession::commandFailedOnSendQueueFull()
{
    Rig rig;
    rig.goOnline();
    QSignalSpy finSpy(rig.session.get(), &DeviceSession::commandFinished);

    rig.transport->acceptWrites = false;
    const quint32 seq = rig.session->sendCommand(2);
    rig.transport->injectError({dms::ErrorCode::SendQueueFull, QStringLiteral("full")});

    CommandOutcome out;
    QVERIFY(findOutcome(finSpy, seq, out));
    QCOMPARE(out.state, CommandState::Failed);
    QCOMPARE(rig.session->state(), SessionState::Online); // 不降级不失联
}

// ---- S6 ----

// 每解码块的坏/好帧快照:告警引擎 PROTOCOL_CRC 的唯一输入
void TestDeviceSession::protocolHealthCountsBadAndGoodFrames()
{
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::protocolHealth);

    QByteArray bad = heartbeatFrame(1);
    bad[bad.size() - 1] = char(uchar(bad[bad.size() - 1]) ^ 0xFF); // 破坏 CRC
    transport->feed(bad);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.last().at(0).toUInt(), 1u); // badCrc
    QCOMPARE(spy.last().at(1).toUInt(), 0u);

    transport->feed(heartbeatFrame(2));
    QCOMPARE(spy.size(), 2);
    QCOMPARE(spy.last().at(0).toUInt(), 0u);
    QCOMPARE(spy.last().at(1).toUInt(), 1u);

    // 同一块"坏+好+好":一次信号合并计数(告警引擎按块累积 streak)
    transport->feed(QByteArray(bad + heartbeatFrame(3) + heartbeatFrame(4)));
    QCOMPARE(spy.size(), 3);
    QCOMPARE(spy.last().at(0).toUInt(), 1u);
    QCOMPARE(spy.last().at(1).toUInt(), 2u);
}

// 载荷拒绝(NaN)与坏头不计入好帧:与 noteValidReceive 同口径,
// 它们不能解除连续坏 CRC(LLD §2/§6)
void TestDeviceSession::protocolHealthSkipsPayloadRejectedAndBadHeader()
{
    auto *transport = new FakeTransport;
    DeviceSession session(QStringLiteral("sim-tcp-01"), transport);
    QSignalSpy spy(&session, &DeviceSession::protocolHealth);

    // NaN temperature TLV:0001 01 04 7fc00000
    const QByteArray nanTlv = QByteArray::fromHex("000101047fc00000");
    transport->feed(telemetryFrame(1, 100, {}, nanTlv));
    QCOMPARE(spy.size(), 0); // 整帧拒绝,非有效帧

    QByteArray badVersion = heartbeatFrame(5);
    badVersion[2] = 0x02; // 头非法:不进坏 CRC 计数
    transport->feed(badVersion);
    QCOMPARE(spy.size(), 0);

    transport->feed(heartbeatFrame(6));
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.last().at(1).toUInt(), 1u);
}

// Stop 确认后:遥测长期缺席不再把 Online 拉成 Degraded(LLD §6 免除条件)
void TestDeviceSession::stopAckExemptsStaleFromHealthCheck()
{
    Rig rig;
    rig.goOnline();
    const quint32 seq = rig.session->sendCommand(dms::protocol::CommandId::Stop);
    QVERIFY(seq != 0);
    rig.transport->feed(ackFrame(seq, dms::protocol::CommandId::Stop, AckStatus::Success));

    // Stop 后只有心跳:指标年龄涨到 4000ms(超 metricTimeout)仍留 Online
    for (int i = 0; i < 4; ++i) {
        rig.advance(1000);
        rig.transport->feed(heartbeatFrame(200 + quint32(i), quint64(rig.now)));
    }
    QCOMPARE(rig.session->state(), SessionState::Online);
}

// Start 确认后陈旧窗口重新起算:窗口内 Online,到点恢复陈旧降级
void TestDeviceSession::startAckResumesStaleWindow()
{
    Rig rig;
    rig.goOnline();

    quint32 seq = rig.session->sendCommand(dms::protocol::CommandId::Stop);
    rig.transport->feed(ackFrame(seq, dms::protocol::CommandId::Stop, AckStatus::Success));
    for (int i = 0; i < 3; ++i) { // 暂停期:静默 3s 不降级
        rig.advance(1000);
        rig.transport->feed(heartbeatFrame(300 + quint32(i), quint64(rig.now)));
    }
    QCOMPARE(rig.session->state(), SessionState::Online);

    seq = rig.session->sendCommand(dms::protocol::CommandId::Start);
    rig.transport->feed(ackFrame(seq, dms::protocol::CommandId::Start, AckStatus::Success));

    rig.advance(2999); // Start 重置计时后窗口内
    rig.transport->feed(heartbeatFrame(400, quint64(rig.now)));
    QCOMPARE(rig.session->state(), SessionState::Online);

    rig.advance(1); // 恰 3000ms:陈旧检测恢复,降级
    QCOMPARE(rig.session->state(), SessionState::Degraded);
}

QTEST_MAIN(TestDeviceSession)
#include "tst_devicesession.moc"
