// dms_simulator:设备模拟器 CLI(S2 TCP;S7 增 UDP/串口形态,docs/15 §5)
// 全部设备共享本进程唯一事件循环;固定种子决定数据序列
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>
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

// 故障与传输形态的合法组合(docs/15 §5):
//   数据报三故障仅 UDP——TCP/串口跳一条应用帧不是"网络丢包";
//   fragment/merge 仅字节流——UDP 报文边界天然保留,无半包/粘包;
//   disconnect/ackDelay/ackNoReply 仅有命令路径的形态——UDP 不收命令、无连接
bool faultAllowedOnTransport(dms::sim::FaultMode mode, dms::sim::SimTransport transport)
{
    using dms::sim::FaultMode;
    using dms::sim::SimTransport;
    const bool udpOnly = mode == FaultMode::DatagramDrop || mode == FaultMode::DatagramReorder
                         || mode == FaultMode::DatagramDuplicate;
    if (udpOnly)
        return transport == SimTransport::Udp;
    const bool streamOnly = mode == FaultMode::Fragment || mode == FaultMode::Merge;
    if (streamOnly)
        return transport != SimTransport::Udp;
    const bool commandOnly =
        mode == FaultMode::Disconnect || mode == FaultMode::AckDelay || mode == FaultMode::AckNoReply;
    if (commandOnly)
        return transport != SimTransport::Udp;
    return true; // badCrc/abnormalValue/stopSending:三形态皆可
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dms-simulator"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "DMS 设备模拟器(协议 v1):TCP/UDP/串口三形态,四指标遥测+心跳+命令/ACK(TCP/串口),"
        "支持可复现故障注入"));
    parser.addHelpOption();
    parser.addVersionOption();
    // 一次性注册全部选项并检查返回值:短名撞车(如 -v 已被 --version 占用)
    // 会让 addOptions 静默失败,整组选项消失——必须在启动期暴露
    const bool optionsRegistered = parser.addOptions({
        {{"c", "count"},
         QStringLiteral("模拟设备数(TCP/UDP:各占一个端口;serial:由 --serial-ports 个数决定)"),
         QStringLiteral("n"),
         QStringLiteral("10")},
        {{"t", "transport"},
         QStringLiteral("传输形态 tcp|udp|serial"),
         QStringLiteral("kind"),
         QStringLiteral("tcp")},
        {{"p", "port"},
         QStringLiteral("起始端口(TCP 监听 / UDP 固定源端口)"),
         QStringLiteral("port"),
         QStringLiteral("9001")},
        {QStringLiteral("peer"),
         QStringLiteral("UDP 单发目标(DMS 绑定端点)host:port;udp 必填"),
         QStringLiteral("host:port")},
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
                        "fragment|merge|datagramDrop|datagramReorder|datagramDuplicate"),
         QStringLiteral("spec")},
        {QStringLiteral("serial-ports"),
         QStringLiteral("串口名逗号列表(serial 必填,如 COM3,COM5)"),
         QStringLiteral("list")},
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

    // ---- 传输形态 ----
    dms::sim::SimTransport transport = dms::sim::SimTransport::Tcp;
    {
        const QString t = parser.value(QStringLiteral("transport")).toLower();
        if (t == QStringLiteral("tcp")) transport = dms::sim::SimTransport::Tcp;
        else if (t == QStringLiteral("udp")) transport = dms::sim::SimTransport::Udp;
        else if (t == QStringLiteral("serial")) transport = dms::sim::SimTransport::Serial;
        else {
            usageError(QStringLiteral("--transport must be tcp|udp|serial"));
            return 1;
        }
    }

    // ---- UDP 单发目标(DMS 绑定端点)----
    QHostAddress peerAddress;
    quint16 peerPort = 0;
    {
        const QString peerText = parser.value(QStringLiteral("peer"));
        if (transport == dms::sim::SimTransport::Udp) {
            const qsizetype colon = peerText.indexOf(QLatin1Char(':'));
            if (colon <= 0 || peerText.indexOf(QLatin1Char(':'), colon + 1) >= 0
                || peerText.isEmpty()) {
                usageError(QStringLiteral("--peer is required for udp (expect host:port)"));
                return 1;
            }
            peerAddress = QHostAddress(peerText.left(colon));
            bool okPort = false;
            const quint16 parsed = peerText.mid(colon + 1).toUShort(&okPort);
            if (peerAddress.isNull() || !okPort || parsed == 0) {
                usageError(QStringLiteral("invalid --peer '%1' (expect host:port)").arg(peerText));
                return 1;
            }
            peerPort = parsed;
        } else if (parser.isSet(QStringLiteral("peer"))) {
            usageError(QStringLiteral("--peer only applies to --transport udp"));
            return 1;
        }
    }

    // ---- 串口名列表 ----
    QStringList serialPorts;
    if (transport == dms::sim::SimTransport::Serial) {
        const QString list = parser.value(QStringLiteral("serial-ports"));
        if (list.isEmpty()) {
            usageError(QStringLiteral("--serial-ports is required for --transport serial"));
            return 1;
        }
        for (QString &name : list.split(QLatin1Char(','))) {
            const QString trimmed = name.trimmed();
            if (trimmed.isEmpty()) {
                usageError(QStringLiteral("--serial-ports contains an empty name"));
                return 1;
            }
            serialPorts.append(trimmed);
        }
    } else if (parser.isSet(QStringLiteral("serial-ports"))) {
        usageError(QStringLiteral("--serial-ports only applies to --transport serial"));
        return 1;
    }

    // ---- 参数校验(非法值明确报错退出,不悄悄改默认值)----
    bool ok = false;
    int count = parser.value(QStringLiteral("count")).toInt(&ok);
    if (!ok || count < 1 || count > 100) {
        usageError(QStringLiteral("--count must be 1-100"));
        return 1;
    }
    if (transport == dms::sim::SimTransport::Serial) {
        // serial 设备数 = 串口名个数;显式 --count 与之不符是配置笔误,报错
        if (parser.isSet(QStringLiteral("count")) && count != serialPorts.size()) {
            usageError(QStringLiteral("--count (%1) != --serial-ports entries (%2)")
                           .arg(count)
                           .arg(serialPorts.size()));
            return 1;
        }
        count = serialPorts.size();
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
        if (!faultAllowedOnTransport(f.mode, transport)) {
            usageError(QStringLiteral("fault %1 not supported on transport %2 (docs/15 §5)")
                           .arg(dms::sim::faultModeName(f.mode),
                                parser.value(QStringLiteral("transport")).toLower()));
            return 1;
        }
        faults.append(f);
    }

    // ---- 建设备:监听/绑定/打开失败立即退出(CONFIG_INVALID)----
    QList<dms::sim::DeviceSimulator *> devices;
    for (int i = 0; i < count; ++i) {
        dms::sim::DeviceSimulator::Params p;
        p.index = i;
        p.transport = transport;
        p.samplePeriodMs = period;
        p.seed = seed;
        p.faults = faults;
        if (transport == dms::sim::SimTransport::Serial) {
            p.serialPortName = serialPorts.at(i);
        } else {
            p.listenPort = quint16(basePort + i);
            p.peerAddress = peerAddress;
            p.peerPort = peerPort;
        }
        auto *device = new dms::sim::DeviceSimulator(p, &app);
        QString err;
        if (!device->startListening(&err)) {
            usageError(QStringLiteral("cannot start %1 endpoint %2: %3")
                           .arg(parser.value(QStringLiteral("transport")).toLower(),
                                device->endpointDesc(), err));
            return 1;
        }
        devices.append(device);
    }

    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, {},
                    QStringLiteral("simulator started: transport=%1 devices=%2 basePort=%3 "
                                   "period=%4ms seed=%5 faults=%6")
                        .arg(parser.value(QStringLiteral("transport")).toLower())
                        .arg(count)
                        .arg(transport == dms::sim::SimTransport::Serial
                                 ? QStringLiteral("n/a")
                                 : QString::number(basePort))
                        .arg(period)
                        .arg(seed)
                        .arg(faults.size()));

    // 集成测试的同步点:所有端点就绪后输出一行(stdout,不走日志)。
    // TCP/UDP 是端口号,serial 是串口名(tst_integration 按形态解析)
    printf("SIM READY");
    for (const auto *d : devices)
        printf(" %s", qPrintable(d->endpointDesc()));
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
