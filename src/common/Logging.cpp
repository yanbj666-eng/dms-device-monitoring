#include "Logging.h"

#include <QDateTime>
#include <QDir>
#include <QMutexLocker>

#include <chrono>
#include <cstdio>

namespace dms::common {

namespace {

QMutex g_instanceMutex;
Logger *g_instance = nullptr; // initializeLogging 之前为空,日志降级 stderr

// 行格式(LLD §8):UTC 时间 level category device_id session_id code message
// 缺省字段写 "-",message 永远最后,按空白切分即可解析
QString formatLine(LogLevel level, LogCategory category, const LogContext &ctx,
                   const QString &message)
{
    const QString ts =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
    const QString device = ctx.deviceId.isEmpty() ? QStringLiteral("-") : ctx.deviceId;
    const QString session =
        ctx.sessionId.isNull() ? QStringLiteral("-") : ctx.sessionId.toString(QUuid::WithoutBraces);
    const QString code =
        ctx.code == ErrorCode::None ? QStringLiteral("-") : errorCodeName(ctx.code);
    return QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
        .arg(ts, logLevelName(level).leftJustified(5), logCategoryName(category), device,
             session, code, message);
}

QString aggregateKey(LogLevel level, LogCategory category, const LogContext &ctx)
{
    // 多参 arg() 只收字符串;int 先显式转换
    return QStringLiteral("%1|%2|%3|%4")
        .arg(QString::number(int(level)), QString::number(int(category)), ctx.deviceId,
             errorCodeName(ctx.code));
}

// 原始帧行:消息体(不含公共字段列)截到 256 字符
QString rawFrameMessage(bool tx, const QByteArray &frame)
{
    const QString head =
        QStringLiteral("RAW %1 %2B ").arg(tx ? QStringLiteral("TX") : QStringLiteral("RX")).arg(frame.size());
    QString hex = frame.toHex();
    if (head.size() + hex.size() > 256) {
        const int keep = 256 - head.size() - QStringLiteral(" +TRUNC").size();
        hex = hex.left(keep) + QStringLiteral(" +TRUNC");
    }
    return head + hex;
}

} // namespace

QString logLevelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("DEBUG");
    case LogLevel::Info:  return QStringLiteral("INFO");
    case LogLevel::Warn:  return QStringLiteral("WARN");
    case LogLevel::Error: return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}

QString logCategoryName(LogCategory category)
{
    switch (category) {
    case LogCategory::App:      return QStringLiteral("APP");
    case LogCategory::Net:      return QStringLiteral("NET");
    case LogCategory::Serial:   return QStringLiteral("SERIAL");
    case LogCategory::Protocol: return QStringLiteral("PROTOCOL");
    case LogCategory::Db:       return QStringLiteral("DB");
    case LogCategory::Ui:       return QStringLiteral("UI");
    case LogCategory::Test:     return QStringLiteral("TEST");
    }
    return QStringLiteral("APP");
}

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::ConfigInvalid:       return QStringLiteral("CONFIG_INVALID");
    case ErrorCode::ConnectTimeout:      return QStringLiteral("CONNECT_TIMEOUT");
    case ErrorCode::ConnectionLost:      return QStringLiteral("CONNECTION_LOST");
    case ErrorCode::FrameLength:         return QStringLiteral("FRAME_LENGTH");
    case ErrorCode::FrameCrc:            return QStringLiteral("FRAME_CRC");
    case ErrorCode::FramePayload:        return QStringLiteral("FRAME_PAYLOAD");
    case ErrorCode::FrameTimeout:        return QStringLiteral("FRAME_TIMEOUT");
    case ErrorCode::RxOverflow:          return QStringLiteral("RX_OVERFLOW");
    case ErrorCode::SendQueueFull:       return QStringLiteral("SEND_QUEUE_FULL");
    case ErrorCode::CommandTimeout:      return QStringLiteral("COMMAND_TIMEOUT");
    case ErrorCode::DbWriteFailed:       return QStringLiteral("DB_WRITE_FAILED");
    case ErrorCode::DbQueueFull:         return QStringLiteral("DB_QUEUE_FULL");
    case ErrorCode::DbSchemaUnsupported: return QStringLiteral("DB_SCHEMA_UNSUPPORTED");
    case ErrorCode::None: break;
    }
    return QStringLiteral("-");
}

qint64 Logger::defaultClock()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Logger::Logger(Settings settings, const QString &directory, Clock clock)
    : settings_(settings), directory_(directory), clock_(clock)
{
    if (!openFile()) {
        // 目录不可写:记录到 stderr,Logger 继续存活但条目会被丢弃;
        // 调用方(startup)应把该失败作为启动问题显示
        fprintf(stderr, "DMS logging: cannot open log file under %s\n",
                qPrintable(directory_));
    }
}

Logger::~Logger()
{
    flushAggregates();
    QMutexLocker lock(&mutex_);
    if (file_.isOpen())
        file_.close();
}

void Logger::write(LogLevel level, LogCategory category, const LogContext &ctx,
                   const QString &message)
{
    if (!wouldLog(level))
        return;
    QMutexLocker lock(&mutex_);
    const QString line = formatLine(level, category, ctx, message);

    // 同类高频错误聚合计数(LLD §8):窗口内只落首条,重复条折叠计数,
    // 窗口滑过后先补一行 "(aggregated N repeats)" 再落新首条
    if (level >= LogLevel::Warn && ctx.code != ErrorCode::None) {
        const QString key = aggregateKey(level, category, ctx);
        const qint64 now = clock_();
        const auto it = aggregates_.find(key);
        if (it != aggregates_.end()) {
            if (now - it->windowStartMs < settings_.aggregateWindowMs) {
                ++it->suppressed;
                return;
            }
            if (it->suppressed > 0)
                writeLine(QStringLiteral("%1 (aggregated %2 repeats)")
                              .arg(it->firstLine, QString::number(it->suppressed)));
            it->windowStartMs = now;
            it->suppressed = 0;
            it->firstLine = line;
        } else {
            Aggregate a;
            a.windowStartMs = now;
            a.firstLine = line;
            aggregates_.insert(key, a);
        }
    }
    writeLine(line);
}

void Logger::writeRawFrame(bool tx, const QString &deviceId, const QByteArray &frame)
{
    if (!settings_.rawFrames)
        return; // 默认关;开关独立于 level,开了就出 DEBUG 行
    QMutexLocker lock(&mutex_);
    LogContext ctx;
    ctx.deviceId = deviceId;
    writeLine(formatLine(LogLevel::Debug, LogCategory::Net, ctx, rawFrameMessage(tx, frame)));
}

void Logger::flushAggregates()
{
    QMutexLocker lock(&mutex_);
    for (auto it = aggregates_.begin(); it != aggregates_.end(); ++it) {
        if (it->suppressed > 0) {
            writeLine(QStringLiteral("%1 (aggregated %2 repeats)")
                          .arg(it->firstLine, QString::number(it->suppressed)));
            it->suppressed = 0;
        }
    }
}

bool Logger::openFile()
{
    QDir().mkpath(directory_);
    file_.setFileName(directory_ + QStringLiteral("/dms.log"));
    // Append+Text:Windows 上换行转 CRLF,记事本可直接看
    return file_.open(QIODevice::Append | QIODevice::Text);
}

void Logger::writeLine(const QString &line)
{
    // 调用方已持锁
    if (!file_.isOpen() && !openFile())
        return;
    file_.write((line + QStringLiteral("\n")).toUtf8());
    // 每行落盘:监控工具崩溃时日志尽量完整;本系统量级(≤百行/s)可接受
    file_.flush();
    if (file_.size() > settings_.maxFileBytes)
        rotate();
}

void Logger::rotate()
{
    // 调用方已持锁。dms.log -> dms.1.log -> ... -> dms.(maxFiles-1).log
    file_.close();
    const int generations = settings_.maxFiles - 1; // 当前文件之外的历史代数
    const QString current = directory_ + QStringLiteral("/dms.log");
    if (generations <= 0) {
        QFile::remove(current); // 只保留 1 个文件:删旧开新
    } else {
        QFile::remove(QStringLiteral("%1/dms.%2.log").arg(directory_).arg(generations));
        for (int g = generations; g > 1; --g) {
            QFile::rename(QStringLiteral("%1/dms.%2.log").arg(directory_).arg(g - 1),
                          QStringLiteral("%1/dms.%2.log").arg(directory_).arg(g));
        }
        QFile::rename(current, QStringLiteral("%1/dms.1.log").arg(directory_));
    }
    openFile();
}

// ---- 进程级便捷入口 ----

void initializeLogging(const Logger::Settings &settings, const QString &directory)
{
    QMutexLocker lock(&g_instanceMutex);
    delete g_instance; // 旧实例析构时 flush 聚合尾巴
    g_instance = new Logger(settings, directory);
}

void shutdownLogging()
{
    QMutexLocker lock(&g_instanceMutex);
    delete g_instance;
    g_instance = nullptr;
}

void logMessage(LogLevel level, LogCategory category, const LogContext &ctx,
                const QString &message)
{
    QMutexLocker lock(&g_instanceMutex);
    if (g_instance) {
        g_instance->write(level, category, ctx, message);
        return;
    }
    // 未初始化(启动极早期/未调用 initialize):降级 stderr,不丢日志。
    // 直接 fprintf:不依赖 Qt 默认消息处理器的路由行为
    fprintf(stderr, "%s\n", qPrintable(formatLine(level, category, ctx, message)));
}

void logRawFrame(bool tx, const QString &deviceId, const QByteArray &frame)
{
    QMutexLocker lock(&g_instanceMutex);
    if (g_instance && g_instance->rawFramesEnabled())
        g_instance->writeRawFrame(tx, deviceId, frame);
    // 未初始化或开关关闭:直接丢弃,RAW 帧不做 stderr 降级(量太大)
}

bool rawFramesEnabled()
{
    QMutexLocker lock(&g_instanceMutex);
    return g_instance && g_instance->rawFramesEnabled();
}

LogLevel minLogLevel()
{
    QMutexLocker lock(&g_instanceMutex);
    return g_instance ? g_instance->settings().level : LogLevel::Debug; // 降级期全放行
}

} // namespace dms::common
