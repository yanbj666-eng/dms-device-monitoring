#include "MessageCodec.h"

#include <QSet>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace dms::protocol {

namespace {
// TLV 固定头:metric_id(2) + value_type(1) + len(1)(协议 §4.1)
constexpr int kTlvHeaderSize = 4;
// command 载荷固定头:command_id(2) + parameter_count(2)(协议 §4.2)
constexpr int kCommandHeaderSize = 4;

void appendTlvHeader(QByteArray &out, quint16 id, quint8 type, quint8 len)
{
    char hdr[kTlvHeaderSize];
    qToBigEndian<quint16>(id, hdr);
    hdr[2] = char(type);
    hdr[3] = char(len);
    out.append(hdr, kTlvHeaderSize);
}
} // namespace

quint8 expectedValueType(quint16 metricId) noexcept
{
    switch (metricId) {
    case quint16(MetricId::Temperature):
    case quint16(MetricId::Pressure):
    case quint16(MetricId::Voltage):
        return kValueTypeFloat32;
    case quint16(MetricId::Rpm):
        return kValueTypeUint32;
    default:
        return 0;
    }
}

DecodeError MessageCodec::decodeTelemetry(const QByteArray &payload, TelemetryResult &out)
{
    out = TelemetryResult{};
    if (payload.isEmpty())
        return DecodeError::Empty; // 空 TELEMETRY 非法(协议 §4)

    const uchar *d = reinterpret_cast<const uchar *>(payload.constData());
    const int size = payload.size();
    QSet<quint16> seen; // 重复 metric_id 含未知指标,整帧拒绝

    int pos = 0;
    while (pos < size) {
        // 先校验 TLV 头和剩余长度,再读取值(协议 §4.1)
        if (size - pos < kTlvHeaderSize)
            return DecodeError::TruncatedTlv; // 尾部不足一个完整 TLV

        const quint16 metricId = qFromBigEndian<quint16>(d + pos);
        const quint8 valueType = d[pos + 2];
        const quint8 len = d[pos + 3];
        pos += kTlvHeaderSize;
        if (size - pos < len)
            return DecodeError::TruncatedTlv;

        const quint8 expect = expectedValueType(metricId);
        if (expect != 0 && (valueType != expect || len != 4))
            return DecodeError::KnownTypeMismatch;

        if (seen.contains(metricId))
            return DecodeError::DuplicateMetric;
        seen.insert(metricId);

        if (expect == 0) {
            ++out.unknownMetrics; // 结构合法的未知指标:按 len 跳过并计数
        } else if (expect == kValueTypeFloat32) {
            TelemetrySample s;
            s.metricId = metricId;
            s.valueType = valueType;
            const quint32 bits = qFromBigEndian<quint32>(d + pos); // 位模式大端(协议 §1)
            memcpy(&s.asFloat, &bits, sizeof(float)); // 不做未对齐强转
            if (!std::isfinite(s.asFloat))
                return DecodeError::NonFiniteFloat; // 真实超阈值有限值不得在此过滤
            out.knownSamples.append(s);
        } else {
            TelemetrySample s;
            s.metricId = metricId;
            s.valueType = valueType;
            s.asUint = qFromBigEndian<quint32>(d + pos);
            out.knownSamples.append(s);
        }
        pos += len;
    }

    out.structurallyValid = true; // 恰好消费至末尾
    return DecodeError::None;
}

QByteArray MessageCodec::encodeTelemetry(const QList<TelemetrySample> &samples)
{
    QByteArray p;
    p.reserve(samples.size() * (kTlvHeaderSize + 4));
    for (const auto &s : samples) {
        const quint8 expect = expectedValueType(s.metricId);
        if (expect == 0)
            continue; // 编码端只发已知指标;未知指标无类型定义,跳过防脏数据

        if (expect == kValueTypeFloat32) {
            quint32 bits = 0;
            memcpy(&bits, &s.asFloat, sizeof(float)); // NaN/Inf 属发送端 bug,如实编码
            char value[4];
            qToBigEndian<quint32>(bits, value);
            appendTlvHeader(p, s.metricId, kValueTypeFloat32, 4);
            p.append(value, 4);
        } else {
            char value[4];
            qToBigEndian<quint32>(s.asUint, value);
            appendTlvHeader(p, s.metricId, kValueTypeUint32, 4);
            p.append(value, 4);
        }
    }
    return p;
}

DecodeError MessageCodec::decodeCommand(const QByteArray &payload, CommandMessage &out)
{
    out = CommandMessage{};
    if (payload.size() < kCommandHeaderSize)
        return DecodeError::Empty;

    const uchar *d = reinterpret_cast<const uchar *>(payload.constData());
    const int size = payload.size();
    out.commandId = qFromBigEndian<quint16>(d);
    const quint16 count = qFromBigEndian<quint16>(d + 2);

    int pos = kCommandHeaderSize;
    for (quint16 i = 0; i < count; ++i) {
        if (size - pos < kTlvHeaderSize)
            return DecodeError::TruncatedTlv; // 结构不完整:整帧丢弃,设备不回 ACK
        const quint16 id = qFromBigEndian<quint16>(d + pos);
        const quint8 type = d[pos + 2];
        const quint8 len = d[pos + 3];
        pos += kTlvHeaderSize;
        if (size - pos < len)
            return DecodeError::TruncatedTlv;

        CommandParam param;
        param.id = id;
        param.valueType = type;
        param.value = QByteArray(reinterpret_cast<const char *>(d + pos), len);
        out.params.append(param);
        pos += len;
    }
    if (pos != size)
        return DecodeError::TruncatedTlv; // count 耗尽仍有剩余字节:结构错
    return DecodeError::None;
}

QByteArray MessageCodec::encodeCommand(quint16 commandId, const QList<CommandParam> &params)
{
    QByteArray p;
    p.reserve(kCommandHeaderSize + params.size() * (kTlvHeaderSize + 4));
    char hdr[kCommandHeaderSize];
    qToBigEndian<quint16>(commandId, hdr);
    qToBigEndian<quint16>(quint16(params.size()), hdr + 2);
    p.append(hdr, kCommandHeaderSize);

    for (const auto &param : params) {
        appendTlvHeader(p, param.id, param.valueType, quint8(param.value.size()));
        p.append(param.value);
    }
    return p;
}

bool MessageCodec::decodeCommandAck(const QByteArray &payload, quint16 &commandId,
                                    AckStatus &status)
{
    if (payload.size() != 4) // 固定 4B,多一字节都算错(协议 §4.3)
        return false;
    const auto *d = reinterpret_cast<const uchar *>(payload.constData());
    commandId = qFromBigEndian<quint16>(d);
    status = static_cast<AckStatus>(qFromBigEndian<quint16>(d + 2));
    return true;
}

QByteArray MessageCodec::encodeCommandAck(quint16 commandId, AckStatus status)
{
    QByteArray p(4, Qt::Uninitialized);
    auto *d = reinterpret_cast<uchar *>(p.data());
    qToBigEndian<quint16>(commandId, d);
    qToBigEndian<quint16>(static_cast<quint16>(status), d + 2);
    return p;
}

} // namespace dms::protocol
