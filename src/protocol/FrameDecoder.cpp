#include "FrameDecoder.h"

#include <QtEndian>

#include "Crc32.h"

namespace dms::protocol {

namespace {
// 单独字符比较需转 uchar:MSVC 的 char 默认有符号,直接 == 0xAA 会永假
inline bool isHighMagic(uchar c) { return c == 0xAA; }
inline bool isLowMagic(uchar c) { return c == 0x55; }
} // namespace

FrameDecoder::MagicSearch FrameDecoder::findMagic(const uchar *d, int size, int from, int &pos) const
{
    for (int i = from; i < size - 1; ++i) {
        if (isHighMagic(d[i]) && isLowMagic(d[i + 1])) {
            pos = i;
            return MagicSearch::Found;
        }
    }
    // 扫到倒数第一字节都没有完整 Magic:末尾单 0xAA 可能是跨块的半个候选
    if (size > from && isHighMagic(d[size - 1]))
        return MagicSearch::PartialTail;
    return MagicSearch::None;
}

void FrameDecoder::trimPrefix(int prefixBytes)
{
    if (prefixBytes > 0)
        buffer_.remove(0, prefixBytes);
}

FrameDecoder::Result FrameDecoder::decode(const QByteArray &chunk)
{
    Result r;

    // 溢出恢复:新增数据会超上限时,整体丢弃并置标志,下一块重新找同步。
    // 不做部分保留——错误洪泛场景下任何前缀都不可信(LLD §3 隔离原则)
    if (buffer_.size() + chunk.size() > maxBufferBytes_) {
        r.droppedBytes = quint32(buffer_.size() + chunk.size());
        r.rxOverflow = true;
        buffer_.clear();
        return r;
    }
    buffer_.append(chunk);

    int consumed = 0;      // 已消费(交付/跳过/丢弃)的前缀长度
    int processed = 0;     // 本轮已处理帧数(交付 + 未知跳过),含 100 帧上限
    const uchar *d = reinterpret_cast<const uchar *>(buffer_.constData());
    const int size = buffer_.size();

    while (processed < kMaxFramesPerRound) {
        int magic = -1;
        const MagicSearch ms = findMagic(d, size, consumed, magic);

        if (ms == MagicSearch::PartialTail) {
            // 末尾单 AA:可能半个 Magic 在下一块;丢弃 AA 之前的所有字节
            r.droppedBytes += quint32(size - 1 - consumed);
            trimPrefix(size - 1);
            return r;
        }
        if (ms == MagicSearch::None) {
            // 无任何候选:全部丢弃(LLD §2 规则 2)
            r.droppedBytes += quint32(size - consumed);
            trimPrefix(size);
            return r;
        }

        // 候选前的垃圾字节计入丢弃
        if (magic > consumed) {
            r.droppedBytes += quint32(magic - consumed);
            consumed = magic;
        }

        if (size - consumed < kFixedHeaderSize) {
            // 不足 18B 头:保留候选等待下一块,不丢弃不计数
            trimPrefix(consumed);
            return r;
        }

        FrameHeader h;
        if (!FrameHeader::decode(d + consumed, h)) {
            // 头非法:候选后一字节起重搜索(协议 §6)
            ++r.badFrames;
            ++consumed;
            continue;
        }

        const int total = frameTotalSize(h.payloadLen);
        if (size - consumed < total) {
            // 头合法但整帧未到齐:保留等待;超时判断归会话层
            trimPrefix(consumed);
            return r;
        }

        const uchar *frame = d + consumed;
        const quint32 expect = qFromBigEndian<quint32>(frame + total - kCrcSize);
        if (Crc32::compute(frame, total - kCrcSize) != expect) {
            // CRC 错:同样候选 +1 重同步;坏帧不更新任何存活/指标时间(协议 §6)
            ++r.badFrames;
            ++consumed;
            continue;
        }

        ++processed;
        if (!isKnownMessageType(h.type)) {
            // CRC 正确的未知类型:完整消费,只计数不交付(协议 §4)
            ++r.unknownTypeFrames;
        } else {
            r.frames.append(DecodedFrame{
                h.type,
                h.seq,
                h.timestampMs,
                QByteArray(reinterpret_cast<const char *>(frame + kFixedHeaderSize),
                           h.payloadLen),
            });
        }
        consumed += total;
    }

    // 达到单轮上限让出事件循环;未消费数据留在缓存,由下一次 decode() 继续
    trimPrefix(consumed);
    return r;
}

} // namespace dms::protocol
