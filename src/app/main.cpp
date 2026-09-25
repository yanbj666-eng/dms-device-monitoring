#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QThread>

#include "Config.h"
#include "domain/DeviceManager.h"
#include "ui/MainWindow.h"
#include "common/Logging.h"

// 上位机装配(S4):
//   GUI 线程:QApplication + MainWindow + QThread 管理;
//   IO 线程:DeviceManager worker-object(无 parent → moveToThread →
//   started 槽里 applySetups,会话与 socket 全部 IO 线程亲和,HLD §2);
//   退出顺序:停会话 → 线程 quit/wait,不强杀(HLD §4)
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

    QThread ioThread;
    auto *manager = new dms::domain::DeviceManager;
    manager->moveToThread(&ioThread);
    QObject::connect(&ioThread, &QThread::finished, manager, &QObject::deleteLater);
    QObject::connect(&ioThread, &QThread::started, manager, [manager, setups] {
        manager->applySetups(setups); // connectOnStart=true:装配即连接
    });

    dms::ui::MainWindow window(loaded.app.refreshIntervalMs, loaded.app.curvePoints,
                               deviceIdNames);
    // 跨线程全部 auto 连接:GUI<-IO 传值对象,GUI->IO 调槽(HLD §2 红线)
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

    ioThread.start();
    window.show();

    const int rc = app.exec();

    // 退出顺序(HLD §4):阻塞等 IO 线程把会话停完,再收线程;
    // BlockingQueued 让停会话在 IO 线程执行,与 socket/定时器同线程
    QMetaObject::invokeMethod(manager, "stopAll", Qt::BlockingQueuedConnection);
    ioThread.quit();
    ioThread.wait(5000);
    dms::shutdownLogging();
    return rc;
}
