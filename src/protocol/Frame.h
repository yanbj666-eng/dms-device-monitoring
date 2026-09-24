#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace dms::protocol {

// ---- 协议 v1 帧常量(协议规范 §2)----
inline constexpr quint16 kMagic = 0xAA55;       // 帧头 AA 55(大端字节序传输)
inline constexpr quint8 kProtocolVersion = 0x01;
inline constexpr int kFixedHeaderSize = 18;     // Magic2 + Ver1 + Type1 + Seq4 + Len2 + Ts8
inline constexpr int kCrcSize = 4;
inline constexpr int kMaxPayloadSize = 4096;    // 载荷上限,不是整帧上限(协议 §2)
inline constexpr int kMinFrameSize = kFixedHeaderSize + kCrcSize;                    // 22
inline constexpr int kMaxFrameSize = kFixedHeaderSize + kMaxPayloadSize + kCrcSize;  // 4118
// v1 时间戳只接受 0..INT64_MAX,避免写入 SQLite 有符号整数溢出(协议 §1)
inline constexpr quint64 kMaxTimestamp = 9223372036854775807ULL;

// v1 实际使用的消息类型;0x20/0x30 保留不发送,
// 解码侧按未知类型"CRC 正确则完整跳过并计数"(协议 §4)
enum class MessageType : quint8 {
    Heartbeat  = 0x01,
    Telemetry  = 0x02,
    Command    = 0x10,
    CommandAck = 0x11,
};

constexpr bool isKnownMessageType(quint8 type) noexcept
{
    return type == static_cast<quint8>(MessageType::Heartbeat)
        || type == static_cast<quint8>(MessageType::Telemetry)
        || type == static_cast<quint8>(MessageType::Command)
        || type == static_cast<quint8>(MessageType::CommandAck);
}

// 解码输出值对象(LLD §2:不暴露缓存内部地址)
struct DecodedFrame {
    quint8 type = 0;
    quint32 seq = 0;
    quint64 timestampMs = 0;  // 0 = 发送端时间未知,接收端单独记接收时间
    QByteArray payload;
};

// 整帧总长 22+N(含尾部 CRC)
inline constexpr int frameTotalSize(int payloadLen)
{
    return kFixedHeaderSize + payloadLen + kCrcSize;
}

// 组装完整帧:18B 头 + 载荷 + CRC。
// 发送侧唯一入口:模拟器(S2)/客户端命令(S3)/测试共用,保证编码口径一致
QByteArray encodeFrame(quint8 type, quint32 seq, quint64 timestampMs,
                       const QByteArray &payload);

// 18B 固定头。所有多字节字段大端(协议 §1);
// 禁止未对齐字节强转 struct(LLD §2),必须逐字段 qFromBigEndian
struct FrameHeader
{
    quint8 type = 0;
    quint32 seq = 0;
    quint16 payloadLen = 0;
    quint64 timestampMs = 0;

    // 解析 18B(不校验 Magic,由解码器定位后调用);
    // 校验 Version、payloadLen<=4096、时间戳上限,任一不满足返回 false
    static bool decode(const uchar *header18, FrameHeader &out) noexcept;

    // 编码 18B 到 out(调用方保证 out 至少 kFixedHeaderSize 可写)
    static void encode(const FrameHeader &h, uchar *out18) noexcept;
};

} // namespace dms::protocol
