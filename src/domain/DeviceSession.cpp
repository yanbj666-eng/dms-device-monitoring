#include "DeviceSession.h"

#include "common/Logging.h"

namespace dms::domain {

using dms::protocol::AckStatus;
using dms::protocol::DecodeError;
using dms::protocol::DecodedFrame;
using dms::protocol::FrameDecoder;
using dms::protocol::MessageCodec;
using dms::protocol::MessageType;
using dms::protocol::TelemetryResult;

DeviceSession::DeviceSession(QString deviceId, dms::transport::ITransport *transport,
                             QObject *parent)
    : QObject(parent),
      deviceId_(std::move(deviceId)),
      sessionId_(QUuid::createUuid()), // 本地会话标识,重启即新会话(LLD §4)
      transport_(transport)
{
    Q_ASSERT(transport_);
    transport_->setParent(this); // 所有权归会话(Qt parent-child,CLAUDE.md 约定)

    // queued 连接(跨线程消费信号)需要值对象已注册
    qRegisterMetaType<dms::domain::TelemetryBatch>("dms::domain::TelemetryBatch");

    connect(transport_, &dms::transport::ITransport::opened, this,
            &DeviceSession::sessionOpened);
    connect(transport_, &dms::transport::ITransport::closed, this,
            &DeviceSession::sessionClosed);
    connect(transport_, &dms::transport::ITransport::bytesReceived, this,
            &DeviceSession::onBytesReceived);
    connect(transport_, &dms::transport::ITransport::errorOccurred, this,
            &DeviceSession::onTransportError);
}

void DeviceSession::start()
{
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("session starting"));
    transport_->open();
}

void DeviceSession::stop()
{
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("session stopping"));
    transport_->close();
}

void DeviceSession::onBytesReceived(const QByteArray &bytes)
{
    const FrameDecoder::Result r = decoder_.decode(bytes);

    if (r.rxOverflow)
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::RxOverflow),
                        QStringLiteral("rx overflow, decoder buffer cleared"));
    if (r.badCrcFrames > 0) {
        badCrcFrames_ += r.badCrcFrames;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameCrc),
                        QStringLiteral("%1 crc error frame(s), resynced").arg(r.badCrcFrames));
    }
    if (r.badHeaderFrames > 0) {
        badHeaderFrames_ += r.badHeaderFrames;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameLength),
                        QStringLiteral("%1 bad header frame(s), resynced")
                            .arg(r.badHeaderFrames));
    }
    if (r.droppedBytes > 0)
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameLength),
                        QStringLiteral("%1 byte(s) dropped, no frame").arg(r.droppedBytes));
    if (r.unknownTypeFrames > 0) {
        unknownTypeFrames_ += r.unknownTypeFrames;
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("%1 unknown-type frame(s) skipped")
                            .arg(r.unknownTypeFrames));
    }

    for (const DecodedFrame &f : r.frames) {
        if (f.type == quint8(MessageType::Heartbeat)) {
            ++framesReceived_;
            emit heartbeatReceived(f.seq, f.timestampMs);
        } else if (f.type == quint8(MessageType::Telemetry)) {
            handleTelemetry(f);
        } else if (f.type == quint8(MessageType::CommandAck)) {
            handleCommandAck(f);
        } else {
            // 客户端会话不该收到设备侧 COMMAND;已知类型里只有这一种剩余情况
            dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                            logCtx(dms::ErrorCode::FramePayload),
                            QStringLiteral("unexpected COMMAND frame from device, seq=%1")
                                .arg(f.seq));
        }
    }
}

void DeviceSession::handleTelemetry(const DecodedFrame &frame)
{
    TelemetryResult result;
    if (MessageCodec::decodeTelemetry(frame.payload, result) != DecodeError::None) {
        // 载荷错误整帧拒绝,不交付部分指标(LLD §2)
        ++payloadErrors_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FramePayload),
                        QStringLiteral("telemetry payload rejected, seq=%1").arg(frame.seq));
        return;
    }

    ++framesReceived_;
    TelemetryBatch batch;
    batch.samples = result.knownSamples;
    batch.unknownMetrics = result.unknownMetrics;
    batch.seq = frame.seq;
    batch.timestampMs = frame.timestampMs;
    emit telemetryReceived(batch);
}

void DeviceSession::handleCommandAck(const DecodedFrame &frame)
{
    quint16 commandId = 0;
    AckStatus status = AckStatus::Failed;
    if (!MessageCodec::decodeCommandAck(frame.payload, commandId, status)) {
        ++payloadErrors_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FramePayload),
                        QStringLiteral("malformed ack payload, seq=%1").arg(frame.seq));
        return;
    }

    ++framesReceived_;
    emit commandAckReceived(frame.seq, commandId, quint16(status));
}

void DeviceSession::onTransportError(const dms::transport::TransportError &error)
{
    // transport 侧已按自身上下文记过日志;这里只向会话消费者转发结构化错误。
    // 不在这里重连/改状态——那是 S3 状态机的事
    emit sessionError(error.code, error.message);
}

dms::LogContext DeviceSession::logCtx(dms::ErrorCode code) const
{
    dms::LogContext ctx;
    ctx.deviceId = deviceId_;
    ctx.sessionId = sessionId_;
    ctx.code = code;
    return ctx;
}

} // namespace dms::domain
