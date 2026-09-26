#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

#include "DeviceSession.h"
#include "MessageCodec.h"
#include "transport/TcpTransport.h"

namespace dms::domain {

// 设备装配描述:app 层把 devices.json + app.ini 映射成这个值对象,
// queued 传给 IO 线程的 DeviceManager(依赖方向 app -> domain,不反向)
struct DeviceSetup {
    QString deviceId;
    dms::transport::TcpTransportConfig transport;
    dms::domain::SessionConfig session;
    bool connectOnStart = true;
};

// 命令受理记录(S5 持久化用):sendCommand 被会话受理后发射,
// DB worker 据此落 commands 表 Sent 行;终态经 deviceCommandFinished 补全
struct CommandSubmission {
    QString deviceId;
    QUuid sessionId;
    quint32 seq = 0;
    quint16 commandId = 0;
    QList<dms::protocol::CommandParam> params;
};

// 设备管理器(LLD §5):IO 线程 worker-object,拥有全部 DeviceSession,
// 负责配置快照、统一启停与命令路由。
//
// 线程约定(HLD §2):无 parent 构造后 moveToThread(ioThread);
// GUI 只经 queued 信号/invoke 调用本类,返回值类结果经信号异步报告,
// 不直接触碰 DeviceSession(值对象信号之外的接口仅所属线程可用)。
class DeviceManager : public QObject
{
    Q_OBJECT

public:
    // transport 工厂:默认按 setup 建 TcpTransport;测试注入 FakeTransport。
    // 只在装配期(moveToThread 之前)或同线程调用
    using TransportFactory =
        std::function<dms::transport::ITransport *(const DeviceSetup &)>;

    explicit DeviceManager(QObject *parent = nullptr);

    void setTransportFactory(TransportFactory factory);
    int deviceCount() const; // 仅所属线程诊断用

public slots:
    // 全量替换会话:先停旧会话再建新(配置快照原则,LLD §5)
    void applySetups(QList<dms::domain::DeviceSetup> setups);
    void startAll();
    void stopAll();
    void startDevice(QString deviceId);
    void stopDevice(QString deviceId);
    // 命令路由:进展经 deviceCommandSent / deviceCommandFinished 异步报告,
    // 拒绝(设备不存在/会话不受理)经 commandRejected 报告
    void sendCommand(QString deviceId, quint16 commandId,
                     QList<dms::protocol::CommandParam> params = {});

signals:
    // Session 信号的转发版,统一带上 deviceId(GUI 侧不需要自行关联)
    void deviceStateChanged(QString deviceId, dms::domain::SessionState state);
    void deviceTelemetry(QString deviceId, dms::domain::TelemetryBatch batch);
    void deviceHeartbeat(QString deviceId, quint32 seq, quint64 timestampMs);
    void deviceCommandSent(QString deviceId, quint32 seq, quint16 commandId);
    void deviceCommandFinished(QString deviceId, dms::domain::CommandOutcome outcome);
    void deviceError(QString deviceId, dms::ErrorCode code, QString message);
    void commandRejected(QString deviceId, quint16 commandId, QString reason);
    // 会话受理了命令(Sent 起点):比 deviceCommandSent 多带 params 与
    // sessionId,供 DB worker 写 commands 表(S5)
    void commandSubmitted(dms::domain::CommandSubmission submission);
    void managerStopped(); // stopAll 完成(退出序列的同步点)

private:
    void createSession(const DeviceSetup &setup);
    void stopSessions(); // 停全部会话,不发信号(applySetups 内部复用)

    QHash<QString, DeviceSession *> sessions_;
    TransportFactory transportFactory_;
};

} // namespace dms::domain

Q_DECLARE_METATYPE(dms::domain::DeviceSetup)
Q_DECLARE_METATYPE(dms::domain::CommandSubmission)
