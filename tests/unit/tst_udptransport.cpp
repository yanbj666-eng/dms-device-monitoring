// tst_udptransport:LLD §1/协议 §7 UdpTransport 测试
// 本机回环真 QUdpSocket 做对端:bind 语义/来源隔离/同端点共享 socket/
// 写路径/关闭语义/bind 失败分类
#include <QtTest>

#include <QNetworkDatagram>
#include <QUdpSocket>

#include "transport/UdpTransport.h"

using dms::transport::ITransport;
using dms::transport::TransportError;
using dms::transport::UdpTransport;
using dms::transport::UdpTransportConfig;

namespace {

const QHostAddress kLocal = QHostAddress(QHostAddress::LocalHost);

// 测试对端:模拟"来源固定的设备",bind 127.0.0.1:0 由内核分配源端口
class TestPeer
{
public:
    bool bind() { return socket_.bind(kLocal, 0); }
    quint16 port() const { return socket_.localPort(); }
    QUdpSocket &socket() { return socket_; }

    qint64 sendTo(quint16 destPort, const QByteArray &bytes)
    {
        return socket_.writeDatagram(bytes, kLocal, destPort);
    }

private:
    QUdpSocket socket_;
};

UdpTransportConfig makeConfig(quint16 bindPort, const TestPeer &peer,
                              const char *deviceId = "sim-udp-01")
{
    UdpTransportConfig cfg;
    cfg.deviceId = QString::fromLatin1(deviceId);
    cfg.bindAddress = kLocal;
    cfg.bindPort = bindPort; // 0 = 内核分配
    cfg.peerAddress = kLocal;
    cfg.peerPort = peer.port();
    return cfg;
}

// 探测一个空闲 UDP 端口(探测 socket 关闭后让出;测试环境竞态可接受)
quint16 findFreeUdpPort()
{
    QUdpSocket probe;
    if (!probe.bind(kLocal, 0))
        return 0;
    return probe.localPort();
}

} // namespace

class TestUdpTransport : public QObject
{
    Q_OBJECT

private slots:
    void bindOpensAndReceivesFromPeer();
    void unknownSourceDropped();
    void sharedEndpointRoutesBySource();
    void bindFailureReportsConnectionLost();
    void reopenAfterCloseBindsAgain();
    void writeReachesPeer();
    void closeEmitsClosedExactlyOnce();
};

// 基本路径:open=bind(同步确认)、来源报文整报转发、报文边界与来源保留
void TestUdpTransport::bindOpensAndReceivesFromPeer()
{
    TestPeer peer;
    QVERIFY(peer.bind());

    UdpTransport transport(makeConfig(0, peer));
    ITransport *it = &transport; // 只经接口操作

    QSignalSpy openedSpy(it, &ITransport::opened);
    QSignalSpy datagramSpy(it, &ITransport::datagramReceived);
    QSignalSpy bytesSpy(it, &ITransport::bytesReceived); // UDP 不该走流信号

    it->open();
    QCOMPARE(openedSpy.size(), 1); // opened 仅表示 bind 成功(LLD §1)
    const quint16 boundPort = transport.localPort();
    QVERIFY(boundPort != 0);
    QVERIFY(it->isOpen());

    const QByteArray payload("frame-bytes");
    QVERIFY(peer.sendTo(boundPort, payload) == payload.size());
    QTRY_COMPARE_WITH_TIMEOUT(datagramSpy.size(), 1, 3000);
    const QVariantList args = datagramSpy.first();
    QCOMPARE(args.at(0).toByteArray(), payload);
    QCOMPARE(args.at(1).value<QHostAddress>(), kLocal);
    QCOMPARE(args.at(2).value<quint16>(), peer.port());
    QCOMPARE(bytesSpy.size(), 0); // 流信号是 TCP/串口专用
    QCOMPARE(transport.unknownSourceDrops(), quint64(0));
}

// 来源隔离(REQ-UDP-001):未配置来源整报丢弃并计数,合法来源不受影响
void TestUdpTransport::unknownSourceDropped()
{
    TestPeer peer;
    QVERIFY(peer.bind());
    TestPeer stranger;
    QVERIFY(stranger.bind());

    UdpTransport transport(makeConfig(0, peer));
    QSignalSpy datagramSpy(&transport, &UdpTransport::datagramReceived);
    transport.open();
    const quint16 boundPort = transport.localPort();

    stranger.sendTo(boundPort, QByteArray("junk"));
    QTRY_COMPARE(transport.unknownSourceDrops(), quint64(1)); // 事件循环驱动收包
    QTest::qWait(100);
    QCOMPARE(datagramSpy.size(), 0);

    peer.sendTo(boundPort, QByteArray("good"));
    QTRY_COMPARE_WITH_TIMEOUT(datagramSpy.size(), 1, 3000);
    QCOMPARE(datagramSpy.first().at(0).toByteArray(), QByteArray("good"));
}

// 同一绑定端点共享 socket(协议 §7):按来源路由互不串扰;
// 单个 transport 关闭不拆共享 socket,全部关闭后端口可重新绑定
void TestUdpTransport::sharedEndpointRoutesBySource()
{
    TestPeer peerA;
    QVERIFY(peerA.bind());
    TestPeer peerB;
    QVERIFY(peerB.bind());

    // 显式端口才能命中共享池(bindPort=0 各自独立绑定);
    // A 先 bind,B 用同一端口复用 → 同一 socket
    const quint16 sharedPort = findFreeUdpPort();
    QVERIFY(sharedPort != 0);
    UdpTransport a(makeConfig(sharedPort, peerA, "sim-udp-a"));
    a.open();
    QCOMPARE(a.localPort(), sharedPort);

    UdpTransport b(makeConfig(sharedPort, peerB, "sim-udp-b"));
    QSignalSpy openedB(&b, &UdpTransport::opened);
    b.open();
    QCOMPARE(openedB.size(), 1); // 复用端点无需再 bind,直接确认
    QCOMPARE(b.localPort(), sharedPort);

    QSignalSpy spyA(&a, &UdpTransport::datagramReceived);
    QSignalSpy spyB(&b, &UdpTransport::datagramReceived);

    peerA.sendTo(sharedPort, QByteArray("for-a"));
    QTRY_COMPARE_WITH_TIMEOUT(spyA.size(), 1, 3000);
    QCOMPARE(spyB.size(), 0);

    peerB.sendTo(sharedPort, QByteArray("for-b"));
    QTRY_COMPARE_WITH_TIMEOUT(spyB.size(), 1, 3000);
    QCOMPARE(spyA.size(), 1); // 各收各的,不重复

    // B 关闭:A 不受影响(共享引用计数未归零)
    QSignalSpy closedB(&b, &UdpTransport::closed);
    b.close();
    QCOMPARE(closedB.size(), 1);
    peerA.sendTo(sharedPort, QByteArray("still-a"));
    QTRY_COMPARE_WITH_TIMEOUT(spyA.size(), 2, 3000);

    // A 也关闭:端点释放,新 transport 可以重新绑定同端口(池中 weak 过期)
    a.close();
    UdpTransport c(makeConfig(sharedPort, peerA, "sim-udp-c"));
    QSignalSpy openedC(&c, &UdpTransport::opened);
    c.open();
    QCOMPARE(openedC.size(), 1);
    QCOMPARE(c.localPort(), sharedPort);
}

// bind 失败(端口被占):按暂时性链路错误上报,不进 Fault
void TestUdpTransport::bindFailureReportsConnectionLost()
{
    QUdpSocket blocker;
    QVERIFY(blocker.bind(kLocal, 0));
    const quint16 takenPort = blocker.localPort();

    TestPeer peer;
    QVERIFY(peer.bind());

    UdpTransport transport(makeConfig(takenPort, peer));
    QSignalSpy errorSpy(&transport, &UdpTransport::errorOccurred);
    QSignalSpy openedSpy(&transport, &UdpTransport::opened);
    QSignalSpy closedSpy(&transport, &UdpTransport::closed);

    transport.open();
    QCOMPARE(openedSpy.size(), 0);
    QCOMPARE(closedSpy.size(), 0); // 从未成功 open 不发 closed
    QCOMPARE(errorSpy.size(), 1);
    const TransportError error = errorSpy.first().at(0).value<TransportError>();
    QCOMPARE(error.code, dms::ErrorCode::ConnectionLost);
    QVERIFY(!error.message.isEmpty());
    QVERIFY(!transport.isOpen());
}

// 重复 open 幂等:已绑定时直接确认 opened(LLD §3 失联保持绑定的重试语义)
void TestUdpTransport::reopenAfterCloseBindsAgain()
{
    TestPeer peer;
    QVERIFY(peer.bind());

    UdpTransport transport(makeConfig(0, peer));
    QSignalSpy openedSpy(&transport, &UdpTransport::opened);
    transport.open();
    const quint16 firstPort = transport.localPort();
    QVERIFY(firstPort != 0);

    // open 幂等:同一绑定直接再确认
    transport.open();
    QCOMPARE(openedSpy.size(), 2);
    QCOMPARE(transport.localPort(), firstPort);

    // close 后重新 open:端点重建(bindPort=0 → 新分配)
    transport.close();
    transport.open();
    QCOMPARE(openedSpy.size(), 3);
    QVERIFY(transport.localPort() != 0);
}

// 写路径:writeDatagram 到配置 peer,writeAccepted 关联 requestId
void TestUdpTransport::writeReachesPeer()
{
    TestPeer peer;
    QVERIFY(peer.bind());

    UdpTransport transport(makeConfig(0, peer));
    QSignalSpy acceptedSpy(&transport, &UdpTransport::writeAccepted);
    transport.open();

    const QByteArray payload("outbound");
    transport.write(payload, 42);
    QCOMPARE(acceptedSpy.size(), 1);
    QCOMPARE(acceptedSpy.first().at(0).value<quint64>(), quint64(42));

    QTRY_VERIFY_WITH_TIMEOUT(peer.socket().hasPendingDatagrams(), 3000);
    const QNetworkDatagram dg = peer.socket().receiveDatagram();
    QCOMPARE(dg.data(), payload);
    QCOMPARE(dg.senderPort(), transport.localPort()); // 源端口 = 绑定端口
}

// closed 恰好一次;close 后不再接收(来源路由已注销)
void TestUdpTransport::closeEmitsClosedExactlyOnce()
{
    TestPeer peer;
    QVERIFY(peer.bind());

    UdpTransport transport(makeConfig(0, peer));
    QSignalSpy closedSpy(&transport, &UdpTransport::closed);
    QSignalSpy datagramSpy(&transport, &UdpTransport::datagramReceived);
    transport.open();
    const quint16 boundPort = transport.localPort();

    transport.close();
    QCOMPARE(closedSpy.size(), 1);
    QVERIFY(!transport.isOpen());

    peer.sendTo(boundPort, QByteArray("late"));
    QTest::qWait(200);
    QCOMPARE(datagramSpy.size(), 0);

    transport.close(); // 二次 close 不再发
    QCOMPARE(closedSpy.size(), 1);
}

QTEST_MAIN(TestUdpTransport)
#include "tst_udptransport.moc"
