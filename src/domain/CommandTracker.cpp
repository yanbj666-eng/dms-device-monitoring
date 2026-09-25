#include "CommandTracker.h"

#include "MessageCodec.h"

namespace dms::domain {

quint32 CommandTracker::nextSeq()
{
    return ++lastSeq_;
}

bool CommandTracker::start(quint32 seq, quint16 commandId, qint64 nowMs)
{
    if (pending_)
        return false; // 每设备最多一个未决命令(LLD §4)
    pending_ = Pending{seq, commandId, nowMs, false};
    return true;
}

void CommandTracker::markWriteAccepted()
{
    if (pending_)
        pending_->writeAccepted = true;
}

std::optional<CommandOutcome> CommandTracker::onAck(quint32 seq, quint16 commandId,
                                                    quint16 status)
{
    if (!pending_ || pending_->seq != seq || pending_->commandId != commandId)
        return std::nullopt; // 迟到/重复/无关 ACK:不改已结束结果,调用方记日志

    CommandOutcome outcome;
    outcome.seq = seq;
    outcome.commandId = commandId;
    outcome.state = status == quint16(dms::protocol::AckStatus::Success)
                        ? CommandState::Succeeded
                        : CommandState::Rejected;
    outcome.ackStatus = status;
    pending_.reset();
    return outcome;
}

std::optional<CommandOutcome> CommandTracker::pollTimeout(qint64 nowMs, int ackTimeoutMs)
{
    if (!pending_ || nowMs - pending_->startedAtMs < ackTimeoutMs)
        return std::nullopt;

    CommandOutcome outcome;
    outcome.seq = pending_->seq;
    outcome.commandId = pending_->commandId;
    outcome.state = CommandState::TimedOut;
    outcome.detail = QStringLiteral("ack timeout, execution result unknown");
    pending_.reset();
    return outcome;
}

std::optional<CommandOutcome> CommandTracker::cancel(const QString &reason)
{
    if (!pending_)
        return std::nullopt;

    CommandOutcome outcome;
    outcome.seq = pending_->seq;
    outcome.commandId = pending_->commandId;
    outcome.state = CommandState::Cancelled;
    outcome.detail = reason;
    pending_.reset();
    return outcome;
}

std::optional<CommandOutcome> CommandTracker::fail(const QString &reason)
{
    if (!pending_)
        return std::nullopt;

    CommandOutcome outcome;
    outcome.seq = pending_->seq;
    outcome.commandId = pending_->commandId;
    outcome.state = CommandState::Failed;
    outcome.detail = reason;
    pending_.reset();
    return outcome;
}

} // namespace dms::domain
