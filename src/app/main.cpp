#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QThread>

#include "Config.h"
#include "domain/DeviceManager.h"
#include "persistence/DatabaseWorker.h"
#include "ui/MainWindow.h"
#include "common/Logging.h"

// 上位机装配(S4+S5):
//   GUI 线程:QApplication + MainWindow + QThread 管理;
//   IO 线程:DeviceManager worker-object(无 parent → moveToThread →
//   started 槽里 applySetups,会话与 socket 全部 IO 线程亲和,HLD §2);
//   DB 线程(S5):DatabaseWorker 同模式,GUI/IO 只经 queued 信号喂数据;
//   退出顺序(HLD §4):停会话 → IO 收线程 → DB 排空或上报未提交 → DB 收线程
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

    // LoadedConfig → DeviceSetup:仅 tcp 且启用的设备建会话(S4 范围)
    QList<dms::domain::DeviceSetup> setups;
    QVector<QPair<QString, QString>> deviceIdNames;
    for (const dms::app::DeviceConnectionConfig &dev : loaded.devices) {
        if (dev.transport != QStringLiteral("tcp") || !dev.enabled)
            continue;
        dms::domain::DeviceSetup setup;
        setup.deviceId = dev.id;
        setup.transport.deviceId = dev.id;
        setup.transport.host = dev.host;
        setup.transport.port = dev.port;
        setup.transport.connectTimeoutMs = loaded.app.transport.connectTimeoutMs;
        setup.transport.maxTxBytes = loaded.app.transport.maxTxBytes;
        setup.session = loaded.app.session;
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
    QObject::connect(&ioThread, &QThread::started, manager, [manager, setups] {
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
