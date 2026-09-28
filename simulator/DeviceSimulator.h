#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <random>

#include "FaultSpec.h"
#include "FrameDecoder.h"
#include "MessageCodec.h"
#include "common/Logging.h"

namespace dms::sim {

// 传输形态(协议 §7/LLD §3):决定信道原语与会话可见行为——
//   Tcp:监听端口,单客户端字节流,全命令集 + 流式故障;
//   Udp:固定源端口单发到对端端点,一报一帧,只发遥测/心跳不收命令;
//   Serial:独占串口字节流,命令集与 TCP 同,断链故障=关端口
enum class SimTransport {
    Tcp,
    Udp,
    Serial,
};

// 单个模拟设备端点:一个监听端口,同时服务一个客户端连接。
// 单线程运行:全部 socket/timer 挂在模拟器进程唯一的事件循环上
// (HLD §2 "10 设备共享一个 IO 事件循环"在设备侧的镜像)。
//
// 设备侧行为(协议 §4.2):
//   - 遥测默认 100ms 周期发四指标,Start/Stop 控制,心跳 1000ms 恒发;
//   - Reset:先 ACK 后复位周期/业务计数并启动遥测,连接与发送序号不重置;
//   - SetParameter:仅 param_id=1 uint32 sample_period_ms∈[100,1000];
//   - 未知命令/参数越界/重复参数回拒绝 ACK;结构不完整的命令整帧丢弃;
//   - ACK 回显 COMMAND 的 Seq,不消耗设备自身发送序号(协议 §4.3)。
//   - UDP 形态不收命令:遥测/心跳各占一个数据报(协议 §7 一报一帧)
class DeviceSimulator : public QObject
{
    Q_OBJECT

public:
    struct Params {
        int index = 0;             // 0 基:种子分流与日志标签用
        SimTransport transport = SimTransport::Tcp;
        quint16 listenPort = 0;    // TCP:监听端口;UDP:固定源端口
        // UDP 单发目标(DMS 绑定端点):全部设备发往同一端点,按来源区分
        QHostAddress peerAddress;
        quint16 peerPort = 0;
        QString serialPortName;    // Serial:独占端口名
        int samplePeriodMs = 100;  // 可被 SetParameter 修改
        quint32 seed = 1;
        QList<FaultSpec> faults;
    };

    explicit DeviceSimulator(Params params, QObject *parent = nullptr);

    bool startListening(QString *errorOut = nullptr);

    int index() const { return params_.index; }
    quint16 port() const { return params_.listenPort; }
    // SIM READY 行的端点描述:TCP/UDP 为端口数字,串口为端口名
    QString endpointDesc() const;
    quint64 framesSent() const { return framesSent_; }
    quint64 samplesSent() const { return samplesSent_; }

private:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();
    void onSerialReadyRead();
    void onUdpReadyRead();
    // TCP/串口共用:字节流 -> 帧解码 -> 日志/命令分发(UDP 不走此路)
    void processIncomingBytes(const QByteArray &bytes);
    void onTelemetryTick();
    void onHeartbeatTick();
    void onFaultWatchTick();
    void onMergeFlush();

    void handleCommandFrame(const dms::protocol::DecodedFrame &frame);
    dms::protocol::AckStatus validateAndApplyCommand(const dms::protocol::CommandMessage &cmd,
                                                     bool &ackThenAct);
    void sendAck(quint32 echoSeq, quint16 commandId, dms::protocol::AckStatus status);
    void sendAckNow(quint32 echoSeq, quint16 commandId, dms::protocol::AckStatus status);

    // 唯一的帧出口:fragment/merge/badCrc/数据报故障都拦在这里,收发路径不变
    void deliverFrame(const QByteArray &frame, bool isAck);
    void sendDatagram(const QByteArray &frame); // UDP 报文级故障后一报一帧
    void rawWrite(const QByteArray &chunk);     // 字节流写:TCP 客户端/串口
    void closeClient(); // 断信号->abort->延迟回收,顶替/故障拆除共用
    void closeSerial(); // disconnect 故障拆串口;窗口结束在巡检里重开
    bool reopenSerial(QString *errorOut = nullptr);

    bool faultActive(FaultMode mode) const;
    bool hasPeer() const; // TCP:连接在;UDP:已绑定;串口:端口开
    QString deviceId() const;
    dms::LogContext logCtx(dms::ErrorCode code = dms::ErrorCode::None) const;

    Params params_;
    QTcpServer server_;            // 仅 Tcp 形态监听
    QTcpSocket *client_ = nullptr; // 当前唯一客户端连接(Tcp)
    QUdpSocket *udp_ = nullptr;    // Udp 形态的固定源端口 socket
    QSerialPort *serial_ = nullptr; // Serial 形态的独占端口
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
    QByteArray reorderHold_;    // datagramReorder 故障的对调暂存报
    quint16 lastFaultMask_ = 0; // 上次巡检时各模式是否活跃(按 FaultMode 位;11 模式需 16 位)
};

} // namespace dms::sim
