#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

namespace dms::sim {

// 故障模式(docs/15 §5)。UDP 专属的丢包/乱序/重复属 S7,不在此列
enum class FaultMode {
    BadCrc,        // 坏 CRC:帧尾字节翻转
    AbnormalValue, // 异常值:遥测给有限但超常的业务值(喂告警引擎用)
    StopSending,   // 停发有效消息:遥测+心跳全静默(COMM_LOST 场景)
    Disconnect,    // 主动断开当前连接,窗口内拒绝新连接
    AckDelay,      // ACK 延迟 500ms 再回
    AckNoReply,    // 不回 ACK(命令超时场景)
    Fragment,      // 流分片:每帧拆成两段,间隔 15ms
    Merge,         // 合帧:相邻两帧并一次写(粘包场景)
};

struct FaultSpec {
    FaultMode mode;
    qint64 startMs = 0;    // 相对模拟器启动
    qint64 durationMs = 0; // >0
    bool activeAt(qint64 nowMs) const { return nowMs >= startMs && nowMs < startMs + durationMs; }
};

inline QString faultModeName(FaultMode mode)
{
    switch (mode) {
    case FaultMode::BadCrc:        return QStringLiteral("badCrc");
    case FaultMode::AbnormalValue: return QStringLiteral("abnormalValue");
    case FaultMode::StopSending:   return QStringLiteral("stopSending");
    case FaultMode::Disconnect:    return QStringLiteral("disconnect");
    case FaultMode::AckDelay:      return QStringLiteral("ackDelay");
    case FaultMode::AckNoReply:    return QStringLiteral("ackNoReply");
    case FaultMode::Fragment:      return QStringLiteral("fragment");
    case FaultMode::Merge:         return QStringLiteral("merge");
    }
    return QStringLiteral("unknown");
}

inline bool faultModeFromString(const QString &name, FaultMode &out)
{
    static const QHash<QString, FaultMode> map = {
        {QStringLiteral("badcrc"), FaultMode::BadCrc},
        {QStringLiteral("abnormalvalue"), FaultMode::AbnormalValue},
        {QStringLiteral("stopsending"), FaultMode::StopSending},
        {QStringLiteral("disconnect"), FaultMode::Disconnect},
        {QStringLiteral("ackdelay"), FaultMode::AckDelay},
        {QStringLiteral("acknoreply"), FaultMode::AckNoReply},
        {QStringLiteral("fragment"), FaultMode::Fragment},
        {QStringLiteral("merge"), FaultMode::Merge},
    };
    const auto it = map.constFind(name.toLower());
    if (it == map.constEnd())
        return false;
    out = it.value();
    return true;
}

// "badCrc@2000+3000" -> {BadCrc, 2000ms 起,持续 3000ms}
inline bool parseFaultSpec(const QString &text, FaultSpec &out)
{
    const qsizetype at = text.indexOf(QLatin1Char('@'));
    const qsizetype plus = text.indexOf(QLatin1Char('+'), at < 0 ? 0 : at + 1);
    if (at < 0 || plus < 0)
        return false;
    bool okStart = false, okDur = false;
    const qint64 start = text.mid(at + 1, plus - at - 1).toLongLong(&okStart);
    const qint64 dur = text.mid(plus + 1).toLongLong(&okDur);
    if (!okStart || !okDur || start < 0 || dur <= 0)
        return false;
    FaultMode mode;
    if (!faultModeFromString(text.left(at), mode))
        return false;
    out = FaultSpec{mode, start, dur};
    return true;
}

} // namespace dms::sim
