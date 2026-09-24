#pragma once

#include <QByteArray>
#include <QList>
#include <QtGlobal>

namespace dms::protocol {

// ---- 遥测指标(协议 §4.1)----
enum class MetricId : quint16 {
    Temperature = 1, // float32, °C
    Pressure    = 2, // float32, kPa
    Voltage     = 3, // float32, V
    Rpm         = 4, // uint32, r/min
};

// v1 定义的遥测值类型(协议 §4.1 类型表)
inline constexpr quint8 kValueTypeFloat32 = 0x01; // IEEE-754,有限值,len=4
inline constexpr quint8 kValueTypeUint32  = 0x02; // len=4

// 已知指标要求的值类型;未知指标返回 0(按 len 跳过,不校验类型)
quint8 expectedValueType(quint16 metricId) noexcept;

// ---- 载荷解码错误(整帧拒绝的原因,供日志分类)----
enum class DecodeError {
    None,
    Empty,             // 空载荷:TELEMETRY 非法;COMMAND 不足 4B 头
    TruncatedTlv,      // TLV 头或值未到齐,或 count 与实际长度不符
    KnownTypeMismatch, // 已知指标的类型/长度与类型表不符
    DuplicateMetric,   // 同帧重复 metric_id(含未知指标)
    NonFiniteFloat,    // NaN/Infinity,载荷层拒绝(协议 §4.1)
};

// ---- TELEMETRY ----
struct TelemetrySample {
    quint16 metricId = 0;
    quint8 valueType = 0;
    float asFloat = 0.0f; // valueType==kValueTypeFloat32 时有效
    quint32 asUint = 0;   // valueType==kValueTypeUint32 时有效
};

struct TelemetryResult {
    QList<TelemetrySample> knownSamples; // 仅已知指标,按 TLV 顺序
    quint32 unknownMetrics = 0;          // 结构合法的未知指标,已跳过计数
    // 结构合法即 true(全是未知指标也算):存活判定用;
    // 遥测时间是否刷新由会话层按 knownSamples 是否为空决定(协议 §4.1)
    bool structurallyValid = false;
};

// ---- COMMAND(客户端 -> 设备,协议 §4.2)----
struct CommandParam {
    quint16 id = 0;     // 独立于遥测的命令参数空间
    quint8 valueType = 0;
    QByteArray value;   // 原始字节;语义校验(SetParameter 范围等)归设备侧
};

struct CommandMessage {
    quint16 commandId = 0;
    QList<CommandParam> params;
};

// ---- COMMAND_ACK(设备 -> 客户端,固定 4B,协议 §4.3)----
enum class AckStatus : quint16 {
    Success     = 0,
    Unsupported = 1,
    BadParams   = 2,
    Busy        = 3,
    Failed      = 4,
};

// 载荷级编解码:只管结构,不含帧头/CRC(Frame/Decoder 负责)。
// 语义(未知命令、参数越界、重复参数是否执行)由设备侧按 §4.2 拒绝并回 ACK
class MessageCodec
{
public:
    MessageCodec() = delete;

    // 遥测载荷:TLV 序列必须恰好消费至末尾;空载荷非法
    static DecodeError decodeTelemetry(const QByteArray &payload, TelemetryResult &out);
    // 编码已知指标;超阈值有限值原样编码,不得在此过滤(协议 §4.1)
    static QByteArray encodeTelemetry(const QList<TelemetrySample> &samples);

    // 命令载荷:command_id + count + count 个 TLV,恰好消费
    static DecodeError decodeCommand(const QByteArray &payload, CommandMessage &out);
    static QByteArray encodeCommand(quint16 commandId, const QList<CommandParam> &params);

    // ACK:载荷必须恰好 4B,否则 false;Seq 回显不在载荷,由帧头负责
    static bool decodeCommandAck(const QByteArray &payload, quint16 &commandId,
                                 AckStatus &status);
    static QByteArray encodeCommandAck(quint16 commandId, AckStatus status);
};

} // namespace dms::protocol
