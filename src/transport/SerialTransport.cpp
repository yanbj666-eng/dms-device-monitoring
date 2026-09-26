#include "SerialTransport.h"

#include <QIODevice>

#include "common/Logging.h"

namespace dms::transport {

namespace {

dms::LogContext serialCtx(const QString &deviceId, dms::ErrorCode code)
{
    dms::LogContext ctx;
    ctx.deviceId = deviceId;
    ctx.code = code;
    return ctx;
}

// 错误二分(LLD §3):权限/参数不支持属本地不可恢复(Fault),
// 设备暂时不在/拔出属链路暂时性错误(退避重试,插回即恢复)
dms::ErrorCode mapSerialError(QSerialPort::SerialPortError error)
{
    switch (error) {
    case QSerialPort::PermissionError:
    case QSerialPort::UnsupportedOperationError:
        return dms::ErrorCode::ConfigInvalid;
    default:
        return dms::ErrorCode::ConnectionLost;
    }
}

} // namespace

SerialTransport::SerialTransport(SerialTransportConfig config, QObject *parent)
    : ITransport(parent), config_(std::move(config))
{
}

void SerialTransport::open()
{
    // 重复 open:丢弃旧串口并抬 generation,已排队的旧回调失效
    if (serial_) {
        serial_->deleteLater();
        serial_ = nullptr;
    }
    const quint64 gen = ++generation_;
    closedEmitted_ = false;

    auto *serial = new QSerialPort(this); // 本线程创建(线程红线)
    serial->setPortName(config_.portName);
    connect(serial, &QSerialPort::readyRead, this, [this, gen] {
        if (gen != generation_)
            return; // 陈旧回调:属于已被替换的会话
        onSerialReadyRead();
    });
    connect(serial, &QSerialPort::errorOccurred, this,
            [this, gen](QSerialPort::SerialPortError e) {
                if (gen != generation_)
                    return;
                onSerialError(e);
            });

    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Serial,
                    serialCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("opening serial port %1").arg(config_.portName));
    // open 失败时 Qt 会同步 emit errorOccurred:那条错误由本函数的失败路径
    // 统一上报(带完整上下文),onSerialError 抑制期内直接忽略,避免双发
    opening_ = true;
    const bool openedOk = serial->open(QIODevice::ReadWrite);
    opening_ = false;
    if (!openedOk) {
        // 打开失败是同步的:此处发出的 errorOccurred 会在 open() 的调用栈内
        // 直达会话(可能触发 close());serial_ 尚未登记,close() 恒为 no-op
        const QSerialPort::SerialPortError e = serial->error();
        const QString detail = serial->errorString();
        serial->deleteLater();
        emitError(mapSerialError(e),
                  QStringLiteral("serial open %1 failed: %2")
                      .arg(config_.portName)
                      .arg(detail));
        return;
    }

    // 参数逐项应用并检查(docs/15 §3:不支持要明确报错,不改默认值)。
    // 端口已成功打开,失败路径必须关闭它
    serial_ = serial;
    QString unsupported;
    if (!applyParameters(&unsupported)) {
        serial_->close();
        ++generation_; // 参数失败的串口不再回调
        emitError(dms::ErrorCode::ConfigInvalid,
                  QStringLiteral("serial %1 does not support: %2")
                      .arg(config_.portName)
                      .arg(unsupported));
        close(); // 走统一路径发 closed(会话侧按 Fault 处理)
        return;
    }

    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Serial,
                    serialCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("serial %1 open at %2 baud")
                        .arg(config_.portName)
                        .arg(config_.baudRate));
    emit opened(); // 同步发出:串口打开没有异步握手阶段
}

void SerialTransport::close()
{
    if (!serial_)
        return; // 从未成功 open:无副作用,不发 closed

    ++generation_; // 未决回调立即失效
    serial_->close(); // 同步关闭,无网络式延迟
    serial_->deleteLater();
    serial_ = nullptr;
    emitClosedOnce(); // close() 返回前同步发出
}

void SerialTransport::write(const QByteArray &bytes, quint64 requestId)
{
    if (!serial_ || !serial_->isOpen()) {
        emitError(dms::ErrorCode::ConnectionLost,
                  QStringLiteral("write on closed serial port (request %1)")
                      .arg(requestId));
        return;
    }

    // 与 TCP 同源的背压模型(LLD §1):Qt 写缓冲 + 本次写入一起算。
    // 串口 115200 全速也只有 ~11.5KB/s,慢对端堆积比 TCP 更常见
    const qint64 pending = serial_->bytesToWrite();
    if (pending + bytes.size() > config_.maxTxBytes) {
        emitError(dms::ErrorCode::SendQueueFull,
                  QStringLiteral("tx pending %1 + %2 exceeds limit %3 (request %4)")
                      .arg(pending)
                      .arg(bytes.size())
                      .arg(config_.maxTxBytes)
                      .arg(requestId));
        return;
    }

    const qint64 written = serial_->write(bytes);
    if (written != bytes.size()) {
        emitError(dms::ErrorCode::ConnectionLost,
                  QStringLiteral("serial write %1/%2 bytes (request %3): %4")
                      .arg(written)
                      .arg(bytes.size())
                      .arg(requestId)
                      .arg(serial_ ? serial_->errorString() : QString()));
        return;
    }

    emit writeAccepted(requestId);
    dms::logRawFrame(true, config_.deviceId, bytes);
}

bool SerialTransport::isOpen() const
{
    return serial_ && serial_->isOpen();
}

void SerialTransport::onSerialReadyRead()
{
    if (!serial_)
        return;
    const QByteArray bytes = serial_->readAll();
    if (bytes.isEmpty())
        return;
    dms::logRawFrame(false, config_.deviceId, bytes);
    emit bytesReceived(bytes);
}

void SerialTransport::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;
    if (opening_)
        return; // open() 栈内 Qt 自动发的错误:由 open 失败路径统一上报
    // 拔出(ResourceError)/权限/参数错误都先上报;是否关闭、进 Fault 还是
    // 退避,由会话按错误码决定(LLD §3)。QSerialPort 遇到部分错误会自动
    // 关闭端口,这里不重复发 closed——closed 只在 close() 路径上发一次
    emitError(mapSerialError(error),
              QStringLiteral("serial %1 error %2: %3")
                  .arg(config_.portName)
                  .arg(int(error))
                  .arg(serial_ ? serial_->errorString() : QString()));
}

bool SerialTransport::applyParameters(QString *unsupportedOut)
{
    if (!serial_)
        return false;
    QStringList failed;
    if (!serial_->setBaudRate(config_.baudRate))
        failed << QStringLiteral("baud_rate=%1").arg(config_.baudRate);
    if (!serial_->setDataBits(config_.dataBits))
        failed << QStringLiteral("data_bits");
    if (!serial_->setParity(config_.parity))
        failed << QStringLiteral("parity");
    if (!serial_->setStopBits(config_.stopBits))
        failed << QStringLiteral("stop_bits");
    if (!serial_->setFlowControl(config_.flowControl))
        failed << QStringLiteral("flow_control");
    if (!failed.isEmpty() && unsupportedOut)
        *unsupportedOut = failed.join(QLatin1String(", "));
    return failed.isEmpty();
}

void SerialTransport::emitError(dms::ErrorCode code, const QString &message)
{
    dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Serial,
                    serialCtx(config_.deviceId, code), message);
    emit errorOccurred(TransportError{code, message});
}

void SerialTransport::emitClosedOnce()
{
    if (closedEmitted_)
        return;
    closedEmitted_ = true;
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Serial,
                    serialCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("serial transport closed"));
    emit closed();
}

} // namespace dms::transport
