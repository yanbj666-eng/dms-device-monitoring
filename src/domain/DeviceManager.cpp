#include "DeviceManager.h"

#include <type_traits>

#include "common/Logging.h"

namespace dms::domain {

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    // queued 调用(applySetups/sendCommand)需要值对象已注册(HLD §2)
    qRegisterMetaType<dms::domain::DeviceSetup>("dms::domain::DeviceSetup");
    qRegisterMetaType<QList<dms::domain::DeviceSetup>>("QList<dms::domain::DeviceSetup>");
    qRegisterMetaType<QList<dms::protocol::CommandParam>>(
        "QList<dms::protocol::CommandParam>");
    qRegisterMetaType<dms::domain::CommandSubmission>("dms::domain::CommandSubmission");

    // 默认工厂:按 DeviceSetup 的 variant 活化分支建对应 transport
    // (S7 起三形态);测试注入 FakeTransport 时整体替换。
    // transport 配置里的 deviceId 只用于日志上下文
    transportFactory_ = [](const DeviceSetup &setup) {
        const auto withId = [id = setup.deviceId](auto cfg) {
            cfg.deviceId = id;
            return cfg;
        };
        return std::visit(
            [&withId](const auto &cfg) -> dms::transport::ITransport * {
                using T = std::decay_t<decltype(cfg)>;
                if constexpr (std::is_same_v<T, dms::transport::TcpTransportConfig>)
                    return new dms::transport::TcpTransport(withId(cfg));
                else if constexpr (std::is_same_v<T, dms::transport::UdpTransportConfig>)
                    return new dms::transport::UdpTransport(withId(cfg));
                else
                    return new dms::transport::SerialTransport(withId(cfg));
            },
            setup.transport);
    };
}

void DeviceManager::setTransportFactory(TransportFactory factory)
{
    transportFactory_ = std::move(factory);
}

int DeviceManager::deviceCount() const
{
    return sessions_.size();
}

void DeviceManager::applySetups(QList<DeviceSetup> setups)
{
    // 先统一停旧会话再替换:配置快照原则,不存在新旧混合运行的状态(LLD §5)
    stopSessions();
    for (DeviceSession *session : sessions_) {
        // 拆转发连接再延迟销毁:deleteLater 排队期间旧会话的信号不再外泄
        QObject::disconnect(session, nullptr, this, nullptr);
        session->deleteLater();
    }
    sessions_.clear();

    int started = 0;
    for (const DeviceSetup &setup : setups) {
        if (sessions_.contains(setup.deviceId)) {
            // Config 层已查重,这里双保险:静默丢设备比报错更难排查
            dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App,
                            dms::LogContext{setup.deviceId, {}, dms::ErrorCode::ConfigInvalid},
                            QStringLiteral("applySetups:重复设备 id,忽略后者"));
            continue;
        }
        createSession(setup);
        if (setup.connectOnStart)
            ++started;
    }
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, {},
                    QStringLiteral("applySetups:%1 台设备,%2 台自动连接")
                        .arg(sessions_.size()).arg(started));
}

void DeviceManager::startAll()
{
    for (DeviceSession *session : sessions_)
        session->start();
    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::App, {},
                    QStringLiteral("startAll:%1 台设备").arg(sessions_.size()));
}

void DeviceManager::stopAll()
{
    stopSessions();
    emit managerStopped();
}

void DeviceManager::stopSessions()
{
    for (DeviceSession *session : sessions_)
        session->stop();
}

void DeviceManager::startDevice(QString deviceId)
{
    DeviceSession *session = sessions_.value(deviceId, nullptr);
    if (!session) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, {deviceId, {}},
                        QStringLiteral("startDevice:设备不存在"));
        return;
    }
    session->start();
}

void DeviceManager::stopDevice(QString deviceId)
{
    DeviceSession *session = sessions_.value(deviceId, nullptr);
    if (!session) {
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::App, {deviceId, {}},
                        QStringLiteral("stopDevice:设备不存在"));
        return;
    }
    session->stop();
}

void DeviceManager::sendCommand(QString deviceId, quint16 commandId,
                                 QList<dms::protocol::CommandParam> params)
{
    DeviceSession *session = sessions_.value(deviceId, nullptr);
    if (!session) {
        emit commandRejected(deviceId, commandId, QStringLiteral("设备不存在"));
        return;
    }
    // 受理记录(commandSubmitted)由会话的 commandSent 转发发出(见 createSession):
    // 它在写 transport 之前,本地写失败同步终结为 Failed 时也保证受理先于终态
    if (session->sendCommand(commandId, params) == 0) {
        // Session 拒绝:状态不在 Online/Degraded、transport 不可写或已有未决命令
        emit commandRejected(deviceId, commandId,
                             QStringLiteral("会话不受理(状态/未决命令/不可写)"));
    }
}

void DeviceManager::createSession(const DeviceSetup &setup)
{
    dms::transport::ITransport *transport = transportFactory_(setup);
    // transport 所有权经 reparent 归会话,会话经 parent-child 归本管理器
    auto *session = new DeviceSession(setup.deviceId, transport, setup.session,
                                      DeviceSession::defaultClock,
                                      DeviceSession::defaultJitter, this);

    // 转发连接统一捕获 deviceId 值:GUI 侧无需自行维护 id->session 关联
    const QString id = setup.deviceId;
    connect(session, &DeviceSession::stateChanged, this,
            [this, id](dms::domain::SessionState state) { emit deviceStateChanged(id, state); });
    connect(session, &DeviceSession::telemetryReceived, this,
            [this, id](dms::domain::TelemetryBatch batch) { emit deviceTelemetry(id, batch); });
    connect(session, &DeviceSession::heartbeatReceived, this,
            [this, id](quint32 seq, quint64 timestampMs) {
                emit deviceHeartbeat(id, seq, timestampMs);
            });
    connect(session, &DeviceSession::protocolHealth, this,
            [this, id](quint32 badCrcFrames, quint32 goodFrames) {
                emit deviceProtocolHealth(id, badCrcFrames, goodFrames);
            });
    connect(session, &DeviceSession::commandSent, this,
            [this, id, session](quint32 seq, quint16 commandId,
                                const QList<dms::protocol::CommandParam> &params) {
                // 受理即广播完整记录:commands 表 Sent 行所需的全部字段(S5)
                CommandSubmission submission;
                submission.deviceId = id;
                submission.sessionId = session->sessionId();
                submission.seq = seq;
                submission.commandId = commandId;
                submission.params = params;
                emit commandSubmitted(submission);
                emit deviceCommandSent(id, seq, commandId);
            });
    connect(session, &DeviceSession::commandFinished, this,
            [this, id](dms::domain::CommandOutcome outcome) {
                emit deviceCommandFinished(id, outcome);
            });
    connect(session, &DeviceSession::sessionError, this,
            [this, id](dms::ErrorCode code, const QString &message) {
                emit deviceError(id, code, message);
            });

    sessions_.insert(id, session);
    if (setup.connectOnStart)
        session->start();
}

} // namespace dms::domain
