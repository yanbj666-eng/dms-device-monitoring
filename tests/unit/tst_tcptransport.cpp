// tst_tcptransport:LLD §1 TcpTransport 测试
// 本机 QTcpServer 做对端:建链/回环收发/拒绝/超时/发送上限/关闭语义
// 生成陈旧回调防护(generation)属防御式设计,由 code review 与 S3 集成覆盖
#include <QtTest>

#include <QTcpServer>
#include <QTcpSocket>

#include "transport/TcpTransport.h"

using dms::transport::ITransport;
using dms::transport::TcpTransport;
using dms::transport::TcpTransportConfig;
using dms::transport::TransportError;

namespace {

dms::transport::TransportError takeError(QSignalSpy &spy)
{
    return spy.first().at(0).value<TransportError>();
}

// 测试对端:监听 127.0.0.1:0(内核分配空闲端口),只保留最近一条连接
class TestPeer
{
public:
    TestPeer()
    {
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (server_.hasPendingConnections())
                adopt(server_.nextPendingConnection());
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server_.serverPort(); }
    QTcpSocket *client() const { return client_; } // 可能仍为 null(未接入)

    void closeCurrent()
    {
        if (client_) {
            client_->abort();
            client_->deleteLater();
            client_ = nullptr;
        }
    }

private:
    void adopt(QTcpSocket *s)
    {
        closeCurrent(); // 新连接顶替旧连接
        client_ = s;
    }

    QTcpServer server_;
    QTcpSocket *client_ = nullptr;
};

TcpTransportConfig localConfig(quint16 port, int connectTimeoutMs = 3000,
                               int maxTxBytes = 65536)
{
    TcpTransportConfig cfg;
    cfg.deviceId = QStringLiteral("sim-tcp-01");
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = port;
    cfg.connectTimeoutMs = connectTimeoutMs;
    cfg.maxTxBytes = maxTxBytes;
    return cfg;
}

} // namespace

class TestTcpTransport : public QObject
{
    Q_OBJECT

private slots:
    void connectAndRoundtrip();
    void connectFailureReportsStructuredError();
    void sendQueueFull();
    void writeBeforeOpenRejected();
    void closeIsImmediateAndFinal();
    void serverClosesConnection();
};

// 经 ITransport* 使用(多态冒烟):建链、双向字节回环、writeAccepted 关联
void TestTcpTransport::connectAndRoundtrip()
{
    TestPeer peer;
    QVERIFY(peer.listen());

    TcpTransport transport(localConfig(peer.port()));
    ITransport *it = &transport; // 只通过接口操作

    QSignalSpy openedSpy(it, &ITransport::opened);
    QSignalSpy closedSpy(it, &ITransport::closed);
    QSignalSpy acceptedSpy(it, &ITransport::writeAccepted);

    it->open();
    QTRY_COMPARE(openedSpy.size(), 1);
    QVERIFY(it->isOpen());

    QTRY_VERIFY(peer.client() != nullptr);
    QByteArray serverGot;
    QObject::connect(peer.client(), &QTcpSocket::readyRead, [&] {
        serverGot += peer.client()->readAll();
    });

    const QByteArray payload = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    it->write(payload, 42);
    QTRY_COMPARE(acceptedSpy.size(), 1);
    QCOMPARE(acceptedSpy.first().at(0).toULongLong(), quint64(42)); // requestId 回传
    QTRY_VERIFY(serverGot == payload);

    QSignalSpy rxSpy(it, &ITransport::bytesReceived);
    peer.client()->write(QByteArray::fromHex("deadbeef"));
    QTRY_COMPARE(rxSpy.size(), 1);
    QCOMPARE(rxSpy.first().at(0).toByteArray(), QByteArray::fromHex("deadbeef"));

    it->close();
    QTRY_COMPARE(closedSpy.size(), 1);
}

// 目标端口不可达:结果必为两种合法结构化错误之一——
//   ConnectionLost(栈快速回 RST/ICMP)或 ConnectTimeout(静默丢弃,定时器兜底)。
// 本机实测走 3000ms 超时分支(安全软件丢弃回环 SYN),两种网络栈都覆盖
void TestTcpTransport::connectFailureReportsStructuredError()
{
    // 先占后放,拿到一个确定空闲的本机端口
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = probe.serverPort();
    probe.close();

    TcpTransport transport(localConfig(port, 400));
    QSignalSpy errorSpy(&transport, &ITransport::errorOccurred);
    QSignalSpy closedSpy(&transport, &ITransport::closed);

    transport.open();
    QTRY_VERIFY(errorSpy.size() >= 1);
    const TransportError err = takeError(errorSpy);
    QVERIFY(err.code == dms::ErrorCode::ConnectionLost
            || err.code == dms::ErrorCode::ConnectTimeout);
    QVERIFY(!err.message.isEmpty()); // 系统细节必须随行
    QTRY_COMPARE(closedSpy.size(), 1);
}

// 对端不读:Qt 写缓冲 + 内核缓冲终将填满,超过 64KiB 上限后拒绝并报 SEND_QUEUE_FULL
void TestTcpTransport::sendQueueFull()
{
    TestPeer peer;
    QVERIFY(peer.listen());

    TcpTransport transport(localConfig(peer.port()));
    QSignalSpy openedSpy(&transport, &ITransport::opened);
    QSignalSpy errorSpy(&transport, &ITransport::errorOccurred);
    QSignalSpy acceptedSpy(&transport, &ITransport::writeAccepted);

    transport.open();
    QTRY_COMPARE(openedSpy.size(), 1);
    QTRY_VERIFY(peer.client() != nullptr);

    // 64KiB × 512 = 32MiB,远超任何回环缓冲;对端应用层一直不读
    const QByteArray chunk(65536, 'x');
    for (int i = 0; i < 512 && errorSpy.isEmpty(); ++i) {
        transport.write(chunk, quint64(i));
        QTest::qWait(1); // 让 socket notifier 尝试向内核刷出
    }
    QTRY_VERIFY(!errorSpy.isEmpty());
    QCOMPARE(takeError(errorSpy).code, dms::ErrorCode::SendQueueFull);
    QVERIFY(acceptedSpy.size() >= 1); // 撑满前有成功入队
}

// 未建链就写:结构化错误,不得伪报 writeAccepted
void TestTcpTransport::writeBeforeOpenRejected()
{
    TcpTransport transport(localConfig(9000));
    QSignalSpy errorSpy(&transport, &ITransport::errorOccurred);
    QSignalSpy acceptedSpy(&transport, &ITransport::writeAccepted);

    transport.write(QByteArray::fromHex("aabb"), 7);
    QCOMPARE(errorSpy.size(), 1); // emit 是同步的
    QCOMPARE(takeError(errorSpy).code, dms::ErrorCode::ConnectionLost);
    QCOMPARE(acceptedSpy.size(), 0);
}

// close():closed 同步发出且只发一次;关闭后对端数据不再上抛;二次 close 幂等
void TestTcpTransport::closeIsImmediateAndFinal()
{
    TestPeer peer;
    QVERIFY(peer.listen());

    TcpTransport transport(localConfig(peer.port()));
    QSignalSpy closedSpy(&transport, &ITransport::closed);
    transport.open();
    QTRY_VERIFY(peer.client() != nullptr);
    QTest::qWait(50); // 等 opened(此处不关心)

    transport.close();
    QCOMPARE(closedSpy.size(), 1); // close() 返回前同步发出
    QVERIFY(!transport.isOpen());

    QSignalSpy rxSpy(&transport, &ITransport::bytesReceived);
    if (peer.client()) // 服务端视角连接可能已被 RST,写失败不影响断言
        peer.client()->write(QByteArray::fromHex("0102"));
    QTest::qWait(150);
    QCOMPARE(rxSpy.size(), 0); // 关闭后不得再上抛数据

    transport.close(); // 幂等
    QCOMPARE(closedSpy.size(), 1);
}

// 对端主动断链:CONNECTION_LOST + closed
void TestTcpTransport::serverClosesConnection()
{
    TestPeer peer;
    QVERIFY(peer.listen());

    TcpTransport transport(localConfig(peer.port()));
    QSignalSpy openedSpy(&transport, &ITransport::opened);
    QSignalSpy errorSpy(&transport, &ITransport::errorOccurred);
    QSignalSpy closedSpy(&transport, &ITransport::closed);

    transport.open();
    QTRY_COMPARE(openedSpy.size(), 1);
    QTRY_VERIFY(peer.client() != nullptr);

    peer.closeCurrent();
    QTRY_VERIFY(errorSpy.size() >= 1);
    QCOMPARE(takeError(errorSpy).code, dms::ErrorCode::ConnectionLost);
    QTRY_COMPARE(closedSpy.size(), 1);
}

QTEST_MAIN(TestTcpTransport)
#include "tst_tcptransport.moc"
