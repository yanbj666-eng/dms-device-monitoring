// tst_integration:S2 端到端集成测试
// QProcess 拉起 dms_simulator 独立进程,真实 TCP 全链路:
//   TcpTransport + DeviceSession vs 模拟设备(遥测/心跳/命令 ACK/多设备/故障注入)
//
// 时序约定:模拟器故障时钟 0 点 = 设备构造时刻,略早于 SIM READY 行;
// 测试以 READY 到达为本地锚点,因此"设备时刻 = 本地时刻 + δ(δ≥0,实测毫秒级)".
// 所有窗口断言取窗口中段,前后各留 200ms+ 余量吸收 δ 与调度抖动.
#include <QtTest>

#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QtEndian>

#include <algorithm>
#include <memory>
#include <vector>

#include "Frame.h"
#include "MessageCodec.h"
#include "domain/DeviceSession.h"
#include "transport/TcpTransport.h"

using dms::domain::DeviceSession;
using dms::domain::TelemetryBatch;
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

// 模拟器进程封装:等 SIM READY 同步行;停止一律强杀(测试设备进程不求优雅退出)
class SimProcess
{
public:
    ~SimProcess() { stop(); }

    bool start(int count, int periodMs, const QStringList &faultArgs, quint32 seed = 7)
    {
        // 复用同一 QProcess 对象:上一个模拟器若还活着,start() 会静默失败而
        // waitForStarted 对 Running 状态立即返回 true,waitReady 白等 15s 超时
        stop();
        for (int attempt = 0; attempt < 3; ++attempt) {
            const quint16 base = findFreePortRange(count);
            if (base == 0)
                return false;
            QStringList args{
                QStringLiteral("-c"), QString::number(count),
                QStringLiteral("-p"), QString::number(base),
                QStringLiteral("-s"), QString::number(periodMs),
                QStringLiteral("-d"), QString::number(seed),
                // stderr 管道无人消费:压到 ERROR,防日志写满管道卡死模拟器
                QStringLiteral("--log-level"), QStringLiteral("ERROR"),
            };
            args << faultArgs;
            process_.setProgram(QStringLiteral(DMS_SIMULATOR_PATH));
            process_.setArguments(args);
            process_.start();
            if (!process_.waitForStarted(5000))
                continue;
            if (waitReady(count, base, 15000)) {
                basePort_ = base;
                return true;
            }
            stop(); // 端口被抢等启动失败:换端口段重试
        }
        return false;
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
    // 读 stdout 直到 "SIM READY p0..pn" 行,并核对端口与请求一致
    bool waitReady(int count, quint16 expectedBase, int timeoutMs)
    {
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
            for (int i = 0; i < count; ++i) {
                bool ok = false;
                const uint p = parts[2 + i].toUInt(&ok);
                if (!ok || p != uint(expectedBase) + uint(i))
                    return false;
            }
            return true;
        }
        return false;
    }

    QProcess process_;
    quint16 basePort_ = 0;
};

CommandParam periodParam(quint32 ms)
{
    QByteArray raw(4, 0);
    qToBigEndian(ms, raw.data());
    return CommandParam{1, kValueTypeUint32, raw}; // param_id=1:sample_period_ms
}

// S2 直通接口发命令:requestId 就用 Seq,方便对照(requestId 本身不断言)
void sendCommand(DeviceSession &session, quint32 seq, quint16 commandId,
                 const QList<CommandParam> &params = {})
{
    session.transport()->write(
        dms::protocol::encodeFrame(quint8(MessageType::Command), seq, 0,
                                   MessageCodec::encodeCommand(commandId, params)),
        seq);
}

// 在 ACK spy 里按 Seq 回显找对应 ACK(协议 §4.3:只认 Seq 匹配,不认"任意 ACK")
bool findAck(const QSignalSpy &spy, quint32 seq, quint16 &commandId, quint16 &status)
{
    for (const QList<QVariant> &args : spy) {
        if (args.at(0).toUInt() == seq) {
            commandId = quint16(args.at(1).toUInt());
            status = quint16(args.at(2).toUInt());
            return true;
        }
    }
    return false;
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
    void faultFragmentAndMergeWindows();

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

    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    QSignalSpy hbSpy(&session, &DeviceSession::heartbeatReceived);
    QSignalSpy ackSpy(&session, &DeviceSession::commandAckReceived);

    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() == 1, 8000);

    quint16 cmd = 0, status = 0;

    // 1) SetParameter(200) 合法:Success(周期随即降为 200ms)
    sendCommand(session, 11, kCmdSetParameter, {periodParam(200)});
    QTRY_VERIFY_WITH_TIMEOUT(findAck(ackSpy, 11, cmd, status), 3000);
    QCOMPARE(cmd, quint16(kCmdSetParameter));
    QCOMPARE(status, quint16(0));

    // 2) 未知命令 99:结构可解析 -> 拒绝但回 ACK(协议 §4.2)
    sendCommand(session, 12, 99);
    QTRY_VERIFY_WITH_TIMEOUT(findAck(ackSpy, 12, cmd, status), 3000);
    QCOMPARE(cmd, quint16(99));
    QCOMPARE(status, quint16(1)); // Unsupported

    // 3) SetParameter(50) 越界 -> BadParams
    sendCommand(session, 13, kCmdSetParameter, {periodParam(50)});
    QTRY_VERIFY_WITH_TIMEOUT(findAck(ackSpy, 13, cmd, status), 3000);
    QCOMPARE(status, quint16(2)); // BadParams

    // 4) Stop:遥测静默,心跳照常。
    // TCP 保序:ACK 字节在 Stop 生效后发出,看到 ACK 时之前的遥测必然已交付,
    // 因此此刻 telSpy 计数就是终态,不存在"在路上的一帧"误判。
    sendCommand(session, 14, kCmdStop);
    QTRY_VERIFY_WITH_TIMEOUT(findAck(ackSpy, 14, cmd, status), 3000);
    QCOMPARE(status, quint16(0));
    const int telAtStop = telSpy.size();
    const int hbAtStop = hbSpy.size();
    {
        QElapsedTimer w;
        w.start();
        WAIT_UNTIL(w, 600);
    }
    QCOMPARE(telSpy.size(), telAtStop);                      // 遥测确实停了
    QTRY_VERIFY_WITH_TIMEOUT(hbSpy.size() > hbAtStop, 3000); // 心跳仍在

    // 5) Start:遥测恢复
    sendCommand(session, 15, kCmdStart);
    QTRY_VERIFY_WITH_TIMEOUT(findAck(ackSpy, 15, cmd, status), 3000);
    QCOMPARE(status, quint16(0));
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
                    [](const Stack &st) { return st.session->transport()->isOpen(); }),
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
}

void TestIntegration::faultDisconnectWindow()
{
    // 主动断开窗口 500-2000ms:开窗即刻拆链,窗口内新连接接受即拆
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("disconnect@500+1500")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-06");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy closedSpy(&session, &DeviceSession::sessionClosed);
    QSignalSpy errSpy(&session, &DeviceSession::sessionError);
    QSignalSpy telSpy(&session, &DeviceSession::telemetryReceived);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() == 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= 1, 5000);

    // 开窗被踢:closed 与结构化错误都要到
    QTRY_VERIFY_WITH_TIMEOUT(closedSpy.size() >= 1, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.size() >= 1, 4000);
    QCOMPARE(errSpy.first().at(0).value<dms::ErrorCode>(), dms::ErrorCode::ConnectionLost);

    // 窗口内重连:TCP 先连上(opened),随即被故障拆除——generation 模式的实战演练
    WAIT_UNTIL(clock, 1000);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() >= 2, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(closedSpy.size() >= 2, 4000);

    // 窗口结束后重连成功,遥测恢复(transport 不做自动重连,由上层决策)
    WAIT_UNTIL(clock, 2300);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() >= 3, 4000);
    const int telBefore = telSpy.size();
    QTRY_VERIFY_WITH_TIMEOUT(telSpy.size() >= telBefore + 2, 4000);
}

void TestIntegration::faultAckNoReplyWindow()
{
    // ACK 不回窗口 500-2000ms:命令照常被吞掉 ACK(命令超时场景)
    QVERIFY(sim_.start(1, 100, {QStringLiteral("-f"), QStringLiteral("ackNoReply@500+1500")}));
    QElapsedTimer clock;
    clock.start();

    dms::transport::TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("itest-07");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = sim_.portAt(0);
    auto *transport = new dms::transport::TcpTransport(cfg);
    DeviceSession session(cfg.deviceId, transport);

    QSignalSpy openedSpy(&session, &DeviceSession::sessionOpened);
    QSignalSpy ackSpy(&session, &DeviceSession::commandAckReceived);
    session.start();
    QTRY_VERIFY_WITH_TIMEOUT(openedSpy.size() == 1, 8000);

    quint16 cmd = 0, status = 0;

    // 窗口前基线:正常 ACK 是毫秒级
    sendCommand(session, 21, kCmdSetParameter, {periodParam(300)});
    QTRY_VERIFY_WITH_TIMEOUT(findAck(ackSpy, 21, cmd, status), 3000);
    QCOMPARE(status, quint16(0));

    // 窗口内:等足 900ms 无 ACK(基线的几十倍),判定"不回"
    WAIT_UNTIL(clock, 800);
    sendCommand(session, 22, kCmdSetParameter, {periodParam(400)});
    WAIT_UNTIL(clock, 1700);
    QVERIFY(!findAck(ackSpy, 22, cmd, status));

    // 窗口结束:ACK 恢复(等 δ 余量后再发)
    WAIT_UNTIL(clock, 2300);
    sendCommand(session, 23, kCmdSetParameter, {periodParam(200)});
    QTRY_VERIFY_WITH_TIMEOUT(findAck(ackSpy, 23, cmd, status), 4000);
    QCOMPARE(status, quint16(0));
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

QTEST_MAIN(TestIntegration)
#include "tst_integration.moc"
