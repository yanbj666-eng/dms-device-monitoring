#include "DeviceSession.h"

#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

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
                             SessionConfig config, Clock clock, JitterPercent jitter,
                             QObject *parent)
    : QObject(parent),
      deviceId_(std::move(deviceId)),
      sessionId_(QUuid::createUuid()), // 本地会话标识,重启即新会话(LLD §4)
      transport_(transport),
      decoder_(config.decoderMaxBufferBytes), // app.ini max_buffer_bytes(S4 接线)
      config_(config),
      clock_(std::move(clock)),
      jitter_(std::move(jitter))
{
    Q_ASSERT(transport_);
    transport_->setParent(this); // 所有权归会话(Qt parent-child,CLAUDE.md 约定)

    // queued 连接(跨线程消费信号)需要值对象已注册
    qRegisterMetaType<dms::domain::TelemetryBatch>("dms::domain::TelemetryBatch");
    qRegisterMetaType<dms::domain::SessionState>("dms::domain::SessionState");
    qRegisterMetaType<dms::domain::CommandOutcome>("dms::domain::CommandOutcome");

    connect(transport_, &dms::transport::ITransport::opened, this,
            &DeviceSession::onTransportOpened);
    connect(transport_, &dms::transport::ITransport::closed, this,
            &DeviceSession::onTransportClosed);
    connect(transport_, &dms::transport::ITransport::writeAccepted, this,
            &DeviceSession::onWriteAccepted);
    connect(transport_, &dms::transport::ITransport::bytesReceived, this,
            &DeviceSession::onBytesReceived);
    connect(transport_, &dms::transport::ITransport::errorOccurred, this,
            &DeviceSession::onTransportError);
}

qint64 DeviceSession::defaultClock()
{
    return dms::Logger::defaultClock();
}

int DeviceSession::defaultJitter()
{
    return QRandomGenerator::global()->bounded(-10, 11);
}

void DeviceSession::start()
{
    if (state_ == SessionState::Connecting || state_ == SessionState::Online
        || state_ == SessionState::Degraded || state_ == SessionState::Reconnecting) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, logCtx(),
                        QStringLiteral("start ignored: session already active"));
        return;
    }
    // Disconnected,或 Fault 经人工修正后重启:全新一轮(LLD §3)
    reconnectAttempts_ = 0;
    retryAtMs_ = 0;
    openedAtMs_ = 0; // 清上一轮打开时刻:首帧超时判定只属于本轮
    setState(SessionState::Connecting);
    if (!pollTimer_) {
        // start() 总在会话所属线程被调(worker 启动槽),timer 与会话同线程
        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(config_.pollIntervalMs);
        connect(pollTimer_, &QTimer::timeout, this, &DeviceSession::checkTimeouts);
    }
    pollTimer_->start();
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("session starting"));
    transport_->open();
}

void DeviceSession::stop()
{
    if (state_ == SessionState::Disconnected) {
        transport_->close(); // 兼容 S2 语义:未启动也把 closed 转发出去
        return;
    }
    // 人工断开:取消未决命令与一切定时器,不新建失联告警(LLD §3)
    finishPending(commands_.cancel(QStringLiteral("manual disconnect")));
    if (pollTimer_)
        pollTimer_->stop();
    setState(SessionState::Disconnected);
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("session stopping"));
    closeByUs_ = true; // 自己拆链,closed 不再触发状态迁移
    transport_->close();
}

quint32 DeviceSession::sendCommand(quint16 commandId,
                                   const QList<dms::protocol::CommandParam> &params)
{
    // 仅 Online/Degraded 且可写时允许(LLD §4);Connecting 下命令直接拒绝
    if (state_ != SessionState::Online && state_ != SessionState::Degraded) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, logCtx(),
                        QStringLiteral("command %1 rejected: state %2")
                            .arg(commandId)
                            .arg(sessionStateName(state_)));
        return 0;
    }
    if (!transport_->isOpen()) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, logCtx(),
                        QStringLiteral("command %1 rejected: transport not writable")
                            .arg(commandId));
        return 0;
    }
    if (commands_.hasPending()) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, logCtx(),
                        QStringLiteral("command %1 rejected: pending command seq=%2")
                            .arg(commandId)
                            .arg(commands_.pendingSeq()));
        return 0;
    }

    const quint32 seq = commands_.nextSeq();
    commands_.start(seq, commandId, clock_());
    const QByteArray frame = dms::protocol::encodeFrame(
        quint8(MessageType::Command), seq, 0,
        MessageCodec::encodeCommand(commandId, params));
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("command %1 sent, seq=%2").arg(commandId).arg(seq));
    emit commandSent(seq, commandId);
    // write 失败同步走 errorOccurred:未决命令可能在本次调用返回前就终结为 Failed
    transport_->write(frame, seq);
    return seq;
}

void DeviceSession::checkTimeouts()
{
    const qint64 now = clock_();

    // 组帧超时:半包滞留超限视为死流,清缓存重新同步(LLD §2/§3)
    if (partialSinceMs_ > 0 && now - partialSinceMs_ >= config_.frameAssemblyTimeoutMs) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameTimeout),
                        QStringLiteral("frame assembly stalled %1ms with %2 byte(s), "
                                        "buffer cleared")
                            .arg(now - partialSinceMs_)
                            .arg(decoder_.bufferedBytes()));
        decoder_.reset();
        partialSinceMs_ = 0;
    }

    // 命令 ACK 超时(LLD §4):结果未知,不自动重发
    finishPending(commands_.pollTimeout(now, config_.commandAckTimeoutMs));

    switch (state_) {
    case SessionState::Connecting:
        // 建链 3000ms 超时由 transport 定时器上报;这里只管"打开后无首个有效帧"
        if (openedAtMs_ > 0 && now - openedAtMs_ >= config_.firstFrameTimeoutMs)
            enterReconnecting(QStringLiteral("no first valid frame"),
                              dms::ErrorCode::FrameTimeout);
        break;
    case SessionState::Online:
    case SessionState::Degraded: {
        // 指标过期只降级;心跳与遥测任一有效接收都算存活,全静默才失联(LLD §3)
        if (state_ == SessionState::Online && anyMetricStale(now))
            setState(SessionState::Degraded);
        if (now - lastValidReceiveMs_ >= config_.offlineTimeoutMs) {
            enterReconnecting(QStringLiteral("no valid frame received"),
                              dms::ErrorCode::ConnectionLost);
            break;
        }
        if (state_ == SessionState::Online
            && now - onlineStableSinceMs_ >= config_.stableOnlineResetMs)
            reconnectAttempts_ = 0; // 稳定 Online 才清零退避计数(LLD §3)
        break;
    }
    case SessionState::Reconnecting:
        if (now >= retryAtMs_) {
            ++reconnectAttempts_; // 本次尝试点火(只允许一个在途尝试)
            // 新尝试尚未打开:必须清上一轮的打开时刻,否则异步建链期间
            // 首个 poll tick 就拿陈旧时间戳误判"无首帧超时",掩盖真实错误
            openedAtMs_ = 0;
            setState(SessionState::Connecting);
            transport_->open();
        }
        break;
    case SessionState::Disconnected:
    case SessionState::Fault:
        break; // 无定时行为:人工动作才会离开这两个状态
    }
}

void DeviceSession::onTransportOpened()
{
    // 新一轮连接:旧半包与指标时间全部作废(新会话将旧值标陈旧,LLD §3)
    const qint64 now = clock_();
    openedAtMs_ = now;
    firstValidSeen_ = false;
    lastValidReceiveMs_ = now;
    consecutiveBadCrc_ = 0;
    consecutiveGoodFrames_ = 0;
    decoder_.reset();
    partialSinceMs_ = 0;
    // 未收到首个指标也从打开时刻计时(LLD §3):未知指标先按"未超时"处理
    for (qint64 &t : metricLastMs_)
        t = now;
    emit sessionOpened();
}

void DeviceSession::onTransportClosed()
{
    emit sessionClosed();
    if (closeByUs_) {
        closeByUs_ = false; // stop()/重连的主动拆链,不触发状态迁移
        return;
    }
    if (state_ == SessionState::Connecting || state_ == SessionState::Online
        || state_ == SessionState::Degraded)
        enterReconnecting(QStringLiteral("transport closed by peer"),
                          dms::ErrorCode::ConnectionLost);
}

void DeviceSession::onWriteAccepted(quint64 requestId)
{
    // writeAccepted 只表示入队:命令仍要等 ACK 才算业务成功
    if (commands_.hasPending() && commands_.pendingSeq() == requestId)
        commands_.markWriteAccepted();
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
        consecutiveBadCrc_ += r.badCrcFrames; // 任一有效帧清零(见 noteValidReceive)
        if (state_ == SessionState::Online
            && consecutiveBadCrc_ >= config_.consecutiveBadCrcLimit)
            setState(SessionState::Degraded); // 通信继续,只是降级(LLD §3)
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameCrc),
                        QStringLiteral("%1 crc error frame(s), resynced")
                            .arg(r.badCrcFrames));
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
        // CRC 正确但类型未知:已消费但不算有效消息,不延长存活(LLD §3)
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("%1 unknown-type frame(s) skipped")
                            .arg(r.unknownTypeFrames));
    }

    for (const DecodedFrame &f : r.frames) {
        if (f.type == quint8(MessageType::Heartbeat)) {
            ++framesReceived_;
            noteValidReceive(); // 心跳刷新存活,但不刷新遥测时间(LLD §3)
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

    // 组帧滞留计时:出现半包起算,清空即止(超时判定在 checkTimeouts)
    if (decoder_.bufferedBytes() > 0) {
        if (partialSinceMs_ == 0)
            partialSinceMs_ = clock_();
    } else {
        partialSinceMs_ = 0;
    }
}

void DeviceSession::handleTelemetry(const DecodedFrame &frame)
{
    TelemetryResult result;
    if (MessageCodec::decodeTelemetry(frame.payload, result) != DecodeError::None) {
        // 载荷错误整帧拒绝,不交付部分指标,也不算有效消息(LLD §2/§3)
        ++payloadErrors_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FramePayload),
                        QStringLiteral("telemetry payload rejected, seq=%1").arg(frame.seq));
        return;
    }

    ++framesReceived_;
    noteTelemetryMetrics(result.knownSamples);
    noteValidReceive();
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
    noteValidReceive();
    emit commandAckReceived(frame.seq, commandId, quint16(status));
    if (auto outcome = commands_.onAck(frame.seq, commandId, quint16(status)))
        emit commandFinished(*outcome);
    else
        // 迟到/重复/无关 ACK:只记日志,不修改已结束结果(LLD §4)
        dms::logMessage(dms::LogLevel::Debug, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("unmatched ack ignored, seq=%1").arg(frame.seq));
}

void DeviceSession::onTransportError(const dms::transport::TransportError &error)
{
    emit sessionError(error.code, error.message); // 结构化转发,不吞错(S2 约定)

    // 未决命令"尚未入队"就出错 = 本地发送失败(LLD §4 Failed);
    // 已入队后断链则由 enterReconnecting 走 Cancelled
    if (commands_.hasPending() && !commands_.pendingWriteAccepted()
        && (error.code == dms::ErrorCode::SendQueueFull
            || error.code == dms::ErrorCode::ConnectionLost)) {
        finishPending(commands_.fail(QStringLiteral("local send failed: ") + error.message));
    }

    if (error.code == dms::ErrorCode::ConfigInvalid) {
        // 配置错误/不可恢复本地错误:Fault,停止重试等人工修正
        // (LLD §3;串口权限错误是未来主要来源)
        finishPending(commands_.cancel(QStringLiteral("session fault")));
        if (pollTimer_)
            pollTimer_->stop();
        closeByUs_ = true;
        transport_->close();
        setState(SessionState::Fault);
        return;
    }
    if (error.code == dms::ErrorCode::SendQueueFull)
        return; // 本地背压,链路仍健康,不动状态机

    // CONNECT_TIMEOUT(建链超时)/ CONNECTION_LOST(链路错误):进入退避重连
    if (state_ == SessionState::Connecting || state_ == SessionState::Online
        || state_ == SessionState::Degraded)
        enterReconnecting(error.message, error.code);
}

void DeviceSession::noteValidReceive()
{
    const qint64 now = clock_();
    lastValidReceiveMs_ = now;
    consecutiveBadCrc_ = 0;
    if (state_ == SessionState::Connecting && !firstValidSeen_) {
        // 首条有效消息:四指标新鲜则 Online,否则 Degraded(LLD §3)
        firstValidSeen_ = true;
        setState(metricsAllFresh(now) ? SessionState::Online : SessionState::Degraded);
    } else if (state_ == SessionState::Degraded) {
        // 恢复条件:四指标新鲜且连续 N 个有效帧(帧本身会刷新指标)
        if (metricsAllFresh(now)
            && ++consecutiveGoodFrames_ >= config_.goodFramesToRecoverOnline)
            setState(SessionState::Online);
    }
}

void DeviceSession::noteTelemetryMetrics(const QList<dms::protocol::TelemetrySample> &samples)
{
    const qint64 now = clock_();
    for (const dms::protocol::TelemetrySample &s : samples) {
        // 已知指标恰为 1..4;防御性检查防未来指标表扩展越界
        if (s.metricId >= 1 && s.metricId <= 4)
            metricLastMs_[int(s.metricId) - 1] = now; // 心跳不刷新遥测时间(LLD §3)
    }
}

bool DeviceSession::metricsAllFresh(qint64 now) const
{
    for (qint64 t : metricLastMs_)
        if (now - t >= config_.metricTimeoutMs)
            return false;
    return true;
}

bool DeviceSession::anyMetricStale(qint64 now) const
{
    for (qint64 t : metricLastMs_)
        if (now - t >= config_.metricTimeoutMs)
            return true;
    return false;
}

void DeviceSession::setState(SessionState next)
{
    if (state_ == next)
        return;
    const SessionState prev = state_;
    state_ = next;
    if (next == SessionState::Online) {
        onlineStableSinceMs_ = clock_();
        consecutiveGoodFrames_ = 0;
    } else if (next == SessionState::Degraded) {
        consecutiveGoodFrames_ = 0;
    }
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("state %1 -> %2")
                        .arg(sessionStateName(prev), sessionStateName(next)));
    emit stateChanged(next);
}

void DeviceSession::enterReconnecting(const QString &reason, dms::ErrorCode code)
{
    if (state_ == SessionState::Reconnecting || state_ == SessionState::Disconnected
        || state_ == SessionState::Fault)
        return;
    // 链路失联:未决命令取消(执行结果未知,不自动重发,LLD §4)
    finishPending(commands_.cancel(QStringLiteral("connection lost")));
    setState(SessionState::Reconnecting);
    dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Net, logCtx(code),
                    QStringLiteral("entering reconnect backoff: %1").arg(reason));
    closeByUs_ = true;
    transport_->close(); // 关旧 transport(幂等:已关则无副作用)
    retryAtMs_ = clock_() + backoffDelayMs();
}

qint64 DeviceSession::backoffDelayMs() const
{
    // 基数 1,2,4,8,10,10…s:2 的幂增长到上限后保持(LLD §3)
    qint64 delay = config_.reconnectBaseMs;
    for (int i = 0; i < reconnectAttempts_ && i < 4; ++i)
        delay *= 2;
    delay = std::min(delay, qint64(config_.reconnectMaxMs));
    // ±10% 抖动后夹回 [0.1s, 10s]
    delay = delay * (100 + jitter_()) / 100;
    return std::clamp(delay, qint64(config_.reconnectMinMs), qint64(config_.reconnectMaxMs));
}

void DeviceSession::finishPending(std::optional<CommandOutcome> outcome)
{
    if (outcome)
        emit commandFinished(*outcome);
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
