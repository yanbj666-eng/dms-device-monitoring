// tst_crc32:CRC-32/ISO-HDLC 固定向量测试
// 向量来源:协议规范 §3 规定的检查值 + 通用公开检查值
// 约束:不得只做编码器/解码器互验,必须用独立固定向量(协议 §3)
#include <QtTest>

#include "Crc32.h"

using dms::protocol::Crc32;

class TestCrc32 : public QObject
{
    Q_OBJECT

private slots:
    // 协议规范强制检查值:ASCII "123456789" -> 0xCBF43926
    void knownVectorDigits();
    // 协议规范强制检查值:空输入 -> 0
    void emptyInput();
    // 单字节 'a' -> 0xE8B7BE43(公开检查值)
    void singleByte();
    // 4 字节 0x00 -> 0x2144DF1C(公开检查值)
    void zeroBytes();
};

void TestCrc32::knownVectorDigits()
{
    QCOMPARE(Crc32::compute(QByteArray("123456789")), quint32(0xCBF43926));
}

void TestCrc32::emptyInput()
{
    QCOMPARE(Crc32::compute(QByteArray()), quint32(0x00000000));
}

void TestCrc32::singleByte()
{
    QCOMPARE(Crc32::compute(QByteArray("a")), quint32(0xE8B7BE43));
}

void TestCrc32::zeroBytes()
{
    QCOMPARE(Crc32::compute(QByteArray::fromHex("00000000")), quint32(0x2144DF1C));
}

QTEST_MAIN(TestCrc32)
#include "tst_crc32.moc"
