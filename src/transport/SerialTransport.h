#pragma once

#include <QObject>
#include <QSerialPort>
#include <QString>

#include "ITransport.h"

namespace dms::transport {

// 串口连接参数(devices.json serial 字段,docs/15 §3 合法域已在 Config 校验)
struct SerialTransportConfig {
    QString deviceId; // 仅用于日志上下文
    QString portName; // COM3 / /dev/ttyUSB0
    qint32 baudRate = 115200;
    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    QSerialPort::Parity parity = QSerialPort::NoParity;
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;
    int maxTxBytes = 65536; // app.ini max_tx_bytes:串口写缓冲同样需要背压
};

// 串口实现(LLD §1/§3):字节流语义与 TCP 相同(FrameDecoder 复用),
// 差异集中在错误分类与生命周期:
//   - open 是同步的(QSerialPort::open 立即返回),opened/errorOccurred
//     在 open() 调用栈内同步发出——会话的槽必须容忍这种重入;
//   - 错误二分(LLD §3"串口暂时拔出可恢复,权限/配置错误进入 Fault"):
//     Permission/UnsupportedOperation → CONFIG_INVALID(Fault,人工修正);
//     DeviceNotFound(重开时不在)/ResourceError(打开后拔出)/OpenError
//     → CONNECTION_LOST(退避重试,插回即恢复);
//   - 参数不支持要明确报错,不悄悄改默认值(docs/15 §3):打开后逐一
//     应用参数并检查返回值,失败即 ConfigInvalid;
//   - 无自动重连(S3 Session 职责);generation 防 open 重建期间的陈旧回调
class SerialTransport : public ITransport
{
    Q_OBJECT

public:
    explicit SerialTransport(SerialTransportConfig config, QObject *parent = nullptr);

    void open() override;
    void close() override;
    void write(const QByteArray &bytes, quint64 requestId) override;
    bool isOpen() const override;

private:
    void onSerialReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);
    bool applyParameters(QString *unsupportedOut); // 打开后应用参数,失败报具体项

    void emitError(dms::ErrorCode code, const QString &message);
    void emitClosedOnce();

    SerialTransportConfig config_;
    QSerialPort *serial_ = nullptr; // open() 时在本线程创建(线程红线)
    quint64 generation_ = 0;        // 每次 open/close 递增,使陈旧回调失效
    bool opening_ = false;          // open() 栈内:Qt 自动发的错误由 open 路径统一上报
    bool closedEmitted_ = false;
};

} // namespace dms::transport
