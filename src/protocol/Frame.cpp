#include "Frame.h"

#include <QtEndian>

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

} // namespace dms::protocol
