#include "TcpTransport.h"

#include <QAbstractSocket>
#include <QMutexLocker>

namespace dms::transport {

namespace {

dms::LogContext netCtx(const QString &deviceId, dms::ErrorCode code)
{
    dms::LogContext ctx;
    ctx.deviceId = deviceId;
    ctx.code = code;
    return ctx;
}

} // namespace

TcpTransport::TcpTransport(TcpTransportConfig config, QObject *parent)
    : ITransport(parent), config_(std::move(config))
{
}

void TcpTransport::open()
{
    // 重复 open:丢弃旧 socket 与定时器并抬 generation。
    // 已排队的旧回调会因 generation 不匹配而失效(deleteLater 亦会断开其连接)
    if (socket_) {
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (connectTimer_) {
        connectTimer_->deleteLater();
        connectTimer_ = nullptr;
    }
    const quint64 gen = ++generation_;
    closedEmitted_ = false;

    // socket/timer 在本线程创建:与调用 open() 的线程(未来的 IO worker)绑定
    socket_ = new QTcpSocket(this);
    connect(socket_, &QTcpSocket::connected, this, [this, gen] {
        if (gen != generation_)
            return; // 陈旧回调:属于已被替换的会话
        onSocketConnected();
    });
    connect(socket_, &QTcpSocket::disconnected, this, [this, gen] {
        if (gen != generation_)
            return;
        onSocketDisconnected();
    });
    connect(socket_, &QTcpSocket::errorOccurred, this, [this, gen](QAbstractSocket::SocketError e) {
        if (gen != generation_)
            return;
        onSocketError(e);
    });
    connect(socket_, &QTcpSocket::readyRead, this, [this, gen] {
        if (gen != generation_)
            return;
        onReadyRead();
    });

    connectTimer_ = new QTimer(this);
    connectTimer_->setSingleShot(true);
    connectTimer_->setInterval(config_.connectTimeoutMs);
    connect(connectTimer_, &QTimer::timeout, this, [this, gen] {
        if (gen != generation_)
            return;
        if (!socket_ || socket_->state() != QAbstractSocket::ConnectingState)
            return;
        socket_->abort();
        const QString msg = QStringLiteral("connect to %1:%2 timed out after %3ms")
                                .arg(config_.host)
                                .arg(config_.port)
                                .arg(config_.connectTimeoutMs);
        emitError(dms::ErrorCode::ConnectTimeout, msg);
        emitClosedOnce();
    });
    connectTimer_->start();

    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net,
                    netCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("connecting to %1:%2").arg(config_.host).arg(config_.port));
    socket_->connectToHost(config_.host, config_.port);
}

void TcpTransport::close()
{
    if (connectTimer_)
        connectTimer_->stop();
    if (!socket_)
        return; // 从未 open:无副作用,不发 closed

    ++generation_;     // 未决回调立即全部失效
    socket_->abort();  // 立即拆除:监控端关闭要确定性,不等对端读走缓冲
    socket_->deleteLater();
    socket_ = nullptr;
    emitClosedOnce();  // close() 返回前同步发出
}

void TcpTransport::write(const QByteArray &bytes, quint64 requestId)
{
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) {
        // 状态错误:按链路不可用上报(Session 在 S3 会用状态机拦截这种调用)
        emitError(dms::ErrorCode::ConnectionLost,
                  QStringLiteral("write on non-connected transport (request %1)")
                      .arg(requestId));
        return;
    }

    // 待发送总量上限(LLD §1):Qt 内部 bytesToWrite + 本次字节一起算,
    // 否则对端不读时应用层会无限垫高缓冲
    const qint64 pending = socket_->bytesToWrite();
    if (pending + bytes.size() > config_.maxTxBytes) {
        emitError(dms::ErrorCode::SendQueueFull,
                  QStringLiteral("tx pending %1 + %2 exceeds limit %3 (request %4)")
                      .arg(pending)
                      .arg(bytes.size())
                      .arg(config_.maxTxBytes)
                      .arg(requestId));
        return;
    }

    const qint64 written = socket_->write(bytes);
    if (written != bytes.size()) {
        // 缓冲式 socket 正常要么全入队要么 -1;不一致按失败处理
        emitError(dms::ErrorCode::ConnectionLost,
                  QStringLiteral("write failed after %1/%2 bytes (request %3): %4")
                      .arg(written)
                      .arg(bytes.size())
                      .arg(requestId)
                      .arg(socket_ ? socket_->errorString() : QString()));
        return;
    }

    emit writeAccepted(requestId); // 只代表入队;送达与否要靠 ACK(S3)
    dms::logRawFrame(true, config_.deviceId, bytes);
}

bool TcpTransport::isOpen() const
{
    return socket_ && socket_->state() == QAbstractSocket::ConnectedState;
}

void TcpTransport::onSocketConnected()
{
    if (connectTimer_)
        connectTimer_->stop();
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net,
                    netCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("connected to %1:%2").arg(config_.host).arg(config_.port));
    emit opened();
}

void TcpTransport::onSocketDisconnected()
{
    emitClosedOnce();
}

void TcpTransport::onSocketError(QAbstractSocket::SocketError err)
{
    if (connectTimer_)
        connectTimer_->stop();
    // LLD §8 未按 socket 错误细分错误码:连接失败/链路错误统一 CONNECTION_LOST,
    // 系统细节进 message;CONNECT_TIMEOUT 由定时器路径单独上报
    const QString detail = socket_ ? socket_->errorString() : QString();
    emitError(dms::ErrorCode::ConnectionLost,
              QStringLiteral("socket error %1: %2").arg(int(err)).arg(detail));

    // 多数错误随后到达 Unconnected;个别路径 Qt 不再发 disconnected,这里兜底
    if (!socket_ || socket_->state() == QAbstractSocket::UnconnectedState)
        emitClosedOnce();
}

void TcpTransport::onReadyRead()
{
    if (!socket_)
        return;
    const QByteArray bytes = socket_->readAll();
    if (bytes.isEmpty())
        return;
    dms::logRawFrame(false, config_.deviceId, bytes);
    emit bytesReceived(bytes);
}

void TcpTransport::emitClosedOnce()
{
    if (closedEmitted_)
        return;
    closedEmitted_ = true;
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net,
                    netCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("transport closed"));
    emit closed();
}

void TcpTransport::emitError(dms::ErrorCode code, const QString &message)
{
    dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Net, netCtx(config_.deviceId, code),
                    message);
    emit errorOccurred(TransportError{code, message});
}

} // namespace dms::transport
