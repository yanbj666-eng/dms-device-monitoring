#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QMessageBox>
#include <QSerialPort>
#include <QStandardPaths>
#include <QThread>

#include "Config.h"
#include "domain/AlarmEngine.h"
#include "domain/DeviceManager.h"
#include "persistence/DatabaseWorker.h"
#include "ui/MainWindow.h"
#include "common/Logging.h"

// 上位机装配(S4+S5+S6):
//   GUI 线程:QApplication + MainWindow + QThread 管理;
//   IO 线程:DeviceManager + AlarmEngine 双 worker-object(无 parent →
//   moveToThread → started 槽里装配,会话/socket/裁决定时器全 IO 线程亲和,HLD §2);
//   DB 线程(S5):DatabaseWorker 同模式,GUI/IO 只经 queued 信号喂数据;
//   退出顺序(HLD §4):停会话(引擎补发 manual_disconnect 恢复事件)→
//   IO 收线程 → DB 排空或上报未提交 → DB 收线程
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("dms"));
    QApplication::setOrganizationName(QStringLiteral("dms"));

    // 默认运行根 = AppLocalDataLocation(docs/15 §1);
    // --config-dir 供开发/验收指定独立目录,不污染用户配置
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DMS 设备监控上位机"));
    parser.addHelpOption();
    QCommandLineOption configDirOption(QStringLiteral("config-dir"),
                                       QStringLiteral("运行根目录(其下含 config/)"),
                                       QStringLiteral("dir"));
    parser.addOption(configDirOption);
    parser.process(app);

    const QString runRoot = parser.isSet(configDirOption)
        ? parser.value(configDirOption)
        : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString templateDir = QCoreApplication::applicationDirPath()
                                + QStringLiteral("/config");

    const dms::app::LoadedConfig loaded = dms::app::loadConfiguration(runRoot, templateDir);
    if (!loaded.ok) {
        QString detail;
        for (const dms::app::ConfigIssue &issue : loaded.issues)
            detail += QStringLiteral("%1:%2\n").arg(issue.path, issue.message);
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::App,
                        {QString(), {}, dms::ErrorCode::ConfigInvalid},
                        QStringLiteral("配置加载失败:\n%1").arg(detail));
        QMessageBox::critical(nullptr, QStringLiteral("配置错误"),
                              QStringLiteral("配置加载失败,请修正后重启:\n\n%1").arg(detail));
        return 1;
    }

    dms::initializeLogging(loaded.app.logging, runRoot + QStringLiteral("/logs"));
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, dms::LogContext{},
                    QStringLiteral("运行根:%1,设备 %2 台")
                        .arg(QDir(runRoot).absolutePath())
                        .arg(loaded.devices.size()));

    // LoadedConfig → DeviceSetup:启用的设备按 transport 形态建会话(S7 起三形态);
    // app.ini 共享参数合入对应分支,session.transportKind 决定状态机链路语义
    QList<dms::domain::DeviceSetup> setups;
    QVector<QPair<QString, QString>> deviceIdNames;
    for (const dms::app::DeviceConnectionConfig &dev : loaded.devices) {
        if (!dev.enabled)
            continue;
        dms::domain::DeviceSetup setup;
        setup.deviceId = dev.id;
        setup.session = loaded.app.session;
        if (dev.transport == QStringLiteral("tcp")) {
            dms::transport::TcpTransportConfig cfg;
            cfg.deviceId = dev.id;
            cfg.host = dev.host;
            cfg.port = dev.port;
            cfg.connectTimeoutMs = loaded.app.transport.connectTimeoutMs;
            cfg.maxTxBytes = loaded.app.transport.maxTxBytes;
            setup.transport = cfg;
        } else if (dev.transport == QStringLiteral("udp")) {
            dms::transport::UdpTransportConfig cfg;
            cfg.deviceId = dev.id;
            cfg.bindAddress = QHostAddress(dev.bindAddress);
            cfg.bindPort = dev.bindPort;
            cfg.peerAddress = QHostAddress(dev.peerAddress);
            cfg.peerPort = dev.peerPort;
            setup.transport = cfg;
            setup.session.transportKind = dms::transport::TransportKind::Udp;
        } else {
            dms::transport::SerialTransportConfig cfg;
            cfg.deviceId = dev.id;
            cfg.portName = dev.portName;
            cfg.baudRate = dev.baudRate;
            cfg.dataBits = dev.dataBits == 7 ? QSerialPort::Data7 : QSerialPort::Data8;
            cfg.parity = dev.parity == QStringLiteral("even")   ? QSerialPort::EvenParity
                         : dev.parity == QStringLiteral("odd")   ? QSerialPort::OddParity
                                                                 : QSerialPort::NoParity;
            cfg.stopBits = dev.stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop;
            cfg.flowControl = dev.flowControl == QStringLiteral("hardware")
                                  ? QSerialPort::HardwareControl
                              : dev.flowControl == QStringLiteral("software")
                                  ? QSerialPort::SoftwareControl
                                  : QSerialPort::NoFlowControl;
            cfg.maxTxBytes = loaded.app.transport.maxTxBytes;
            setup.transport = cfg;
            setup.session.transportKind = dms::transport::TransportKind::Serial;
        }
        setups.append(setup);
        deviceIdNames.append({dev.id, dev.name});
    }

    // devices 表镜像整个配置文件(含停用与未实现 transport 的设备),
    // 供历史数据外键关联,不反向覆盖 JSON(docs/06 §1)
    QList<dms::persistence::DeviceRow> deviceRows;
    for (const dms::app::DeviceConnectionConfig &dev : loaded.devices) {
        dms::persistence::DeviceRow row;
        row.id = dev.id;
        row.name = dev.name;
        row.transportType = dev.transport;
        row.configJson = dev.rawJson;
        row.enabled = dev.enabled;
        deviceRows.append(row);
    }

    QThread ioThread;
    auto *manager = new dms::domain::DeviceManager;
    manager->moveToThread(&ioThread);
    QObject::connect(&ioThread, &QThread::finished, manager, &QObject::deleteLater);

    // S6 告警引擎:与 manager 同住 IO 线程。评估参数从 [connection] 同源映射
    // (LLD §3/§6 引用同一组阈值,避免第二套配置漂移),规则按设备注入;
    // Config 层已保证 low<high、迟滞域内,引擎信任规则(docs/15 §4)
    dms::domain::AlarmEngine::Settings engineSettings;
    engineSettings.pollIntervalMs = loaded.app.session.pollIntervalMs;
    engineSettings.staleTimeoutMs = loaded.app.session.metricTimeoutMs;
    engineSettings.badCrcTrigger = loaded.app.session.consecutiveBadCrcLimit;
    engineSettings.goodFramesToClearCrc = loaded.app.session.goodFramesToRecoverOnline;
    auto *alarmEngine = new dms::domain::AlarmEngine(engineSettings);
    alarmEngine->moveToThread(&ioThread);
    QObject::connect(&ioThread, &QThread::finished, alarmEngine,
                      &QObject::deleteLater);

    // 规则快照在 GUI 线程建好,started 槽里随会话一起注入(同线程,免锁);
    // S7 起三形态设备都参与评估(规则本身与传输形态无关)
    QHash<QString, QList<dms::domain::AlarmRule>> alarmRules;
    for (const dms::app::DeviceConnectionConfig &dev : loaded.devices) {
        if (!dev.enabled)
            continue; // 只有建会话的设备才评估
        QList<dms::domain::AlarmRule> rules;
        for (const dms::app::AlarmRuleConfig &r : dev.alarmRules) {
            dms::domain::AlarmRule rule;
            rule.metricId = r.metricId;
            rule.low = r.low;
            rule.high = r.high;
            rule.hysteresis = r.hysteresis;
            rules.append(rule);
        }
        alarmRules.insert(dev.id, rules);
    }

    QObject::connect(&ioThread, &QThread::started, manager,
                     [manager, setups, alarmEngine, alarmRules] {
                         for (auto it = alarmRules.cbegin();
                              it != alarmRules.cend(); ++it)
                             alarmEngine->setRules(it.key(), it.value());
                         alarmEngine->start(); // 裁决定时器建在本线程(亲和)
                         manager->applySetups(setups); // connectOnStart=true:装配即连接
                     });

    // DB 线程(S5):app.ini [database] → DbWorkerSettings;
    // path 是运行根下相对路径,装配层解析成绝对路径(docs/15 §4)
    dms::persistence::DbWorkerSettings dbSettings;
    dbSettings.databasePath =
        QFileInfo(QDir(runRoot).filePath(loaded.app.db.path)).absoluteFilePath();
    dbSettings.batchSamples = loaded.app.db.batchSamples;
    dbSettings.flushIntervalMs = loaded.app.db.flushIntervalMs;
    dbSettings.queueSamples = loaded.app.db.queueSamples;
    dbSettings.eventQueueSize = loaded.app.db.eventQueueSize;
    dbSettings.busyTimeoutMs = loaded.app.db.busyTimeoutMs;
    dbSettings.shutdownFlushMs = loaded.app.db.shutdownFlushMs;

    QThread dbThread;
    auto *dbWorker = new dms::persistence::DatabaseWorker(dbSettings);
    dbWorker->moveToThread(&dbThread);
    QObject::connect(&dbThread, &QThread::finished, dbWorker, &QObject::deleteLater);

    dms::ui::MainWindow window(loaded.app.refreshIntervalMs, loaded.app.curvePoints,
                               deviceIdNames);    // 跨线程全部 auto 连接:GUI<-IO 传值对象,GUI->IO 调槽(HLD §2 红线)
    QObject::connect(manager, &dms::domain::DeviceManager::deviceStateChanged, &window,
                     &dms::ui::MainWindow::onDeviceStateChanged);
    QObject::connect(manager, &dms::domain::DeviceManager::deviceTelemetry, &window,
                     &dms::ui::MainWindow::onDeviceTelemetry);
    QObject::connect(manager, &dms::domain::DeviceManager::deviceCommandFinished, &window,
                     &dms::ui::MainWindow::onDeviceCommandFinished);
    QObject::connect(manager, &dms::domain::DeviceManager::commandRejected, &window,
                     &dms::ui::MainWindow::onCommandRejected);
    QObject::connect(&window, &dms::ui::MainWindow::startAllRequested, manager,
                     &dms::domain::DeviceManager::startAll);
    QObject::connect(&window, &dms::ui::MainWindow::stopAllRequested, manager,
                     &dms::domain::DeviceManager::stopAll);
    QObject::connect(&window, &dms::ui::MainWindow::sendCommandRequested, manager,
                     &dms::domain::DeviceManager::sendCommand);

    // IO → DB:跨线程 queued,全部传值对象(HLD §2 红线)
    QObject::connect(manager, &dms::domain::DeviceManager::deviceTelemetry, dbWorker,
                     &dms::persistence::DatabaseWorker::onTelemetry);
    QObject::connect(manager, &dms::domain::DeviceManager::commandSubmitted, dbWorker,
                     &dms::persistence::DatabaseWorker::onCommandSubmitted);
    QObject::connect(manager, &dms::domain::DeviceManager::deviceCommandFinished,
                     dbWorker, &dms::persistence::DatabaseWorker::onCommandFinished);

    // manager → 引擎:两者同住 IO 线程,auto 连接解析为 direct,
    // 值对象零拷贝直达裁决点(LLD §6 四类告警的输入面)
    QObject::connect(manager, &dms::domain::DeviceManager::deviceTelemetry, alarmEngine,
                     &dms::domain::AlarmEngine::onTelemetry);
    QObject::connect(manager, &dms::domain::DeviceManager::deviceStateChanged,
                     alarmEngine, &dms::domain::AlarmEngine::onSessionState);
    QObject::connect(manager, &dms::domain::DeviceManager::deviceProtocolHealth,
                     alarmEngine, &dms::domain::AlarmEngine::onProtocolHealth);
    QObject::connect(manager, &dms::domain::DeviceManager::deviceCommandFinished,
                     alarmEngine, &dms::domain::AlarmEngine::onCommandFinished);

    // 引擎唯一出口分流:DB 落库与 GUI 显示,均为 queued 传 AlarmEvent 值对象;
    // GUI 确认请求反方向 queued 进引擎,结果仍经 alarmEvent 异步广播,不返回值
    QObject::connect(alarmEngine, &dms::domain::AlarmEngine::alarmEvent, dbWorker,
                     &dms::persistence::DatabaseWorker::onAlarmEvent);
    QObject::connect(alarmEngine, &dms::domain::AlarmEngine::alarmEvent, &window,
                     &dms::ui::MainWindow::onAlarmEvent);
    QObject::connect(&window, &dms::ui::MainWindow::alarmAckRequested, alarmEngine,
                     &dms::domain::AlarmEngine::acknowledge);
    // 建库失败不拖垮通信与显示(docs/06 §5):GUI 提示一次,
    // worker 侧继续显式计数丢弃,连接/曲线照常
    QObject::connect(dbWorker, &dms::persistence::DatabaseWorker::dbFatal, &window,
                     [](dms::ErrorCode code, const QString &message) {
                         QMessageBox::warning(
                             nullptr, QStringLiteral("数据库不可用"),
                             QStringLiteral("历史数据将不会保存(错误码 %1):\n%2")
                                 .arg(dms::errorCodeName(code))
                                 .arg(message));
                     });

    // DB 先就绪再放行采集:initialize 含建库/恢复/设备 upsert,若晚于首批
    // 遥测,未入库设备会被防毒丸逻辑丢弃。装配期阻塞一次,不进事件循环
    dbThread.start();
    QMetaObject::invokeMethod(
        dbWorker, [dbWorker, deviceRows] { dbWorker->initialize(deviceRows); },
        Qt::BlockingQueuedConnection);

    ioThread.start();
    window.show();

    const int rc = app.exec();

    // 退出顺序(HLD §4):禁止新命令/停采集 → IO 停会话并收线程 →
    // DB 排空或上报未提交 → DB 收线程;全程不强杀、不析构运行中的线程
    QMetaObject::invokeMethod(manager, "stopAll", Qt::BlockingQueuedConnection);
    ioThread.quit();
    ioThread.wait(5000);

    // closed 不能经 BlockingQueued 拿返回值:Direct 连接让它在 DB 线程内
    // 即时落日志,不依赖即将退出的 GUI 事件循环(日志器线程安全,无 QWidget)
    QObject::connect(dbWorker, &dms::persistence::DatabaseWorker::closed,
                     [](qint64 uncommittedSamples, qint64 uncommittedEvents,
                        quint64 droppedSamples, quint64 droppedEvents) {
                         const bool clean = uncommittedSamples == 0
                                            && uncommittedEvents == 0
                                            && droppedSamples == 0
                                            && droppedEvents == 0;
                         dms::logMessage(
                             clean ? dms::LogLevel::Info : dms::LogLevel::Warn,
                             dms::LogCategory::Db, {},
                             QStringLiteral("DB 关闭:未提交遥测 %1/命令事件 %2,"
                                             "累计丢弃样本 %3/事件 %4")
                                 .arg(uncommittedSamples)
                                 .arg(uncommittedEvents)
                                 .arg(droppedSamples)
                                 .arg(droppedEvents));
                     });
    QMetaObject::invokeMethod(dbWorker, "drainAndClose", Qt::BlockingQueuedConnection);
    dbThread.quit();
    dbThread.wait(5000);
    dms::shutdownLogging();
    return rc;
}
