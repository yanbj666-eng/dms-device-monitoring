#pragma once

#include <QTcpSocket>
#include <QTimer>

#include "ITransport.h"

namespace dms::transport {

struct TcpTransportConfig {
    QString deviceId;      // 仅用于日志上下文,不影响连接行为
    QString host;
    quint16 port = 0;
    int connectTimeoutMs = 3000; // app.ini connect_timeout_ms
    int maxTxBytes = 65536;      // app.ini max_tx_bytes,含 Qt 内部 bytesToWrite
};

// TCP 实现(LLD §1):
//   - 待发送总量上限 = Qt 写缓冲(bytesToWrite)+ 本次写入,超限拒绝并报
//     SEND_QUEUE_FULL——只限制应用队列而无限填充 Qt 内部缓冲是不够的;
//   - 每次 open 抬 generation,重建 socket;陈旧回调比对失败即丢弃,
//     不得污染新会话;
//   - close() 立即 abort 拆链(确定性,不陪对端等缓冲排空),closed 同步发出;
//   - 不做自动重连(S3 Session 职责)
class TcpTransport : public ITransport
{
    Q_OBJECT

public:
    explicit TcpTransport(TcpTransportConfig config, QObject *parent = nullptr);

    void open() override;
    void close() override;
    void write(const QByteArray &bytes, quint64 requestId) override;
    bool isOpen() const override;

private:
    // 以下槽只处理当前 generation 的回调(open 时捕获比对)
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError err);
    void onReadyRead();

    void emitClosedOnce(); // closed 恰好一次的保证点
    void emitError(dms::ErrorCode code, const QString &message);

    TcpTransportConfig config_;
    QTcpSocket *socket_ = nullptr;   // open() 时在本线程创建(线程红线)
    QTimer *connectTimer_ = nullptr; // 连接超时,同样 open() 时创建
    quint64 generation_ = 0;         // 每次 open/close 递增,使陈旧回调失效
    bool closedEmitted_ = false;
};

} // namespace dms::transport
