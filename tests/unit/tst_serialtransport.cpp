// tst_serialtransport:LLD §1/§3 SerialTransport 测试
// 本机无物理/虚拟串口,覆盖可在任意环境复现的错误路径:
// 打开不存在端口的错误分类(暂时性,可恢复)、close/write 的状态防御。
// 参数应用成功路径与拔出恢复由集成测试 TC-SERIAL-001(虚拟串口对)覆盖;
// 无串口环境下该必需用例按测试计划记 Blocked(docs/07 §7)
#include <QtTest>

#include "transport/SerialTransport.h"

using dms::transport::ITransport;
using dms::transport::SerialTransport;
using dms::transport::SerialTransportConfig;
using dms::transport::TransportError;

namespace {

SerialTransportConfig missingPortConfig()
{
    SerialTransportConfig cfg;
    cfg.deviceId = QStringLiteral("sim-serial-01");
    // 高概率不存在的端口名:Windows 上打开失败报 DeviceNotFoundError
    cfg.portName = QStringLiteral("COM_NONEXIST_DMS");
    return cfg;
}

} // namespace

class TestSerialTransport : public QObject
{
    Q_OBJECT

private slots:
    void openMissingPortFailsRecoverably();
    void closeBeforeOpenIsNoop();
    void writeBeforeOpenRejected();
};

// 暂时拔出/设备不在的重连路径基石:打开失败映射 CONNECTION_LOST,
// 会话按可恢复链路错误退避重试,而不是进 Fault(LLD §3)
void TestSerialTransport::openMissingPortFailsRecoverably()
{
    SerialTransport transport(missingPortConfig());
    ITransport *it = &transport;

    QSignalSpy errorSpy(it, &ITransport::errorOccurred);
    QSignalSpy openedSpy(it, &ITransport::opened);
    QSignalSpy closedSpy(it, &ITransport::closed);

    it->open(); // QSerialPort::open 同步,信号在返回前已发出
    QCOMPARE(openedSpy.size(), 0);
    QCOMPARE(closedSpy.size(), 0); // 从未成功 open 不发 closed
    QCOMPARE(errorSpy.size(), 1);
    const TransportError error = errorSpy.first().at(0).value<TransportError>();
    QCOMPARE(error.code, dms::ErrorCode::ConnectionLost);
    QVERIFY(error.message.contains(QStringLiteral("COM_NONEXIST_DMS")));
    QVERIFY(!it->isOpen());

    // 同步错误风暴下的重入安全:会话收到错误可能立即 close(),这里模拟
    it->close();
    QCOMPARE(closedSpy.size(), 0); // 仍未成功 open 过,依旧无副作用
}

// 从未 open 过:close() 无任何信号(ITransport 契约)
void TestSerialTransport::closeBeforeOpenIsNoop()
{
    SerialTransport transport(missingPortConfig());
    QSignalSpy closedSpy(&transport, &SerialTransport::closed);
    transport.close();
    QCOMPARE(closedSpy.size(), 0);
    QVERIFY(!transport.isOpen());
}

// 未打开时写:状态错误按链路不可用上报(与 TcpTransport 同语义)
void TestSerialTransport::writeBeforeOpenRejected()
{
    SerialTransport transport(missingPortConfig());
    QSignalSpy errorSpy(&transport, &SerialTransport::errorOccurred);
    transport.write(QByteArray("x"), 7);
    QCOMPARE(errorSpy.size(), 1);
    const TransportError error = errorSpy.first().at(0).value<TransportError>();
    QCOMPARE(error.code, dms::ErrorCode::ConnectionLost);
}

QTEST_MAIN(TestSerialTransport)
#include "tst_serialtransport.moc"
