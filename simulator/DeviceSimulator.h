#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <random>

#include "FaultSpec.h"
#include "FrameDecoder.h"
#include "MessageCodec.h"
#include "common/Logging.h"

namespace dms::sim {

// 单个模拟设备端点:一个监听端口,同时服务一个客户端连接。
// 单线程运行:全部 socket/timer 挂在模拟器进程唯一的事件循环上
// (HLD §2 "10 设备共享一个 IO 事件循环"在设备侧的镜像)。
//
// 设备侧行为(协议 §4.2):
//   - 遥测默认 100ms 周期发四指标,Start/Stop 控制,心跳 1000ms 恒发;
//   - Reset:先 ACK 后复位周期/业务计数并启动遥测,连接与发送序号不重置;
//   - SetParameter:仅 param_id=1 uint32 sample_period_ms∈[100,1000];
//   - 未知命令/参数越界/重复参数回拒绝 ACK;结构不完整的命令整帧丢弃;
//   - ACK 回显 COMMAND 的 Seq,不消耗设备自身发送序号(协议 §4.3)
class DeviceSimulator : public QObject
{
    Q_OBJECT

public:
    struct Params {
        int index = 0;             // 0 基:种子分流与日志标签用
        quint16 listenPort = 0;
        int samplePeriodMs = 100;  // 可被 SetParameter 修改
        quint32 seed = 1;
        QList<FaultSpec> faults;
    };

    explicit DeviceSimulator(Params params, QObject *parent = nullptr);

    bool startListening(QString *errorOut = nullptr);

    int index() const { return params_.index; }
    quint16 port() const { return params_.listenPort; }
    quint64 framesSent() const { return framesSent_; }
    quint64 samplesSent() const { return samplesSent_; }

private:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();
    void onTelemetryTick();
    void onHeartbeatTick();
    void onFaultWatchTick();
    void onMergeFlush();

    void handleCommandFrame(const dms::protocol::DecodedFrame &frame);
    dms::protocol::AckStatus validateAndApplyCommand(const dms::protocol::CommandMessage &cmd,
                                                     bool &ackThenAct);
    void sendAck(quint32 echoSeq, quint16 commandId, dms::protocol::AckStatus status);
    void sendAckNow(quint32 echoSeq, quint16 commandId, dms::protocol::AckStatus status);

    // 唯一的帧出口:fragment/merge/badCrc 故障都拦在这里,收发路径不变
    void deliverFrame(const QByteArray &frame, bool isAck);
    void rawWrite(const QByteArray &chunk);
    void closeClient(); // 断信号->abort->延迟回收,顶替/故障拆除共用

    bool faultActive(FaultMode mode) const;
    bool hasClient() const;
    QString deviceId() const;
    dms::LogContext logCtx(dms::ErrorCode code = dms::ErrorCode::None) const;

    Params params_;
    QTcpServer server_;
    QTcpSocket *client_ = nullptr; // 当前唯一客户端连接
    QTimer telemetryTimer_;
    QTimer heartbeatTimer_;
    QTimer faultWatchTimer_; // 100ms:记录故障窗口开/关 + 触发主动断开
    QTimer mergeFlushTimer_; // merge 模式下防止孤帧滞留
    dms::protocol::FrameDecoder decoder_; // 每连接独立
    QElapsedTimer clock_;                  // 故障窗口的时基(相对启动)

    std::mt19937 rng_; // 种子 = seed + index*1000003,每设备独立可复现流
    std::normal_distribution<float> tempDist_{25.0f, 4.0f};
    std::normal_distribution<float> pressDist_{101.3f, 3.0f};
    std::normal_distribution<float> voltDist_{12.0f, 0.5f};
    std::uniform_int_distribution<quint32> rpmDist_{500, 3500};

    quint32 sendSeq_ = 0;       // 设备普通发送序号(新连接清零;ACK 不占用)
    bool telemetryRunning_ = true;
    quint64 framesSent_ = 0;    // 业务计数(Reset 清零)
    quint64 samplesSent_ = 0;
    QByteArray mergeBuffer_;    // merge 故障的待并帧
    int mergePending_ = 0;
    QByteArray fragmentTail_;   // fragment 故障的待补字节(半帧尾巴 + 排队的整帧)
    quint8 lastFaultMask_ = 0;  // 上次巡检时各模式是否活跃(按 FaultMode 位)
};

} // namespace dms::sim
