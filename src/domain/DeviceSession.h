#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QUuid>

#include <functional>

#include "CommandTracker.h"
#include "FrameDecoder.h"
#include "MessageCodec.h"
#include "transport/ITransport.h"

class QTimer;

namespace dms::domain {

// 会话交付的遥测值对象(跨线程 queued signal 必须是值类型,HLD §2)
struct TelemetryBatch {
    QList<dms::protocol::TelemetrySample> samples; // 仅已知指标
    quint32 unknownMetrics = 0;                    // 结构合法的未知指标计数
    quint32 seq = 0;                               // 发送端帧序号
    quint64 timestampMs = 0;                       // 发送端时间,0=未知
    QUuid sessionId;                               // 本地会话标识,入库唯一键用(S5)
};

// 会话状态机状态(LLD §3)
enum class SessionState {
    Disconnected, // 人工断开/未启动
    Connecting,   // 建链中或已打开等待首条有效消息
    Online,       // 通信与四指标都健康
    Degraded,     // 链路活着但指标过期/连续坏帧,通信继续
    Reconnecting, // 失联,退避等待下一次尝试(只允许一个在途尝试)
    Fault,        // 配置错误等不可恢复本地错误,停止重试等人工修正
};

inline QString sessionStateName(SessionState state)
{
    switch (state) {
    case SessionState::Disconnected:  return QStringLiteral("Disconnected");
    case SessionState::Connecting:    return QStringLiteral("Connecting");
    case SessionState::Online:        return QStringLiteral("Online");
    case SessionState::Degraded:      return QStringLiteral("Degraded");
    case SessionState::Reconnecting:  return QStringLiteral("Reconnecting");
    case SessionState::Fault:         return QStringLiteral("Fault");
    }
    return QStringLiteral("Unknown");
}

// 超时与重试参数(app.ini 可配,LLD §3);全部可注入,受控测试用假时钟驱动。
// transportKind 不是 ini 字段:由装配按 devices.json 的 transport 填入,
// 决定会话状态机的链路语义差异(LLD §3)
struct SessionConfig {
    dms::transport::TransportKind transportKind = dms::transport::TransportKind::Tcp;
    int pollIntervalMs = 100;           // 状态检查周期
    int offlineTimeoutMs = 4000;        // 无有效接收 → 失联
    int firstFrameTimeoutMs = 4000;     // 打开后无首个有效帧 → 重连
    int metricTimeoutMs = 3000;         // 必需指标无更新 → Degraded
    int frameAssemblyTimeoutMs = 2000;  // 组帧半包滞留上限
    int decoderMaxBufferBytes = 65536;  // 解码缓存上限 app.ini max_buffer_bytes
    int commandAckTimeoutMs = 2000;     // 命令 ACK 超时
    int consecutiveBadCrcLimit = 5;     // 连续 CRC 错 → Degraded
    int goodFramesToRecoverOnline = 3;  // Degraded → Online 需连续有效帧
    qint64 stableOnlineResetMs = 10000; // 稳定 Online 才清零重试计数
    int reconnectBaseMs = 1000;         // 重试基数 1,2,4,8,10,10…s
    int reconnectMinMs = 100;           // 抖动后下限 0.1s
    int reconnectMaxMs = 10000;         // reconnect_max
};

// 设备会话(S3):连接 -> FrameDecoder -> MessageCodec -> 值对象信号,
// 加上状态机(Online/Degraded/Reconnecting)、自动重连与退避、
// 组帧 2000ms 超时、命令生命周期(LLD §3/§4)。
//
// 线程约定:会话与其 transport 同线程创建使用(IO 线程);
// GUI 侧只连接信号消费值对象,不直接调方法(除 S4 以后的快照接口)
class DeviceSession : public QObject
{
    Q_OBJECT

public:
    // 单调毫秒时钟与 ±10% 抖动源,均可注入(LLD §3"所有时间间隔用可注入时钟")
    using Clock = std::function<qint64()>;
    using JitterPercent = std::function<int()>; // 返回 -10..+10

    // 接管 transport 所有权(reparent 到本会话)
    DeviceSession(QString deviceId, dms::transport::ITransport *transport,
                  SessionConfig config = SessionConfig(),
                  Clock clock = DeviceSession::defaultClock,
                  JitterPercent jitter = DeviceSession::defaultJitter,
                  QObject *parent = nullptr);

    QString deviceId() const { return deviceId_; }
    QUuid sessionId() const { return sessionId_; } // 本地每会话 UUID,不上帧(LLD §4)
    SessionState state() const { return state_; }

    void start(); // 人工连接 → Connecting;活动状态下重复 start 忽略
    void stop();  // 人工断开 → Disconnected,取消定时器与未决命令

    // 发命令并纳入生命周期(LLD §4):返回分配的 Seq,0 = 拒绝
    // (状态不在 Online/Degraded、transport 不可写、已有未决命令)。
    // 进展经 commandSent / commandFinished 异步报告,不自动重发。
    quint32 sendCommand(quint16 commandId,
                        const QList<dms::protocol::CommandParam> &params = {});

    // 周期超时检查:内部 QTimer 驱动;测试注入假时钟后手动调用以推进时间
    void checkTimeouts();

    // 计数:仅所属线程读;framesReceived 只含成功交付的帧
    quint64 framesReceived() const { return framesReceived_; }
    quint32 badCrcFrames() const { return badCrcFrames_; }
    quint32 badHeaderFrames() const { return badHeaderFrames_; }
    quint32 unknownTypeFrames() const { return unknownTypeFrames_; }
    quint32 payloadErrors() const { return payloadErrors_; }
    // UDP 诊断(协议 §7):非"恰好一帧"被整报拒绝的报数;旧/重复 seq 丢弃的帧数
    quint32 malformedDatagrams() const { return malformedDatagrams_; }
    quint32 staleSeqFrames() const { return staleSeqFrames_; }

    static qint64 defaultClock(); // steady_clock 毫秒(与 Logger 同源)
    static int defaultJitter();   // QRandomGenerator 均匀取 -10..+10

signals:
    void sessionOpened();
    void sessionClosed();
    void stateChanged(dms::domain::SessionState newState);
    void heartbeatReceived(quint32 seq, quint64 timestampMs);
    // 结构合法的遥测(可能全是未知指标:samples 空 + unknownMetrics>0,
    // 存活/新鲜度语义由状态机解释)
    void telemetryReceived(dms::domain::TelemetryBatch batch);
    // status 是 dms::protocol::AckStatus 的数值(quint16 直传,省一次 metatype)
    void commandAckReceived(quint32 seq, quint16 commandId, quint16 status);
    void commandSent(quint32 seq, quint16 commandId);
    void commandFinished(dms::domain::CommandOutcome outcome);
    void sessionError(dms::ErrorCode code, QString message);
    // 每个解码块的协议健康(S6 告警引擎消费,LLD §6):
    // 坏 CRC 帧数 + 有效帧数(心跳/遥测/ACK,CRC 与载荷都通过)。
    // 同一块先计坏帧后计好帧,与会话自身 streak 更新顺序一致
    void protocolHealth(quint32 badCrcFrames, quint32 goodFrames);

private:
    void onTransportOpened();
    void onTransportClosed();
    void onWriteAccepted(quint64 requestId);
    void onBytesReceived(const QByteArray &bytes);
    // UDP 单报入口(协议 §7):一报一帧,不跨报拼接;seq 模差防御
    void onDatagramReceived(const QByteArray &bytes, const QHostAddress &sender,
                            quint16 senderPort);
    void onTransportError(const dms::transport::TransportError &error);
    // 返回"是否有效帧"(载荷通过):onBytesReceived 据此计 protocolHealth 的好帧数
    bool handleTelemetry(const dms::protocol::DecodedFrame &frame);
    bool handleCommandAck(const dms::protocol::DecodedFrame &frame);

    void setState(SessionState next);
    // 失联/建链失败/首帧超时的统一入口:取消未决命令、关旧 transport、排退避
    void enterReconnecting(const QString &reason, dms::ErrorCode code);
    qint64 backoffDelayMs() const;
    // 任一有效帧(心跳/遥测/ACK,CRC 与载荷通过)的公共记账
    void noteValidReceive();
    void noteTelemetryMetrics(const QList<dms::protocol::TelemetrySample> &samples);
    bool metricsAllFresh(qint64 now) const;
    bool anyMetricStale(qint64 now) const;
    void finishPending(std::optional<CommandOutcome> outcome);
    dms::LogContext logCtx(dms::ErrorCode code = dms::ErrorCode::None) const;
    // UDP seq 模差判断(协议 §7):d=(new-last) mod 2^32,0<d<2^31 为新;
    // 无基准(首轮/失联清除后)任何 seq 都可作为新基准
    bool seqIsNew(quint32 seq) const;
    // 解码结果的计数与日志(流式/单报共用)
    void accountDecodedEvents(const dms::protocol::FrameDecoder::Result &result);
    // 单帧分发:心跳/遥测/ACK;返回 protocolHealth 口径的好帧
    bool dispatchFrame(const dms::protocol::DecodedFrame &frame);

    QString deviceId_;
    QUuid sessionId_;
    dms::transport::ITransport *transport_ = nullptr;
    dms::protocol::FrameDecoder decoder_; // 每 Session 独立流缓存(LLD §2)
    CommandTracker commands_;             // 命令生命周期(LLD §4)
    QTimer *pollTimer_ = nullptr;         // start() 时在所属线程懒创建(线程红线)
    SessionConfig config_;
    Clock clock_;
    JitterPercent jitter_;

    SessionState state_ = SessionState::Disconnected;
    // 状态机时间锚点(全部取自注入时钟)
    qint64 openedAtMs_ = 0;           // 本轮 transport 打开时刻,0=从未打开
    bool firstValidSeen_ = false;     // 本轮是否已见首条有效消息
    qint64 lastValidReceiveMs_ = 0;   // 任一有效帧时刻,失联判定用
    qint64 metricLastMs_[4] = {0, 0, 0, 0}; // 已知指标 1..4 的最后遥测时刻
    qint64 partialSinceMs_ = 0;       // 组帧缓存开始滞留时刻,0=无半包
    qint64 onlineStableSinceMs_ = 0;  // Online 起始时刻,稳定 10s 清零退避
    qint64 retryAtMs_ = 0;            // 下一次重连尝试到期时刻
    int reconnectAttempts_ = 0;       // 已点火的尝试次数,决定退避档位
    int consecutiveBadCrc_ = 0;       // 连续 CRC 错,任一有效帧清零
    int consecutiveGoodFrames_ = 0;   // Degraded 期间连续有效帧,Online 恢复用
    bool closeByUs_ = false;          // 区分主动拆链与对端关闭

    quint64 framesReceived_ = 0;
    quint32 badCrcFrames_ = 0;
    quint32 badHeaderFrames_ = 0;
    quint32 unknownTypeFrames_ = 0;
    quint32 payloadErrors_ = 0;
    // UDP 单报防御计数(协议 §7):异常整报拒绝 / 旧·重复 seq 帧丢弃
    quint32 malformedDatagrams_ = 0;
    quint32 staleSeqFrames_ = 0;
    quint32 udpLastSeq_ = 0;      // 最近接受的帧 seq 基准
    bool udpSeqBaseValid_ = false; // 无基准时首条有效帧建立新基准
    // 采集启停(S6,LLD §6"Stop 已 ACK 后暂停遥测陈旧检测"):
    // 设备确认 Stop 后,指标陈旧不再把 Online 拉成 Degraded;
    // Start 确认后重新计时。只认 ACK 成功,不臆断未确认命令的执行结果
    bool acquisitionActive_ = true;
};

} // namespace dms::domain

Q_DECLARE_METATYPE(dms::domain::TelemetryBatch)
Q_DECLARE_METATYPE(dms::domain::SessionState)
// CommandOutcome 的 Q_DECLARE_METATYPE 在 CommandTracker.h(避免重复特化)
