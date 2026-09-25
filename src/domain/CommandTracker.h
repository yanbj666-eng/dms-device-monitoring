#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace dms::domain {

// 命令生命周期状态(LLD §4):Sent 是唯一非终态
enum class CommandState {
    Sent,      // 已入发送队列,等待 ACK
    Succeeded, // ACK=Success
    Rejected,  // ACK=Unsupported/BadParams/Busy/Failed
    TimedOut,  // 2000ms 无 ACK,执行结果未知
    Cancelled, // 人工断开/链路失联
    Failed,    // 编码或本地发送失败
};

inline QString commandStateName(CommandState state)
{
    switch (state) {
    case CommandState::Sent:      return QStringLiteral("Sent");
    case CommandState::Succeeded: return QStringLiteral("Succeeded");
    case CommandState::Rejected:  return QStringLiteral("Rejected");
    case CommandState::TimedOut:  return QStringLiteral("TimedOut");
    case CommandState::Cancelled: return QStringLiteral("Cancelled");
    case CommandState::Failed:    return QStringLiteral("Failed");
    }
    return QStringLiteral("Unknown");
}

// 命令结果值对象:跨线程 queued signal 传值(HLD §2)
struct CommandOutcome {
    quint32 seq = 0;
    quint16 commandId = 0;
    CommandState state = CommandState::Sent;
    quint16 ackStatus = 0; // AckStatus 数值,仅 Succeeded/Rejected 有意义
    QString detail;        // Failed/Cancelled 的原因
};

// 命令生命周期跟踪(LLD §4):
//   - Seq 按方向递增,会话存活期间不复位(重连后旧 ACK 无法关联新命令);
//   - 每设备最多一个未决命令;
//   - ACK 只终结匹配 Seq+commandId 的未决命令,迟到/重复由调用方记日志;
//   - 不自动重发(有副作用的命令不能靠重发保证幂等)。
// 纯逻辑、无 QObject:时钟推进与信号发射由 DeviceSession 驱动
class CommandTracker
{
public:
    // 从 1 开始,0 保留为"无效";32 位用尽前应更换会话(LLD §4),此处不做回绕处理
    quint32 nextSeq();

    // 新命令登记为 Sent;false = 已有未决命令
    bool start(quint32 seq, quint16 commandId, qint64 nowMs);

    bool hasPending() const { return pending_.has_value(); }
    quint32 pendingSeq() const { return pending_.has_value() ? pending_->seq : 0; }
    bool pendingWriteAccepted() const
    {
        return pending_.has_value() && pending_->writeAccepted;
    }
    void markWriteAccepted(); // transport writeAccepted:已入队,只欠 ACK

    // 三个终结路径,返回结果交会话发射 commandFinished;无未决命令时 nullopt
    std::optional<CommandOutcome> onAck(quint32 seq, quint16 commandId, quint16 status);
    std::optional<CommandOutcome> pollTimeout(qint64 nowMs, int ackTimeoutMs);
    std::optional<CommandOutcome> cancel(const QString &reason); // 人工断开/链路失联
    std::optional<CommandOutcome> fail(const QString &reason);   // 本地发送失败

private:
    struct Pending {
        quint32 seq = 0;
        quint16 commandId = 0;
        qint64 startedAtMs = 0; // 入发送队列时刻,ACK 超时起点(LLD §4)
        bool writeAccepted = false;
    };

    std::optional<Pending> pending_;
    quint32 lastSeq_ = 0;
};

} // namespace dms::domain

Q_DECLARE_METATYPE(dms::domain::CommandOutcome)
