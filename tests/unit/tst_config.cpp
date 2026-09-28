#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "app/Config.h"

// 配置装配层测试(docs/15 §3/§4):
// devices.json 与 app.ini 的全量校验、首启模板初始化、默认值与映射
using namespace dms::app;

namespace {

// 有效 devices.json:tcp 启用 + udp/serial 禁用(与 docs/15 §2 模板同构)
const char *kValidDevices = R"JSON({
  "schema_version": 1,
  "devices": [
    {"id": "sim-tcp-01", "name": "TCP 模拟设备 01", "enabled": true, "transport": "tcp",
     "host": "127.0.0.1", "port": 9001,
     "alarm_rules": [{"metric_id": 1, "low": 0.0, "high": 80.0, "hysteresis": 2.0}]},
    {"id": "sim-udp-01", "name": "UDP 模拟设备 01", "enabled": false, "transport": "udp",
     "bind_address": "127.0.0.1", "bind_port": 9100,
     "peer_address": "127.0.0.1", "peer_port": 9101, "alarm_rules": []},
    {"id": "sim-serial-01", "name": "串口模拟设备 01", "enabled": false, "transport": "serial",
     "port_name": "COM3", "baud_rate": 115200, "data_bits": 8, "parity": "none",
     "stop_bits": 1, "flow_control": "none", "alarm_rules": []}
  ]
})JSON";

// 完整合法 app.ini(docs/15 §4 默认值)
const char *kValidIni = R"INI([app]
schema_version=1

[connection]
connect_timeout_ms=3000
heartbeat_interval_ms=1000
offline_timeout_ms=4000
health_check_ms=100
stale_timeout_ms=3000
command_timeout_ms=2000
reconnect_max_ms=10000

[protocol]
assembly_timeout_ms=2000
max_buffer_bytes=65536
max_tx_bytes=65536

[ui]
refresh_interval_ms=100
curve_points=600
)INI";

void writeUtf8(const QTemporaryDir &dir, const QString &relPath, const QByteArray &bytes)
{
    const QString path = dir.filePath(relPath);
    QDir(dir.path()).mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(f.errorString()));
    f.write(bytes);
}

// 单台 tcp 设备 JSON 片段;extraFields 以 ",\"key\":value" 形式注入
QString tcpDev(const QString &id = QStringLiteral("d1"),
               const QString &host = QStringLiteral("127.0.0.1"),
               const QString &port = QStringLiteral("9001"),
               const QString &extraFields = QString())
{
    return QStringLiteral(
               "{\"id\":\"%1\",\"name\":\"D1\",\"enabled\":true,\"transport\":\"tcp\","
               "\"host\":\"%2\",\"port\":%3%4}")
        .arg(id, host, port, extraFields);
}

QString devicesJson(const QString &body)
{
    return QStringLiteral("{\"schema_version\":1,\"devices\":[%1]}").arg(body);
}

// 是否存在指向 path 的 issue(错误定位必须能指到字段,便于用户一轮修完)
bool hasIssueFor(const QList<ConfigIssue> &issues, const QString &pathSubstr,
                 const QString &msgSubstr = QString())
{
    for (const ConfigIssue &i : issues) {
        if (!i.path.contains(pathSubstr, Qt::CaseInsensitive))
            continue;
        if (msgSubstr.isEmpty() || i.message.contains(msgSubstr, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace

class TestConfig : public QObject
{
    Q_OBJECT

private slots:
    // ---- 首启与加载 ----
    void templateFirstRun();
    void missingFilesNoTemplate();
    void existingConfigNotOverwritten();
    void templatePartial();

    // ---- devices.json ----
    void badJsonSyntax();
    void schemaVersionRejected();
    void deviceIdInvalid();
    void deviceIdDuplicate();
    void badDevicesNoSpuriousDuplicates();
    void unknownField();
    void mixedTransportFields();
    void portRange();
    void duplicateEndpoint();
    void udpEnabledValidatesAndStores();
    void udpTupleUniqueness();
    void serialPortOccupancy();
    void serialEnabledValidatesAndStores();
    void udpDisabledWithBadAddress();
    void serialBadEnum();
    void alarmRuleMetricRange();
    void alarmRuleDuplicateMetric();
    void alarmRuleLowHigh();
    void alarmRuleHysteresis();

    // ---- app.ini ----
    void iniUnknownKey();
    void iniTypeError();
    void iniRangeViolation();
    void iniDefaults();
    void iniMapping();
    void iniKeyCaseInsensitive();
    void iniCrossFieldOfflineHeartbeat();
    void iniCrossFieldStaleOffline();
    void iniCrossFieldQueueBatch();
    void iniDbPathAbsolute();
    void iniLogLevelInvalid();

    // ---- 汇总 ----
    void allIssuesCollected();
};

// ---- 首启与加载 ----

void TestConfig::templateFirstRun()
{
    QTemporaryDir runRoot, templateDir;
    writeUtf8(templateDir, "config/devices.json", kValidDevices);
    writeUtf8(templateDir, "config/app.ini", kValidIni);

    const LoadedConfig loaded = loadConfiguration(runRoot.path(), templateDir.filePath("config"));
    QVERIFY(loaded.ok);
    QVERIFY(loaded.issues.isEmpty());
    QVERIFY(loaded.firstRunInitialized);
    QCOMPARE(loaded.devices.size(), 3);

    const DeviceConnectionConfig &tcp = loaded.devices[0];
    QCOMPARE(tcp.id, QStringLiteral("sim-tcp-01"));
    QCOMPARE(tcp.transport, QStringLiteral("tcp"));
    QVERIFY(tcp.enabled);
    QCOMPARE(tcp.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(tcp.port, quint16(9001));
    QCOMPARE(tcp.alarmRules.size(), 1);
    QCOMPARE(tcp.alarmRules[0].metricId, quint16(1));
    QCOMPARE(tcp.alarmRules[0].high, 80.0);

    // rawJson:原始对象紧凑文本(devices 表 config_json 镜像用,S5)
    const QJsonObject raw = QJsonDocument::fromJson(tcp.rawJson.toUtf8()).object();
    QCOMPARE(raw.value(QStringLiteral("id")).toString(), QStringLiteral("sim-tcp-01"));
    QCOMPARE(raw.value(QStringLiteral("port")).toInt(), 9001);
    QVERIFY(!tcp.rawJson.contains(QLatin1Char('\n'))); // 紧凑单行

    // 第二次加载:文件已存在,不再视为首启
    const LoadedConfig again = loadConfiguration(runRoot.path(), templateDir.filePath("config"));
    QVERIFY(again.ok);
    QVERIFY(!again.firstRunInitialized);
}

void TestConfig::missingFilesNoTemplate()
{
    QTemporaryDir runRoot, templateDir;
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), templateDir.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices.json", "缺少"));
    QVERIFY(hasIssueFor(loaded.issues, "app.ini", "缺少"));
}

void TestConfig::existingConfigNotOverwritten()
{
    QTemporaryDir runRoot, templateDir;
    writeUtf8(templateDir, "config/devices.json", kValidDevices);
    writeUtf8(templateDir, "config/app.ini", kValidIni);
    // 用户已有自定义 devices.json:模板不得覆盖(docs/15 §1);
    // app.ini 缺失由模板补齐是合法的首启初始化
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("custom-dev"))).toUtf8());

    const LoadedConfig loaded = loadConfiguration(runRoot.path(), templateDir.filePath("config"));
    QVERIFY(loaded.ok);
    QCOMPARE(loaded.devices.size(), 1);
    QCOMPARE(loaded.devices[0].id, QStringLiteral("custom-dev"));
    QVERIFY(QFile::exists(runRoot.filePath("config/app.ini")));
}

void TestConfig::templatePartial()
{
    QTemporaryDir runRoot, templateDir;
    writeUtf8(templateDir, "config/app.ini", kValidIni); // 模板只带 app.ini
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), templateDir.filePath("config"));
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices.json", "缺少"));
    QVERIFY(!hasIssueFor(loaded.issues, "app.ini", "缺少"));
}

// ---- devices.json ----

void TestConfig::badJsonSyntax()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", QByteArray("{ not json"));
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices.json", "语法错误"));
}

void TestConfig::schemaVersionRejected()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json",
              QByteArray("{\"schema_version\":2,\"devices\":[]}"));
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "schema_version"));
}

void TestConfig::deviceIdInvalid()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("bad id!"))).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[0].id"));
}

void TestConfig::deviceIdDuplicate()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev() + "," + tcpDev()).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "id", "重复"));
}

// 两台设备各带一个字段错误:应只报各自的真实问题,
// 不能因为半成品默认值再冒出"id 重复"之类噪音
void TestConfig::badDevicesNoSpuriousDuplicates()
{
    QTemporaryDir runRoot;
    const QString badName = QStringLiteral(
        "{\"id\":\"a\",\"transport\":\"tcp\",\"host\":\"127.0.0.1\",\"port\":9001}");
    writeUtf8(runRoot, "config/devices.json", devicesJson(badName + "," + badName).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    // 每台设备恰好一条 name 缺失,无重复 id 噪音
    int nameIssues = 0;
    for (const ConfigIssue &i : loaded.issues) {
        if (i.path.contains(".name"))
            nameIssues++;
        else
            QFAIL(qPrintable(QStringLiteral("意外 issue:%1 %2").arg(i.path, i.message)));
    }
    QCOMPARE(nameIssues, 2);
}

void TestConfig::unknownField()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("9001"),
                                 QStringLiteral(",\"foo\":1")))
                  .toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[0].foo", "未知字段"));
}

void TestConfig::mixedTransportFields()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("9001"),
                                 QStringLiteral(",\"bind_address\":\"127.0.0.1\"")))
                  .toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[0].bind_address", "不允许"));
}

void TestConfig::portRange()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("65536"))).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    QVERIFY(!loadConfiguration(runRoot.path(), runRoot.path()).ok);

    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("9001.5"))).toUtf8());
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[0].port"));
}

void TestConfig::duplicateEndpoint()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1")) + ","
                          + tcpDev(QStringLiteral("d2")))
                  .toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[1]", "占用"));
}

// S7 起 udp 设备可启用;四字段进入快照供装配建会话
void TestConfig::udpEnabledValidatesAndStores()
{
    QTemporaryDir runRoot;
    const QString udp = QStringLiteral(
        "{\"id\":\"u1\",\"name\":\"U\",\"enabled\":true,\"transport\":\"udp\","
        "\"bind_address\":\"127.0.0.1\",\"bind_port\":9100,"
        "\"peer_address\":\"127.0.0.1\",\"peer_port\":9101,\"alarm_rules\":[]}");
    writeUtf8(runRoot, "config/devices.json", devicesJson(udp).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(loaded.ok);
    QCOMPARE(loaded.devices.size(), 1);
    const auto &dev = loaded.devices.first();
    QVERIFY(dev.enabled);
    QCOMPARE(dev.transport, QStringLiteral("udp"));
    QCOMPARE(dev.bindAddress, QStringLiteral("127.0.0.1"));
    QCOMPARE(dev.bindPort, quint16(9100));
    QCOMPARE(dev.peerAddress, QStringLiteral("127.0.0.1"));
    QCOMPARE(dev.peerPort, quint16(9101));
}

// udp 四元组唯一(docs/15 §3);同 bind 不同 peer 合法(共享 socket 按来源路由)
void TestConfig::udpTupleUniqueness()
{
    QTemporaryDir runRoot;
    const auto mk = [](const char *id, const char *peerPort, const char *bindPort) {
        return QStringLiteral(
            "{\"id\":\"%1\",\"name\":\"U\",\"enabled\":true,\"transport\":\"udp\","
            "\"bind_address\":\"127.0.0.1\",\"bind_port\":%2,"
            "\"peer_address\":\"127.0.0.1\",\"peer_port\":%3,\"alarm_rules\":[]}")
            .arg(QLatin1String(id), QLatin1String(bindPort), QLatin1String(peerPort));
    };
    // 同 bind 不同 peer:合法
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(mk("u1", "9201", "9100") + "," + mk("u2", "9202", "9100")).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    QVERIFY(loadConfiguration(runRoot.path(), runRoot.path()).ok);

    // 完整四元组重复:拒绝
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(mk("u1", "9201", "9100") + "," + mk("u2", "9201", "9100")).toUtf8());
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[1]", "四元组"));
}

// 启用设备不能重复占用同一串口;停用设备同名不冲突(docs/15 §3)
void TestConfig::serialPortOccupancy()
{
    QTemporaryDir runRoot;
    const auto mk = [](const char *id, bool enabled) {
        return QStringLiteral(
            "{\"id\":\"%1\",\"name\":\"S\",\"enabled\":%2,\"transport\":\"serial\","
            "\"port_name\":\"COM3\",\"baud_rate\":115200,\"data_bits\":8,"
            "\"parity\":\"none\",\"stop_bits\":1,\"flow_control\":\"none\","
            "\"alarm_rules\":[]}")
            .arg(QLatin1String(id), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    };
    // 两个都停用:合法(不打开串口)
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(mk("s1", false) + "," + mk("s2", false)).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    QVERIFY(loadConfiguration(runRoot.path(), runRoot.path()).ok);

    // 都启用:第二个被拒
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(mk("s1", true) + "," + mk("s2", true)).toUtf8());
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[1]", "串口"));
}

// serial 全字段进入快照供装配映射(QSerialPort 枚举由 main 装配转换)
void TestConfig::serialEnabledValidatesAndStores()
{
    QTemporaryDir runRoot;
    const QString serial = QStringLiteral(
        "{\"id\":\"s1\",\"name\":\"S\",\"enabled\":true,\"transport\":\"serial\","
        "\"port_name\":\"COM3\",\"baud_rate\":57600,\"data_bits\":7,\"parity\":\"even\","
        "\"stop_bits\":2,\"flow_control\":\"hardware\",\"alarm_rules\":[]}");
    writeUtf8(runRoot, "config/devices.json", devicesJson(serial).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(loaded.ok);
    const auto &dev = loaded.devices.first();
    QCOMPARE(dev.portName, QStringLiteral("COM3"));
    QCOMPARE(dev.baudRate, qint32(57600));
    QCOMPARE(dev.dataBits, 7);
    QCOMPARE(dev.parity, QStringLiteral("even"));
    QCOMPARE(dev.stopBits, 2);
    QCOMPARE(dev.flowControl, QStringLiteral("hardware"));
}

void TestConfig::udpDisabledWithBadAddress()
{
    QTemporaryDir runRoot;
    const QString udp = QStringLiteral(
        "{\"id\":\"u1\",\"name\":\"U\",\"enabled\":false,\"transport\":\"udp\","
        "\"bind_address\":\"not-an-ip\",\"bind_port\":9100,"
        "\"peer_address\":\"127.0.0.1\",\"peer_port\":9101,\"alarm_rules\":[]}");
    writeUtf8(runRoot, "config/devices.json", devicesJson(udp).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[0].bind_address"));
}

void TestConfig::serialBadEnum()
{
    QTemporaryDir runRoot;
    const QString serial = QStringLiteral(
        "{\"id\":\"s1\",\"name\":\"S\",\"enabled\":false,\"transport\":\"serial\","
        "\"port_name\":\"COM3\",\"baud_rate\":12345,\"data_bits\":8,\"parity\":\"none\","
        "\"stop_bits\":1,\"flow_control\":\"none\",\"alarm_rules\":[]}");
    writeUtf8(runRoot, "config/devices.json", devicesJson(serial).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "devices[0].baud_rate"));
}

void TestConfig::alarmRuleMetricRange()
{
    QTemporaryDir runRoot;
    const QString rule = QStringLiteral(
        ",\"alarm_rules\":[{\"metric_id\":5,\"low\":0,\"high\":10,\"hysteresis\":0}]");
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("9001"), rule)).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "alarm_rules[0].metric_id"));
}

void TestConfig::alarmRuleDuplicateMetric()
{
    QTemporaryDir runRoot;
    const QString rule = QStringLiteral(
        ",\"alarm_rules\":[{\"metric_id\":1,\"low\":0,\"high\":10,\"hysteresis\":0},"
        "{\"metric_id\":1,\"low\":2,\"high\":8,\"hysteresis\":0}]");
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("9001"), rule)).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "alarm_rules[1].metric_id", "重复"));
}

void TestConfig::alarmRuleLowHigh()
{
    QTemporaryDir runRoot;
    const QString rule = QStringLiteral(
        ",\"alarm_rules\":[{\"metric_id\":1,\"low\":10,\"high\":10,\"hysteresis\":0}]");
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("9001"), rule)).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "alarm_rules[0]", "low"));
}

void TestConfig::alarmRuleHysteresis()
{
    QTemporaryDir runRoot;
    // (high-low)/2 = 5,hysteresis=6 越界
    const QString rule = QStringLiteral(
        ",\"alarm_rules\":[{\"metric_id\":1,\"low\":0,\"high\":10,\"hysteresis\":6}]");
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("d1"), QStringLiteral("127.0.0.1"), QStringLiteral("9001"), rule)).toUtf8());
    writeUtf8(runRoot, "config/app.ini", kValidIni);
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "alarm_rules[0].hysteresis"));
}

// ---- app.ini ----

void TestConfig::iniUnknownKey()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n[foo]\nbar=1\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "foo/bar", "未知"));
}

void TestConfig::iniTypeError()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n[connection]\noffline_timeout_ms=abc\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "offline_timeout_ms", "整数"));
}

void TestConfig::iniRangeViolation()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n[connection]\nconnect_timeout_ms=5000\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "connect_timeout_ms"));
}

void TestConfig::iniDefaults()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini", QByteArray("[app]\nschema_version=1\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(loaded.ok);

    const AppConfig &a = loaded.app;
    QCOMPARE(a.transport.connectTimeoutMs, 3000);
    QCOMPARE(a.transport.maxTxBytes, 65536);
    QCOMPARE(a.heartbeatIntervalMs, 1000);
    QCOMPARE(a.session.pollIntervalMs, 100);
    QCOMPARE(a.session.offlineTimeoutMs, 4000);
    QCOMPARE(a.session.metricTimeoutMs, 3000);
    QCOMPARE(a.session.commandAckTimeoutMs, 2000);
    QCOMPARE(a.session.reconnectMaxMs, 10000);
    QCOMPARE(a.session.frameAssemblyTimeoutMs, 2000);
    QCOMPARE(a.session.decoderMaxBufferBytes, 65536);
    QCOMPARE(a.refreshIntervalMs, 100);
    QCOMPARE(a.curvePoints, 600);
    QCOMPARE(a.db.batchSamples, 500);
    QCOMPARE(a.db.queueSamples, 50000);
    QCOMPARE(a.logging.level, dms::common::LogLevel::Info);
}

void TestConfig::iniMapping()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n"
                         "[connection]\nconnect_timeout_ms=1500\nhealth_check_ms=50\n"
                         "[protocol]\nmax_buffer_bytes=8192\nmax_tx_bytes=8192\n"
                         "[ui]\nrefresh_interval_ms=150\ncurve_points=300\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(loaded.ok);

    const AppConfig &a = loaded.app;
    QCOMPARE(a.transport.connectTimeoutMs, 1500);
    QCOMPARE(a.transport.maxTxBytes, 8192);
    QCOMPARE(a.session.pollIntervalMs, 50);
    QCOMPARE(a.session.decoderMaxBufferBytes, 8192);
    QCOMPARE(a.refreshIntervalMs, 150);
    QCOMPARE(a.curvePoints, 300);
}

// QSettings 对 INI 键做大小写折叠,加载器必须同样折叠后才比较白名单
void TestConfig::iniKeyCaseInsensitive()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n"
                         "[CONNECTION]\nCONNECT_TIMEOUT_MS=1500\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(loaded.ok);
    QCOMPARE(loaded.app.transport.connectTimeoutMs, 1500);
}

void TestConfig::iniCrossFieldOfflineHeartbeat()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    // 2500 < 3×1000
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n"
                         "[connection]\noffline_timeout_ms=2500\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "offline_timeout_ms", "heartbeat"));
}

void TestConfig::iniCrossFieldStaleOffline()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    // stale 3500 > offline 3000
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n"
                         "[connection]\noffline_timeout_ms=3000\nstale_timeout_ms=3500\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "stale_timeout_ms", "offline_timeout_ms"));
}

void TestConfig::iniCrossFieldQueueBatch()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n"
                         "[database]\nbatch_samples=500\nqueue_samples=100\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "queue_samples", "batch_samples"));
}

void TestConfig::iniDbPathAbsolute()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    // 绝对路径必须被拒;C:/ 形态在 Linux 上反而是相对路径,须按平台取真
    // 绝对路径(QDir::tempPath 两侧都是正斜杠,不触发 INI 反斜杠转义)
    const QString absPath = QDir::temp().absoluteFilePath(QStringLiteral("dms-abs.db"));
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n[database]\npath=")
                  + absPath.toUtf8() + QByteArray("\n"));
    QVERIFY(!loadConfiguration(runRoot.path(), runRoot.path()).ok);

    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n"
                         "[database]\npath=../escape.db\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "database/path"));
}

void TestConfig::iniLogLevelInvalid()
{
    QTemporaryDir runRoot;
    writeUtf8(runRoot, "config/devices.json", kValidDevices);
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n[logging]\nlevel=VERBOSE\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QVERIFY(hasIssueFor(loaded.issues, "logging/level"));
}

// ---- 汇总 ----

void TestConfig::allIssuesCollected()
{
    QTemporaryDir runRoot;
    // 设备 id 非法 + INI 未知键:一次加载全部报出
    writeUtf8(runRoot, "config/devices.json",
              devicesJson(tcpDev(QStringLiteral("bad id!"))).toUtf8());
    writeUtf8(runRoot, "config/app.ini",
              QByteArray("[app]\nschema_version=1\n\n[foo]\nbar=1\n"));
    const LoadedConfig loaded = loadConfiguration(runRoot.path(), runRoot.path());
    QVERIFY(!loaded.ok);
    QCOMPARE(loaded.issues.size(), 2);
    QVERIFY(hasIssueFor(loaded.issues, "devices[0].id"));
    QVERIFY(hasIssueFor(loaded.issues, "foo/bar"));
}

QTEST_MAIN(TestConfig)
#include "tst_config.moc"
