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
    connect(transport_, &dms::transport::ITransport::errorOccurred, this,
            &DeviceSession::onTransportError);
    if (config_.transportKind == dms::transport::TransportKind::Udp) {
        // UDP 单报入口:报文边界保留,来源已在传输层过滤(协议 §7)
        connect(transport_, &dms::transport::ITransport::datagramReceived, this,
                &DeviceSession::onDatagramReceived);
    } else {
        connect(transport_, &dms::transport::ITransport::bytesReceived, this,
                &DeviceSession::onBytesReceived);
    }
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
    // 断开后设备采集状态未知:下一轮按默认"采集活跃"评估(AlarmEngine 对
    // Disconnected 同口径,否则引擎报 DATA_STALE 而会话仍显示 Online)
    acquisitionActive_ = true;
    setState(SessionState::Disconnected);
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("session stopping"));
    closeTransport();
}

quint32 DeviceSession::sendCommand(quint16 commandId,
                                   const QList<dms::protocol::CommandParam> &params)
{
    // UDP 不承载控制命令(协议 §7/LLD §4):不承诺可靠控制,
    // 拒绝比"发出但永远等不到 ACK"更诚实
    if (config_.transportKind == dms::transport::TransportKind::Udp) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, logCtx(),
                        QStringLiteral("command %1 rejected: udp transport does not "
                                       "carry commands")
                            .arg(commandId));
        return 0;
    }
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
    commandSessionId_ = sessionId_;
    // 两层编码都可能拒绝(参数值 >255B / 载荷 >4096B),空 = 编码失败
    const QByteArray payload = MessageCodec::encodeCommand(commandId, params);
    const QByteArray frame = payload.isEmpty()
                                 ? QByteArray()
                                 : dms::protocol::encodeFrame(quint8(MessageType::Command),
                                                              seq, 0, payload);
    emit commandSent(seq, commandId, params);
    if (frame.isEmpty()) {
        // LLD §4 Failed:编码失败不写半截帧;受理已广播,终态紧随其后
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FramePayload),
                        QStringLiteral("command %1 seq=%2 encoding failed").arg(commandId).arg(seq));
        finishPending(commands_.fail(QStringLiteral("command encoding failed")));
        return seq;
    }
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, logCtx(),
                    QStringLiteral("command %1 sent, seq=%2").arg(commandId).arg(seq));
    // write 失败同步走 errorOccurred:未决命令可能在本次调用返回前就终结为 Failed
    transport_->write(frame, seq);
    return seq;
}

void DeviceSession::checkTimeouts()
{
    const qint64 now = clock_();

    // 组帧超时(LLD §2.4/§2.5):候选滞留超限,从候选起点后一字节重新同步,
    // 保留排在其"等待区"里的有效帧,而不是清空整个缓存
    if (partialSinceMs_ > 0 && now - partialSinceMs_ >= config_.frameAssemblyTimeoutMs) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameTimeout),
                        QStringLiteral("frame assembly stalled %1ms with %2 byte(s), "
                                        "resyncing after candidate")
                            .arg(now - partialSinceMs_)
                            .arg(decoder_.bufferedBytes()));
        partialSinceMs_ = 0;
        processStreamResult(decoder_.skipStalledCandidate(), /*headAdvanced=*/true);
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
    // UDP 的 open 只是确认绑定,会话边界是 seq 基准重建(见 onDatagramReceived)
    if (config_.transportKind != dms::transport::TransportKind::Udp)
        sessionId_ = QUuid::createUuid();
    openedAtMs_ = now;
    firstValidSeen_ = false;
    lastValidReceiveMs_ = now;
    consecutiveBadCrc_ = 0;
    consecutiveGoodFrames_ = 0;
    decoder_.reset();
    partialSinceMs_ = 0;
    udpSeqBaseValid_ = false; // 新一轮基准由首条有效帧建立(协议 §7)
    // 未收到首个指标也从打开时刻计时(LLD §3):未知指标先按"未超时"处理
    for (qint64 &t : metricLastMs_)
        t = now;
    emit sessionOpened();
}

void DeviceSession::onTransportClosed()
{
    emit sessionClosed();
    if (closeByUs_)
        return; // stop()/重连/Fault 的主动拆链,不触发状态迁移
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

// 解码结果的计数与日志(流式/单报共用):不动帧分发,只做记账。
// badCrc 段含 Online→Degraded 迁移,两种路径共享同一状态机语义
void DeviceSession::accountDecodedEvents(const FrameDecoder::Result &r)
{
    if (r.rxOverflow)
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::RxOverflow),
                        QStringLiteral("rx overflow, decoder buffer cleared"));
    if (r.badCrcFrames > 0) {
        badCrcFrames_ += r.badCrcFrames;
        consecutiveBadCrc_ += r.badCrcFrames; // 任一有效帧清零(见 noteValidReceive)
        consecutiveGoodFrames_ = 0; // Degraded→Online 要求"连续"有效帧(LLD §3)
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
}

// 单帧分发:心跳/遥测/ACK 记账与信号,COMMAND 意外丢弃。
// 返回"结构+载荷+CRC 全通过"的好帧(protocolHealth 口径)
bool DeviceSession::dispatchFrame(const DecodedFrame &f)
{
    if (f.type == quint8(MessageType::Heartbeat)) {
        ++framesReceived_;
        noteValidReceive(); // 心跳刷新存活,但不刷新遥测时间(LLD §3)
        emit heartbeatReceived(f.seq, f.timestampMs);
        return true;
    }
    if (f.type == quint8(MessageType::Telemetry))
        return handleTelemetry(f);
    if (f.type == quint8(MessageType::CommandAck))
        return handleCommandAck(f);
    // 客户端会话不该收到设备侧 COMMAND;已知类型里只有这一种剩余情况
    dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                    logCtx(dms::ErrorCode::FramePayload),
                    QStringLiteral("unexpected COMMAND frame from device, seq=%1")
                        .arg(f.seq));
    return false;
}

void DeviceSession::onBytesReceived(const QByteArray &bytes)
{
    const qsizetype before = decoder_.bufferedBytes();
    const FrameDecoder::Result r = decoder_.decode(bytes);
    // 有字节被消费/丢弃 = 缓存头部前进:上一候选已结束,剩余半包是新候选
    const bool headAdvanced =
        r.rxOverflow || decoder_.bufferedBytes() < before + bytes.size();
    processStreamResult(r, headAdvanced);
}

void DeviceSession::processStreamResult(const FrameDecoder::Result &r, bool headAdvanced)
{
    accountDecodedEvents(r);

    // 有效帧计数:心跳/遥测/ACK 且载荷通过才计入(与 noteValidReceive 同口径,
    // 载荷拒绝的遥测不算"好帧",不解除连续坏 CRC)
    quint32 goodFrames = 0;
    for (const DecodedFrame &f : r.frames) {
        if (dispatchFrame(f))
            ++goodFrames;
    }

    // 协议健康单块快照:纯坏头/垃圾块(两者皆 0)不发,避免空信号噪声
    if (r.badCrcFrames > 0 || goodFrames > 0)
        emit protocolHealth(r.badCrcFrames, goodFrames);

    // 组帧滞留计时(LLD §2.4):从候选出现起算,不随追加数据延长;头部前进
    // 说明换了新候选,重新起算——否则背靠背连续流缓存永不为空,会被误判超时
    if (decoder_.bufferedBytes() == 0)
        partialSinceMs_ = 0;
    else if (headAdvanced || partialSinceMs_ == 0)
        partialSinceMs_ = clock_();

    // LLD §2.1:单轮上限后让出事件循环,剩余缓存排队续解——不能等下一块数据
    // (设备停发时会一直滞留)。context=this:会话销毁时排队调用自动作废
    if (r.roundLimitReached && !drainScheduled_) {
        drainScheduled_ = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                drainScheduled_ = false;
                onBytesReceived(QByteArray());
            },
            Qt::QueuedConnection);
    }
}

// UDP 单报入口(协议 §7):一报一帧,不跨报拼接;来源已由传输层过滤,
// sender 仅用于日志上下文(未配置来源根本到不了这里)
void DeviceSession::onDatagramReceived(const QByteArray &bytes, const QHostAddress &sender,
                                       quint16 senderPort)
{
    Q_UNUSED(sender);
    Q_UNUSED(senderPort);

    // 单报独立解码:先清流缓存,半帧绝不留到下一报
    decoder_.reset();
    const FrameDecoder::Result r = decoder_.decode(bytes);
    accountDecodedEvents(r); // 坏 CRC/未知类型等链路质量事件照常计数

    // 恰好一个结构完整帧才受理:多帧合一报、缺尾、超长、垃圾前缀整报拒绝
    // (协议 §7/REQ-UDP-001)。CRC 坏/未知类型的帧也算"结构完整"——
    // 拒绝的是报文结构异常,不是链路质量事件
    const quint32 frameEvents = quint32(r.frames.size()) + r.unknownTypeFrames
                                + r.badCrcFrames + r.badHeaderFrames;
    const bool exactlyOneFrame = frameEvents == 1 && r.droppedBytes == 0
                                 && !r.rxOverflow && decoder_.bufferedBytes() == 0;
    if (!exactlyOneFrame) {
        ++malformedDatagrams_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameLength),
                        QStringLiteral("datagram rejected (not exactly one frame): "
                                       "%1 byte(s), %2 frame(s), %3 trailing")
                            .arg(bytes.size())
                            .arg(frameEvents)
                            .arg(decoder_.bufferedBytes()));
        decoder_.reset();
        return;
    }

    quint32 goodFrames = 0;
    for (const DecodedFrame &f : r.frames) {
        // seq 模差防御(协议 §7):旧/重复不更新实时值、存活或落盘。
        // 重复报文可能是缓存重放,不证明链路当前健康,也不清 CRC streak
        if (!seqIsNew(f.seq)) {
            ++staleSeqFrames_;
            dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Protocol, logCtx(),
                            QStringLiteral("stale/duplicate seq %1 dropped").arg(f.seq));
            continue;
        }
        if (!udpSeqBaseValid_)
            sessionId_ = QUuid::createUuid(); // 新基准 = 新会话(docs/06 §3)
        udpLastSeq_ = f.seq;
        udpSeqBaseValid_ = true;
        // UDP 只承载 HEARTBEAT/TELEMETRY(协议 §7):ACK/COMMAND 意外,丢弃记日志
        if (f.type != quint8(MessageType::Heartbeat)
            && f.type != quint8(MessageType::Telemetry)) {
            dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                            logCtx(dms::ErrorCode::FramePayload),
                            QStringLiteral("unexpected frame type %1 over udp, seq=%2")
                                .arg(f.type)
                                .arg(f.seq));
            continue;
        }
        if (dispatchFrame(f))
            ++goodFrames;
    }

    if (r.badCrcFrames > 0 || goodFrames > 0)
        emit protocolHealth(r.badCrcFrames, goodFrames);
}

bool DeviceSession::handleTelemetry(const DecodedFrame &frame)
{
    TelemetryResult result;
    if (MessageCodec::decodeTelemetry(frame.payload, result) != DecodeError::None) {
        // 载荷错误整帧拒绝,不交付部分指标,也不算有效消息(LLD §2/§3)
        ++payloadErrors_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FramePayload),
                        QStringLiteral("telemetry payload rejected, seq=%1").arg(frame.seq));
        return false;
    }

    ++framesReceived_;
    noteTelemetryMetrics(result.knownSamples);
    noteValidReceive();
    TelemetryBatch batch;
    batch.samples = result.knownSamples;
    batch.unknownMetrics = result.unknownMetrics;
    batch.seq = frame.seq;
    batch.timestampMs = frame.timestampMs;
    batch.sessionId = sessionId_;
    emit telemetryReceived(batch);
    return true;
}

bool DeviceSession::handleCommandAck(const DecodedFrame &frame)
{
    quint16 commandId = 0;
    AckStatus status = AckStatus::Failed;
    if (!MessageCodec::decodeCommandAck(frame.payload, commandId, status)) {
        ++payloadErrors_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FramePayload),
                        QStringLiteral("malformed ack payload, seq=%1").arg(frame.seq));
        return false;
    }

    ++framesReceived_;
    noteValidReceive();
    emit commandAckReceived(frame.seq, commandId, quint16(status));
    if (auto outcome = commands_.onAck(frame.seq, commandId, quint16(status))) {
        // 与超时/取消/失败路径一致,经 finishPending 补 sessionId 再发射(S5 落库依赖)
        finishPending(outcome);
        // 设备确认过的采集启停才影响健康判定(LLD §6):Rejected/超时结果
        // 未知,不臆断;Start 重新起算陈旧窗口。Reset 按协议 §4.2 也启动遥测
        if (outcome->state == CommandState::Succeeded) {
            if (outcome->commandId == dms::protocol::CommandId::Stop) {
                acquisitionActive_ = false;
            } else if (outcome->commandId == dms::protocol::CommandId::Start
                       || outcome->commandId == dms::protocol::CommandId::Reset) {
                acquisitionActive_ = true;
                for (qint64 &t : metricLastMs_)
                    t = clock_();
            }
        }
    } else {
        // 迟到/重复/无关 ACK:只记日志,不修改已结束结果(LLD §4)
        dms::logMessage(dms::LogLevel::Debug, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("unmatched ack ignored, seq=%1").arg(frame.seq));
    }
    return true;
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
        closeTransport();
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
    } else if (state_ == SessionState::Reconnecting) {
        // UDP:失联期间绑定仍在等数据,数据回来了直接恢复,不等退避到期
        // (LLD §3"收到新的有效遥测直接恢复");TCP/串口 transport 已关,
        // 此路径对它们不可达
        firstValidSeen_ = true;
        setState(metricsAllFresh(now) ? SessionState::Online : SessionState::Degraded);
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
    // 采集被设备确认停止时,陈旧条件不参与健康判定(LLD §6):
    // Stop 后心跳仍活,会话必须能稳定留在 Online
    if (!acquisitionActive_)
        return true;
    for (qint64 t : metricLastMs_)
        if (now - t >= config_.metricTimeoutMs)
            return false;
    return true;
}

bool DeviceSession::anyMetricStale(qint64 now) const
{
    if (!acquisitionActive_)
        return false; // 同上:免除被明确停止的遥测新鲜条件
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
    if (config_.transportKind == dms::transport::TransportKind::Udp) {
        // UDP:保持绑定等数据,不建 TCP 式连接(LLD §3);退避重试点上
        // open() 幂等确认绑定仍在。失联清 seq 基准:恢复后第一条有效帧
        // 重建基准,允许设备重启后序号归零(协议 §7)
        udpSeqBaseValid_ = false;
    } else {
        closeTransport(); // 关旧 transport(幂等:已关则无副作用)
    }
    retryAtMs_ = clock_() + backoffDelayMs();
}

void DeviceSession::closeTransport()
{
    // ITransport 契约:closed 在 close() 栈内同步发出且恰好一次。标记只覆盖
    // 这一次调用——对端已先关时 close() 不再发 closed,残留标记会吞掉下一轮
    // 连接真正的对端关闭
    closeByUs_ = true;
    transport_->close();
    closeByUs_ = false;
    // 链路已拆:旧流残留不再有意义,免得退避期间误报组帧超时
    decoder_.reset();
    partialSinceMs_ = 0;
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
    if (outcome) {
        // 入库按会话定位唯一键(S5):用受理时的 ID,不受之后的连接轮换影响
        outcome->sessionId = commandSessionId_;
        emit commandFinished(*outcome);
    }
}

bool DeviceSession::seqIsNew(quint32 seq) const
{
    if (!udpSeqBaseValid_)
        return true; // 首轮或失联清除后:任何 seq 都可作为新基准
    // 模差判断(协议 §7):无符号算术天然 mod 2^32,
    // 0<d<2^31 为新;d=0 重复;d≥2^31 旧/乱序(含设备重启归零的回绕接受)
    const quint32 d = seq - udpLastSeq_;
    return d != 0 && d < 0x80000000u;
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
