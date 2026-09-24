// tst_framedecoder:流式帧解码器测试
// 主向量:协议规范 §5 的独立固定帧(含 CRC),不做编码器↔解码器互验
// 边界向量:用库内 encodeFrame 构造(半包/粘包/坏CRC/未知类型/溢出/单轮上限)
#include <QtTest>

#include "Frame.h"
#include "FrameDecoder.h"

using dms::protocol::DecodedFrame;
using dms::protocol::FrameDecoder;
using dms::protocol::MessageType;

namespace {

// 委托给库内统一编码入口,与模拟器(S2)/命令发送(S3)同一套实现
QByteArray buildFrame(quint8 type, quint32 seq, quint64 ts, const QByteArray &payload)
{
    return dms::protocol::encodeFrame(type, seq, ts, payload);
}

} // namespace

class TestFrameDecoder : public QObject
{
    Q_OBJECT

private slots:
    // ---- 协议规范 §5 固定向量 ----
    void specHeartbeatVector();
    void specTelemetryVector();
    // ---- 分块/粘包边界 ----
    void halfPacketAcrossChunks();
    void stickyFramesInOneChunk();
    void magicSplitAcrossChunks();
    void truncatedTailKept();
    // ---- 异常与重同步 ----
    void garbageNoMagicDropped();
    void badCrcResyncKeepsNextFrame();
    void invalidVersionResync();
    void payloadLenOverLimitResync();
    void timestampOverLimitResync();
    void unknownTypeConsumedNotDelivered();
    void embeddedMagicInsidePayload();
    // ---- 上限与恢复 ----
    void overflowDropsAndRecovers();
    void hundredFrameRoundLimit();

private:
    static constexpr quint8 kUnknownType = 0x20; // v1 保留(协议 §4)
};

// §5 向量1:空 HEARTBEAT,Seq=1,Ts=0,22B
void TestFrameDecoder::specHeartbeatVector()
{
    FrameDecoder dec;
    const auto r = dec.decode(QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf"));
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.badFrames, 0u);
    QCOMPARE(r.frames[0].type, quint8(MessageType::Heartbeat));
    QCOMPARE(r.frames[0].seq, 1u);
    QCOMPARE(r.frames[0].timestampMs, 0ull);
    QVERIFY(r.frames[0].payload.isEmpty());
}

// §5 向量2:TELEMETRY,Seq=2,Ts=1000,temperature=25.0,30B
void TestFrameDecoder::specTelemetryVector()
{
    FrameDecoder dec;
    const auto r = dec.decode(QByteArray::fromHex(
        "aa55010200000002000800000000000003e80001010441c800003fe22715"));
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0].type, quint8(MessageType::Telemetry));
    QCOMPARE(r.frames[0].seq, 2u);
    QCOMPARE(r.frames[0].timestampMs, 1000ull);
    QCOMPARE(r.frames[0].payload, QByteArray::fromHex("0001010441c80000"));
}

// 半包:30B 帧切成 10/5/15 三块喂入,仍恰好解出一帧
void TestFrameDecoder::halfPacketAcrossChunks()
{
    const QByteArray whole = QByteArray::fromHex(
        "aa55010200000002000800000000000003e80001010441c800003fe22715");
    FrameDecoder dec;
    QVERIFY(dec.decode(whole.left(10)).frames.isEmpty());
    QVERIFY(dec.decode(whole.mid(10, 5)).frames.isEmpty());
    const auto r = dec.decode(whole.mid(15));
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0].seq, 2u);
    QCOMPARE(dec.bufferedBytes(), 0);
}

// 粘包:两个固定向量连成一整块,一次解出两帧且顺序正确
void TestFrameDecoder::stickyFramesInOneChunk()
{
    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    const QByteArray tl = QByteArray::fromHex(
        "aa55010200000002000800000000000003e80001010441c800003fe22715");
    FrameDecoder dec;
    const auto r = dec.decode(hb + tl);
    QCOMPARE(r.frames.size(), 2);
    QCOMPARE(r.frames[0].type, quint8(MessageType::Heartbeat));
    QCOMPARE(r.frames[1].type, quint8(MessageType::Telemetry));
}

// Magic AA 55 被块边界切开:前块结尾 AA,后块开头 55,不能丢帧
void TestFrameDecoder::magicSplitAcrossChunks()
{
    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    FrameDecoder dec;
    QVERIFY(dec.decode(QByteArray::fromHex("00ff")).frames.isEmpty()); // 前置垃圾
    QVERIFY(dec.decode(hb.left(1)).frames.isEmpty());                  // 单 AA 缓存
    const auto r = dec.decode(hb.mid(1));
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0].seq, 1u);
}

// 尾部不足 18B 头的候选要完整保留,不提前丢弃
void TestFrameDecoder::truncatedTailKept()
{
    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    FrameDecoder dec;
    QVERIFY(dec.decode(hb.left(12)).frames.isEmpty());
    QCOMPARE(dec.bufferedBytes(), 12);
    const auto r = dec.decode(hb.mid(12));
    QCOMPARE(r.frames.size(), 1);
}

// 无 Magic 的纯垃圾:全部丢弃,只留末尾 AA;不崩溃不残留
void TestFrameDecoder::garbageNoMagicDropped()
{
    FrameDecoder dec;
    const auto r = dec.decode(QByteArray::fromHex("11223344aabb"));
    QVERIFY(r.frames.isEmpty());
    QCOMPARE(r.droppedBytes, 6u);
    QCOMPARE(dec.bufferedBytes(), 0);
}

// 坏 CRC:不交付、计数,且不影响同一缓存中紧随其后的有效帧
void TestFrameDecoder::badCrcResyncKeepsNextFrame()
{
    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    QByteArray corrupted = hb;
    corrupted[21] = char(uchar(corrupted[21]) ^ 0xFF); // 破坏 CRC 高位字节

    FrameDecoder dec;
    const auto r = dec.decode(corrupted + hb); // 坏帧 + 好帧同块
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.badFrames, 1u);
    QCOMPARE(r.frames[0].seq, 1u);
}

// 非法版本(0x02):整帧无效,重同步后保留后续帧
void TestFrameDecoder::invalidVersionResync()
{
    QByteArray bad = buildFrame(0x01, 7, 100, "");
    bad[2] = 0x02; // 篡改 Version

    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    FrameDecoder dec;
    const auto r = dec.decode(bad + hb);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.badFrames, 1u);
    QCOMPARE(r.frames[0].seq, 1u);
}

// PayloadLen=4097 超上限:非法头重同步
void TestFrameDecoder::payloadLenOverLimitResync()
{
    QByteArray bad = buildFrame(0x01, 7, 100, "");
    bad[8] = 0x10; // Len 高位篡为 0x1000=4096... 需要精确 4097:
    bad[9] = 0x01; // 低字节 1 -> 4097

    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    FrameDecoder dec;
    const auto r = dec.decode(bad + hb);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.badFrames, 1u);
}

// 时间戳超 INT64_MAX:非法头重同步(SQLite 兼容约束,协议 §1)
void TestFrameDecoder::timestampOverLimitResync()
{
    const QByteArray bad = buildFrame(0x01, 7, 0xFFFFFFFFFFFFFFFFull, "");
    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    FrameDecoder dec;
    const auto r = dec.decode(bad + hb);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.badFrames, 1u);
}

// 未知类型 0x20:CRC 正确也要完整消费、计数,不交付载荷
void TestFrameDecoder::unknownTypeConsumedNotDelivered()
{
    const auto unknown = buildFrame(kUnknownType, 42, 500, QByteArray::fromHex("0102"));
    FrameDecoder dec;
    const auto r = dec.decode(unknown);
    QVERIFY(r.frames.isEmpty());
    QCOMPARE(r.unknownTypeFrames, 1u);
    QCOMPARE(r.badFrames, 0u);
    QCOMPARE(dec.bufferedBytes(), 0); // 已完整消费
}

// 合法载荷里包含 AA 55:只是数据,不得被当作新帧起点截断
void TestFrameDecoder::embeddedMagicInsidePayload()
{
    const QByteArray payload = QByteArray::fromHex("aa55ff");
    const auto frame = buildFrame(quint8(MessageType::Command), 9, 300, payload);
    FrameDecoder dec;
    const auto r = dec.decode(frame);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0].payload, payload);
    QCOMPARE(r.badFrames, 0u);
}

// 缓存超上限:记录溢出并清空;后续有效帧能正常恢复解码
void TestFrameDecoder::overflowDropsAndRecovers()
{
    FrameDecoder dec(1024); // 注入小上限,构造可复现溢出
    QByteArray junk(2048, 0x55); // 单块即超上限,必然触发溢出路径
    const auto r1 = dec.decode(junk);
    QVERIFY(r1.rxOverflow);
    QVERIFY(r1.frames.isEmpty());
    QCOMPARE(dec.bufferedBytes(), 0);

    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    const auto r2 = dec.decode(hb);
    QCOMPARE(r2.frames.size(), 1);
    QVERIFY(!r2.rxOverflow);
}

// 单轮 100 帧上限:150 帧只交付 100,剩余 50 留缓存,下一轮(空块)继续
void TestFrameDecoder::hundredFrameRoundLimit()
{
    const QByteArray hb = QByteArray::fromHex(
        "aa5501010000000100000000000000000000ee17d1cf");
    QByteArray stream;
    for (int i = 0; i < 150; ++i)
        stream += hb;

    FrameDecoder dec;
    const auto r1 = dec.decode(stream);
    QCOMPARE(r1.frames.size(), 100);
    QVERIFY(dec.bufferedBytes() > 0);

    const auto r2 = dec.decode(QByteArray());
    QCOMPARE(r2.frames.size(), 50);
    QCOMPARE(dec.bufferedBytes(), 0);
}

QTEST_MAIN(TestFrameDecoder)
#include "tst_framedecoder.moc"
