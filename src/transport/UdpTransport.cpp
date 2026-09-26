#include "UdpTransport.h"

#include <QNetworkDatagram>

#include "common/Logging.h"

namespace dms::transport {

namespace {

dms::LogContext netCtx(const QString &deviceId, dms::ErrorCode code)
{
    dms::LogContext ctx;
    ctx.deviceId = deviceId;
    ctx.code = code;
    return ctx;
}

QString routeKey(const QHostAddress &address, quint16 port)
{
    return address.toString() + QLatin1Char(':') + QString::number(port);
}

// 进程级端点池:同一绑定端点复用 socket(协议 §7)。
// 仅 IO 线程访问,与 ITransport::isOpen 同一线程约定,不加锁;
// weak_ptr 让上一批 transport 全部关闭后端点自然过期,端口可重新 bind
QHash<QString, std::weak_ptr<UdpEndpointSocket>> &endpointPool()
{
    static QHash<QString, std::weak_ptr<UdpEndpointSocket>> pool;
    return pool;
}

} // namespace

// ---- UdpEndpointSocket ----

UdpEndpointSocket::UdpEndpointSocket(QHostAddress bindAddress, quint16 bindPort,
                                     QObject *parent)
    : QObject(parent), bindAddress_(bindAddress), bindPort_(bindPort)
{
    // socket 与本对象同线程创建:绑定它的 transport 在 IO 线程 open(),
    // 此后收发/析构全部发生在该线程(线程红线,HLD §2)
    connect(&socket_, &QUdpSocket::readyRead, this, &UdpEndpointSocket::onReadyRead);
}

bool UdpEndpointSocket::bind(QString *errorOut)
{
    // ShareAddress|ReuseAddressHint:允许同机其他进程占用同端口时快速失败
    // 以外的场景仍语义正确;来源路由保证数据报不会串设备(协议 §7)
    if (!socket_.bind(bindAddress_, bindPort_)) {
        if (errorOut)
            *errorOut = socket_.errorString();
        return false;
    }
    return true;
}

bool UdpEndpointSocket::addRoute(const QHostAddress &peer, quint16 peerPort,
                                 UdpTransport *transport)
{
    const QString key = routeKey(peer, peerPort);
    if (routes_.contains(key))
        return false; // 来源映射冲突:协议 §7 要求映射不得冲突
    routes_.insert(key, transport);
    return true;
}

void UdpEndpointSocket::removeRoute(const QHostAddress &peer, quint16 peerPort)
{
    routes_.remove(routeKey(peer, peerPort));
}

qint64 UdpEndpointSocket::sendTo(const QByteArray &bytes, const QHostAddress &dest,
                                 quint16 destPort)
{
    return socket_.writeDatagram(bytes, dest, destPort);
}

QString UdpEndpointSocket::errorString() const
{
    return socket_.errorString();
}

quint16 UdpEndpointSocket::localPort() const
{
    return socket_.localPort();
}

void UdpEndpointSocket::onReadyRead()
{
    while (socket_.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket_.receiveDatagram();
        const QHostAddress &sender = datagram.senderAddress();
        const quint16 senderPort = datagram.senderPort();
        const auto it = routes_.constFind(routeKey(sender, senderPort));
        if (it == routes_.constEnd()) {
            // 未配置来源:整报丢弃(REQ-UDP-001),不解析不缓存
            ++unknownSourceDrops_;
            dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Net,
                            netCtx(QString(), dms::ErrorCode::None),
                            QStringLiteral("datagram from unknown source %1:%2 dropped "
                                           "(total %3)")
                                .arg(sender.toString())
                                .arg(senderPort)
                                .arg(unknownSourceDrops_));
            continue;
        }
        // 报文边界保留:一报一信号,上层按单报解码(协议 §7)
        emit it.value()->datagramReceived(datagram.data(), sender, senderPort);
    }
}

// ---- UdpTransport ----

UdpTransport::UdpTransport(UdpTransportConfig config, QObject *parent)
    : ITransport(parent), config_(std::move(config))
{
}

UdpTransport::~UdpTransport()
{
    // 正常路径 close() 已注销;这里兜底会话异常销毁的情形,
    // 防止共享端点的路由表留下悬垂指针
    if (endpoint_)
        endpoint_->removeRoute(config_.peerAddress, config_.peerPort);
}

void UdpTransport::open()
{
    if (endpoint_) {
        // 幂等(LLD §3:UDP 失联保持绑定):退避重试点确认仍在等数据
        emit opened();
        return;
    }
    closedEmitted_ = false;

    // 同端点复用(协议 §7);weak 过期 = 上一批用户已全部关闭,重建。
    // bindPort=0(内核自动分配,测试/诊断用)不进池:两个 0 无法在配置上
    // 对上号,各自独立绑定;生产配置端口 1-65535(docs/15 §3)恒可共享
    const bool shareable = config_.bindPort != 0;
    const QString key = routeKey(config_.bindAddress, config_.bindPort);
    auto &pool = endpointPool();
    std::shared_ptr<UdpEndpointSocket> endpoint;
    if (shareable)
        endpoint = pool.value(key).lock();
    if (!endpoint) {
        endpoint = std::make_shared<UdpEndpointSocket>(config_.bindAddress,
                                                       config_.bindPort);
        QString error;
        if (!endpoint->bind(&error)) {
            // 暂时性错误(端口被占等):按 CONNECTION_LOST 退避重试,
            // 不进 Fault——占用者退出后重试即恢复
            emitError(dms::ErrorCode::ConnectionLost,
                      QStringLiteral("udp bind %1:%2 failed: %3")
                          .arg(config_.bindAddress.toString())
                          .arg(config_.bindPort)
                          .arg(error));
            return; // 失败端点不入池,shared_ptr 离开作用域即销毁
        }
        if (shareable)
            pool.insert(key, endpoint);
    }

    if (!endpoint->addRoute(config_.peerAddress, config_.peerPort, this)) {
        // Config 层四元组唯一性之外的运行期双保险
        emitError(dms::ErrorCode::ConfigInvalid,
                  QStringLiteral("udp route conflict on %1:%2 for peer %3:%4")
                      .arg(config_.bindAddress.toString())
                      .arg(config_.bindPort)
                      .arg(config_.peerAddress.toString())
                      .arg(config_.peerPort));
        return;
    }

    endpoint_ = std::move(endpoint);
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net,
                    netCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("udp bound on %1:%2, expecting peer %3:%4")
                        .arg(config_.bindAddress.toString())
                        .arg(config_.bindPort)
                        .arg(config_.peerAddress.toString())
                        .arg(config_.peerPort));
    emit opened(); // 仅表示 bind 成功,不代表对端存在(LLD §1)
}

void UdpTransport::close()
{
    if (!endpoint_)
        return; // 从未成功 open:无副作用,不发 closed

    endpoint_->removeRoute(config_.peerAddress, config_.peerPort);
    endpoint_.reset(); // 最后一个引用释放时 socket 才真正关闭
    emitClosedOnce();
}

void UdpTransport::write(const QByteArray &bytes, quint64 requestId)
{
    if (!endpoint_) {
        emitError(dms::ErrorCode::ConnectionLost,
                  QStringLiteral("write before bind (request %1)").arg(requestId));
        return;
    }
    // UDP 无发送缓冲累积语义(writeDatagram 同步入内核),64KiB 上限的
    // 背压模型不适用;正常装配下监控端不会向 UDP 设备写数据(不控制)
    const qint64 sent = endpoint_->sendTo(bytes, config_.peerAddress, config_.peerPort);
    if (sent != bytes.size()) {
        emitError(dms::ErrorCode::ConnectionLost,
                  QStringLiteral("writeDatagram %1/%2 bytes (request %3): %4")
                      .arg(sent)
                      .arg(bytes.size())
                      .arg(requestId)
                      .arg(endpoint_->errorString()));
        return;
    }
    emit writeAccepted(requestId);
    dms::logRawFrame(true, config_.deviceId, bytes);
}

bool UdpTransport::isOpen() const
{
    return endpoint_ != nullptr; // "绑定中"而非"已连接":UDP 无连接语义
}

quint16 UdpTransport::localPort() const
{
    return endpoint_ ? endpoint_->localPort() : 0;
}

quint64 UdpTransport::unknownSourceDrops() const
{
    return endpoint_ ? endpoint_->unknownSourceDrops() : 0;
}

void UdpTransport::emitError(dms::ErrorCode code, const QString &message)
{
    dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Net,
                    netCtx(config_.deviceId, code), message);
    emit errorOccurred(TransportError{code, message});
}

void UdpTransport::emitClosedOnce()
{
    if (closedEmitted_)
        return;
    closedEmitted_ = true;
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net,
                    netCtx(config_.deviceId, dms::ErrorCode::None),
                    QStringLiteral("udp transport closed"));
    emit closed();
}

} // namespace dms::transport
