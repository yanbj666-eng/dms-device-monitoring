#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <cstdint>

namespace dms::protocol {

// CRC-32/ISO-HDLC,参数按协议规范 §3:
//   poly=0x04C11DB7, init=0xFFFFFFFF, refin=true, refout=true, xorout=0xFFFFFFFF
// 实现按反射多项式 0xEDB88320 右移查表(每字节低位先入),最终异或输出。
// 检查值:ASCII "123456789" -> 0xCBF43926;空输入 -> 0x00000000。
// CRC 只做完整性校验,不承担认证或加密(协议 §1)。
class Crc32
{
public:
    // 计算 data[0..len) 的 CRC;len 为 0 时返回 0
    static quint32 compute(const uchar *data, qsizetype len) noexcept;
    static quint32 compute(const QByteArray &bytes) noexcept;

    Crc32() = delete; // 纯静态工具类,禁止实例化
};

} // namespace dms::protocol
