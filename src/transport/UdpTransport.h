#pragma once

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>
#include <QString>

#include <memory>

#include "ITransport.h"

namespace dms::transport {

class UdpTransport; // UdpEndpointSocket 的路由表需要

struct UdpTransportConfig {
    QString deviceId;          // 仅用于日志上下文
    QHostAddress bindAddress;  // 本地绑定端点:opened 只表示 bind 成功(LLD §1)
    quint16 bindPort = 0;
    QHostAddress peerAddress;  // 唯一合法来源,其余整报丢弃(REQ-UDP-001)
    quint16 peerPort = 0;
};

// 同一 (bindAddress, bindPort) 共享的 socket 与来源路由(协议 §7
// "同一本地端口共享一个 socket")。由 UdpTransport 经 shared_ptr 持有,
// 最后一个引用释放才真正关 socket。仅限 IO 线程使用(线程约定同 isOpen)
class UdpEndpointSocket : public QObject
{
    Q_OBJECT

public:
    UdpEndpointSocket(QHostAddress bindAddress, quint16 bindPort,
                      QObject *parent = nullptr);

    bool bind(QString *errorOut);
    // 注册/注销来源路由;冲突(Config 四元组唯一性之外的双保险)返回 false
    bool addRoute(const QHostAddress &peer, quint16 peerPort, UdpTransport *transport);
    void removeRoute(const QHostAddress &peer, quint16 peerPort);

    qint64 sendTo(const QByteArray &bytes, const QHostAddress &dest, quint16 destPort);
    QString errorString() const;             // socket 错误细节转发(诊断日志用)
    quint16 localPort() const;               // 实际绑定端口(0 = 未绑定;测试/诊断用)
    quint64 unknownSourceDrops() const { return unknownSourceDrops_; }

private:
    void onReadyRead();

    QUdpSocket socket_;
    QHostAddress bindAddress_; // bind() 用构造参数,失败信息带原始端点
    quint16 bindPort_ = 0;
    QHash<QString, UdpTransport *> routes_; // key = "ip:port"
    quint64 unknownSourceDrops_ = 0;
};

// UDP 实现(协议 §7/LLD §1/§3):
//   - open = bind,没有对端握手;重复 open 幂等:已绑定时直接确认 opened,
//     会话层退避重试对 UDP 是"确认仍在等数据",不是重建连接;
//   - 来源隔离在传输层完成:未配置来源的数据报整报丢弃并计数,
//     上层收到的一定来自配置 peer(REQ-UDP-001);
//   - 失联不关绑定(LLD §3"UDP 保持绑定等待数据"):close() 只注销来源
//     路由并停收,socket 是否真关由共享引用计数决定;
//   - 报文边界天然保留:一个数据报一次 datagramReceived,不跨报拼接;
//   - bind 失败按暂时性错误上报(CONNECTION_LOST):端口被占等场景
//     退避重试可恢复,不进 Fault
class UdpTransport : public ITransport
{
    Q_OBJECT

public:
    explicit UdpTransport(UdpTransportConfig config, QObject *parent = nullptr);
    ~UdpTransport() override; // 兜底注销来源路由,防共享表悬垂

    void open() override;
    void close() override;
    void write(const QByteArray &bytes, quint64 requestId) override;
    bool isOpen() const override;

    // 诊断:实际绑定端口(bindPort=0 时由内核分配;测试/装配日志用)
    quint16 localPort() const;
    // 诊断:绑定端点上未知来源的整报丢弃计数(端点级,共享者同值)
    quint64 unknownSourceDrops() const;

private:
    UdpTransportConfig config_;
    std::shared_ptr<UdpEndpointSocket> endpoint_; // null = 从未成功 open
    bool closedEmitted_ = false;

    void emitError(dms::ErrorCode code, const QString &message);
    void emitClosedOnce();
};

} // namespace dms::transport
