// tst_logging:LLD §8 日志器测试
// 覆盖:等级过滤、行格式(UTC/等级/分类/device/session/code)、10MiB×N 滚动、
// RAW 帧开关与 256 截断、同类错误 1s 聚合计数、进程级入口降级
#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "common/Logging.h"

using dms::Logger;

namespace {

// 小文件+注入假时钟,滚动/聚合不必等真实时间
Logger::Settings tinySettings()
{
    Logger::Settings s;
    s.level = dms::LogLevel::Info;
    s.rawFrames = false;
    s.maxFileBytes = 10 * 1024 * 1024;
    s.maxFiles = 5;
    s.aggregateWindowMs = 1000;
    return s;
}

QStringList logFiles(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral("dms*.log")}, QDir::Files, QDir::Name);
}

// 跨代拼接全部非空行(顺序:最老 -> 最新)
QStringList allLines(const QString &dir)
{
    QStringList out;
    for (const QString &n : logFiles(dir)) {
        QFile f(QDir(dir).filePath(n));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        while (!f.atEnd())
            out << QString::fromUtf8(f.readLine()).trimmed();
    }
    return out;
}

// 只读当前代 dms.log
QStringList currentLines(const QString &dir)
{
    QFile f(QDir(dir).filePath(QStringLiteral("dms.log")));
    QStringList out;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    while (!f.atEnd())
        out << QString::fromUtf8(f.readLine()).trimmed();
    return out;
}

} // namespace

class TestLogging : public QObject
{
    Q_OBJECT

private slots:
    void levelFilter();
    void lineFormat();
    void rotation();
    void rawFrameSwitch();
    void rawFrameTruncation();
    void errorAggregation();
    void globalEntryPoints();

private:
    // Qt Test 对一个实例依次调所有槽:每个用例用独立子目录,互不串文件
    QString freshDir();

    QTemporaryDir tempRoot_;
};

QString TestLogging::freshDir()
{
    static int counter = 0;
    const QString sub =
        QDir(tempRoot_.path()).filePath(QString::number(++counter));
    QDir().mkpath(sub);
    return sub;
}

// INFO 级别下 DEBUG 被过滤,WARN/ERROR 放行
void TestLogging::levelFilter()
{
    Logger::Settings s = tinySettings();
    s.level = dms::LogLevel::Info;
    const QString dir = freshDir();
    Logger log(s, dir);

    QVERIFY(!log.wouldLog(dms::LogLevel::Debug));
    QVERIFY(log.wouldLog(dms::LogLevel::Info));

    log.write(dms::LogLevel::Debug, dms::LogCategory::App, {}, QStringLiteral("dbg"));
    log.write(dms::LogLevel::Info, dms::LogCategory::App, {}, QStringLiteral("inf"));
    log.write(dms::LogLevel::Error, dms::LogCategory::App, {}, QStringLiteral("err"));

    const QStringList lines = currentLines(dir);
    QCOMPARE(lines.size(), 2);
    const QString joined = lines.join(QLatin1Char('\n'));
    QVERIFY(!joined.contains(QLatin1String("dbg")));
    QVERIFY(joined.contains(QLatin1String("inf")));
    QVERIFY(joined.contains(QLatin1String("err")));
}

// 行字段:UTC ISO 时间 LEVEL CATEGORY device session code message
void TestLogging::lineFormat()
{
    const QString dir = freshDir();
    Logger log(tinySettings(), dir);

    dms::LogContext ctx;
    ctx.deviceId = QStringLiteral("sim-tcp-01");
    ctx.sessionId = QUuid(QStringLiteral("12345678-1234-5678-1234-567812345678"));
    ctx.code = dms::ErrorCode::FrameCrc;
    log.write(dms::LogLevel::Warn, dms::LogCategory::Net, ctx, QStringLiteral("crc mismatch"));

    const QStringList lines = currentLines(dir);
    QCOMPARE(lines.size(), 1);
    // 公共 6 列 + message 词数;等级列的补位空格显式跳过
    const QStringList cols = lines[0].split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QCOMPARE(cols.size(), 8);
    const QDateTime ts =
        QDateTime::fromString(cols[0], QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
    QVERIFY(ts.isValid()); // 末尾字面量 Z 匹配,证明 UTC 格式
    QCOMPARE(cols[1], QStringLiteral("WARN")); // 等级列左对齐 5 宽,切分后无尾空格
    QCOMPARE(cols[2], QStringLiteral("NET"));
    QCOMPARE(cols[3], QStringLiteral("sim-tcp-01"));
    QCOMPARE(cols[4], QStringLiteral("12345678-1234-5678-1234-567812345678"));
    QCOMPARE(cols[5], QStringLiteral("FRAME_CRC"));
    QVERIFY(lines[0].endsWith(QLatin1String("crc mismatch")));
}

// 滚动:超限轮转,maxFiles 含当前文件;旧代下沉,最老内容被挤掉
void TestLogging::rotation()
{
    Logger::Settings s = tinySettings();
    s.maxFileBytes = 240; // 每文件 ~3 行(行约 80B)
    s.maxFiles = 3;
    const QString dir = freshDir();
    {
        Logger log(s, dir);
        for (int i = 0; i < 40; ++i)
            log.write(dms::LogLevel::Info, dms::LogCategory::App, {},
                      QStringLiteral("rotation line %1 padding-padding-padding").arg(i));
    }

    QCOMPARE(logFiles(dir).size(), 3); // dms.log + dms.1.log + dms.2.log
    // 当前文件承载最后一条
    QVERIFY(currentLines(dir).last().contains(QLatin1String("rotation line 39")));
    // 最早的内容已被轮转挤出
    const QString joined = allLines(dir).join(QLatin1Char('\n'));
    QVERIFY(!joined.contains(QLatin1String("rotation line 0")));
    QVERIFY(joined.contains(QLatin1String("rotation line 39")));
}

// RAW 帧默认关;开启后出现且不受 level=INFO 过滤(独立开关)
void TestLogging::rawFrameSwitch()
{
    {
        const QString dir = freshDir();
        Logger log(tinySettings(), dir); // rawFrames 默认 false
        log.writeRawFrame(false, QStringLiteral("sim-tcp-01"), QByteArray::fromHex("aa55"));
        QVERIFY(currentLines(dir).isEmpty());
    }
    {
        const QString dir = freshDir();
        Logger::Settings s = tinySettings();
        s.rawFrames = true;
        Logger log(s, dir);
        log.writeRawFrame(true, QStringLiteral("sim-tcp-01"), QByteArray::fromHex("00112233"));
        const QStringList lines = currentLines(dir);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines[0].contains(QLatin1String("RAW TX 4B 00112233")));
        QVERIFY(lines[0].contains(QLatin1String(" DEBUG NET "))); // 等级列标 DEBUG
    }
}

// 大帧:RAW 消息体截到 256 字符并标注 +TRUNC
void TestLogging::rawFrameTruncation()
{
    const QString dir = freshDir();
    Logger::Settings s = tinySettings();
    s.rawFrames = true;
    Logger log(s, dir);

    QByteArray big(512, '\xAB'); // 512B -> 1024 个十六进制字符
    log.writeRawFrame(false, QStringLiteral("sim-tcp-01"), big);

    const QStringList lines = currentLines(dir);
    QCOMPARE(lines.size(), 1);
    const QString message = lines[0].mid(lines[0].indexOf(QLatin1String("RAW")));
    QVERIFY(message.size() <= 256);
    QVERIFY(message.startsWith(QLatin1String("RAW RX 512B ")));
    QVERIFY(message.contains(QLatin1String("+TRUNC")));
}

// 同类高频错误 1s 聚合:窗口内折叠计数,窗口滑过补 "(aggregated N repeats)";
// 不同 code 不互相合并;flushAggregates 收尾
void TestLogging::errorAggregation()
{
    qint64 now = 1000; // 假时钟,测试自己拨
    Logger::Settings s = tinySettings();
    s.aggregateWindowMs = 1000;
    const QString dir = freshDir();
    {
        Logger log(s, dir, [&now] { return now; });

        dms::LogContext crc;
        crc.deviceId = QStringLiteral("sim-tcp-01");
        crc.code = dms::ErrorCode::FrameCrc;

        log.write(dms::LogLevel::Warn, dms::LogCategory::Protocol, crc, QStringLiteral("first"));
        now = 1100;
        log.write(dms::LogLevel::Warn, dms::LogCategory::Protocol, crc, QStringLiteral("first"));
        now = 1200;
        // 不同 code 另立窗口,不被 FrameCrc 折叠
        dms::LogContext len = crc;
        len.code = dms::ErrorCode::FrameLength;
        log.write(dms::LogLevel::Warn, dms::LogCategory::Protocol, len, QStringLiteral("other"));
        // 回到 CRC:仍在窗口内,继续折叠
        log.write(dms::LogLevel::Warn, dms::LogCategory::Protocol, crc, QStringLiteral("first"));

        now = 2500; // 窗口滑过:先补 agg 行,再落新首条
        log.write(dms::LogLevel::Warn, dms::LogCategory::Protocol, crc, QStringLiteral("first"));
        now = 2600; // 窗口内再折叠一条
        log.write(dms::LogLevel::Warn, dms::LogCategory::Protocol, crc, QStringLiteral("first"));
        log.flushAggregates(); // 未过窗的尾巴也要能收尾
    }

    const QStringList lines = currentLines(dir);
    const QStringList crcLines =
        lines.filter(QRegularExpression(QStringLiteral("FRAME_CRC.* first")));
    QCOMPARE(crcLines.size(), 4); // 首条 + agg(2) + 新首条 + agg(1)
    QVERIFY(crcLines[1].contains(QLatin1String("(aggregated 2 repeats)")));
    QVERIFY(crcLines[3].contains(QLatin1String("(aggregated 1 repeats)")));
    // other 未被聚合:原样一条
    QCOMPARE(lines.filter(QLatin1String("other")).size(), 1);
}

// 进程级入口:未初始化不崩溃(降级 stderr);初始化后落文件;shutdown 后回降级
void TestLogging::globalEntryPoints()
{
    // 未初始化:只降级不崩
    dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, {},
                    QStringLiteral("before init"));
    QVERIFY(!dms::rawFramesEnabled());

    const QString dir = freshDir();
    Logger::Settings s = tinySettings();
    s.rawFrames = true;
    dms::initializeLogging(s, dir);
    QVERIFY(dms::rawFramesEnabled());

    dms::LogContext ctx;
    ctx.deviceId = QStringLiteral("sim-tcp-01");
    ctx.code = dms::ErrorCode::DbWriteFailed;
    dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db, ctx, QStringLiteral("boom"));
    dms::shutdownLogging();

    const QStringList lines = currentLines(dir);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines[0].contains(QLatin1String("ERROR DB sim-tcp-01")));
    QVERIFY(lines[0].contains(QLatin1String("DB_WRITE_FAILED")));
    QVERIFY(lines[0].endsWith(QLatin1String("boom")));

    // shutdown 后再次使用:降级不崩
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, {},
                    QStringLiteral("after shutdown"));
}

QTEST_MAIN(TestLogging)
#include "tst_logging.moc"
