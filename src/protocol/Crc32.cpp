#include "Crc32.h"

#include <array>

namespace dms::protocol {

namespace {

// 反射多项式 0xEDB88320 = 0x04C11DB7 的位反转;
// 编译期生成查表,避免硬编码魔数表且不付出运行时开销
constexpr std::array<quint32, 256> makeTable()
{
    std::array<quint32, 256> table{};
    for (quint32 i = 0; i < 256; ++i) {
        quint32 c = i;
        for (int bit = 0; bit < 8; ++bit)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}

constexpr auto kTable = makeTable();

} // namespace

quint32 Crc32::compute(const uchar *data, qsizetype len) noexcept
{
    // init=0xFFFFFFFF 起,xorout=0xFFFFFFFF 终(ISO-HDLC 参数)
    quint32 crc = 0xFFFFFFFFu;
    for (qsizetype i = 0; i < len; ++i)
        crc = kTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

quint32 Crc32::compute(const QByteArray &bytes) noexcept
{
    // QByteArray::constData() 在空对象上可能返回 nullptr,len=0 时循环体不执行,安全
    return compute(reinterpret_cast<const uchar *>(bytes.constData()), bytes.size());
}

} // namespace dms::protocol
