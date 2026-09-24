// tst_messagecodec:TLV 载荷编解码测试
// 真值来源:协议规范 §4 类型表 + §5 固定向量的载荷字节;
// 编码器字节必须与规范逐字节一致,不靠编码↔解码互验自证
#include <QtTest>

#include "Frame.h"
#include "MessageCodec.h"

using dms::protocol::AckStatus;
using dms::protocol::CommandMessage;
using dms::protocol::CommandParam;
using dms::protocol::DecodeError;
using dms::protocol::encodeFrame;
using dms::protocol::MetricId;
using dms::protocol::MessageType;
using dms::protocol::TelemetryResult;
using dms::protocol::TelemetrySample;

namespace {
TelemetrySample floatSample(dms::protocol::MetricId id, float v)
{
    TelemetrySample s;
    s.metricId = quint16(id);
    s.valueType = dms::protocol::kValueTypeFloat32;
    s.asFloat = v;
    return s;
}
TelemetrySample uintSample(dms::protocol::MetricId id, quint32 v)
{
    TelemetrySample s;
    s.metricId = quint16(id);
    s.valueType = dms::protocol::kValueTypeUint32;
    s.asUint = v;
    return s;
}
} // namespace

class TestMessageCodec : public QObject
{
    Q_OBJECT

private slots:
    // ---- 编码器对照规范固定字节 ----
    void heartbeatFrameMatchesSpecVector();
    void telemetryEncodeMatchesSpecPayload();
    void commandEncodeStartMatchesBytes();
    void ackEncodeMatchesBytes();
    void frameOversizedPayloadRejected();
    // ---- 遥测解码 ----
    void decodeSpecTelemetryPayload();
    void decodeStandardFourMetrics();
    void emptyTelemetryRejected();
    void truncatedTlvHeaderRejected();
    void truncatedTlvValueRejected();
    void knownMetricTypeMismatchRejected();
    void knownMetricLenMismatchRejected();
    void duplicateKnownMetricRejected();
    void duplicateUnknownMetricRejected();
    void nonFiniteFloatRejected();
    void unknownMetricSkippedAndCounted();
    void allUnknownStillStructurallyValid();
    // ---- 命令/ACK ----
    void commandStartDecode();
    void setParameterDecode();
    void commandTruncatedRejected();
    void commandTrailingBytesRejected();
    void ackRoundTrip();
    void ackStrictLength();
};

// 编码整帧必须等于 §5 向量1:空 HEARTBEAT,Seq=1,Ts=0,22B
void TestMessageCodec::heartbeatFrameMatchesSpecVector()
{
    QCOMPARE(encodeFrame(quint8(MessageType::Heartbeat), 1, 0, QByteArray()),
             QByteArray::fromHex("aa5501010000000100000000000000000000ee17d1cf"));
}

// 编码 temperature=25.0 的载荷必须等于 §5 向量2 的载荷字节
void TestMessageCodec::telemetryEncodeMatchesSpecPayload()
{
    const QByteArray p = dms::protocol::MessageCodec::encodeTelemetry(
        { floatSample(MetricId::Temperature, 25.0f) });
    QCOMPARE(p, QByteArray::fromHex("0001010441c80000"));
}

// Start 命令(0 参数):command_id=1, count=0
void TestMessageCodec::commandEncodeStartMatchesBytes()
{
    QCOMPARE(dms::protocol::MessageCodec::encodeCommand(1, {}),
             QByteArray::fromHex("00010000"));
}

// ACK:command_id=4, status=Busy(3)
void TestMessageCodec::ackEncodeMatchesBytes()
{
    QCOMPARE(dms::protocol::MessageCodec::encodeCommandAck(4, AckStatus::Busy),
             QByteArray::fromHex("00040003"));
}

// 发送侧防御:载荷超 4096 必须显式失败,不得发出长度声明错误的帧
void TestMessageCodec::frameOversizedPayloadRejected()
{
    constexpr int kMax = dms::protocol::kMaxPayloadSize;
    QVERIFY(encodeFrame(quint8(MessageType::Telemetry), 1, 0,
                        QByteArray(kMax + 1, 0)).isEmpty());
    QVERIFY(!encodeFrame(quint8(MessageType::Telemetry), 1, 0,
                         QByteArray(kMax, 0)).isEmpty());
}

// §5 向量2 载荷:仅 temperature=25.0
void TestMessageCodec::decodeSpecTelemetryPayload()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("0001010441c80000"), r),
             DecodeError::None);
    QVERIFY(r.structurallyValid);
    QCOMPARE(r.knownSamples.size(), 1);
    QCOMPARE(r.knownSamples[0].metricId, quint16(MetricId::Temperature));
    QCOMPARE(r.knownSamples[0].asFloat, 25.0f);
    QCOMPARE(r.unknownMetrics, 0u);
}

// 标准四指标负载:编码后解码,值逐项还原(32B 载荷,协议 §4.1)
void TestMessageCodec::decodeStandardFourMetrics()
{
    const QList<TelemetrySample> in = {
        floatSample(MetricId::Temperature, 25.0f),
        floatSample(MetricId::Pressure, 101.325f),
        floatSample(MetricId::Voltage, 3.7f),
        uintSample(MetricId::Rpm, 1200),
    };
    const QByteArray payload = dms::protocol::MessageCodec::encodeTelemetry(in);
    QCOMPARE(payload.size(), 32);

    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(payload, r), DecodeError::None);
    QCOMPARE(r.knownSamples.size(), 4);
    QCOMPARE(r.knownSamples[0].asFloat, 25.0f);
    QCOMPARE(r.knownSamples[1].asFloat, 101.325f);
    QCOMPARE(r.knownSamples[2].asFloat, 3.7f);
    QCOMPARE(r.knownSamples[3].asUint, quint32(1200));
    QCOMPARE(r.unknownMetrics, 0u);
}

void TestMessageCodec::emptyTelemetryRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(QByteArray(), r),
             DecodeError::Empty);
    QVERIFY(!r.structurallyValid);
}

void TestMessageCodec::truncatedTlvHeaderRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("000101"), r), // 3B:不足 TLV 头
             DecodeError::TruncatedTlv);
}

void TestMessageCodec::truncatedTlvValueRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("0001010441c8"), r), // len=4 只有 3 字节值
             DecodeError::TruncatedTlv);
}

// metric 1(temperature)必须是 float32;给 uint32 类型即整帧拒绝
void TestMessageCodec::knownMetricTypeMismatchRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("0001020400000064"), r),
             DecodeError::KnownTypeMismatch);
}

// 类型对但长度错(8,值字节给足):同样整帧拒绝
void TestMessageCodec::knownMetricLenMismatchRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("000101080000000000000000"), r),
             DecodeError::KnownTypeMismatch);
}

void TestMessageCodec::duplicateKnownMetricRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("0001010441c80000"
                                     "0001010441c80000"), r),
             DecodeError::DuplicateMetric);
}

// 重复 metric_id 的规则同样约束未知指标(协议 §4.1:同帧重复)
void TestMessageCodec::duplicateUnknownMetricRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("00630102aabb"
                                     "00630102aabb"), r),
             DecodeError::DuplicateMetric);
}

// NaN 位模式 0x7FC00000:载荷层拒绝,不得流入业务(协议 §4.1)
void TestMessageCodec::nonFiniteFloatRejected()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("000101047fc00000"), r),
             DecodeError::NonFiniteFloat);
}

// 未知指标结构合法:跳过计数;不影响同帧已知指标
void TestMessageCodec::unknownMetricSkippedAndCounted()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("00630102aabb"
                                     "00040204000004b0"), r), // rpm=1200
             DecodeError::None);
    QCOMPARE(r.knownSamples.size(), 1);
    QCOMPARE(r.knownSamples[0].asUint, quint32(1200));
    QCOMPARE(r.unknownMetrics, 1u);
    QVERIFY(r.structurallyValid);
}

// 全部是未知指标:结构有效(存活判定可用),但无已知遥测样本
void TestMessageCodec::allUnknownStillStructurallyValid()
{
    TelemetryResult r;
    QCOMPARE(dms::protocol::MessageCodec::decodeTelemetry(
                 QByteArray::fromHex("00630102aabb"), r),
             DecodeError::None);
    QVERIFY(r.knownSamples.isEmpty());
    QCOMPARE(r.unknownMetrics, 1u);
    QVERIFY(r.structurallyValid);
}

// Start:载荷 4B 头,0 参数
void TestMessageCodec::commandStartDecode()
{
    CommandMessage m;
    QCOMPARE(dms::protocol::MessageCodec::decodeCommand(
                 QByteArray::fromHex("00010000"), m),
             DecodeError::None);
    QCOMPARE(m.commandId, quint16(1));
    QVERIFY(m.params.isEmpty());
}

// SetParameter:cmd=4, 1 参数 param_id=1, uint32, 250
void TestMessageCodec::setParameterDecode()
{
    CommandMessage m;
    QCOMPARE(dms::protocol::MessageCodec::decodeCommand(
                 QByteArray::fromHex("0004000100010204000000fa"), m),
             DecodeError::None);
    QCOMPARE(m.commandId, quint16(4));
    QCOMPARE(m.params.size(), 1);
    QCOMPARE(m.params[0].id, quint16(1));
    QCOMPARE(m.params[0].valueType, dms::protocol::kValueTypeUint32);
    QCOMPARE(m.params[0].value, QByteArray::fromHex("000000fa"));
}

// 结构不完整:头不足 / count=1 但无 TLV -> 整帧丢弃,设备侧不回 ACK
void TestMessageCodec::commandTruncatedRejected()
{
    CommandMessage m;
    QCOMPARE(dms::protocol::MessageCodec::decodeCommand(
                 QByteArray::fromHex("0004"), m),
             DecodeError::Empty);
    QCOMPARE(dms::protocol::MessageCodec::decodeCommand(
                 QByteArray::fromHex("00040001"), m),
             DecodeError::TruncatedTlv);
}

// count 耗尽仍有剩余字节:结构错,拒绝
void TestMessageCodec::commandTrailingBytesRejected()
{
    CommandMessage m;
    QCOMPARE(dms::protocol::MessageCodec::decodeCommand(
                 QByteArray::fromHex("00010000ff"), m),
             DecodeError::TruncatedTlv);
}

void TestMessageCodec::ackRoundTrip()
{
    quint16 id = 0;
    AckStatus st = AckStatus::Success;
    QVERIFY(dms::protocol::MessageCodec::decodeCommandAck(
        dms::protocol::MessageCodec::encodeCommandAck(4, AckStatus::Busy), id, st));
    QCOMPARE(id, quint16(4));
    QCOMPARE(st, AckStatus::Busy);
}

// ACK 载荷必须恰好 4B(协议 §4.3),3B/5B 均拒绝
void TestMessageCodec::ackStrictLength()
{
    quint16 id = 0;
    AckStatus st = AckStatus::Success;
    QVERIFY(!dms::protocol::MessageCodec::decodeCommandAck(
        QByteArray::fromHex("000400"), id, st));
    QVERIFY(!dms::protocol::MessageCodec::decodeCommandAck(
        QByteArray::fromHex("0004000300"), id, st));
}

QTEST_MAIN(TestMessageCodec)
#include "tst_messagecodec.moc"
