#pragma once

#include <QList>
#include <QString>

#include "common/Logging.h"
#include "domain/DeviceSession.h"
#include "transport/TcpTransport.h"

namespace dms::app {

// 阈值告警规则(devices.json alarm_rules 项):
// S4 只做校验与承载,消费方是 S6 AlarmEngine(LLD §6)
struct AlarmRuleConfig {
    quint16 metricId = 0;
    double low = 0.0;
    double high = 0.0;
    double hysteresis = 0.0;
};

// 单台设备连接配置(devices.json 一项,校验通过后的快照)。
// transport 为 tcp/udp/serial 三者之一;S4 只有 TCP 会话实现,
// 启用的非 tcp 设备在加载期被拒绝(docs/15:不能悄悄改默认值)。
struct DeviceConnectionConfig {
    QString id;
    QString name;
    bool enabled = false;
    QString transport; // "tcp" | "udp" | "serial"
    // tcp 专用字段
    QString host;
    quint16 port = 0;
    QList<AlarmRuleConfig> alarmRules;
    // 原始对象紧凑文本(重新序列化,键序可能与源文件不同):
    // devices 表 config_json 镜像用,不掺默认值(docs/06 §1,S5 消费)
    QString rawJson;
};

// app.ini 全部节的解析结果,已按归属拆到各模块配置值对象;
// 未映射的节(database)整结构保留给 S5 消费
struct AppConfig {
    dms::transport::TcpTransportConfig transport; // connect_timeout_ms / max_tx_bytes
    dms::domain::SessionConfig session;           // [connection]/[protocol] 各超时
    int heartbeatIntervalMs = 1000;               // 校验合法,S6 起消费

    int refreshIntervalMs = 100; // [ui] GUI 合并显示周期
    int curvePoints = 600;       // [ui] 每指标曲线上限点数

    // [database](S5 消费,现在只校验与存储)
    struct Db {
        QString path = QStringLiteral("data/dms.db");
        int batchSamples = 500;
        int flushIntervalMs = 1000;
        int queueSamples = 50000;
        int eventQueueSize = 1000;
        int busyTimeoutMs = 1000;
        int shutdownFlushMs = 5000;
    } db;

    dms::common::Logger::Settings logging; // [logging]
};

// 一条配置问题:path 是 JSON 路径(devices[2].port)或 INI 键
// (connection/offline_timeout_ms),message 说明违反的规则
struct ConfigIssue {
    QString path;
    QString message;
};

// loadConfiguration 的结果:ok=false 时 issues 非空,app/devices 不含部分结果
struct LoadedConfig {
    bool ok = false;
    AppConfig app;
    QList<DeviceConnectionConfig> devices;
    QList<ConfigIssue> issues;
    bool firstRunInitialized = false; // 首启从模板复制过配置
};

// 从 runRoot/config/{devices.json,app.ini} 加载并全量校验(docs/15 §3/§4):
//   - 文件缺失时先尝试从 templateDir 复制(发布包模板,首启初始化);
//   - 所有字段一次性收集全部 issues,便于用户一轮修完,不改默认值凑合;
//   - 未知字段/键、类型错误、范围越界、跨字段约束都拒绝;
//   - 校验失败时保留磁盘原文件,不自动覆盖(docs/15 §3)。
LoadedConfig loadConfiguration(const QString &runRoot, const QString &templateDir);

} // namespace dms::app
