#include <QtTest>

#include "domain/DeviceManager.h"
#include "protocol/Frame.h"
#include "protocol/MessageCodec.h"

// DeviceManager 测试(LLD §5):装配/替换/统一启停/命令路由/信号转发。
// 全部同线程直驱 + FakeTransport,跨线程语义由 main 装配(HLD §2)
using namespace dms::domain;
using dms::protocol::AckStatus;
using dms::protocol::CommandParam;
using dms::protocol::MessageType;
using dms::protocol::TelemetrySample;

namespace {

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
};

QByteArray telemetryFrame(quint32 seq, quint64 ts, const QList<TelemetrySample> &samples)
{
    return dms::protocol::encodeFrame(quint8(MessageType::Telemetry), seq, ts,
                                      dms::protocol::MessageCodec::encodeTelemetry(samples));
}

QByteArray heartbeatFrame(quint32 seq)
{
    return dms::protocol::encodeFrame(quint8(MessageType::Heartbeat), seq, 0, {});
}

QByteArray ackFrame(quint32 seq, quint16 commandId, AckStatus status)
{
    return dms::protocol::encodeFrame(quint8(MessageType::CommandAck), seq, 0,
                                      dms::protocol::MessageCodec::encodeCommandAck(commandId,
                                                                                    status));
}

QList<TelemetrySample> fourMetrics()
{
    return {{1, dms::protocol::kValueTypeFloat32, 25.5f, 0},
            {2, dms::protocol::kValueTypeFloat32, 101.3f, 0},
            {3, dms::protocol::kValueTypeFloat32, 12.0f, 0},
            {4, dms::protocol::kValueTypeUint32, 0, 1500}};
}

struct Captured
{
    QStringList stateOrder; // "id:state"
    int telemetryCount = 0;
    QString lastTelemetryId;
    int telemetrySampleCount = 0;
    QString lastCommandFinishedId;
    dms::domain::CommandOutcome lastOutcome;
    QString lastRejectedId;
    QString lastRejectedReason;
    QString lastErrorId;
    dms::ErrorCode lastErrorCode = dms::ErrorCode::None;
    int managerStoppedCount = 0;
    QString lastHealthId; // S6:deviceProtocolHealth 转发
    quint32 lastHealthBad = 0;
    quint32 lastHealthGood = 0;
    int healthCount = 0;
};

void captureAll(DeviceManager *manager, Captured *out)
{
    QObject::connect(manager, &DeviceManager::deviceStateChanged,
                     [out](const QString &id, dms::domain::SessionState state) {
                         out->stateOrder.append(
                             id + QStringLiteral(":") + sessionStateName(state));
                     });
    QObject::connect(manager, &DeviceManager::deviceTelemetry,
                     [out](const QString &id, dms::domain::TelemetryBatch batch) {
                         out->telemetryCount++;
                         out->lastTelemetryId = id;
                         out->telemetrySampleCount = batch.samples.size();
                     });
    QObject::connect(manager, &DeviceManager::deviceCommandFinished,
                     [out](const QString &id, dms::domain::CommandOutcome outcome) {
                         out->lastCommandFinishedId = id;
                         out->lastOutcome = outcome;
                     });
    QObject::connect(manager, &DeviceManager::commandRejected,
                     [out](const QString &id, quint16, const QString &reason) {
                         out->lastRejectedId = id;
                         out->lastRejectedReason = reason;
                     });
    QObject::connect(manager, &DeviceManager::deviceError,
                     [out](const QString &id, dms::ErrorCode code, const QString &) {
                         out->lastErrorId = id;
                         out->lastErrorCode = code;
                     });
    QObject::connect(manager, &DeviceManager::managerStopped,
                     [out]() { out->managerStoppedCount++; });
    QObject::connect(manager, &DeviceManager::deviceProtocolHealth,
                     [out](const QString &id, quint32 bad, quint32 good) {
                         out->healthCount++;
                         out->lastHealthId = id;
                         out->lastHealthBad = bad;
                         out->lastHealthGood = good;
                     });
}

DeviceSetup setupFor(const QString &id)
{
    DeviceSetup setup;
    setup.deviceId = id;
    // 默认工厂会真连 127.0.0.1:9001(失败即走退避,测试注入 FakeTransport
    // 时无副作用);variant 活化 tcp 分支,与生产装配同构
    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = id;
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = 9001;
    setup.transport = cfg;
    return setup;
}

// UDP/Serial 分支的 setup(variant 活化 + transportKind 填充,装配同构)
DeviceSetup udpSetupFor(const QString &id, quint16 bindPort, quint16 peerPort)
{
    DeviceSetup setup;
    setup.deviceId = id;
    dms::transport::UdpTransportConfig cfg;
    cfg.deviceId = id;
    cfg.bindAddress = QHostAddress(QHostAddress::LocalHost);
    cfg.bindPort = bindPort;
    cfg.peerAddress = QHostAddress(QHostAddress::LocalHost);
    cfg.peerPort = peerPort;
    setup.transport = cfg;
    setup.session.transportKind = dms::transport::TransportKind::Udp;
    return setup;
}

DeviceSetup serialSetupFor(const QString &id, const QString &portName)
{
    DeviceSetup setup;
    setup.deviceId = id;
    dms::transport::SerialTransportConfig cfg;
    cfg.deviceId = id;
    cfg.portName = portName;
    setup.transport = cfg;
    setup.session.transportKind = dms::transport::TransportKind::Serial;
    return setup;
}

} // namespace

class TestDeviceManager : public QObject
{
    Q_OBJECT

private slots:
    void applySetupsCreatesAndStarts();
    void connectOnStartFalseSkipsStart();
    void telemetryForwardedWithDeviceId();
    void sendCommandRoutesAndFinishes();
    void sendCommandUnknownDeviceRejected();
    void sendCommandRejectedWhenOffline();
    void applySetupsReplacesSessions();
    void stopAllStopsEverything();
    void startStopDeviceById();
    void errorForwardedWithDeviceId();
    void protocolHealthForwardedWithDeviceId();
};

void TestDeviceManager::applySetupsCreatesAndStarts()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);

    manager.applySetups({setupFor(QStringLiteral("d1")), setupFor(QStringLiteral("d2"))});

    QCOMPARE(manager.deviceCount(), 2);
    QCOMPARE(transports.size(), 2);
    QCOMPARE(transports.value(QStringLiteral("d1"))->openCount, 1);
    // connectOnStart=true → start() → Connecting
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d1:Connecting")));
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d2:Connecting")));
}

void TestDeviceManager::connectOnStartFalseSkipsStart()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);

    DeviceSetup setup = setupFor(QStringLiteral("d1"));
    setup.connectOnStart = false;
    manager.applySetups({setup});

    QCOMPARE(manager.deviceCount(), 1);
    QCOMPARE(transports.value(QStringLiteral("d1"))->openCount, 0); // 未 start
    QVERIFY(captured.stateOrder.isEmpty());

    manager.startAll();
    QCOMPARE(transports.value(QStringLiteral("d1"))->openCount, 1);
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d1:Connecting")));
}

void TestDeviceManager::telemetryForwardedWithDeviceId()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);
    manager.applySetups({setupFor(QStringLiteral("d1")), setupFor(QStringLiteral("d2"))});

    // 只喂 d1:转发必须带对 id,且不串到 d2
    transports.value(QStringLiteral("d1"))->feed(
        telemetryFrame(1, 1000, fourMetrics()));

    QCOMPARE(captured.telemetryCount, 1);
    QCOMPARE(captured.lastTelemetryId, QStringLiteral("d1"));
    QCOMPARE(captured.telemetrySampleCount, 4);
    // 首条有效遥测 → Online
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d1:Online")));
    QVERIFY(!captured.stateOrder.contains(QStringLiteral("d2:Online")));
}

// S6:protocolHealth 转发带 deviceId,坏/好计数不改(告警引擎同线程直连)
void TestDeviceManager::protocolHealthForwardedWithDeviceId()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);
    manager.applySetups({setupFor(QStringLiteral("d1")), setupFor(QStringLiteral("d2"))});

    QByteArray bad = heartbeatFrame(1); // 心跳帧 + CRC 翻转
    bad[bad.size() - 1] = char(uchar(bad[bad.size() - 1]) ^ 0xFF);
    transports.value(QStringLiteral("d2"))->feed(bad);
    transports.value(QStringLiteral("d1"))->feed(heartbeatFrame(2));

    QCOMPARE(captured.healthCount, 2);
    QCOMPARE(captured.lastHealthId, QStringLiteral("d1"));
    QCOMPARE(captured.lastHealthBad, 0u);
    QCOMPARE(captured.lastHealthGood, 1u);
}

void TestDeviceManager::sendCommandRoutesAndFinishes()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);
    manager.applySetups({setupFor(QStringLiteral("d1"))});
    FakeTransport *transport = transports.value(QStringLiteral("d1"));
    transport->feed(telemetryFrame(1, 1000, fourMetrics())); // → Online

    quint32 reportedSeq = 0;
    QObject::connect(&manager, &DeviceManager::deviceCommandSent,
                     [&reportedSeq](const QString &, quint32 seq, quint16) {
                         reportedSeq = seq;
                     });

    manager.sendCommand(QStringLiteral("d1"), 3); // Reset

    QCOMPARE(transport->written.size(), 1); // 路由到了 d1 的 transport
    QVERIFY(reportedSeq != 0);
    QCOMPARE(captured.lastRejectedReason, QString()); // 未拒绝

    transport->feed(ackFrame(reportedSeq, 3, AckStatus::Success));

    QCOMPARE(captured.lastCommandFinishedId, QStringLiteral("d1"));
    QCOMPARE(captured.lastOutcome.state, dms::domain::CommandState::Succeeded);
    QCOMPARE(captured.lastOutcome.commandId, quint16(3));
}

void TestDeviceManager::sendCommandUnknownDeviceRejected()
{
    DeviceManager manager;
    Captured captured;
    captureAll(&manager, &captured);

    manager.sendCommand(QStringLiteral("nobody"), 3);

    QCOMPARE(captured.lastRejectedId, QStringLiteral("nobody"));
    QVERIFY(captured.lastRejectedReason.contains(QStringLiteral("不存在")));
}

void TestDeviceManager::sendCommandRejectedWhenOffline()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);
    DeviceSetup setup = setupFor(QStringLiteral("d1"));
    setup.connectOnStart = false; // 停在 Disconnected
    manager.applySetups({setup});

    manager.sendCommand(QStringLiteral("d1"), 3);

    QCOMPARE(captured.lastRejectedId, QStringLiteral("d1"));
    QVERIFY(captured.lastRejectedReason.contains(QStringLiteral("会话")));
}

void TestDeviceManager::applySetupsReplacesSessions()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    int factoryCalls = 0;
    manager.setTransportFactory([&transports, &factoryCalls](const DeviceSetup &setup) {
        ++factoryCalls;
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);
    manager.applySetups({setupFor(QStringLiteral("d1")), setupFor(QStringLiteral("d2"))});

    // 全量替换:旧会话先停(Disconnected),再建新
    manager.applySetups({setupFor(QStringLiteral("d3"))});

    QCOMPARE(factoryCalls, 3);
    QCOMPARE(manager.deviceCount(), 1);
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d1:Disconnected")));
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d3:Connecting")));
    // 旧 transport 不再被使用:喂数据无转发
    const int telemetryBefore = captured.telemetryCount;
    transports.value(QStringLiteral("d1"))->feed(telemetryFrame(9, 999, fourMetrics()));
    QCOMPARE(captured.telemetryCount, telemetryBefore);
}

void TestDeviceManager::stopAllStopsEverything()
{
    DeviceManager manager;
    manager.setTransportFactory([](const DeviceSetup &) { return new FakeTransport; });

    Captured captured;
    captureAll(&manager, &captured);
    manager.applySetups({setupFor(QStringLiteral("d1")), setupFor(QStringLiteral("d2"))});

    manager.stopAll();

    QVERIFY(captured.stateOrder.contains(QStringLiteral("d1:Disconnected")));
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d2:Disconnected")));
    QCOMPARE(captured.managerStoppedCount, 1);
}

void TestDeviceManager::startStopDeviceById()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);
    manager.applySetups({setupFor(QStringLiteral("d1")), setupFor(QStringLiteral("d2"))});

    manager.stopDevice(QStringLiteral("d1"));
    QVERIFY(captured.stateOrder.contains(QStringLiteral("d1:Disconnected")));
    QVERIFY(!captured.stateOrder.contains(QStringLiteral("d2:Disconnected")));

    manager.startDevice(QStringLiteral("d1"));
    QCOMPARE(transports.value(QStringLiteral("d1"))->openCount, 2); // 重连再 open

    // 未知设备:记日志,不崩溃
    manager.startDevice(QStringLiteral("nobody"));
    manager.stopDevice(QStringLiteral("nobody"));
}

void TestDeviceManager::errorForwardedWithDeviceId()
{
    DeviceManager manager;
    QHash<QString, FakeTransport *> transports;
    manager.setTransportFactory([&transports](const DeviceSetup &setup) {
        auto *t = new FakeTransport;
        transports.insert(setup.deviceId, t);
        return t;
    });

    Captured captured;
    captureAll(&manager, &captured);
    manager.applySetups({setupFor(QStringLiteral("d1"))});

    dms::transport::TransportError error;
    error.code = dms::ErrorCode::ConnectionLost;
    error.message = QStringLiteral("connection reset");
    transports.value(QStringLiteral("d1"))->injectError(error);

    QCOMPARE(captured.lastErrorId, QStringLiteral("d1"));
    QCOMPARE(captured.lastErrorCode, dms::ErrorCode::ConnectionLost);
}

QTEST_MAIN(TestDeviceManager)
#include "tst_devicemanager.moc"
