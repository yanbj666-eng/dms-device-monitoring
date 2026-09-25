// tst_devicesession:S2 会话雏形测试
// FakeTransport 注入字节流:固定向量/半包/坏CRC/载荷拒绝/ACK/错误转发/日志集成
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "Frame.h"
#include "MessageCodec.h"
#include "domain/DeviceSession.h"

using dms::domain::DeviceSession;
using dms::domain::TelemetryBatch;
using dms::protocol::AckStatus;
using dms::protocol::CommandParam;
using dms::protocol::kValueTypeFloat32;
using dms::protocol::kValueTypeUint32;
using dms::protocol::MessageType;
using dms::protocol::TelemetrySample;

namespace {

// 测试替身:不发真字节,由测试直接喂 bytesReceived / 注入错误
class FakeTransport : public dms::transport::ITransport
{
public:
    using ITransport::ITransport;

    void open() override { emit opened(); }
    void close() override { emit closed(); }
    void write(const QByteArray &, quint64 requestId) override { emit writeAccepted(requestId); }
    bool isOpen() const override { return true; }

    void feed(const QByteArray &bytes) { emit bytesReceived(bytes); }
    void injectError(dms::transport::TransportError e) { emit errorOccurred(e); }
};

QByteArray telemetryFrame(quint32 seq, quint64 ts, const QList<TelemetrySample> &samples,
                          const QByteArray &extraTlv = {})
{
    return dms::protocol::encodeFrame(quint8(MessageType::Telemetry), seq, ts,
                                      dms::protocol::MessageCodec::encodeTelemetry(samples)
                                          + extraTlv);
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

} // namespace

class TestDeviceSession : public QObject
{
    Q_OBJECT

private slots:
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
    session.stop();
    QCOMPARE(closedSpy.size(), 1);
    QCOMPARE(session.transport(), transport); // S2 直通访问
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

QTEST_MAIN(TestDeviceSession)
#include "tst_devicesession.moc"
