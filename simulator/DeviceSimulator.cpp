#include "DeviceSimulator.h"

#include <QDateTime>
#include <QNetworkDatagram>
#include <QtEndian>

#include <algorithm>

#include "Frame.h"
#include "common/Logging.h"

namespace dms::sim {

using dms::protocol::AckStatus;
using dms::protocol::CommandMessage;
using dms::protocol::CommandParam;
using dms::protocol::DecodeError;
using dms::protocol::DecodedFrame;
using dms::protocol::FrameDecoder;
using dms::protocol::kValueTypeUint32;
using dms::protocol::kValueTypeFloat32;
using dms::protocol::MessageCodec;
using dms::protocol::MessageType;
using dms::protocol::MetricId;
using dms::protocol::TelemetrySample;

namespace {
constexpr quint16 kCmdStart = 1;
constexpr quint16 kCmdStop = 2;
constexpr quint16 kCmdReset = 3;
constexpr quint16 kCmdSetParameter = 4;
constexpr quint32 kMinPeriodMs = 100;  // SetParameter 合法域(协议 §4.2)
constexpr quint32 kMaxPeriodMs = 1000;
} // namespace

DeviceSimulator::DeviceSimulator(Params params, QObject *parent)
    : QObject(parent), params_(std::move(params))
{
    clock_.start();
    // 每设备独立种子流:固定 seed 下数据/注入序列可复现(docs/15 §5)
    rng_.seed(params_.seed + quint32(params_.index) * 1000003u);

    connect(&server_, &QTcpServer::newConnection, this, &DeviceSimulator::onNewConnection);

    telemetryTimer_.setInterval(params_.samplePeriodMs);
    connect(&telemetryTimer_, &QTimer::timeout, this, &DeviceSimulator::onTelemetryTick);
    telemetryTimer_.start();

    heartbeatTimer_.setInterval(1000); // 心跳恒 1000ms(协议 §4)
    connect(&heartbeatTimer_, &QTimer::timeout, this, &DeviceSimulator::onHeartbeatTick);
    heartbeatTimer_.start();

    faultWatchTimer_.setInterval(100);
    connect(&faultWatchTimer_, &QTimer::timeout, this, &DeviceSimulator::onFaultWatchTick);
    faultWatchTimer_.start();

    mergeFlushTimer_.setSingleShot(true);
    connect(&mergeFlushTimer_, &QTimer::timeout, this, &DeviceSimulator::onMergeFlush);
}

bool DeviceSimulator::startListening(QString *errorOut)
{
    switch (params_.transport) {
    case SimTransport::Tcp: {
        if (server_.isListening())
            return true;
        if (!server_.listen(QHostAddress::LocalHost, params_.listenPort)) {
            if (errorOut)
                *errorOut = server_.errorString();
            return false;
        }
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                        QStringLiteral("listening on 127.0.0.1:%1").arg(params_.listenPort));
        return true;
    }
    case SimTransport::Udp: {
        if (udp_)
            return true;
        udp_ = new QUdpSocket(this);
        // 固定源端口 = 设备身份(协议 §7):DMS 按来源四元组区分设备,
        // 源口漂移会被当成未知来源整报丢弃
        if (!udp_->bind(QHostAddress::LocalHost, params_.listenPort)) {
            if (errorOut)
                *errorOut = udp_->errorString();
            udp_->deleteLater();
            udp_ = nullptr;
            return false;
        }
        connect(udp_, &QUdpSocket::readyRead, this, &DeviceSimulator::onUdpReadyRead);
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                        QStringLiteral("udp source port %1 -> %2:%3")
                            .arg(params_.listenPort)
                            .arg(params_.peerAddress.toString())
                            .arg(params_.peerPort));
        return true;
    }
    case SimTransport::Serial: {
        if (serial_ && serial_->isOpen())
            return true;
        if (!serial_) {
            serial_ = new QSerialPort(this);
            // 信号只连一次:disconnect 故障窗口结束的重开不重复连接
            connect(serial_, &QSerialPort::readyRead, this,
                    &DeviceSimulator::onSerialReadyRead);
            connect(serial_, &QSerialPort::errorOccurred, this,
                    [this](QSerialPort::SerialPortError error) {
                        // 拔出等运行期错误靠日志留痕;恢复路径由 disconnect 故障/重开覆盖
                        if (error != QSerialPort::NoError)
                            dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Net,
                                            logCtx(dms::ErrorCode::ConnectionLost),
                                            QStringLiteral("serial error 0x%1: %2")
                                                .arg(int(error), 0, 16)
                                                .arg(serial_->errorString()));
                    });
        }
        serial_->setPortName(params_.serialPortName);
        // 与 DMS SerialTransportConfig 默认一致(115200 8N1 无流控):
        // 虚拟串口对两端参数必须相同
        serial_->setBaudRate(115200);
        serial_->setDataBits(QSerialPort::Data8);
        serial_->setParity(QSerialPort::NoParity);
        serial_->setStopBits(QSerialPort::OneStop);
        serial_->setFlowControl(QSerialPort::NoFlowControl);
        if (!serial_->open(QIODevice::ReadWrite)) {
            if (errorOut)
                *errorOut = serial_->errorString();
            return false;
        }
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                        QStringLiteral("serial port %1 open").arg(params_.serialPortName));
        return true;
    }
    }
    return false;
}

void DeviceSimulator::onNewConnection()
{
    while (server_.hasPendingConnections()) {
        QTcpSocket *s = server_.nextPendingConnection();
        if (faultActive(FaultMode::Disconnect)) {
            // 故障窗口内:接受后立刻断开,客户端看到快速失败(重连在 S3 验证)
            s->abort();
            s->deleteLater();
            dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                            QStringLiteral("connection rejected (disconnect fault active)"));
            continue;
        }
        closeClient(); // 新连接顶替旧连接
        client_ = s;
        connect(client_, &QTcpSocket::readyRead, this, &DeviceSimulator::onClientReadyRead);
        connect(client_, &QTcpSocket::disconnected, this,
                &DeviceSimulator::onClientDisconnected);
        decoder_.reset();
        sendSeq_ = 0; // 新 TCP 会话:设备发送序号重新计(ACK 不占用此序号)
        mergeBuffer_.clear();
        mergePending_ = 0;
        fragmentTail_.clear(); // 旧连接的分片尾巴不得写进新流
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                        QStringLiteral("client connected from %1:%2")
                            .arg(s->peerAddress().toString())
                            .arg(s->peerPort()));
    }
}

void DeviceSimulator::onClientDisconnected()
{
    auto *s = qobject_cast<QTcpSocket *>(sender());
    if (!s)
        return;
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                    QStringLiteral("client disconnected"));
    if (s == client_)
        closeClient();
    else
        s->deleteLater(); // 已被顶替的旧连接:延迟回收
}

void DeviceSimulator::onClientReadyRead()
{
    if (!client_)
        return;
    processIncomingBytes(client_->readAll());
}

void DeviceSimulator::onSerialReadyRead()
{
    if (!serial_)
        return;
    processIncomingBytes(serial_->readAll());
}

void DeviceSimulator::onUdpReadyRead()
{
    // 协议 §7:UDP 只承载设备->DMS 单向遥测/心跳,不收命令。
    // 排空待读数据报即可——留着不读只会让 readyRead 反复触发
    while (udp_->hasPendingDatagrams()) {
        const QByteArray dgram = udp_->receiveDatagram().data();
        if (!dgram.isEmpty())
            dms::logMessage(dms::LogLevel::Debug, dms::LogCategory::Protocol, logCtx(),
                            QStringLiteral("udp datagram ignored (%1 byte(s), no commands on udp)")
                                .arg(dgram.size()));
    }
}

void DeviceSimulator::processIncomingBytes(const QByteArray &bytes)
{
    const FrameDecoder::Result r = decoder_.decode(bytes);

    if (r.rxOverflow)
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::RxOverflow),
                        QStringLiteral("rx overflow, decoder buffer cleared"));
    if (r.badCrcFrames > 0)
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameCrc),
                        QStringLiteral("%1 crc error frame(s), resynced").arg(r.badCrcFrames));
    if (r.badHeaderFrames > 0)
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameLength),
                        QStringLiteral("%1 bad header frame(s), resynced").arg(r.badHeaderFrames));
    if (r.droppedBytes > 0)
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FrameLength),
                        QStringLiteral("%1 byte(s) dropped, no frame").arg(r.droppedBytes));
    if (r.unknownTypeFrames > 0)
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("%1 unknown-type frame(s) skipped")
                            .arg(r.unknownTypeFrames));

    for (const DecodedFrame &f : r.frames) {
        if (f.type == quint8(MessageType::Command)) {
            handleCommandFrame(f);
            continue;
        }
        if (f.type == quint8(MessageType::Heartbeat))
            continue; // 客户端心跳:静默(不逐包回声,协议 §4)
        // 客户端发遥测/ACK:结构合法但设备语义外,忽略
        dms::logMessage(dms::LogLevel::Debug, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("unexpected client frame type=0x%1")
                            .arg(f.type, 2, 16, QChar::fromLatin1('0')));
    }
}

void DeviceSimulator::onTelemetryTick()
{
    if (!telemetryRunning_)
        return; // Stop 后只停遥测,心跳与命令照常(协议 §4.2)
    if (faultActive(FaultMode::StopSending))
        return; // 停发有效消息:遥测+心跳全静默

    const bool abnormal = faultActive(FaultMode::AbnormalValue);
    QList<TelemetrySample> samples;
    samples.reserve(4);
    TelemetrySample s;
    s.metricId = quint16(MetricId::Temperature);
    s.valueType = kValueTypeFloat32;
    s.asFloat = abnormal ? 999.0f : std::clamp(tempDist_(rng_), -50.0f, 150.0f);
    samples.append(s);
    s.metricId = quint16(MetricId::Pressure);
    s.asFloat = abnormal ? -5.0f : std::clamp(pressDist_(rng_), 0.0f, 200.0f);
    samples.append(s);
    s.metricId = quint16(MetricId::Voltage);
    s.asFloat = abnormal ? 300.0f : std::clamp(voltDist_(rng_), 0.0f, 15.0f);
    samples.append(s);
    s.metricId = quint16(MetricId::Rpm);
    s.valueType = kValueTypeUint32;
    s.asUint = abnormal ? 4000000000u : rpmDist_(rng_);
    samples.append(s);

    ++sendSeq_;
    deliverFrame(dms::protocol::encodeFrame(quint8(MessageType::Telemetry), sendSeq_,
                                            QDateTime::currentMSecsSinceEpoch(),
                                            MessageCodec::encodeTelemetry(samples)),
                 false);
    ++framesSent_;
    samplesSent_ += samples.size(); // 标准负载四指标(协议 §4.1)
}

void DeviceSimulator::onHeartbeatTick()
{
    if (faultActive(FaultMode::StopSending))
        return;
    ++sendSeq_;
    deliverFrame(dms::protocol::encodeFrame(quint8(MessageType::Heartbeat), sendSeq_,
                                            QDateTime::currentMSecsSinceEpoch(), {}),
                 false);
    ++framesSent_;
}

void DeviceSimulator::onFaultWatchTick()
{
    quint16 mask = 0; // 11 种模式,quint8 放不下
    for (int m = 0; m < kFaultModeCount; ++m)
        if (faultActive(FaultMode(m)))
            mask |= quint16(quint16(1) << m);

    for (int m = 0; m < kFaultModeCount; ++m) {
        const bool nowActive = (mask & (1u << m)) != 0;
        const bool wasActive = (lastFaultMask_ & (1u << m)) != 0;
        if (nowActive == wasActive)
            continue;
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                        QStringLiteral("fault %1 %2")
                            .arg(dms::sim::faultModeName(FaultMode(m)),
                                 nowActive ? QStringLiteral("active")
                                           : QStringLiteral("inactive")));
        if (nowActive) {
            // 窗口开启即刻拆链(docs/15 §5);UDP 无连接可拆,该模式已被 CLI 拒绝
            if (FaultMode(m) == FaultMode::Disconnect) {
                if (client_)
                    closeClient();
                if (serial_ && serial_->isOpen())
                    closeSerial();
            }
            continue;
        }
        // ---- 窗口结束的收尾 ----
        if (FaultMode(m) == FaultMode::Disconnect
            && params_.transport == SimTransport::Serial) {
            // 串口重新打开,会话侧退避重连即恢复(TC-SERIAL-001)
            QString err;
            if (reopenSerial(&err))
                dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                                QStringLiteral("serial port %1 reopened")
                                    .arg(params_.serialPortName));
            else
                dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Net,
                                logCtx(dms::ErrorCode::ConnectionLost),
                                QStringLiteral("serial reopen failed: %1").arg(err));
        }
        if (FaultMode(m) == FaultMode::DatagramReorder && !reorderHold_.isEmpty()) {
            // 窗口结束:被扣住未对调的报文补发,不静默丢失
            sendDatagram(reorderHold_);
            reorderHold_.clear();
        }
    }
    lastFaultMask_ = mask;
}

void DeviceSimulator::onMergeFlush()
{
    if (mergeBuffer_.isEmpty())
        return;
    rawWrite(mergeBuffer_); // 孤帧兜底:对端停发时不能一直滞留
    mergeBuffer_.clear();
    mergePending_ = 0;
}

void DeviceSimulator::handleCommandFrame(const DecodedFrame &frame)
{
    CommandMessage cmd;
    if (MessageCodec::decodeCommand(frame.payload, cmd) != DecodeError::None) {
        // 结构不完整:整帧丢弃并记日志,不回 ACK(协议 §4.2)
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Protocol,
                        logCtx(dms::ErrorCode::FramePayload),
                        QStringLiteral("malformed command dropped, seq=%1").arg(frame.seq));
        return;
    }

    AckStatus status = AckStatus::Success;
    switch (cmd.commandId) {
    case kCmdStart:
        if (!cmd.params.isEmpty()) {
            status = AckStatus::BadParams;
        } else {
            telemetryRunning_ = true;
        }
        break;
    case kCmdStop:
        if (!cmd.params.isEmpty()) {
            status = AckStatus::BadParams;
        } else {
            telemetryRunning_ = false;
        }
        break;
    case kCmdReset:
        if (!cmd.params.isEmpty()) {
            status = AckStatus::BadParams;
            break;
        }
        // 先 ACK 后复位:连接/帧序号不重置(协议 §4.2)
        sendAck(frame.seq, cmd.commandId, AckStatus::Success);
        params_.samplePeriodMs = 100;
        telemetryTimer_.setInterval(params_.samplePeriodMs);
        telemetryRunning_ = true;
        framesSent_ = 0; // 业务计数
        samplesSent_ = 0;
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("reset applied: period=100ms, counters cleared"));
        return; // ACK 已单独发出
    case kCmdSetParameter:
        status = [this](const QList<CommandParam> &params) {
            // 恰好 1 个参数(为 1 时不可能重复);id=1、uint32、len=4、值域 [100,1000]
            if (params.size() != 1)
                return AckStatus::BadParams;
            const CommandParam &p = params.first();
            if (p.id != 1 || p.valueType != kValueTypeUint32 || p.value.size() != 4)
                return AckStatus::BadParams;
            const quint32 v = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(p.value.constData()));
            if (v < kMinPeriodMs || v > kMaxPeriodMs)
                return AckStatus::BadParams;
            params_.samplePeriodMs = int(v);
            telemetryTimer_.setInterval(params_.samplePeriodMs);
            dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Protocol, logCtx(),
                            QStringLiteral("sample period -> %1ms").arg(params_.samplePeriodMs));
            return AckStatus::Success;
        }(cmd.params);
        break;
    default:
        status = AckStatus::Unsupported; // 未知命令:结构可解析,拒绝执行但回 ACK
        break;
    }

    sendAck(frame.seq, cmd.commandId, status);
}

void DeviceSimulator::sendAck(quint32 echoSeq, quint16 commandId, AckStatus status)
{
    if (faultActive(FaultMode::AckNoReply)) {
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Protocol, logCtx(),
                        QStringLiteral("ack suppressed (ackNoReply fault)"));
        return;
    }
    if (faultActive(FaultMode::AckDelay)) {
        QTimer::singleShot(500, this, [this, echoSeq, commandId, status] {
            sendAckNow(echoSeq, commandId, status);
        });
        return;
    }
    sendAckNow(echoSeq, commandId, status);
}

void DeviceSimulator::sendAckNow(quint32 echoSeq, quint16 commandId, AckStatus status)
{
    if (!hasPeer())
        return;
    // ACK 的帧头 Seq 回显命令 Seq,不占用设备发送序号(协议 §4.3)
    deliverFrame(dms::protocol::encodeFrame(quint8(MessageType::CommandAck), echoSeq,
                                            QDateTime::currentMSecsSinceEpoch(),
                                            MessageCodec::encodeCommandAck(commandId, status)),
                 true);
    ++framesSent_;
}

void DeviceSimulator::deliverFrame(const QByteArray &frame, bool isAck)
{
    if (!hasPeer())
        return; // 无对端:设备不缓存离线数据,帧丢弃

    if (params_.transport == SimTransport::Udp) {
        // UDP 报文级故障(REQ-SIM-002):报文是交付单位,不存在半包/粘包
        if (!isAck) {
            if (faultActive(FaultMode::DatagramDrop))
                return; // 整报丢弃:真网络丢包(流式跳帧不配叫丢包,docs/15 §5)
            if (faultActive(FaultMode::DatagramReorder)) {
                // 相邻对调:扣住本报,下一报先发再补发本报;接收端对旧报按
                // seq 模差判"旧"丢弃(REQ-UDP-001),只放行先到的新报
                if (reorderHold_.isEmpty()) {
                    reorderHold_ = frame;
                    return;
                }
                sendDatagram(frame);
                sendDatagram(reorderHold_);
                reorderHold_.clear();
                return;
            }
            if (faultActive(FaultMode::DatagramDuplicate)) {
                sendDatagram(frame); // 同报连发两次:seq 重复应被丢弃
                sendDatagram(frame);
                return;
            }
        }
        QByteArray out = frame;
        if (faultActive(FaultMode::BadCrc))
            out[out.size() - 1] = char(uchar(out[out.size() - 1]) ^ 0xFF);
        sendDatagram(out);
        return;
    }

    // ---- 以下为字节流形态(TCP/串口):帧级故障 ----
    if (!isAck && faultActive(FaultMode::Merge)) {
        // 合帧:凑满两帧一次写;孤帧由 mergeFlushTimer 兜底
        mergeBuffer_.append(frame);
        if (++mergePending_ >= 2) {
            mergeFlushTimer_.stop();
            rawWrite(mergeBuffer_);
            mergeBuffer_.clear();
            mergePending_ = 0;
        } else {
            mergeFlushTimer_.start(300);
        }
        return;
    }
    if (!isAck && faultActive(FaultMode::Fragment)) {
        // 流分片:半帧 + 15ms 后半帧,强制制造跨写边界(喂解码器半包路径)。
        // 上一帧尾巴未补完时,新帧必须整帧排队随尾巴一起补——字节流有序,
        // 两个半帧交错(遥测与心跳同毫秒触发时)会真的腐蚀字节流,而非制造半包
        if (fragmentTail_.isEmpty()) {
            const int half = frame.size() / 2;
            rawWrite(frame.left(half));
            fragmentTail_ = frame.mid(half);
            QTimer::singleShot(15, this, [this] {
                rawWrite(fragmentTail_);
                fragmentTail_.clear();
            });
        } else {
            fragmentTail_.append(frame);
        }
        return;
    }
    QByteArray out = frame;
    if (faultActive(FaultMode::BadCrc))
        out[out.size() - 1] = char(uchar(out[out.size() - 1]) ^ 0xFF); // 翻转 CRC 末字节
    rawWrite(out);
}

void DeviceSimulator::sendDatagram(const QByteArray &frame)
{
    if (!udp_)
        return;
    // 一报一帧(协议 §7):报文边界即帧边界,接收端无需跨报状态
    udp_->writeDatagram(frame, params_.peerAddress, params_.peerPort);
}

void DeviceSimulator::rawWrite(const QByteArray &chunk)
{
    if (params_.transport == SimTransport::Serial) {
        if (serial_ && serial_->isOpen())
            serial_->write(chunk);
        return;
    }
    if (client_ && client_->state() == QAbstractSocket::ConnectedState)
        client_->write(chunk);
}

void DeviceSimulator::closeClient()
{
    if (!client_)
        return;
    // 先断信号再拆链:已被顶替/故障拆除的连接不得再触发任何槽
    disconnect(client_, nullptr, this, nullptr);
    client_->abort();
    client_->deleteLater();
    client_ = nullptr;
}

void DeviceSimulator::closeSerial()
{
    if (!serial_)
        return;
    // 只关端口不删对象:disconnect 窗口结束由巡检重开,信号连接保留
    serial_->close();
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Net, logCtx(),
                    QStringLiteral("serial port %1 closed (disconnect fault)")
                        .arg(params_.serialPortName));
}

bool DeviceSimulator::reopenSerial(QString *errorOut)
{
    if (!serial_)
        return false;
    if (serial_->isOpen())
        return true;
    if (!serial_->open(QIODevice::ReadWrite)) {
        if (errorOut)
            *errorOut = serial_->errorString();
        return false;
    }
    return true;
}

bool DeviceSimulator::faultActive(FaultMode mode) const
{
    const qint64 now = clock_.elapsed();
    for (const FaultSpec &f : params_.faults)
        if (f.mode == mode && f.activeAt(now))
            return true;
    return false;
}

bool DeviceSimulator::hasPeer() const
{
    switch (params_.transport) {
    case SimTransport::Tcp:
        return client_ && client_->state() == QAbstractSocket::ConnectedState;
    case SimTransport::Udp:
        return udp_ != nullptr; // 无连接概念:绑定即"对端可达"
    case SimTransport::Serial:
        return serial_ && serial_->isOpen();
    }
    return false;
}

QString DeviceSimulator::deviceId() const
{
    // 前缀与形态一致:日志里一眼看出 sim 走的哪条链路
    QString pattern = QStringLiteral("sim-tcp-%1");
    if (params_.transport == SimTransport::Udp)
        pattern = QStringLiteral("sim-udp-%1");
    else if (params_.transport == SimTransport::Serial)
        pattern = QStringLiteral("sim-ser-%1");
    return pattern.arg(params_.index + 1, 2, 10, QChar::fromLatin1('0'));
}

QString DeviceSimulator::endpointDesc() const
{
    // TCP/UDP 用端口号,串口用端口名:SIM READY 同步行按此输出
    if (params_.transport == SimTransport::Serial)
        return params_.serialPortName;
    return QString::number(params_.listenPort);
}

dms::LogContext DeviceSimulator::logCtx(dms::ErrorCode code) const
{
    dms::LogContext ctx;
    ctx.deviceId = deviceId();
    ctx.code = code;
    return ctx;
}

} // namespace dms::sim
