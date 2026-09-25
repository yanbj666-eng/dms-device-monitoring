// dms_simulator:TCP 设备模拟器 CLI(S2,docs/15 §5)
// 全部设备共享本进程唯一事件循环;固定种子决定数据序列
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QTimer>

#include <cstdio>

#include "DeviceSimulator.h"
#include "FaultSpec.h"
#include "common/Logging.h"

namespace {

bool parseLevel(const QString &text, dms::LogLevel &out)
{
    const QString t = text.toLower();
    if (t == QStringLiteral("debug")) out = dms::LogLevel::Debug;
    else if (t == QStringLiteral("info")) out = dms::LogLevel::Info;
    else if (t == QStringLiteral("warn")) out = dms::LogLevel::Warn;
    else if (t == QStringLiteral("error")) out = dms::LogLevel::Error;
    else return false;
    return true;
}

void usageError(const QString &message)
{
    // 参数错误同时走 stderr(给集成测试)与日志(给人)
    fprintf(stderr, "dms_simulator: %s\n", qPrintable(message));
    dms::LogContext ctx;
    ctx.code = dms::ErrorCode::ConfigInvalid;
    dms::logMessage(dms::LogLevel::Error, dms::LogCategory::App, ctx, message);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dms-simulator"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "DMS TCP 设备模拟器(协议 v1):每设备一个监听端口,四指标遥测+心跳+命令/ACK,"
        "支持可复现故障注入"));
    parser.addHelpOption();
    parser.addVersionOption();
    // 一次性注册全部选项并检查返回值:短名撞车(如 -v 已被 --version 占用)
    // 会让 addOptions 静默失败,整组选项消失——必须在启动期暴露
    const bool optionsRegistered = parser.addOptions({
        {{"c", "count"},
         QStringLiteral("模拟设备数(各占一个端口)"),
         QStringLiteral("n"),
         QStringLiteral("10")},
        {{"p", "port"},
         QStringLiteral("起始 TCP 监听端口"),
         QStringLiteral("port"),
         QStringLiteral("9001")},
        {{"s", "period"},
         QStringLiteral("遥测采样周期 ms(100-1000)"),
         QStringLiteral("ms"),
         QStringLiteral("100")},
        {{"d", "seed"},
         QStringLiteral("随机种子,决定数据与注入序列"),
         QStringLiteral("n"),
         QStringLiteral("42")},
        {{"f", "fault"},
         QStringLiteral("故障注入 mode@startMs+durationMs(可重复)。mode: badCrc|"
                        "abnormalValue|stopSending|disconnect|ackDelay|ackNoReply|"
                        "fragment|merge"),
         QStringLiteral("spec")},
        {{"r", "run-seconds"},
         QStringLiteral("运行秒数,0 表示直到手动停止"),
         QStringLiteral("s"),
         QStringLiteral("0")},
        {{"l", "log-dir"},
         QStringLiteral("日志目录;缺省只输出到 stderr"),
         QStringLiteral("dir")},
        {QStringLiteral("log-level"),
         QStringLiteral("DEBUG/INFO/WARN/ERROR"),
         QStringLiteral("level"),
         QStringLiteral("INFO")},
    });
    if (!optionsRegistered) {
        fprintf(stderr, "dms_simulator: internal error registering options\n");
        return 1;
    }
    parser.process(app);

    // ---- 日志先行:后续所有启动输出都进日志 ----
    dms::LogLevel level = dms::LogLevel::Info;
    if (!parseLevel(parser.value(QStringLiteral("log-level")), level)) {
        usageError(QStringLiteral("invalid --log-level"));
        return 1;
    }
    const QString logDir = parser.value(QStringLiteral("log-dir"));
    if (!logDir.isEmpty()) {
        dms::Logger::Settings settings;
        settings.level = level;
        dms::initializeLogging(settings, logDir);
    }
    // 无 --log-dir:不初始化,全局日志自动降级 stderr

    // ---- 参数校验(非法值明确报错退出,不悄悄改默认值)----
    bool ok = false;
    const int count = parser.value(QStringLiteral("count")).toInt(&ok);
    if (!ok || count < 1 || count > 100) {
        usageError(QStringLiteral("--count must be 1-100"));
        return 1;
    }
    const int basePort = parser.value(QStringLiteral("port")).toInt(&ok);
    if (!ok || basePort < 1 || basePort > 65535 || basePort + count - 1 > 65535) {
        usageError(QStringLiteral("--port out of range for the requested device count"));
        return 1;
    }
    const int period = parser.value(QStringLiteral("period")).toInt(&ok);
    if (!ok || period < 100 || period > 1000) {
        usageError(QStringLiteral("--period must be 100-1000"));
        return 1;
    }
    const quint32 seed = parser.value(QStringLiteral("seed")).toUInt(&ok);
    if (!ok) {
        usageError(QStringLiteral("--seed must be a 32-bit unsigned integer"));
        return 1;
    }
    const int runSeconds = parser.value(QStringLiteral("run-seconds")).toInt(&ok);
    if (!ok || runSeconds < 0 || runSeconds > 86400) {
        usageError(QStringLiteral("--run-seconds must be 0-86400"));
        return 1;
    }

    QList<dms::sim::FaultSpec> faults;
    for (const QString &spec : parser.values(QStringLiteral("fault"))) {
        dms::sim::FaultSpec f;
        if (!dms::sim::parseFaultSpec(spec, f)) {
            usageError(QStringLiteral("invalid --fault spec '%1' (expect mode@startMs+durationMs)")
                           .arg(spec));
            return 1;
        }
        faults.append(f);
    }

    // ---- 建设备:监听失败立即退出(CONFIG_INVALID)----
    QList<dms::sim::DeviceSimulator *> devices;
    for (int i = 0; i < count; ++i) {
        dms::sim::DeviceSimulator::Params p;
        p.index = i;
        p.listenPort = quint16(basePort + i);
        p.samplePeriodMs = period;
        p.seed = seed;
        p.faults = faults;
        auto *device = new dms::sim::DeviceSimulator(p, &app);
        QString err;
        if (!device->startListening(&err)) {
            usageError(QStringLiteral("cannot listen on 127.0.0.1:%1: %2")
                           .arg(p.listenPort)
                           .arg(err));
            return 1;
        }
        devices.append(device);
    }

    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, {},
                    QStringLiteral("simulator started: devices=%1 basePort=%2 period=%3ms "
                                   "seed=%4 faults=%5")
                        .arg(count)
                        .arg(basePort)
                        .arg(period)
                        .arg(seed)
                        .arg(faults.size()));

    // 集成测试的同步点:所有端口就绪后输出一行(stdout,不走日志)
    printf("SIM READY");
    for (const auto *d : devices)
        printf(" %u", unsigned(d->port()));
    printf("\n");
    fflush(stdout);

    // 周期统计(docs/15 §5:日志记录参数、累计发送帧/样本数、故障时间)
    QTimer statsTimer;
    QObject::connect(&statsTimer, &QTimer::timeout, &app, [&devices] {
        quint64 frames = 0, samples = 0;
        for (const auto *d : devices) {
            frames += d->framesSent();
            samples += d->samplesSent();
        }
        dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, {},
                        QStringLiteral("stats: devices=%1 framesSent=%2 samplesSent=%3")
                            .arg(devices.size())
                            .arg(frames)
                            .arg(samples));
    });
    statsTimer.start(10000);

    if (runSeconds > 0)
        QTimer::singleShot(runSeconds * 1000, &app, &QCoreApplication::quit);

    const int rc = app.exec();

    quint64 frames = 0, samples = 0;
    for (const auto *d : devices) {
        frames += d->framesSent();
        samples += d->samplesSent();
    }
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, {},
                    QStringLiteral("simulator exit: framesSent=%1 samplesSent=%2")
                        .arg(frames)
                        .arg(samples));
    dms::shutdownLogging();
    return rc;
}
