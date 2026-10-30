#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include "common/Logging.h"

namespace dms::transport {

// 结构化传输错误:code 进日志/上层状态机(LLD §8),message 含系统细节。
// 值类型,允许跨线程经 queued signal 传递
struct TransportError {
    dms::ErrorCode code = dms::ErrorCode::None;
    QString message;
};

// 传输形态:会话状态机据此区分语义(LLD §3)——
//   Udp:无连接概念,失联保持绑定等数据,收到有效帧直接恢复;
//   Tcp/Serial:面向连接/独占端口,失联关链路走退避重连
enum class TransportKind {
    Tcp,
    Udp,
    Serial,
};

// 传输抽象(LLD §1):TCP/串口发 bytesReceived,UDP 另发 datagramReceived(S7)。
// 约束:
//   - open/close/write 全异步,结果只经信号报告,不阻塞调用线程;
//   - writeAccepted 只表示发送入队;设备执行成功需等 ACK(命令生命周期归 S3);
//   - isOpen 仅所属线程调用,GUI 拿快照(HLD §2);
//   - 不做自动重连:重连与退避归 Session(S3);
//   - 实现类须与其 socket/timer 同线程创建和使用(worker 在 moveToThread
//     后的启动槽里 open,见 HLD §2 线程红线)
class ITransport : public QObject
{
    Q_OBJECT

public:
    explicit ITransport(QObject *parent = nullptr)
        : QObject(parent)
    {
        // queued 连接(跨线程)要求自定义参数类型已注册;幂等且廉价
        qRegisterMetaType<dms::transport::TransportError>("dms::transport::TransportError");
    }
    ~ITransport() override = default;

    ITransport(const ITransport &) = delete;
    ITransport &operator=(const ITransport &) = delete;

    // 连接配置在实现类的构造参数中(TcpTransportConfig 等);
    // open 期间与之后的一切结果经 opened/errorOccurred 报告
    virtual void open() = 0;

    // 关闭并停止一切收发;closed 恰好发一次(从未 open 过/已关闭则不发),
    // 且在 close() 返回前同步发出——会话据此区分主动拆链与对端关闭
    virtual void close() = 0;

    // 异步写;requestId 由调用方提供并随 writeAccepted/error 回传,用于关联
    virtual void write(const QByteArray &bytes, quint64 requestId) = 0;

    virtual bool isOpen() const = 0; // 仅所属线程

signals:
    void opened();
    void closed();
    void writeAccepted(quint64 requestId); // 已入队,不等于已送达/已执行
    void errorOccurred(dms::transport::TransportError error);
    void bytesReceived(const QByteArray &bytes);
    // UDP 专用(LLD §1):保留来源与报文边界。来源过滤在 UdpTransport
    // 内部完成(未配置来源整报丢弃),因此 sender 一定是配置的 peer。
    // IO 线程内 direct 连接,不做 queued 跨线程注册
    void datagramReceived(const QByteArray &bytes, const QHostAddress &sender,
                          quint16 senderPort);
};

} // namespace dms::transport

Q_DECLARE_METATYPE(dms::transport::TransportError)
