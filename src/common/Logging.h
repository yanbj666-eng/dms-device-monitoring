#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QUuid>

#include <functional>

namespace dms::common {

// ---- 日志等级(LLD §8)----
enum class LogLevel { Debug, Info, Warn, Error };

// ---- 日志分类(LLD §8)----
enum class LogCategory { App, Net, Serial, Protocol, Db, Ui, Test };

// ---- 错误码(LLD §8 基础十三种 + S5 读路径补充;transport/protocol/db 共用,故放 common)----
enum class ErrorCode {
    None = 0,
    ConfigInvalid,       // CONFIG_INVALID
    ConnectTimeout,      // CONNECT_TIMEOUT
    ConnectionLost,      // CONNECTION_LOST
    FrameLength,         // FRAME_LENGTH
    FrameCrc,            // FRAME_CRC
    FramePayload,        // FRAME_PAYLOAD
    FrameTimeout,        // FRAME_TIMEOUT
    RxOverflow,          // RX_OVERFLOW
    SendQueueFull,       // SEND_QUEUE_FULL
    CommandTimeout,      // COMMAND_TIMEOUT
    DbWriteFailed,       // DB_WRITE_FAILED
    DbQueueFull,         // DB_QUEUE_FULL
    DbSchemaUnsupported, // DB_SCHEMA_UNSUPPORTED
    DbReadFailed,        // DB_READ_FAILED(S5:查询/导出/只读打开失败)
};

// 日志/测试断言用的可读名
QString logLevelName(LogLevel level);
QString logCategoryName(LogCategory category);
QString errorCodeName(ErrorCode code);

// 单条日志附加上下文:device/session 可空;code!=None 才有错误码列
struct LogContext {
    QString deviceId;
    QUuid sessionId;
    ErrorCode code = ErrorCode::None;
};

// 文件日志器:线程安全(QMutex),非 QObject,任意线程可直接调用。
// 测试可注入小时钟/小文件,验证滚动与聚合计数(LLD §8)
class Logger
{
public:
    // 单调毫秒时钟;注入假时钟后聚合一秒窗的测试不必真等一秒
    using Clock = std::function<qint64()>;

    struct Settings {
        LogLevel level = LogLevel::Info;        // app.ini [logging] level
        bool rawFrames = false;                 // raw_frames,默认关
        qint64 maxFileBytes = 10 * 1024 * 1024; // max_file_bytes(10MiB)
        int maxFiles = 5;                       // max_files,含当前文件
        qint64 aggregateWindowMs = 1000;        // 同类高频错误聚合计数窗口
    };

    Logger(Settings settings, const QString &directory, Clock clock = defaultClock);
    ~Logger(); // flush 聚合尾巴并关文件

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    void write(LogLevel level, LogCategory category, const LogContext &ctx,
               const QString &message);
    // RX/TX 原始帧:受 rawFrames 独立开关控制(不看 level);
    // 每条消息体截到 256 字符并标截断(LLD §8)
    void writeRawFrame(bool tx, const QString &deviceId, const QByteArray &frame);
    void flushAggregates(); // 退出前把还在窗口内的聚合计数写出去

    bool wouldLog(LogLevel level) const { return level >= settings_.level; }
    bool rawFramesEnabled() const { return settings_.rawFrames; }
    const Settings &settings() const { return settings_; }

    static qint64 defaultClock(); // steady_clock 毫秒

private:
    struct Aggregate {
        qint64 windowStartMs = 0;
        quint32 suppressed = 0; // 窗口内被折叠的重复条数
        QString firstLine;      // 窗口首条完整行,flush 时引用
    };

    void writeLine(const QString &line); // 调用方已持锁;含滚动
    bool openFile();
    void rotate();

    Settings settings_;
    QString directory_;
    Clock clock_;
    QFile file_;
    QMutex mutex_; // 跨 GUI/IO 线程共用的进程级资源
    // key: level|category|device|code,同类高频错误聚合
    QHash<QString, Aggregate> aggregates_;
};

// ---- 进程级便捷入口 ----
// initializeLogging 之前调用日志函数会降级写 stderr,启动早期日志不丢;
// initializeLogging 可重复调用(旧实例先 flush 再销毁)
void initializeLogging(const Logger::Settings &settings, const QString &directory);
void shutdownLogging(); // flush 聚合并销毁;之后日志回到 stderr 降级
void logMessage(LogLevel level, LogCategory category, const LogContext &ctx,
                const QString &message);
void logRawFrame(bool tx, const QString &deviceId, const QByteArray &frame);
bool rawFramesEnabled(); // 热路径先查开关再拼十六进制串
LogLevel minLogLevel();

} // namespace dms::common

// 全库别名:调用点写 dms::logMessage(...) 免去 common:: 前缀
namespace dms {
using common::ErrorCode;
using common::LogCategory;
using common::LogContext;
using common::LogLevel;
using common::Logger;
using common::initializeLogging;
using common::logCategoryName;
using common::logMessage;
using common::logRawFrame;
using common::logLevelName;
using common::errorCodeName;
using common::rawFramesEnabled;
using common::shutdownLogging;
} // namespace dms
