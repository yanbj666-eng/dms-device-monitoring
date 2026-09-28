// tst_integration:S2-S7 端到端集成测试
// QProcess 拉起 dms_simulator 独立进程,真实全链路(TCP/UDP/串口三形态):
//   ITransport 三实现 + DeviceSession(状态机/自动重连/命令生命周期/UDP 语义)
//   vs 模拟设备(遥测/心跳/命令 ACK/多设备/故障注入/数据报故障)
//
// 时序约定:模拟器故障时钟 0 点 = 设备构造时刻,略早于 SIM READY 行;
// 测试以 READY 到达为本地锚点,因此"设备时刻 = 本地时刻 + δ(δ≥0,实测毫秒级)".
// 所有窗口断言取窗口中段,前后各留 200ms+ 余量吸收 δ 与调度抖动.
#include <QtTest>

#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QSerialPortInfo>
#include <QtEndian>

#include <algorithm>
#include <memory>
#include <vector>

#include "Frame.h"
#include "MessageCodec.h"
#include "domain/AlarmEngine.h"
#include "domain/DeviceManager.h"
#include "domain/DeviceSession.h"
#include "persistence/Database.h"
#include "persistence/DatabaseWorker.h"
#include "persistence/Repository.h"
#include "transport/SerialTransport.h"
#include "transport/TcpTransport.h"
#include "transport/UdpTransport.h"

using dms::domain::CommandOutcome;
using dms::domain::CommandState;
using dms::domain::DeviceManager;
using dms::domain::DeviceSession;
using dms::domain::SessionState;
using dms::domain::TelemetryBatch;
using dms::persistence::Database;
using dms::persistence::DatabaseWorker;
using dms::persistence::DeviceRow;
using dms::persistence::Repository;
using dms::protocol::CommandParam;
using dms::protocol::kValueTypeUint32;
using dms::protocol::MessageCodec;
using dms::protocol::MessageType;
using dms::protocol::MetricId;

namespace {

// 协议 §4.2 命令 id(与设备侧口径一致,集成测试不依赖模拟器头文件)
constexpr quint16 kCmdStart = 1;
constexpr quint16 kCmdStop = 2;
constexpr quint16 kCmdSetParameter = 4;

// 找一段连续空闲监听端口;0 = 失败(调用方重试)
quint16 findFreePortRange(int count)
{
    for (int attempt = 0; attempt < 30; ++attempt) {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0))
            continue;
        const quint16 base = probe.serverPort();
        probe.close();
        bool allFree = base + count - 1 <= 65535;
        for (int i = 1; allFree && i < count; ++i) {
            QTcpServer next;
            if (!next.listen(QHostAddress::LocalHost, quint16(base + i)))
                allFree = false;
        }
        if (allFree)
            return base;
    }
    return 0;
}

// 找一个空闲 UDP 绑定端口(DMS 共享端点用);0 = 失败。
// 关掉即让出,存在极小竞争窗——被抢时 UdpTransport bind 失败会退避重试
quint16 findFreeUdpPort()
{
    QUdpSocket probe;
    if (!probe.bind(QHostAddress::LocalHost, 0))
        return 0;
    const quint16 port = probe.localPort();
    probe.close();
    return port;
}

// 模拟器进程封装:等 SIM READY 同步行;停止一律强杀(测试设备进程不求优雅退出)。
// 三形态:TCP 监听端口 / UDP 固定源端口单发 / Serial 独占串口名
class SimProcess
{
public:
    ~SimProcess() { stop(); }

    // TCP:每设备一个监听端口(既有用例)
    bool start(int count, int periodMs, const QStringList &faultArgs, quint32 seed = 7)
    {
        stop();
        for (int attempt = 0; attempt < 3; ++attempt) {
            const quint16 base = findFreePortRange(count);
            if (base == 0)
                return false;
            QStringList args = baseArgs(periodMs, seed);
            args << QStringLiteral("-c") << QString::number(count)
                 << QStringLiteral("-p") << QString::number(base)
                 << faultArgs;
            if (launchWaitReady(args, portEndpoints(count, base))) {
                basePort_ = base;
                return true;
            }
            stop(); // 端口被抢等启动失败:换端口段重试
        }
        return false;
    }

    // UDP:设备固定源端口 base..base+count-1(设备身份),单发 DMS 绑定端点
    bool startUdp(int count, int periodMs, quint16 dmsBindPort,
                  const QStringList &faultArgs, quint32 seed = 7)
    {
        stop();
        for (int attempt = 0; attempt < 3; ++attempt) {
            const quint16 base = findFreePortRange(count);
            if (base == 0)
                return false;
            QStringList args = baseArgs(periodMs, seed);
            args << QStringLiteral("--transport") << QStringLiteral("udp")
                 << QStringLiteral("-c") << QString::number(count)
                 << QStringLiteral("-p") << QString::number(base)
                 << QStringLiteral("--peer")
                 << QStringLiteral("127.0.0.1:%1").arg(dmsBindPort)
                 << faultArgs;
            if (launchWaitReady(args, portEndpoints(count, base))) {
                basePort_ = base;
                return true;
            }
            stop();
        }
        return false;
    }

    // Serial:端口名即端点,无端口段可换(重试无意义);模拟器占传入的一端
    bool startSerial(const QStringList &portNames, int periodMs,
                     const QStringList &faultArgs, quint32 seed = 7)
    {
        stop();
        QStringList args = baseArgs(periodMs, seed);
        args << QStringLiteral("--transport") << QStringLiteral("serial")
             << QStringLiteral("--serial-ports") << portNames.join(QLatin1Char(','))
             << faultArgs;
        return launchWaitReady(args, portNames);
    }

    quint16 portAt(int index) const { return quint16(basePort_ + index); }

    void stop()
    {
        if (process_.state() == QProcess::NotRunning)
            return;
        process_.kill();
        process_.waitForFinished(5000);
    }

private:
    static QStringList baseArgs(int periodMs, quint32 seed)
    {
        return {QStringLiteral("-s"), QString::number(periodMs),
                QStringLiteral("-d"), QString::number(seed),
                // stderr 管道无人消费:压到 ERROR,防日志写满管道卡死模拟器
                QStringLiteral("--log-level"), QStringLiteral("ERROR")};
    }

    static QStringList portEndpoints(int count, quint16 base)
    {
        QStringList endpoints;
        for (int i = 0; i < count; ++i)
            endpoints << QString::number(base + i);
        return endpoints;
    }

    // 复用同一 QProcess 对象:上一个模拟器若还活着,start() 会静默失败而
    // waitForStarted 对 Running 状态立即返回 true,waitReady 白等 15s 超时
    bool launchWaitReady(const QStringList &args, const QStringList &expectedEndpoints)
    {
        process_.setProgram(QStringLiteral(DMS_SIMULATOR_PATH));
        process_.setArguments(args);
        process_.start();
        if (!process_.waitForStarted(5000))
            return false;
        return waitReady(expectedEndpoints, 15000);
    }

    // 读 stdout 直到 "SIM READY e0..en" 行,并核对端点与请求一致
    // (TCP/UDP 为端口号数字,serial 为串口名;统一按字符串比对)
    bool waitReady(const QStringList &expected, int timeoutMs)
    {
        const int count = expected.size();
        QElapsedTimer t;
        t.start();
        QByteArray buf;
        while (t.elapsed() < timeoutMs) {
            if (process_.state() == QProcess::NotRunning)
                return false; // 启动即退出(如端口被抢):stderr 留给失败重试
            if (!process_.waitForReadyRead(500))
                continue;
            buf += process_.readAllStandardOutput();
            const int nl = buf.indexOf('\n');
            if (nl < 0)
                continue;
            const QList<QByteArray> parts = buf.left(nl).trimmed().split(' ');
            if (parts.size() != 2 + count || parts[0] != "SIM" || parts[1] != "READY")
                return false;
            for (int i = 0; i < count; ++i)
                if (QString::fromLatin1(parts[2 + i]) != expected.at(i))
                    return false;
            return true;
        }
        return false;
    }

    QProcess process_;
    quint16 basePort_ = 0; // TCP=监听端口;UDP=源端口(serial 不用)
};

CommandParam periodParam(quint32 ms)
{
    QByteArray raw(4, 0);
    qToBigEndian(ms, raw.data());
    return CommandParam{1, kValueTypeUint32, raw}; // param_id=1:sample_period_ms
}

// 等会话进入 Online(首条四指标遥测即达,LLD §3 首条有效消息路径)
void waitOnline(DeviceSession &session)
{
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == SessionState::Online, 8000);
}

// 在 commandFinished spy 里按 Seq 找结果;未终结返回 false
bool findFinished(const QSignalSpy &spy, quint32 seq, CommandOutcome &out)
{
    for (const QList<QVariant> &args : spy) {
        if (args.at(0).value<CommandOutcome>().seq == seq) {
            out = args.at(0).value<CommandOutcome>();
            return true;
        }
    }
    return false;
}

// ---- S6 告警 E2E 辅助(与 main.cpp 同拓扑;线程跳变在装配与单元层覆盖)----

// manager → engine 四路直连:同线程 auto 解析为 direct
void connectAlarmPipeline(DeviceManager *manager, dms::domain::AlarmEngine *engine)
{
    QObject::connect(manager, &DeviceManager::deviceTelemetry, engine,
                     &dms::domain::AlarmEngine::onTelemetry);
    QObject::connect(manager, &DeviceManager::deviceStateChanged, engine,
                     &dms::domain::AlarmEngine::onSessionState);
    QObject::connect(manager, &DeviceManager::deviceProtocolHealth, engine,
                     &dms::domain::AlarmEngine::onProtocolHealth);
    QObject::connect(manager, &DeviceManager::deviceCommandFinished, engine,
                     &dms::domain::AlarmEngine::onCommandFinished);
}

// 统计 kind+code+device(可选 metricId)匹配的事件数;0xFFFF = 不限指标
int countAlarms(const QSignalSpy &spy, dms::domain::AlarmEvent::Kind kind,
                const QString &code, const QString &deviceId,
                quint16 metricId = 0xFFFF)
{
    int count = 0;
    for (const QList<QVariant> &args : spy) {
        const dms::domain::AlarmEvent event =
            args.at(0).value<dms::domain::AlarmEvent>();
        if (event.kind == kind && event.code == code && event.deviceId == deviceId
            && (metricId == 0xFFFF || event.metricId == metricId))
            ++count;
    }
    return count;
}

// 在 manager 级 commandFinished spy(参数 1 是 outcome)里找 Succeeded 终态
bool findManagerFinished(const QSignalSpy &spy, quint16 commandId)
{
    for (const QList<QVariant> &args : spy) {
        const CommandOutcome out = args.at(1).value<CommandOutcome>();
        if (out.commandId == commandId && out.state == CommandState::Succeeded)
            return true;
    }
    return false;
}

// S7 起 DeviceSetup.transport 是 variant:TCP 用例统一经此构型
// (UDP/串口 E2E 直接建 transport+session,不经 DeviceSetup)
dms::domain::DeviceSetup tcpSetup(const QString &id, quint16 port)
{
    dms::domain::DeviceSetup setup;
    setup.deviceId = id;
    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = id;
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = port;
    setup.transport = cfg;
    return setup;
}

// UDP 会话语义(LLD §3):失联不关绑定、Reconnecting 收帧直接恢复、禁命令
dms::domain::SessionConfig udpSessionConfig()
{
    dms::domain::SessionConfig cfg;
    cfg.transportKind = dms::transport::TransportKind::Udp;
    return cfg;
}

// 受控"设备"报文(与模拟器同一编码路径):单指标遥测,seq 显式指定
QByteArray fakeTelFrame(quint32 seq)
{
    dms::protocol::TelemetrySample s;
    s.metricId = quint16(dms::protocol::MetricId::Temperature);
    s.valueType = dms::protocol::kValueTypeFloat32;
    s.asFloat = 25.0f;
    return dms::protocol::encodeFrame(quint8(dms::protocol::MessageType::Telemetry), seq,
                                      QDateTime::currentMSecsSinceEpoch(),
                                      dms::protocol::MessageCodec::encodeTelemetry({s}));
}

} // namespace

// 事件循环保持运转地等到锚点时刻(QTRY 每步处理事件,socket/timer 不挨饿)
#define WAIT_UNTIL(clock, targetMs)                                                 \
    do {                                                                            \
        if ((clock).elapsed() < (targetMs))                                         \
            QTRY_VERIFY_WITH_TIMEOUT((clock).elapsed() >= (targetMs),               \
                                     int((targetMs) - (clock).elapsed()) + 10000);  \
    } while (false)

class TestIntegration : public QObject
{
    Q_OBJECT

private slots:
    void singleDeviceTelemetryAndHeartbeat();
    void commandAckLifecycle();
    void multiDeviceConcurrency();
    void faultBadCrcWindow();
    void faultStopSendingWindow();
    void faultDisconnectWindow();
    void faultAckNoReplyWindow();
    void faultAckDelayWindow();
    void faultFragmentAndMergeWindows();
    void faultOfflineRecovery();
    void persistenceRoundTrip();
    void alarmThresholdsEndToEnd();
    void alarmCrcEndToEnd();
    void alarmStaleAndCommLostEndToEnd();
    void acquisitionPauseExemptsStaleAlarm();
    void udpSharedEndpointAndSourceIsolation();
    void udpDatagramFaultsEndToEnd();
    void serialLinkEndToEnd();

private:
    SimProcess sim_; // 每用例一个全新模拟器进程,状态隔离
};

void TestIntegration::singleDeviceTelemetryAndHeartbeat()
{
    QVERIFY(sim_.start(1, 100, {}));

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-01");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg); // 会话接管(parent-child)
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    QSignalSpy hbSpy(&session, &DeviceSession::heartbeatReceived);

    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() == 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 5, 5000);  // 100ms 周期
    QTRY_VERIFY_WITH_TIMEOUT(hbSpy.size() >= 1, 5000);   // 1000ms 心跳

    // 标准负载四指标齐全,取值在模拟器标称域内(docs/15 §5)
    const TelemetryBatch batch = telSpy.first().at(0).value<TelemetryBatch>();
    QCOMPARE(batch.samples.size(), 4);
    bool seenTemp = false, seenPress = false, seenVolt = false, seenRpm = false;
    for (const auto &s : batch.samples) {
        switch (s.metricId) {
        case quint16(MetricId::Temperature):
            seenTemp = true;
            QVERIFY(s.asFloat >= -50.0f && s.asFloat <= 150.0f);
            break;
        case quint16(MetricId::Pressure):
            seenPress = true;
            QVERIFY(s.asFloat >= 0.0f && s.asFloat <= 200.0f);
            break;
        case quint16(MetricId::Voltage):
            seenVolt = true;
            QVERIFY(s.asFloat >= 0.0f && s.asFloat <= 15.0f);
            break;
        case quint16(MetricId::Rpm):
            seenRpm = true;
            QVERIFY(s.asUint >= 500u && s.asUint <= 3500u);
            break;
        default:
            QFAIL("unexpected metric id in standard load");
        }
    }
    QVERIFY(seenTemp && seenPress && seenVolt && seenRpm);

    QCOMPARE(session.badCrcFrames(), 0u);
    QCOMPARE(session.badHeaderFrames(), 0u);
    QCOMPARE(session.payloadErrors(), 0u);
    QVERIFY(session.framesReceived() >= quint64(telSpy.size() + hbSpy.size()));
}

void TestIntegration::commandAckLifecycle()
{
    QVERIFY(sim_.start(1, 100, {}));

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-02");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    QSignalSpy hbSpy(&session, &DeviceSession::heartbeatReceived);
    QSignalSpy finishedSpy(&session, &DeviceSession::commandFinished);

    session.start();
    waitOnline(session); // 命令仅在 Online/Degraded 下允许(LLD §4)

    CommandOutcome out;

    // 1) SetParameter(200) 合法:Success(周期随即降为 200ms)
    quint32 seq = session.sendCommand(kCmdSetParameter, {periodParam(200)});
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.state, CommandState::Succeeded);
    QCOMPARE(out.ackStatus, quint16(0));

    // 2) 未知命令 99:结构可解析 -> 拒绝但回 ACK(协议 §4.2)
    seq = session.sendCommand(99);
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.commandId, quint16(99));
    QCOMPARE(out.state, CommandState::Rejected);
    QCOMPARE(out.ackStatus, quint16(1)); // Unsupported

    // 3) SetParameter(50) 越界 -> BadParams
    seq = session.sendCommand(kCmdSetParameter, {periodParam(50)});
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.state, CommandState::Rejected);
    QCOMPARE(out.ackStatus, quint16(2)); // BadParams

    // 4) Stop:遥测静默,心跳照常。
    // TCP 保序:ACK 字节在 Stop 生效后发出,看到 ACK 时之前的遥测必然已交付,
    // 因此此刻 telSpy 计数就是终态,不存在"在路上的一帧"误判。
    seq = session.sendCommand(kCmdStop);
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.state, CommandState::Succeeded);
    const int telAtStop = telSpy.size();
    const int hbAtStop = hbSpy.size();
    {
        QElapsedTimer w;
        w.start();
        WAIT_UNTIL(w, 600);
    }
    QCOMPARE(telSpy.size(), telAtStop);                      // 遥测确实停了
    QTRY_VERIFY_WITH_TIMEOUT(hbSpy.size() > hbAtStop, 3000); // 心跳仍在

    // 5) Start:遥测恢复(Stop 后无遥测,会话可能已 Degraded,命令仍允许)
    seq = session.sendCommand(kCmdStart);
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.state, CommandState::Succeeded);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() > telAtStop, 3000);
}

void TestIntegration::multiDeviceConcurrency()
{
    constexpr int kCount = 4;
    QVERIFY(sim_.start(kCount, 100, {}));

    // 同一线程同一事件循环挂 4 条会话:S2 目标形态(10 设备单 IO 循环)的小样。
    // Stack 含 unique_ptr 是 move-only:QList 的增长路径要求可拷贝构造,用 std::vector
    struct Stack {
        std::unique_ptr<DeviceSession> session;
        std::unique_ptr<QSignalSpy> telSpy;
    };
    std::vector<Stack> stacks;
    stacks.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        dms::transport::TcpTransportConfig cfg;
        cfg.deviceId = QStringLiteral("itest-m%1").arg(i + 1);
        cfg.host = QStringLiteral("127.0.0.1");
        cfg.port = sim_.portAt(i);
        auto *transport = new dms::transport::TcpTransport(cfg);
        Stack st;
        st.session = std::make_unique<DeviceSession>(cfg.deviceId, transport);
        st.telSpy = std::make_unique<QSignalSpy>(st.session.get(),
                                                 &DeviceSession::telemetryReceived);
        st.session->start();
        stacks.push_back(std::move(st));
    }

    QTRY_VERIFY_WITH_TIMEOUT(
        std::all_of(stacks.begin(), stacks.end(),
                    [](const Stack &st) {
                        return st.session->state() == SessionState::Online;
                    }),
        8000);
    QTRY_VERIFY_WITH_TIMEOUT(
        std::all_of(stacks.begin(), stacks.end(),
                    [](const Stack &st) { return st.telSpy->size() >= 3; }),
        8000);

    for (const Stack &st : stacks) {
        QCOMPARE(st.session->badCrcFrames(), 0u);
        QCOMPARE(st.session->payloadErrors(), 0u);
    }
}

void TestIntegration::faultBadCrcWindow()
{
    // 坏 CRC 窗口 600-1600ms(设备时刻):出口帧 CRC 末字节翻转
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("badCrc@600+1000")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-04");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() == 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 2, 5000); // 窗口前正常解码

    QTRY_VERIFY_WITH_TIMEOUT(session.badCrcFrames() >= 3u, 4000); // 窗口开启后坏帧计数

    // 窗口中段:有效交付完全停滞(坏帧不交付),坏帧只增不减
    WAIT_UNTIL(clock, 1100);
    const quint64 delivered = session.framesReceived();
    const quint32 badSoFar = session.badCrcFrames();
    WAIT_UNTIL(clock, 1350); // 设备时刻 ≤1600 仍在窗内
    QCOMPARE(session.framesReceived(), delivered);
    QVERIFY(session.badCrcFrames() >= badSoFar);

    // 窗口结束:解码自动恢复,无需任何重连/重置
    const int telBefore = telSpy.size();
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= telBefore + 3, 4000);
}

void TestIntegration::faultStopSendingWindow()
{
    // 停发窗口 600-1800ms:遥测+心跳全静默(COMM_LOST 前置场景)
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("stopSending@600+1200")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-05");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    QSignalSpy hbSpy(&session, &DeviceSession::heartbeatReceived);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() == 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 2, 5000); // 窗口前有流量

    // 窗口中段:什么帧都不再到(是"没发",不是"发坏")
    WAIT_UNTIL(clock, 1200);
    const quint64 frames = session.framesReceived();
    WAIT_UNTIL(clock, 1550); // 设备时刻 ≤1800 仍在窗内
    QCOMPARE(session.framesReceived(), frames);
    QCOMPARE(session.badCrcFrames(), 0u);
    QCOMPARE(session.badHeaderFrames(), 0u);

    // 窗口结束:遥测与心跳都恢复(心跳此前一直被窗口压着,首跳可能迟来)
    const int telBefore = telSpy.size();
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= telBefore + 2, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(hbSpy.size() >= 1, 4000);

    // 窗口 1.2s 远小于指标 3000ms/失联 4000ms:全程不降级不失联
    QCOMPARE(session.state(), SessionState::Online);
}

void TestIntegration::faultDisconnectWindow()
{
    // 主动断开窗口 500-2000ms:开窗即刻拆链,窗口内新连接接受即拆。
    // S3 自动重连:被踢 → Reconnecting(退避)→ 窗口结束后的尝试恢复 Online
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("disconnect@500+1500")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-06");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy errSpy(&session, &DeviceSession::sessionError);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    session.start();
    waitOnline(session);

    // 开窗被踢:结构化错误 + 失联进 Reconnecting
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.size() >= 1, 4000);
    QCOMPARE(errSpy.first().at(0).value<dms::ErrorCode>(), dms::ErrorCode::ConnectionLost);
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == SessionState::Reconnecting, 4000);

    // 自动重连:窗口(设备时刻 ≤2000)结束后的某次尝试成功,恢复 Online。
    // LLD §3:稳定可达端点恢复后 ≤15s Online
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == SessionState::Online, 15000);

    // 恢复后遥测继续增长:链路是真的活着
    const int telBefore = telSpy.size();
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= telBefore + 2, 4000);
}

void TestIntegration::faultAckNoReplyWindow()
{
    // ACK 不回窗口 500-2000ms:命令照常发出,ACK 被吞 → 2000ms 超时 TimedOut
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("ackNoReply@500+1500")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-07");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy finishedSpy(&session, &DeviceSession::commandFinished);
    session.start();
    waitOnline(session);

    CommandOutcome out;

    // 窗口前基线:正常 ACK 是毫秒级
    quint32 seq = session.sendCommand(kCmdSetParameter, {periodParam(300)});
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.state, CommandState::Succeeded);

    // 窗口内:命令发出但 ACK 被吞,2000ms 后 TimedOut(执行结果未知,不重发)
    WAIT_UNTIL(clock, 800);
    seq = session.sendCommand(kCmdSetParameter, {periodParam(400)});
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 5000);
    QCOMPARE(out.state, CommandState::TimedOut);

    // 窗口结束:ACK 恢复(此命令发出时 TimedOut 已落地,窗口必已关闭)
    WAIT_UNTIL(clock, 2300);
    seq = session.sendCommand(kCmdSetParameter, {periodParam(200)});
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 4000);
    QCOMPARE(out.state, CommandState::Succeeded);
}

void TestIntegration::faultFragmentAndMergeWindows()
{
    // 分片窗 300-1100ms + 合帧窗 1400-2200ms:解码器对半包/粘包应完全透明
    QVERIFY(sim_.start(1, 100,
                       {QStringLiteral("-f"), QStringLiteral("fragment@300+800"),
                        QStringLiteral("-f"), QStringLiteral("merge@1400+800")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-08");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() == 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 2, 5000);

    // 分片窗结束后:窗内 ~8 帧虽被拆成两段仍全部解码
    WAIT_UNTIL(clock, 1250);
    const int afterFragment = telSpy.size();
    QVERIFY(afterFragment >= 5);
    QCOMPARE(session.badCrcFrames(), 0u);
    QCOMPARE(session.badHeaderFrames(), 0u);

    // 合帧窗结束(+孤帧兜底 300ms 余量):粘在一起的帧同样全部解开
    WAIT_UNTIL(clock, 2500);
    QVERIFY(telSpy.size() >= afterFragment + 5);
    QCOMPARE(session.badCrcFrames(), 0u);
    QCOMPARE(session.badHeaderFrames(), 0u);
    QCOMPARE(session.payloadErrors(), 0u);
}

void TestIntegration::faultAckDelayWindow()
{
    // ACK 延迟窗口 500-2000ms:ACK 迟到 500ms,仍在 2000ms 命令超时内 → Succeeded
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("ackDelay@500+1500")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-09");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy finishedSpy(&session, &DeviceSession::commandFinished);
    session.start();
    waitOnline(session);

    CommandOutcome out;

    // 窗口前基线
    quint32 seq = session.sendCommand(kCmdSetParameter, {periodParam(300)});
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.state, CommandState::Succeeded);

    // 窗口中段发命令:ACK 延迟 500ms 回来,未超 2000ms 超时
    WAIT_UNTIL(clock, 900);
    seq = session.sendCommand(kCmdSetParameter, {periodParam(400)});
    QVERIFY(seq != 0);
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 4000);
    QCOMPARE(out.state, CommandState::Succeeded);
}

void TestIntegration::faultOfflineRecovery()
{
    // 停发长窗 600-6600ms:指标过期 → Degraded;4000ms 无有效帧 → Reconnecting;
    // 重连后窗口仍在(无首帧)再次退避;窗口结束后恢复 Online(自动重连全链路)
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("stopSending@600+6000")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-10");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    session.start();
    waitOnline(session);

    // 四指标 3000ms 无更新:降级但链路活着(心跳也停了,存活判定靠最后一帧)
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == SessionState::Degraded, 6000);
    // 4000ms 无有效接收:失联进 Reconnecting,自动退避重连
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == SessionState::Reconnecting, 8000);
    // 窗口结束 + 自动重连成功:恢复 Online(LLD §3 稳定可达端点恢复后 ≤15s)
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == SessionState::Online, 20000);
    QVERIFY(clock.elapsed() <= 22000); // 总预算(含开窗 600ms 与 δ)
}

// S5 端到端:模拟器 → TCP → DeviceManager → DatabaseWorker → SQLite,
// 再经 Repository 只读连接读回校验;含命令 Sent→Succeeded 全生命周期。
// 线程跳变(GUI/IO/DB 三线程)已在装配与单元层覆盖,此处同线程直连验数据链路
void TestIntegration::persistenceRoundTrip()
{
    QVERIFY(sim_.start(1, 100, {}));

    dms::domain::DeviceSetup setup =
        tcpSetup(QStringLiteral("itest-07"), sim_.portAt(0));
    DeviceManager manager;

    QTemporaryDir dir;
    dms::persistence::DbWorkerSettings settings;
    settings.databasePath = dir.filePath(QStringLiteral("e2e.db"));
    settings.batchSamples = 8; // 两帧即达批阈值
    settings.flushIntervalMs = 200;
    settings.queueSamples = 5000;
    settings.eventQueueSize = 100;
    settings.busyTimeoutMs = 200;
    settings.shutdownFlushMs = 3000;

    DatabaseWorker worker(settings);
    DeviceRow row;
    row.id = QStringLiteral("itest-07");
    row.name = QStringLiteral("集成设备");
    row.transportType = QStringLiteral("tcp");
    row.configJson = QStringLiteral("{\"id\":\"itest-07\"}");
    worker.initialize({row});

    // 先接管线再放采集:漏掉的前几帧不会入库,断言只数已入库的
    QObject::connect(&manager, &DeviceManager::deviceTelemetry, &worker,
                     &DatabaseWorker::onTelemetry);
    QObject::connect(&manager, &DeviceManager::commandSubmitted, &worker,
                     &DatabaseWorker::onCommandSubmitted);
    QObject::connect(&manager, &DeviceManager::deviceCommandFinished, &worker,
                     &DatabaseWorker::onCommandFinished);

    QSignalSpy telSpy(&manager, &DeviceManager::deviceTelemetry);
    QSignalSpy finishedSpy(&manager, &DeviceManager::deviceCommandFinished);
    QSignalSpy closedSpy(&worker, &DatabaseWorker::closed);

    manager.applySetups({setup}); // connectOnStart:装配即连
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 10, 8000); // 100ms 周期

    // 命令闭环:150ms 在模拟器合法域内,Success(SetParameter uint32 参数)
    manager.sendCommand(QStringLiteral("itest-07"), kCmdSetParameter,
                        {periodParam(150)});
    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.size() >= 1, 3000);
    const CommandOutcome out = finishedSpy.first().at(1).value<CommandOutcome>();
    QCOMPARE(out.state, CommandState::Succeeded);

    // 批量双触发 + 事件直写:队列归零且零丢弃
    QTRY_VERIFY_WITH_TIMEOUT(worker.pendingSamples() == 0 && worker.pendingEvents() == 0,
                             5000);
    QCOMPARE(worker.droppedSamples(), quint64(0));
    QCOMPARE(worker.droppedEvents(), quint64(0));

    // 只读连接读回:Repository 分页 + 原生 SQL 核对列
    Database reader;
    QVERIFY2(reader.openExisting(settings.databasePath, 200).ok, "只读打开失败");
    Repository repo(&reader);

    dms::persistence::TelemetryQuery query;
    query.deviceId = QStringLiteral("itest-07");
    dms::persistence::TelemetryCursor cursor;
    QString err;
    const auto page = repo.queryPage(query, &cursor, 1000, &err);
    QVERIFY2(page.has_value(), qPrintable(err));
    QVERIFY(!page->hasMore);
    QVERIFY(page->rows.size() >= 40); // ≥10 帧 × 4 指标

    qint64 prevReceivedAt = 0;
    bool seen[5] = {};
    for (const dms::persistence::TelemetryRow &r : page->rows) {
        QVERIFY(r.metricId >= 1 && r.metricId <= 4);
        seen[r.metricId] = true;
        QVERIFY(r.ts > 0);
        QVERIFY(r.receivedAt > 0);
        QVERIFY(r.receivedAt >= prevReceivedAt); // (received_at,id) 升序
        prevReceivedAt = r.receivedAt;
    }
    QVERIFY(seen[1] && seen[2] && seen[3] && seen[4]);

    QSqlQuery cmdQuery(reader.sql());
    QVERIFY2(cmdQuery.exec(QStringLiteral(
                  "SELECT command_type, request_json, result, ack_status, session_id"
                  " FROM commands WHERE device_id='itest-07'")),
             qPrintable(cmdQuery.lastError().text()));
    QVERIFY(cmdQuery.next());
    QCOMPARE(cmdQuery.value(0).toString(), QStringLiteral("SetParameter"));
    QVERIFY(cmdQuery.value(1).toString().contains(QStringLiteral("\"value\":150")));
    QCOMPARE(cmdQuery.value(2).toString(), QStringLiteral("Succeeded"));
    QCOMPARE(cmdQuery.value(3).toInt(), 0);
    const QString commandSession = cmdQuery.value(4).toString();
    QVERIFY(!commandSession.isEmpty());
    QVERIFY(!cmdQuery.next()); // 全程只受理一条

    // 遥测与命令同一连接会话,session_id 一致入库
    QSqlQuery telSession(reader.sql());
    QVERIFY2(telSession.exec(QStringLiteral(
                  "SELECT DISTINCT session_id FROM telemetry WHERE device_id='itest-07'")),
             qPrintable(telSession.lastError().text()));
    QVERIFY(telSession.next());
    QCOMPARE(telSession.value(0).toString(), commandSession);
    QVERIFY(!telSession.next());

    // 排空关闭:零未提交零丢弃,不伪报成功
    worker.drainAndClose();
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(closedSpy.first().at(0).toLongLong(), qint64(0));
    QCOMPARE(closedSpy.first().at(1).toLongLong(), qint64(0));
    QCOMPARE(closedSpy.first().at(2).toULongLong(), quint64(0));
    QCOMPARE(closedSpy.first().at(3).toULongLong(), quint64(0));
}

// S6 E2E 之一(TC-ALARM-001/002):越界值窗口 → 阈值触发/恢复 → 落库。
// 越界帧 温度 999 / 压力 -5 / 电压 300 / 转速 4e9;规则只配 1/2,
// 3/4 同窗异常不产生阈值告警(未配置的指标不评估,LLD §6)
void TestIntegration::alarmThresholdsEndToEnd()
{
    QVERIFY(sim_.start(1, 100,
                       {QStringLiteral("-f"), QStringLiteral("abnormalValue@600+1000")}));

    const QString id = QStringLiteral("itest-11");
    const dms::domain::DeviceSetup setup =
        tcpSetup(id, sim_.portAt(0));

    // 默认参数即 app.ini [connection] 默认值:poll 100/stale 3000/坏 CRC 5/好 3
    dms::domain::AlarmEngine engine{dms::domain::AlarmEngine::Settings{}};
    engine.setRules(id, {{1, -60.0, 80.0, 0.0}, {2, 10.0, 250.0, 0.0}});
    DeviceManager manager;
    connectAlarmPipeline(&manager, &engine);
    QSignalSpy alarmSpy(&engine, &dms::domain::AlarmEngine::alarmEvent);

    // 落库链路同款:引擎出口直连 worker(FIFO 语义,worker 单测已覆盖队列面)
    QTemporaryDir dir;
    dms::persistence::DbWorkerSettings ws;
    ws.databasePath = dir.filePath(QStringLiteral("alarms.db"));
    ws.busyTimeoutMs = 200;
    DatabaseWorker worker(ws);
    DeviceRow row;
    row.id = id;
    row.name = QStringLiteral("阈值设备");
    row.transportType = QStringLiteral("tcp");
    // config_json NOT NULL:空 QString 绑定成 NULL 会让 upsert 整体回滚(实测踩坑)
    row.configJson = QStringLiteral("{\"id\":\"itest-11\"}");
    worker.initialize({row});
    QObject::connect(&engine, &dms::domain::AlarmEngine::alarmEvent, &worker,
                     &DatabaseWorker::onAlarmEvent);
    QSignalSpy closedSpy(&worker, &DatabaseWorker::closed);

    engine.start();
    manager.applySetups({setup});

    // 窗口内:温度越上界、压力越下界,各触发一次(持续越界去重)
    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                    QStringLiteral("THRESHOLD_HIGH"), id, 1)
            == 1,
        8000);
    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                    QStringLiteral("THRESHOLD_LOW"), id, 2)
            == 1,
        8000);

    // 窗口结束:值回带内,恢复事件 reason=recovered
    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Cleared,
                    QStringLiteral("THRESHOLD_HIGH"), id, 1)
            == 1,
        8000);
    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Cleared,
                    QStringLiteral("THRESHOLD_LOW"), id, 2)
            == 1,
        8000);
    bool sawRecovered = false;
    int metric34Triggers = 0;
    for (const QList<QVariant> &args : alarmSpy) {
        const dms::domain::AlarmEvent event =
            args.at(0).value<dms::domain::AlarmEvent>();
        if (event.kind == dms::domain::AlarmEvent::Kind::Cleared
            && event.clearReason == QStringLiteral("recovered"))
            sawRecovered = true;
        if (event.kind == dms::domain::AlarmEvent::Kind::Triggered
            && event.metricId >= 3)
            ++metric34Triggers; // 电压/转速同窗异常但无规则:零告警
    }
    QVERIFY(sawRecovered);
    QCOMPARE(metric34Triggers, 0);

    // 事件队列排空零丢弃,读回 alarms:两行都已恢复,等级与恢复原因落库
    QTRY_VERIFY_WITH_TIMEOUT(worker.pendingEvents() == 0, 5000);
    QCOMPARE(worker.droppedEvents(), quint64(0));
    Database reader;
    QVERIFY2(reader.openExisting(ws.databasePath, 200).ok, "只读打开失败");
    QSqlQuery q(reader.sql());
    QVERIFY2(q.exec(QStringLiteral(
                  "SELECT code, metric_id, level, acknowledged, cleared_at,"
                  " clear_reason FROM alarms WHERE device_id='%1' ORDER BY id")
                  .arg(id)),
             qPrintable(q.lastError().text()));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("THRESHOLD_HIGH"));
    QCOMPARE(q.value(1).toInt(), 1);
    QCOMPARE(q.value(2).toInt(), 3); // 阈值 = Error
    QCOMPARE(q.value(3).toInt(), 0); // 未确认
    QVERIFY(!q.value(4).isNull());
    QCOMPARE(q.value(5).toString(), QStringLiteral("recovered"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("THRESHOLD_LOW"));
    QCOMPARE(q.value(1).toInt(), 2);
    QVERIFY(!q.value(4).isNull());
    QVERIFY(!q.next()); // 恰好两行,去重面 E2E 复核

    worker.drainAndClose();
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(closedSpy.first().at(1).toLongLong(), qint64(0)); // 未提交事件
}

// S6 E2E 之二(TC-ALARM-003 前半):坏 CRC 窗口 ~10 坏帧 → 第 5 帧触发,
// 窗口结束 3 有效帧解除;持续坏帧期间只触发一次
void TestIntegration::alarmCrcEndToEnd()
{
    QVERIFY(sim_.start(1, 100,
                       {QStringLiteral("-f"), QStringLiteral("badCrc@600+1000")}));

    const QString id = QStringLiteral("itest-13");
    const dms::domain::DeviceSetup setup =
        tcpSetup(id, sim_.portAt(0));

    DeviceManager manager;
    dms::domain::AlarmEngine engine{dms::domain::AlarmEngine::Settings{}};
    connectAlarmPipeline(&manager, &engine);
    QSignalSpy alarmSpy(&engine, &dms::domain::AlarmEngine::alarmEvent);

    engine.start();
    manager.applySetups({setup});

    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                    QStringLiteral("PROTOCOL_CRC"), id)
            == 1,
        8000);
    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Cleared,
                    QStringLiteral("PROTOCOL_CRC"), id)
            == 1,
        8000);
    // 触发后窗口仍在(坏帧继续):去重保住只此一次;恢复后不再重触发
    QCOMPARE(countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                         QStringLiteral("PROTOCOL_CRC"), id),
             1);
}

// S6 E2E 之三(TC-ALARM-003/004):停发长窗 → 四指标 DATA_STALE + COMM_LOST,
// 重连恢复后全部解除。停发 600-6600ms:陈旧 ~3.6s、失联 ~4.6s(设备时刻)
void TestIntegration::alarmStaleAndCommLostEndToEnd()
{
    QVERIFY(sim_.start(1, 100,
                       {QStringLiteral("-f"), QStringLiteral("stopSending@600+6000")}));

    const QString id = QStringLiteral("itest-14");
    const dms::domain::DeviceSetup setup =
        tcpSetup(id, sim_.portAt(0));

    DeviceManager manager;
    dms::domain::AlarmEngine engine{dms::domain::AlarmEngine::Settings{}};
    // 无阈值规则:只评估陈旧/CRC/失联
    connectAlarmPipeline(&manager, &engine);
    QSignalSpy alarmSpy(&engine, &dms::domain::AlarmEngine::alarmEvent);

    engine.start();
    manager.applySetups({setup});

    // 窗口内:每个指标一条 DATA_STALE(3000ms 无有效值)
    for (quint16 m = 1; m <= 4; ++m) {
        QTRY_VERIFY_WITH_TIMEOUT(
            countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                        QStringLiteral("DATA_STALE"), id, m)
                == 1,
            12000);
    }
    // 失联:Reconnecting → COMM_LOST 一次(重试轮次的 Connecting 只重置陈旧
    // 计时,不解除失联告警)
    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                    QStringLiteral("COMM_LOST"), id)
            == 1,
        15000);

    // 窗口结束重连成功:Online + 新遥测 → 陈旧与失联全部解除
    for (quint16 m = 1; m <= 4; ++m) {
        QTRY_VERIFY_WITH_TIMEOUT(
            countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Cleared,
                        QStringLiteral("DATA_STALE"), id, m)
                == 1,
            25000);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Cleared,
                    QStringLiteral("COMM_LOST"), id)
            == 1,
        25000);
}

// S6 E2E 之四(TC-ALARM-003 后半):Stop ACK 后采集暂停,陈旧检测整设备豁免
// ——心跳保 Online、静默 > 陈旧阈不产生 DATA_STALE;Start 后采集恢复仍零误报
void TestIntegration::acquisitionPauseExemptsStaleAlarm()
{
    QVERIFY(sim_.start(1, 100, {}));

    const QString id = QStringLiteral("itest-15");
    const dms::domain::DeviceSetup setup =
        tcpSetup(id, sim_.portAt(0));

    DeviceManager manager;
    dms::domain::AlarmEngine engine{dms::domain::AlarmEngine::Settings{}};
    connectAlarmPipeline(&manager, &engine);
    QSignalSpy alarmSpy(&engine, &dms::domain::AlarmEngine::alarmEvent);
    QSignalSpy telSpy(&manager, &DeviceManager::deviceTelemetry);
    QSignalSpy stateSpy(&manager, &DeviceManager::deviceStateChanged);
    QSignalSpy finishedSpy(&manager, &DeviceManager::deviceCommandFinished);

    engine.start();
    manager.applySetups({setup});
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 3, 8000);

    // Stop(Succeeded):会话侧停发遥测只留心跳,引擎侧豁免陈旧检测
    manager.sendCommand(id, kCmdStop);
    QTRY_VERIFY_WITH_TIMEOUT(findManagerFinished(finishedSpy, kCmdStop), 3000);
    const int telAtStop = telSpy.size();

    // 静默 3600ms(> 3000ms 陈旧阈):心跳维持 Online,零 DATA_STALE
    {
        QElapsedTimer w;
        w.start();
        WAIT_UNTIL(w, 3600);
    }
    QCOMPARE(countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                         QStringLiteral("DATA_STALE"), id),
             0);
    for (const QList<QVariant> &args : stateSpy)
        QVERIFY2(args.at(1).value<SessionState>() != SessionState::Degraded,
                 "Stop 豁免期不应降级");

    // Start:遥测恢复,全程无一次 DATA_STALE 误报
    manager.sendCommand(id, kCmdStart);
    QTRY_VERIFY_WITH_TIMEOUT(findManagerFinished(finishedSpy, kCmdStart), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() > telAtStop, 4000);
    QCOMPARE(countAlarms(alarmSpy, dms::domain::AlarmEvent::Kind::Triggered,
                         QStringLiteral("DATA_STALE"), id),
             0);
}

// S7 E2E 之一(TC-UDP-001):两设备共享 DMS 绑定端点,按来源四元组隔离;
// 未知来源合法报文也整报丢弃;非"恰好一帧"整报拒绝;UDP 不承载命令
void TestIntegration::udpSharedEndpointAndSourceIsolation()
{
    const quint16 dmsBind = findFreeUdpPort();
    QVERIFY(dmsBind != 0);
    QVERIFY(sim_.startUdp(2, 100, dmsBind, {}));

    const dms::domain::SessionConfig udpCfg = udpSessionConfig();

    dms::transport::UdpTransportConfig cfg1;
    cfg1.deviceId = QStringLiteral("itest-16-a");
    cfg1.bindAddress = QHostAddress::LocalHost;
    cfg1.bindPort = dmsBind;
    cfg1.peerAddress = QHostAddress::LocalHost;
    cfg1.peerPort = sim_.portAt(0);
    auto *t1 = new dms::transport::UdpTransport(cfg1); // 会话接管(parent-child)
    DeviceSession s1(cfg1.deviceId, t1, udpCfg);

    // 第二个 transport 同一绑定端点:UdpEndpointSocket 端点池共享 socket(协议 §7)
    dms::transport::UdpTransportConfig cfg2 = cfg1;
    cfg2.deviceId = QStringLiteral("itest-16-b");
    cfg2.peerPort = sim_.portAt(1);
    auto *t2 = new dms::transport::UdpTransport(cfg2);
    DeviceSession s2(cfg2.deviceId, t2, udpCfg);

    QSignalSpy tel1(&s1, &DeviceSession::telemetryReceived);
    QSignalSpy tel2(&s2, &DeviceSession::telemetryReceived);
    s1.start();
    s2.start();
    waitOnline(s1);
    waitOnline(s2);
    QTRY_VERIFY_WITH_TIMEOUT(tel1.size() >= 5, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(tel2.size() >= 5, 5000);
    QCOMPARE(s1.badCrcFrames(), 0u);
    QCOMPARE(s2.badCrcFrames(), 0u);
    QCOMPARE(s1.malformedDatagrams(), 0u);
    QCOMPARE(s2.malformedDatagrams(), 0u);

    // 未知来源(stranger 端口)发合法帧:端点整报丢弃,任何会话都不交付。
    // 断言用 staleSeq 计数而非帧数:遥测仍在流入,帧数必然增长,不可作判据;
    // 该帧 seq=1 若被误投递必判"旧"计数,计 0 即证明从未进会话
    QUdpSocket stranger;
    QVERIFY(stranger.bind(QHostAddress::LocalHost, 0));
    const quint64 dropsBefore = t1->unknownSourceDrops();
    stranger.writeDatagram(fakeTelFrame(1), QHostAddress::LocalHost, dmsBind);
    QTRY_COMPARE(t1->unknownSourceDrops(), dropsBefore + 1);
    {
        QElapsedTimer w;
        w.start();
        WAIT_UNTIL(w, 300);
    }
    QCOMPARE(s1.staleSeqFrames(), 0u);
    QCOMPARE(s2.staleSeqFrames(), 0u);

    // 非"恰好一帧"整报拒绝:受控 socket 扮演第三个已知来源设备
    QUdpSocket fakeDev;
    QVERIFY(fakeDev.bind(QHostAddress::LocalHost, 0));
    dms::transport::UdpTransportConfig cfg3 = cfg1;
    cfg3.deviceId = QStringLiteral("itest-16-fake");
    cfg3.peerPort = fakeDev.localPort();
    auto *t3 = new dms::transport::UdpTransport(cfg3);
    DeviceSession s3(cfg3.deviceId, t3, udpCfg);
    QSignalSpy tel3(&s3, &DeviceSession::telemetryReceived);
    s3.start();

    fakeDev.writeDatagram(fakeTelFrame(10), QHostAddress::LocalHost, dmsBind);
    QTRY_VERIFY_WITH_TIMEOUT(s3.state() == SessionState::Online, 5000);
    QCOMPARE(tel3.size(), 1);

    // 两帧一报 + 半帧:均整报拒绝,不交付。UDP 路径每报独立解码
    // (onDatagramReceived 先 reset 解码器,LLD §3"半帧绝不留到下一报"),
    // 因此好报随后直接交付,无需冲刷残留
    fakeDev.writeDatagram(fakeTelFrame(11) + fakeTelFrame(12),
                          QHostAddress::LocalHost, dmsBind);
    fakeDev.writeDatagram(fakeTelFrame(13).left(15), QHostAddress::LocalHost, dmsBind);
    QTRY_COMPARE(s3.malformedDatagrams(), 2u);
    QCOMPARE(tel3.size(), 1);

    fakeDev.writeDatagram(fakeTelFrame(14), QHostAddress::LocalHost, dmsBind); // 好报恢复
    QTRY_VERIFY_WITH_TIMEOUT(tel3.size() == 2, 3000);
    QCOMPARE(s3.malformedDatagrams(), 2u);
    QCOMPARE(s3.staleSeqFrames(), 0u);

    // UDP 不承载命令(协议 §7):会话侧直接拒绝,Seq=0
    QCOMPARE(s1.sendCommand(kCmdSetParameter, {periodParam(200)}), quint32(0));
}

// S7 E2E 之二(TC-UDP-002):数据报三故障真注入——丢包窗遥测冻结、
// 乱序窗旧报按 seq 模差丢弃、重复窗第二份按 d=0 丢弃;窗口后全恢复
void TestIntegration::udpDatagramFaultsEndToEnd()
{
    const quint16 dmsBind = findFreeUdpPort();
    QVERIFY(dmsBind != 0);
    QVERIFY(sim_.startUdp(1, 100, dmsBind,
                          {QStringLiteral("-f"), QStringLiteral("datagramDrop@3000+1200"),
                           QStringLiteral("-f"), QStringLiteral("datagramReorder@6500+1200"),
                           QStringLiteral("-f"), QStringLiteral("datagramDuplicate@10000+1000")}));
    QElapsedTimer clock;
    clock.start(); // 锚点:READY 已到;设备故障钟 0 点略早(δ≥0,窗中段留足余量)

    dms::transport::UdpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-17");
    cfg.bindAddress = QHostAddress::LocalHost;
    cfg.bindPort = dmsBind;
    cfg.peerAddress = QHostAddress::LocalHost;
    cfg.peerPort = sim_.portAt(0);
    auto *transport = new dms::transport::UdpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport, udpSessionConfig());

    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    session.start();
    waitOnline(session);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 10, 5000);

    // 丢包窗 [3000,4200) 设备时刻:本地 3200-3600 必在窗内(容忍 δ<600ms),
    // 全部数据报被丢,遥测冻结
    WAIT_UNTIL(clock, 3200);
    const int telAtDrop = telSpy.size();
    QVERIFY(telAtDrop >= 10);
    WAIT_UNTIL(clock, 3600);
    QCOMPARE(telSpy.size(), telAtDrop);

    // 窗口结束:链路照常恢复;1200ms 静默 << 4000ms 失联线,不 Reconnecting
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() > telAtDrop + 5, 4000);
    QCOMPARE(session.state(), SessionState::Online);

    // 乱序窗 [6500,7700):相邻对调;每对旧报(后到)按 seq 模差判旧丢弃
    WAIT_UNTIL(clock, 6100);
    const quint32 staleAtReorder = session.staleSeqFrames();
    const int telAtReorder = telSpy.size();
    WAIT_UNTIL(clock, 7900);
    QVERIFY(session.staleSeqFrames() >= staleAtReorder + 2); // 窗内 ~6 次对调
    QVERIFY(telSpy.size() > telAtReorder);                   // 每对新报照常交付
    QCOMPARE(session.state(), SessionState::Online);

    // 重复窗 [10000,11000):同报连发两次,第二份 d=0 判重复丢弃
    WAIT_UNTIL(clock, 9500);
    const quint32 staleAtDup = session.staleSeqFrames();
    WAIT_UNTIL(clock, 11200);
    QVERIFY(session.staleSeqFrames() >= staleAtDup + 2); // 窗内 ~10 份重复
    QCOMPARE(session.state(), SessionState::Online);
    QVERIFY(telSpy.size() > telAtReorder + 10); // 实时值无回退,持续推进
}

// S7 E2E 之三(TC-SERIAL-001):虚拟串口对上真实 QSerialPort 全链路——
// 遥测上线 + 命令/ACK 回走同一串口对。无虚拟对的环境按条件跳过(Blocked)
void TestIntegration::serialLinkEndToEnd()
{
    // Windows: com0com 装出 COM3<->COM5;WSL2: socat 建 pty 对
    const QString pair = qEnvironmentVariable("DMS_TEST_SERIAL_PAIR");
    if (pair.isEmpty())
        QSKIP("需要虚拟串口对:Windows 用 com0com,WSL2 用 socat。设置 "
              "DMS_TEST_SERIAL_PAIR=\"COM3,COM5\"(模拟器占第一端,DMS 占第二端)后启用");
    const QStringList names = pair.split(QLatin1Char(','));
    if (names.size() != 2 || names.at(0).trimmed().isEmpty() || names.at(1).trimmed().isEmpty())
        QSKIP("DMS_TEST_SERIAL_PAIR 需要形如 COM3,COM5 的两个逗号分隔端口名");
    const QString simPort = names.at(0).trimmed();
    const QString dmsPort = names.at(1).trimmed();

    bool dmsPortPresent = false;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        if (info.portName().compare(dmsPort, Qt::CaseInsensitive) == 0)
            dmsPortPresent = true;
    if (!dmsPortPresent)
        QSKIP(QStringLiteral("DMS 侧端口 %1 不存在,请核对 com0com/socat 配置")
                  .arg(dmsPort)
                  .toUtf8()
                  .constData());

    QVERIFY(sim_.startSerial({simPort}, 100, {}));

    dms::domain::SessionConfig serialCfg;
    serialCfg.transportKind = dms::transport::TransportKind::Serial;
    dms::transport::SerialTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-18");
    cfg.portName = dmsPort;
    auto *transport = new dms::transport::SerialTransport(cfg);
    DeviceSession session(cfg.deviceId, transport, serialCfg);

    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    QSignalSpy finishedSpy(&session, &DeviceSession::commandFinished);
    session.start();
    waitOnline(session);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 5, 5000);

    // 命令全链路:命令字节写进 DMS 端口,ACK 从设备端绕回同一对
    const quint32 seq = session.sendCommand(kCmdSetParameter, {periodParam(300)});
    QVERIFY(seq != 0);
    CommandOutcome out;
    QTRY_VERIFY_WITH_TIMEOUT(findFinished(finishedSpy, seq, out), 3000);
    QCOMPARE(out.state, CommandState::Succeeded);
}

QTEST_MAIN(TestIntegration)
#include "tst_integration.moc"
