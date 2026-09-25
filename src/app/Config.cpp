#include "Config.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>

#include <cmath>
#include <optional>

namespace dms::app {
namespace {

// ---- issue 收集:所有校验路径共享,一轮凑齐全部问题再返回 ----
class Issues
{
public:
    void add(const QString &path, const QString &message)
    {
        list_.append(ConfigIssue{path, message});
    }
    bool empty() const { return list_.isEmpty(); }
    int size() const { return list_.size(); }
    const QList<ConfigIssue> &list() const { return list_; }

private:
    QList<ConfigIssue> list_;
};

// ASCII [A-Za-z0-9_-]:QChar::isLetter 对非拉丁字母也返回 true,必须按字节判
bool isValidDeviceId(const QString &id)
{
    if (id.isEmpty() || id.size() > 64)
        return false;
    for (const QChar &c : id) {
        const char l = c.toLatin1(); // 非 Latin-1 映射为 0,自然落入非法
        const bool ok = (l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z')
                        || (l >= '0' && l <= '9') || l == '_' || l == '-';
        if (!ok)
            return false;
    }
    return true;
}

// 合法 IP 或 DNS 名(简化校验:字母数字点短横线、无 "..";完整 RFC 域名不属 MVP)
bool isValidHost(const QString &host)
{
    if (host.isEmpty() || host.size() > 253)
        return false;
    QHostAddress addr;
    if (addr.setAddress(host))
        return true;
    if (host.startsWith('.') || host.endsWith('.') || host.contains(".."))
        return false;
    for (const QChar &c : host) {
        const char l = c.toLatin1();
        const bool ok = (l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z')
                        || (l >= '0' && l <= '9') || l == '.' || l == '-';
        if (!ok)
            return false;
    }
    return true;
}

bool isValidIpAddress(const QString &s) // UDP 字段:只接受字面 IP,不做 DNS 解析
{
    QHostAddress addr;
    return addr.setAddress(s);
}

// JSON 数值字段:必须是整数且落在 [min,max],否则记 issue
// (JSON 数字一律经 double,QJsonValue 没有独立整数类型)
bool jsonInt(const QJsonObject &o, const QString &key, double min, double max,
             const QString &path, qint64 *out, Issues *issues)
{
    const QJsonValue v = o.value(key);
    if (!v.isDouble()) {
        issues->add(path + "." + key, QStringLiteral("应为整数"));
        return false;
    }
    const double d = v.toDouble();
    if (d != std::floor(d) || d < min || d > max) {
        issues->add(path + "." + key,
                    QStringLiteral("应为 [%1, %2] 内的整数,实际 %3")
                        .arg(min).arg(max).arg(d));
        return false;
    }
    *out = qint64(d);
    return true;
}

bool jsonString(const QJsonObject &o, const QString &key, const QString &path,
                QString *out, Issues *issues)
{
    const QJsonValue v = o.value(key);
    if (!v.isString()) {
        issues->add(path + "." + key, QStringLiteral("应为字符串"));
        return false;
    }
    *out = v.toString();
    return true;
}

// 各 transport 的合法字段白名单:混用/未知字段拒绝并指明路径(docs/15 §3)
const QStringList &tcpKeys()
{
    static const QStringList keys = {QStringLiteral("id"), QStringLiteral("name"),
                                     QStringLiteral("enabled"), QStringLiteral("transport"),
                                     QStringLiteral("host"), QStringLiteral("port"),
                                     QStringLiteral("alarm_rules")};
    return keys;
}
const QStringList &udpKeys()
{
    static const QStringList keys = {QStringLiteral("id"), QStringLiteral("name"),
                                     QStringLiteral("enabled"), QStringLiteral("transport"),
                                     QStringLiteral("bind_address"), QStringLiteral("bind_port"),
                                     QStringLiteral("peer_address"), QStringLiteral("peer_port"),
                                     QStringLiteral("alarm_rules")};
    return keys;
}
const QStringList &serialKeys()
{
    static const QStringList keys = {QStringLiteral("id"), QStringLiteral("name"),
                                     QStringLiteral("enabled"), QStringLiteral("transport"),
                                     QStringLiteral("port_name"), QStringLiteral("baud_rate"),
                                     QStringLiteral("data_bits"), QStringLiteral("parity"),
                                     QStringLiteral("stop_bits"), QStringLiteral("flow_control"),
                                     QStringLiteral("alarm_rules")};
    return keys;
}

// 校验 alarm_rules(阈值语义属于 S6 AlarmEngine,这里只保证配置可用)
void parseAlarmRules(const QJsonObject &device, const QString &path,
                     QList<AlarmRuleConfig> *out, Issues *issues)
{
    static const QStringList allowed = {QStringLiteral("metric_id"), QStringLiteral("low"),
                                        QStringLiteral("high"), QStringLiteral("hysteresis")};
    // alarm_rules 可为空(docs/15 §3):缺失视同空数组,只有"写了但类型不对"才拒绝
    if (!device.contains(QStringLiteral("alarm_rules")))
        return;
    const QJsonValue v = device.value(QStringLiteral("alarm_rules"));
    if (!v.isArray()) {
        issues->add(path + ".alarm_rules", QStringLiteral("应为数组"));
        return;
    }
    QSet<quint16> seenMetrics;
    int index = 0;
    for (const QJsonValue &item : v.toArray()) {
        const QString rulePath = QStringLiteral("%1.alarm_rules[%2]").arg(path).arg(index++);
        if (!item.isObject()) {
            issues->add(rulePath, QStringLiteral("应为对象"));
            continue;
        }
        const QJsonObject rule = item.toObject();
        for (const QString &k : rule.keys()) {
            if (!allowed.contains(k))
                issues->add(rulePath + "." + k, QStringLiteral("未知字段"));
        }

        AlarmRuleConfig cfg;
        qint64 metricId = 0;
        bool ok = jsonInt(rule, QStringLiteral("metric_id"), 1, 4, rulePath, &metricId, issues)
                  && !seenMetrics.contains(quint16(metricId));
        if (ok) {
            seenMetrics.insert(quint16(metricId));
        } else if (metricId >= 1 && metricId <= 4) {
            issues->add(rulePath + ".metric_id", QStringLiteral("同设备内重复"));
            ok = false;
        }
        if (!ok)
            continue;
        cfg.metricId = quint16(metricId);

        for (const char *name : {"low", "high", "hysteresis"}) {
            const QJsonValue fv = rule.value(QLatin1String(name));
            if (!fv.isDouble() || !std::isfinite(fv.toDouble())) {
                issues->add(rulePath + "." + name, QStringLiteral("应为有限数"));
                ok = false;
            }
        }
        if (!ok)
            continue;
        cfg.low = rule.value(QStringLiteral("low")).toDouble();
        cfg.high = rule.value(QStringLiteral("high")).toDouble();
        cfg.hysteresis = rule.value(QStringLiteral("hysteresis")).toDouble();
        if (cfg.low >= cfg.high) {
            issues->add(rulePath, QStringLiteral("low 必须小于 high"));
            continue;
        }
        if (cfg.hysteresis < 0.0 || cfg.hysteresis > (cfg.high - cfg.low) / 2) {
            issues->add(rulePath + ".hysteresis",
                        QStringLiteral("应在 [0, (high-low)/2] 内,实际 %1")
                            .arg(cfg.hysteresis));
            continue;
        }
        out->append(cfg);
    }
}

// 单台设备项校验;有 issue 时 out 的内容不可信(调用方不会收录该设备)
void parseDevice(const QJsonObject &device, const QString &path,
                 DeviceConnectionConfig *out, Issues *issues)
{
    for (const QString &k : device.keys()) {
        // transport 值未知时按三种 transport 的并集检查,
        // 让"未知字段"与"transport 非法"一起报出来
        const bool known = tcpKeys().contains(k) || udpKeys().contains(k)
                           || serialKeys().contains(k);
        if (!known)
            issues->add(path + "." + k, QStringLiteral("未知字段"));
    }

    QString id, name, transport;
    if (!jsonString(device, QStringLiteral("id"), path, &id, issues))
        return;
    if (!isValidDeviceId(id)) {
        issues->add(path + ".id",
                    QStringLiteral("应为 1-64 个 ASCII 字母/数字/下划线/短横线"));
        return;
    }
    out->id = id;

    if (!jsonString(device, QStringLiteral("name"), path, &name, issues))
        return;
    if (name.isEmpty() || name.size() > 128) {
        issues->add(path + ".name", QStringLiteral("长度应为 1-128"));
        return;
    }
    out->name = name;

    if (!device.value(QStringLiteral("enabled")).isBool()) {
        issues->add(path + ".enabled", QStringLiteral("应为布尔值"));
        return;
    }
    out->enabled = device.value(QStringLiteral("enabled")).toBool();

    if (!jsonString(device, QStringLiteral("transport"), path, &transport, issues))
        return;
    if (transport != QStringLiteral("tcp") && transport != QStringLiteral("udp")
        && transport != QStringLiteral("serial")) {
        issues->add(path + ".transport", QStringLiteral("应为 tcp/udp/serial"));
        return;
    }
    out->transport = transport;

    // 其余字段按 transport 分派;udp/serial 的深度校验保证配置可用性,
    // 会话实现与字段消费是 S7 范围(S4 只建 tcp 会话)
    if (transport == QStringLiteral("tcp")) {
        for (const QString &k : device.keys()) {
            if ((udpKeys().contains(k) && !tcpKeys().contains(k)))
                issues->add(path + "." + k, QStringLiteral("tcp 设备不允许的 udp 字段"));
            if ((serialKeys().contains(k) && !tcpKeys().contains(k)))
                issues->add(path + "." + k, QStringLiteral("tcp 设备不允许的 serial 字段"));
        }
        QString host;
        if (jsonString(device, QStringLiteral("host"), path, &host, issues)) {
            if (!isValidHost(host))
                issues->add(path + ".host", QStringLiteral("应为合法 IP 或 DNS 名"));
            else
                out->host = host;
        }
        qint64 port = 0;
        if (jsonInt(device, QStringLiteral("port"), 1, 65535, path, &port, issues))
            out->port = quint16(port);
    } else if (transport == QStringLiteral("udp")) {
        if (out->enabled) {
            // S4 只有 TCP 会话:启用即显式拒绝,不悄悄跳过或降级(docs/15 §3)
            issues->add(path, QStringLiteral("S4 仅支持 tcp,启用的 udp 设备暂不可用"));
        }
        for (const QString &k : device.keys()) {
            if ((tcpKeys().contains(k) && !udpKeys().contains(k)))
                issues->add(path + "." + k, QStringLiteral("udp 设备不允许的 tcp 字段"));
            if ((serialKeys().contains(k) && !udpKeys().contains(k)))
                issues->add(path + "." + k, QStringLiteral("udp 设备不允许的 serial 字段"));
        }
        QString bind, peer;
        if (jsonString(device, QStringLiteral("bind_address"), path, &bind, issues)
            && !isValidIpAddress(bind))
            issues->add(path + ".bind_address", QStringLiteral("应为合法 IP"));
        if (jsonString(device, QStringLiteral("peer_address"), path, &peer, issues)) {
            if (!isValidIpAddress(peer))
                issues->add(path + ".peer_address", QStringLiteral("应为合法 IP"));
            else if (peer == QStringLiteral("0.0.0.0") || peer == QStringLiteral("::"))
                issues->add(path + ".peer_address", QStringLiteral("不允许通配地址"));
        }
        qint64 bindPort = 0, peerPort = 0;
        jsonInt(device, QStringLiteral("bind_port"), 1, 65535, path, &bindPort, issues);
        jsonInt(device, QStringLiteral("peer_port"), 1, 65535, path, &peerPort, issues);
    } else { // serial
        if (out->enabled)
            issues->add(path, QStringLiteral("S4 仅支持 tcp,启用的 serial 设备暂不可用"));
        for (const QString &k : device.keys()) {
            if ((tcpKeys().contains(k) && !serialKeys().contains(k)))
                issues->add(path + "." + k, QStringLiteral("serial 设备不允许的 tcp 字段"));
            if ((udpKeys().contains(k) && !serialKeys().contains(k)))
                issues->add(path + "." + k, QStringLiteral("serial 设备不允许的 udp 字段"));
        }
        QString portName;
        if (jsonString(device, QStringLiteral("port_name"), path, &portName, issues)
            && portName.isEmpty())
            issues->add(path + ".port_name", QStringLiteral("不允许为空"));
        static const QList<qint64> kBauds = {9600, 19200, 38400, 57600, 115200};
        qint64 baud = 0, dataBits = 0, stopBits = 0;
        if (jsonInt(device, QStringLiteral("baud_rate"), 9600, 115200, path, &baud, issues)
            && !kBauds.contains(baud)) {
            issues->add(path + ".baud_rate",
                        QStringLiteral("应为 9600/19200/38400/57600/115200 之一"));
        }
        jsonInt(device, QStringLiteral("data_bits"), 7, 8, path, &dataBits, issues);
        jsonInt(device, QStringLiteral("stop_bits"), 1, 2, path, &stopBits, issues);
        for (const char *field : {"parity", "flow_control"}) {
            QString value;
            if (jsonString(device, QLatin1String(field), path, &value, issues)) {
                static const QStringList kParity = {QStringLiteral("none"),
                                                    QStringLiteral("even"), QStringLiteral("odd")};
                static const QStringList kFlow = {QStringLiteral("none"),
                                                  QStringLiteral("hardware"),
                                                  QStringLiteral("software")};
                const bool ok = field[0] == 'p' ? kParity.contains(value) : kFlow.contains(value);
                if (!ok)
                    issues->add(path + "." + field, QStringLiteral("枚举值非法"));
            }
        }
    }

    parseAlarmRules(device, path, &out->alarmRules, issues);
}

void parseDevicesJson(const QByteArray &bytes, QList<DeviceConnectionConfig> *devices,
                      Issues *issues)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        issues->add(QStringLiteral("devices.json"),
                    parseError.error == QJsonParseError::NoError
                        ? QStringLiteral("应为 JSON 对象")
                        : QStringLiteral("JSON 语法错误:%1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject root = doc.object();
    for (const QString &k : root.keys()) {
        if (k != QStringLiteral("schema_version") && k != QStringLiteral("devices"))
            issues->add(QStringLiteral("devices.") + k, QStringLiteral("未知字段"));
    }

    qint64 schemaVersion = 0;
    if (!jsonInt(root, QStringLiteral("schema_version"), 1, 1, QStringLiteral("devices"),
                 &schemaVersion, issues))
        return; // 未知 schema 不猜字段含义,后续校验无意义(docs/15 §3)
    if (schemaVersion != 1) {
        issues->add(QStringLiteral("devices.schema_version"),
                    QStringLiteral("不支持的版本 %1").arg(schemaVersion));
        return;
    }

    const QJsonValue devs = root.value(QStringLiteral("devices"));
    if (!devs.isArray()) {
        issues->add(QStringLiteral("devices.devices"), QStringLiteral("应为数组"));
        return;
    }

    // 跨设备唯一性:id、tcp 端点(docs/15 §3;udp 四元组/串口名的占用检查随 S7 生效)
    QSet<QString> ids, tcpEndpoints;
    int index = 0;
    for (const QJsonValue &item : devs.toArray()) {
        const QString path = QStringLiteral("devices[%1]").arg(index++);
        if (!item.isObject()) {
            issues->add(path, QStringLiteral("应为对象"));
            continue;
        }
        DeviceConnectionConfig cfg;
        const int issuesBefore = issues->size();
        parseDevice(item.toObject(), path, &cfg, issues);
        if (issues->size() != issuesBefore)
            continue; // 该设备有问题:不收录,也不参与唯一性检查(半成品值会误报)
        if (!ids.contains(cfg.id)) {
            ids.insert(cfg.id);
        } else {
            issues->add(path + ".id", QStringLiteral("设备 id 重复:%1").arg(cfg.id));
            continue;
        }
        if (cfg.transport == QStringLiteral("tcp")) {
            const QString endpoint =
                cfg.host + QStringLiteral(":") + QString::number(cfg.port);
            if (!tcpEndpoints.contains(endpoint)) {
                tcpEndpoints.insert(endpoint);
            } else {
                issues->add(path, QStringLiteral("host/port 已被其他设备占用:%1").arg(endpoint));
                continue;
            }
        }
        devices->append(cfg);
    }
}

// ---- app.ini ----

// 整型键:缺失取默认;类型错/越界记 issue(QSettings 的 INI 值都是字符串,必须带 ok 判)
qint64 iniInt(const QSettings &s, const QString &key, qint64 minV, qint64 maxV,
              qint64 fallback, Issues *issues)
{
    if (!s.contains(key))
        return fallback;
    const QString raw = s.value(key).toString().trimmed();
    bool ok = false;
    const qint64 v = raw.toLongLong(&ok);
    if (!ok) {
        issues->add(key, QStringLiteral("应为整数,实际 \"%1\"").arg(raw));
        return fallback;
    }
    if (v < minV || v > maxV) {
        issues->add(key, QStringLiteral("应在 [%1, %2] 内,实际 %3").arg(minV).arg(maxV).arg(v));
        return fallback;
    }
    return v;
}

// app.ini 全部合法键(docs/15 §4;QSettings 对 INI 键做大小写折叠,比较前同样折叠)
QStringList knownIniKeys()
{
    return {
        QStringLiteral("app/schema_version"),
        QStringLiteral("connection/connect_timeout_ms"),
        QStringLiteral("connection/heartbeat_interval_ms"),
        QStringLiteral("connection/offline_timeout_ms"),
        QStringLiteral("connection/health_check_ms"),
        QStringLiteral("connection/stale_timeout_ms"),
        QStringLiteral("connection/command_timeout_ms"),
        QStringLiteral("connection/reconnect_max_ms"),
        QStringLiteral("protocol/assembly_timeout_ms"),
        QStringLiteral("protocol/max_buffer_bytes"),
        QStringLiteral("protocol/max_tx_bytes"),
        QStringLiteral("ui/refresh_interval_ms"),
        QStringLiteral("ui/curve_points"),
        QStringLiteral("database/path"),
        QStringLiteral("database/batch_samples"),
        QStringLiteral("database/flush_interval_ms"),
        QStringLiteral("database/queue_samples"),
        QStringLiteral("database/event_queue_size"),
        QStringLiteral("database/busy_timeout_ms"),
        QStringLiteral("database/shutdown_flush_ms"),
        QStringLiteral("logging/level"),
        QStringLiteral("logging/raw_frames"),
        QStringLiteral("logging/max_file_bytes"),
        QStringLiteral("logging/max_files"),
    };
}

std::optional<dms::common::LogLevel> parseLogLevel(const QString &s)
{
    if (s == QStringLiteral("DEBUG")) return dms::common::LogLevel::Debug;
    if (s == QStringLiteral("INFO"))  return dms::common::LogLevel::Info;
    if (s == QStringLiteral("WARN"))  return dms::common::LogLevel::Warn;
    if (s == QStringLiteral("ERROR")) return dms::common::LogLevel::Error;
    return std::nullopt;
}

// database path 只允许运行根下的相对路径(docs/15 §4)
void validateDbPath(const QString &path, Issues *issues)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path)) {
        issues->add(QStringLiteral("database/path"), QStringLiteral("必须是相对路径"));
        return;
    }
    const QStringList parts = path.split('/', Qt::KeepEmptyParts);
    if (parts.contains(QStringLiteral("..")) || path.contains('\\'))
        issues->add(QStringLiteral("database/path"),
                    QStringLiteral("不允许包含 .. 或反斜杠:%1").arg(path));
}

void parseAppIni(const QString &iniPath, AppConfig *app, Issues *issues)
{
    // 单字段问题先记在本地:跨字段约束只在 INI 自身字段全部合法时执行,
    // 否则拿坏值/回退值做交叉检查只会产生二次噪音
    Issues local;
    QSettings ini(iniPath, QSettings::IniFormat);

    QSet<QString> knownSet;
    for (const QString &k : knownIniKeys())
        knownSet.insert(k.toLower());
    for (const QString &key : ini.allKeys()) {
        if (!knownSet.contains(key.toLower()))
            local.add(key, QStringLiteral("未知配置键"));
    }

    // [app]
    if (!ini.contains(QStringLiteral("app/schema_version"))) {
        local.add(QStringLiteral("app/schema_version"), QStringLiteral("缺失必填键"));
    } else {
        const QString raw = ini.value(QStringLiteral("app/schema_version")).toString().trimmed();
        bool ok = false;
        const qint64 schemaVersion = raw.toLongLong(&ok);
        if (!ok || schemaVersion != 1)
            local.add(QStringLiteral("app/schema_version"),
                      QStringLiteral("必须为 1,实际 \"%1\"").arg(raw));
    }

    // [connection] / [protocol] → SessionConfig + TcpTransportConfig
    app->transport.connectTimeoutMs = int(iniInt(
        ini, QStringLiteral("connection/connect_timeout_ms"), 100, 3000,
        app->transport.connectTimeoutMs, &local));
    app->heartbeatIntervalMs = int(iniInt(
        ini, QStringLiteral("connection/heartbeat_interval_ms"), 100, 1000,
        app->heartbeatIntervalMs, &local));
    app->session.offlineTimeoutMs = int(iniInt(
        ini, QStringLiteral("connection/offline_timeout_ms"), 300, 4500,
        app->session.offlineTimeoutMs, &local));
    app->session.pollIntervalMs = int(iniInt(
        ini, QStringLiteral("connection/health_check_ms"), 10, 100,
        app->session.pollIntervalMs, &local));
    app->session.metricTimeoutMs = int(iniInt(
        ini, QStringLiteral("connection/stale_timeout_ms"), 1000, 4500,
        app->session.metricTimeoutMs, &local));
    app->session.commandAckTimeoutMs = int(iniInt(
        ini, QStringLiteral("connection/command_timeout_ms"), 100, 2000,
        app->session.commandAckTimeoutMs, &local));
    app->session.reconnectMaxMs = int(iniInt(
        ini, QStringLiteral("connection/reconnect_max_ms"), 1000, 10000,
        app->session.reconnectMaxMs, &local));
    app->session.frameAssemblyTimeoutMs = int(iniInt(
        ini, QStringLiteral("protocol/assembly_timeout_ms"), 100, 2000,
        app->session.frameAssemblyTimeoutMs, &local));
    app->session.decoderMaxBufferBytes = int(iniInt(
        ini, QStringLiteral("protocol/max_buffer_bytes"), 4118, 1048576,
        app->session.decoderMaxBufferBytes, &local));
    app->transport.maxTxBytes = int(iniInt(
        ini, QStringLiteral("protocol/max_tx_bytes"), 4118, 1048576,
        app->transport.maxTxBytes, &local));

    // [ui]
    app->refreshIntervalMs = int(iniInt(
        ini, QStringLiteral("ui/refresh_interval_ms"), 100, 200,
        app->refreshIntervalMs, &local));
    app->curvePoints = int(iniInt(
        ini, QStringLiteral("ui/curve_points"), 10, 6000, app->curvePoints, &local));

    // [database](S5 消费,现在只校验与存储)
    if (ini.contains(QStringLiteral("database/path"))) {
        const QString raw = ini.value(QStringLiteral("database/path")).toString();
        app->db.path = raw;
        validateDbPath(raw, &local);
    }
    app->db.batchSamples = int(iniInt(
        ini, QStringLiteral("database/batch_samples"), 1, 5000, app->db.batchSamples, &local));
    app->db.flushIntervalMs = int(iniInt(
        ini, QStringLiteral("database/flush_interval_ms"), 100, 5000,
        app->db.flushIntervalMs, &local));
    app->db.queueSamples = int(iniInt(
        ini, QStringLiteral("database/queue_samples"), 1, 500000,
        app->db.queueSamples, &local));
    app->db.eventQueueSize = int(iniInt(
        ini, QStringLiteral("database/event_queue_size"), 10, 10000,
        app->db.eventQueueSize, &local));
    app->db.busyTimeoutMs = int(iniInt(
        ini, QStringLiteral("database/busy_timeout_ms"), 100, 1000,
        app->db.busyTimeoutMs, &local));
    app->db.shutdownFlushMs = int(iniInt(
        ini, QStringLiteral("database/shutdown_flush_ms"), 1000, 10000,
        app->db.shutdownFlushMs, &local));

    // [logging]
    if (ini.contains(QStringLiteral("logging/level"))) {
        const QString raw = ini.value(QStringLiteral("logging/level")).toString().trimmed();
        const auto level = parseLogLevel(raw);
        if (level)
            app->logging.level = *level;
        else
            local.add(QStringLiteral("logging/level"),
                      QStringLiteral("应为 DEBUG/INFO/WARN/ERROR,实际 \"%1\"").arg(raw));
    }
    if (ini.contains(QStringLiteral("logging/raw_frames"))) {
        const QString raw = ini.value(QStringLiteral("logging/raw_frames")).toString().trimmed();
        if (raw == QStringLiteral("true"))
            app->logging.rawFrames = true;
        else if (raw == QStringLiteral("false"))
            app->logging.rawFrames = false;
        else
            local.add(QStringLiteral("logging/raw_frames"),
                      QStringLiteral("应为 true/false,实际 \"%1\"").arg(raw));
    }
    app->logging.maxFileBytes = iniInt(
        ini, QStringLiteral("logging/max_file_bytes"), 1024 * 1024, 100 * 1024 * 1024,
        app->logging.maxFileBytes, &local);
    app->logging.maxFiles = int(iniInt(
        ini, QStringLiteral("logging/max_files"), 1, 20, app->logging.maxFiles, &local));

    if (local.empty()) {
        const int hb = app->heartbeatIntervalMs;
        if (app->session.offlineTimeoutMs < 3 * hb)
            local.add(QStringLiteral("connection/offline_timeout_ms"),
                      QStringLiteral("应不小于 3×heartbeat_interval_ms(%1)").arg(3 * hb));
        if (app->session.offlineTimeoutMs + app->session.pollIntervalMs > 4600)
            local.add(QStringLiteral("connection/health_check_ms"),
                      QStringLiteral("offline_timeout_ms+health_check_ms 应不超过 4600"));
        if (app->session.metricTimeoutMs > app->session.offlineTimeoutMs)
            local.add(QStringLiteral("connection/stale_timeout_ms"),
                      QStringLiteral("应不大于 offline_timeout_ms"));
        if (app->db.queueSamples < app->db.batchSamples)
            local.add(QStringLiteral("database/queue_samples"),
                      QStringLiteral("应不小于 batch_samples(%1)").arg(app->db.batchSamples));
    }

    for (const ConfigIssue &issue : local.list())
        issues->add(issue.path, issue.message);
}

// 首启初始化:任一配置缺失时从模板目录补齐,已存在的不覆盖(docs/15 §1)
bool copyFromTemplate(const QString &templateDir, const QString &configDir, Issues *issues)
{
    bool copied = false;
    for (const char *name : {"devices.json", "app.ini"}) {
        const QString target = configDir + QLatin1Char('/') + QLatin1String(name);
        if (QFile::exists(target))
            continue;
        const QString source = templateDir + QLatin1Char('/') + QLatin1String(name);
        if (!QFile::exists(source)) {
            issues->add(QStringLiteral("config/") + QLatin1String(name),
                        QStringLiteral("运行目录与模板均缺少该文件"));
            continue;
        }
        if (!QFile::copy(source, target)) {
            issues->add(QStringLiteral("config/") + QLatin1String(name),
                        QStringLiteral("从模板复制失败"));
        } else {
            copied = true;
        }
    }
    return copied;
}

QByteArray readFileOrIssue(const QString &path, const QString &displayPath, Issues *issues)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        issues->add(displayPath, QStringLiteral("无法读取:%1").arg(file.errorString()));
        return {};
    }
    return file.readAll();
}

} // namespace

LoadedConfig loadConfiguration(const QString &runRoot, const QString &templateDir)
{
    LoadedConfig result;
    Issues issues;

    const QString configDir = runRoot + QStringLiteral("/config");
    const QString devicesPath = configDir + QStringLiteral("/devices.json");
    const QString iniPath = configDir + QStringLiteral("/app.ini");

    if (!QFile::exists(devicesPath) || !QFile::exists(iniPath)) {
        QDir().mkpath(configDir);
        result.firstRunInitialized = copyFromTemplate(templateDir, configDir, &issues);
    }

    if (QFile::exists(devicesPath)) {
        parseDevicesJson(readFileOrIssue(devicesPath, QStringLiteral("devices.json"), &issues),
                         &result.devices, &issues);
    }

    if (QFile::exists(iniPath)) {
        parseAppIni(iniPath, &result.app, &issues);
    }

    result.ok = issues.empty();
    result.issues = issues.list();
    return result;
}

} // namespace dms::app
