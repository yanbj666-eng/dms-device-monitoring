#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUuid>

#include "FrameDecoder.h"
#include "MessageCodec.h"
#include "transport/ITransport.h"

namespace dms::domain {

// 会话交付的遥测值对象(跨线程 queued signal 必须是值类型,HLD §2)
struct TelemetryBatch {
    QList<dms::protocol::TelemetrySample> samples; // 仅已知指标
    quint32 unknownMetrics = 0;                    // 结构合法的未知指标计数
    quint32 seq = 0;                               // 发送端帧序号
    quint64 timestampMs = 0;                       // 发送端时间,0=未知
};

// 设备会话(S2 雏形):
//   连接 -> FrameDecoder -> MessageCodec -> 值对象信号;
//   计数与结构化错误进日志(LLD §8)。
// 刻意不做的事(全部 S3):状态机(Online/Degraded/Reconnecting)、
// 心跳/失联判定、自动重连与退避、组帧 2000ms 超时、命令生命周期。
//
// 线程约定:会话与其 transport 同线程创建使用(IO 线程);
// GUI 侧只连接信号消费值对象,不直接调方法(除 S4 以后加的快照接口)
class DeviceSession : public QObject
{
    Q_OBJECT

public:
    // 接管 transport 所有权(reparent 到本会话)
    DeviceSession(QString deviceId, dms::transport::ITransport *transport,
                  QObject *parent = nullptr);

    QString deviceId() const { return deviceId_; }
    QUuid sessionId() const { return sessionId_; } // 本地每会话 UUID,不上帧(LLD §4)

    void start(); // transport->open(),结果经 sessionOpened/sessionError
    void stop();  // transport->close(),结果经 sessionClosed

    // S2 直通用(集成测试写命令);S3 命令生命周期接管后收编
    dms::transport::ITransport *transport() const { return transport_; }

    // 计数:仅所属线程读;framesReceived 只含成功交付的帧
    quint64 framesReceived() const { return framesReceived_; }
    quint32 badCrcFrames() const { return badCrcFrames_; }
    quint32 badHeaderFrames() const { return badHeaderFrames_; }
    quint32 unknownTypeFrames() const { return unknownTypeFrames_; }
    quint32 payloadErrors() const { return payloadErrors_; }

signals:
    void sessionOpened();
    void sessionClosed();
    void heartbeatReceived(quint32 seq, quint64 timestampMs);
    // 结构合法的遥测(可能全是未知指标:samples 空 + unknownMetrics>0,
    // 存活/新鲜度语义由 S3 状态机解释)
    void telemetryReceived(dms::domain::TelemetryBatch batch);
    // status 是 dms::protocol::AckStatus 的数值(quint16 直传,省一次 metatype)
    void commandAckReceived(quint32 seq, quint16 commandId, quint16 status);
    void sessionError(dms::ErrorCode code, QString message);

private:
    void onBytesReceived(const QByteArray &bytes);
    void onTransportError(const dms::transport::TransportError &error);
    void handleTelemetry(const dms::protocol::DecodedFrame &frame);
    void handleCommandAck(const dms::protocol::DecodedFrame &frame);
    dms::LogContext logCtx(dms::ErrorCode code = dms::ErrorCode::None) const;

    QString deviceId_;
    QUuid sessionId_;
    dms::transport::ITransport *transport_ = nullptr;
    dms::protocol::FrameDecoder decoder_; // 每 Session 独立流缓存(LLD §2)

    quint64 framesReceived_ = 0;
    quint32 badCrcFrames_ = 0;
    quint32 badHeaderFrames_ = 0;
    quint32 unknownTypeFrames_ = 0;
    quint32 payloadErrors_ = 0;
};

} // namespace dms::domain

Q_DECLARE_METATYPE(dms::domain::TelemetryBatch)
