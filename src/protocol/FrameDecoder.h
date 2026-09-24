#pragma once

#include <QByteArray>
#include <QList>

#include "Frame.h"

namespace dms::protocol {

// 有界流式帧解码器,TCP/串口共用;UDP 一报一帧不复用本类(协议 §7)。
// 算法遵循 LLD §2:
//   1. 缓存上限 64KiB(可注入),超限记 rxOverflow 并清空恢复;
//   2. 无 Magic 时只保留末尾 AA,防跨块 Magic 丢失;
//   3. 不足 18B 头/不足整帧时等待,不随追加数据无限延长(超时由会话层持有);
//   4. 头非法/CRC 错:从候选 Magic 后一字节重搜索,保留后续有效帧;
//   5. 未知 Type CRC 正确则完整消费并计数,不交付;
//   6. 单轮最多处理 100 帧后让出事件循环,剩余数据留待下一轮。
// 组帧 2000ms 超时不归解码器管:时钟由会话层注入并调用 reset()(LLD §3)。
class FrameDecoder
{
public:
    // 单轮最多处理的帧数(交付 + 未知跳过),防止长时间占用 IO 线程
    static constexpr int kMaxFramesPerRound = 100;

    struct Result
    {
        QList<DecodedFrame> frames;    // 本轮解出的已知类型帧,按到达顺序
        quint32 unknownTypeFrames = 0; // CRC 正确但类型未知,已消费跳过
        quint32 badFrames = 0;         // 头非法或 CRC 错,已重同步
        quint32 droppedBytes = 0;      // 无法构成帧而被丢弃的字节(含溢出清空)
        bool rxOverflow = false;      // 缓存超上限,已清空恢复
    };

    // maxBufferBytes 默认 64KiB(app.ini max_buffer_bytes);
    // 测试可注入小值以复现溢出路径
    explicit FrameDecoder(int maxBufferBytes = 65536)
        : maxBufferBytes_(maxBufferBytes) {}

    // 喂入一块字节流并尝试解码;返回本轮结果。
    // 不完整尾部保留在内部缓存,等下一块补齐;空块仅驱动剩余缓存。
    Result decode(const QByteArray &chunk);

    // 清空缓存:新会话开始或会话层判定组帧超时时调用
    void reset() { buffer_.clear(); }

    qsizetype bufferedBytes() const { return buffer_.size(); }

private:
    // 从消费点起搜索 2 字节 Magic;找到写回 pos,跨块尾的半个 Magic 返回 PartialCandidate
    enum class MagicSearch { Found, None, PartialTail };
    MagicSearch findMagic(const uchar *d, int size, int from, int &pos) const;

    // 删除缓存前 prefixBytes 字节(保留等待中的尾部)
    void trimPrefix(int prefixBytes);

    QByteArray buffer_;      // 未消费字节,起始总是当前候选或等待数据
    const int maxBufferBytes_;
};

} // namespace dms::protocol
