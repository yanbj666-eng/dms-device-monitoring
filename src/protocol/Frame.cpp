#include "Frame.h"

#include <QtEndian>

#include <cstring>

#include "Crc32.h"

namespace dms::protocol {

bool FrameHeader::decode(const uchar *header18, FrameHeader &out) noexcept
{
    if (header18[2] != kProtocolVersion) // 偏移2 = Version;未知版本按非法头重同步(协议 §6)
        return false;

    const auto payloadLen = qFromBigEndian<quint16>(header18 + 8); // 偏移8
    if (payloadLen > kMaxPayloadSize)
        return false;

    const auto ts = qFromBigEndian<quint64>(header18 + 10); // 偏移10
    if (ts > kMaxTimestamp)
        return false;

    out.type = header18[3];
    out.seq = qFromBigEndian<quint32>(header18 + 4); // 偏移4
    out.payloadLen = payloadLen;
    out.timestampMs = ts;
    return true;
}

void FrameHeader::encode(const FrameHeader &h, uchar *out18) noexcept
{
    out18[0] = 0xAA; // Magic 大端:AA 55
    out18[1] = 0x55;
    out18[2] = kProtocolVersion;
    out18[3] = h.type;
    qToBigEndian<quint32>(h.seq, out18 + 4);
    qToBigEndian<quint16>(h.payloadLen, out18 + 8);
    qToBigEndian<quint64>(h.timestampMs, out18 + 10);
}

QByteArray encodeFrame(quint8 type, quint32 seq, quint64 timestampMs,
                       const QByteArray &payload)
{
    // 发送侧防御:payloadLen 是 u16,超长静默截断会造成线上错帧;
    // 返回空数组让调用方显式失败,而不是发出一个声明长度错误的帧
    if (payload.size() > kMaxPayloadSize)
        return QByteArray();

    FrameHeader h;
    h.type = type;
    h.seq = seq;
    h.payloadLen = quint16(payload.size());
    h.timestampMs = timestampMs;

    QByteArray f(frameTotalSize(payload.size()), Qt::Uninitialized);
    auto *p = reinterpret_cast<uchar *>(f.data());
    FrameHeader::encode(h, p);
    if (!payload.isEmpty()) // 空载荷 constData() 可能为 null,memcpy 0 字节是 UB
        memcpy(p + kFixedHeaderSize, payload.constData(), payload.size());
    const quint32 crc = Crc32::compute(reinterpret_cast<const uchar *>(f.constData()),
                                        f.size() - kCrcSize);
    qToBigEndian<quint32>(crc, p + f.size() - kCrcSize);
    return f;
}

} // namespace dms::protocol
