// tst_database:LLD §7 / docs/06 schema v1 基础设施测试
// 覆盖(TC-DB-005/006):首次建库与 PRAGMA 实际值、v1 重开不重建、
// 高版本/未识别 v0 拒绝且不动原数据、打开失败显式报错、
// 启动恢复(遗留命令 Interrupted、活跃告警 restart 关闭)、
// 设备 upsert(镜像更新保 created_at)、FK/唯一键/CHECK 约束生效
#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include "persistence/Database.h"

using dms::ErrorCode;
using dms::persistence::Database;
using dms::persistence::DeviceRow;

namespace {

// 独立第二连接:用裸 SQL 造夹具/断言,被测行为全在 Database 里。
// 只存连接名,每次取句柄,析构先 close 再 removeDatabase(docs/06 §2)
class FixtureDb
{
public:
    explicit FixtureDb(const QString &path)
        : name_(QStringLiteral("fixture_") + QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        db.setDatabaseName(path);
        opened_ = db.open();
    }
    ~FixtureDb()
    {
        {
            QSqlDatabase db = QSqlDatabase::database(name_, false);
            if (db.isValid())
                db.close();
        }
        QSqlDatabase::removeDatabase(name_);
    }
    bool isOpen() const { return opened_; }

    bool exec(const QString &sql)
    {
        QSqlQuery query(QSqlDatabase::database(name_));
        return query.exec(sql);
    }

    QVariant scalar(const QString &sql)
    {
        QSqlQuery query(QSqlDatabase::database(name_));
        if (!query.exec(sql) || !query.next())
            return QVariant();
        return query.value(0);
    }

private:
    QString name_;
    bool opened_ = false;
};

DeviceRow device(const QString &id, const QString &name, bool enabled = true)
{
    DeviceRow row;
    row.id = id;
    row.name = name;
    row.transportType = QStringLiteral("tcp");
    row.configJson = QStringLiteral("{\"id\":\"%1\"}").arg(id);
    row.enabled = enabled;
    return row;
}

// 一行合法 commands 夹具(result 默认 Sent);session 每次独立,避开唯一键
bool insertCommand(FixtureDb &fx, const QString &deviceId, const char *result,
                   qint64 createdAt, qint64 finishedAt = -1)
{
    return fx.exec(QStringLiteral(
        "INSERT INTO commands(device_id,session_id,seq,command_type,request_json,"
        "result,ack_status,created_at,finished_at) VALUES('%1','%2',1,'Reset','[]',"
        "'%3',NULL,%4,%5)")
        .arg(deviceId, QUuid::createUuid().toString(QUuid::WithoutBraces),
             QLatin1String(result), QString::number(createdAt))
        .arg(finishedAt < 0 ? QStringLiteral("NULL") : QString::number(finishedAt)));
}

} // namespace

class TestDatabase : public QObject
{
    Q_OBJECT

private slots:
    void initCreatesSchemaAndPragmas();
    void reopenVersion1KeepsData();
    void rejectsHigherVersion();
    void rejectsV0WithUnknownTables();
    void emptyV0FileInitializes();
    void openFailureIsReported();
    void recoveryInterruptsLegacyCommands();
    void recoveryClosesLegacyAlarms();
    void deviceUpsertMirrorsConfig();
    void telemetryForeignKeyEnforced();
    void telemetryUniqueKeyEnforced();
    void commandCheckConstraintsEnforced();
    void alarmCheckConstraintsEnforced();
};

void TestDatabase::initCreatesSchemaAndPragmas()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("a/dms.db"));
    Database db;
    const auto result = db.initialize(path, 777, {device(QStringLiteral("dev1"), QStringLiteral("d1"))});

    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(db.isOpen());

    // PRAGMA 实际值核对外键/busy_timeout(docs/06 §2),表结构与版本齐备
    QSqlQuery q(db.sql());
    QVERIFY(q.exec(QStringLiteral("PRAGMA foreign_keys")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);

    QVERIFY(q.exec(QStringLiteral("PRAGMA busy_timeout")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 777);

    QVERIFY(q.exec(QStringLiteral("PRAGMA journal_mode")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("wal"));

    QVERIFY(q.exec(QStringLiteral("PRAGMA user_version")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);

    // 逐表断言存在(sqlite_master)
    QVERIFY(q.exec(QStringLiteral(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN"
        " ('devices','telemetry','alarms','commands')")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 4);

    // devices upsert 已在 initialize 里完成
    QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM devices")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
}

void TestDatabase::reopenVersion1KeepsData()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    {
        Database db;
        QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);
    }
    {
        // 独立连接写入一行遥测,模拟上一轮运行留下的历史
        FixtureDb fx(path);
        QVERIFY(fx.isOpen());
        QVERIFY(fx.exec(QStringLiteral(
            "INSERT INTO telemetry(device_id,session_id,seq,metric_id,value,ts,received_at)"
            " VALUES('dev1','s1',7,1,23.5,100,200)")));
    }
    // v1 正常打开:不重建 schema,历史仍在
    Database db;
    const auto result = db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))});
    QVERIFY2(result.ok, qPrintable(result.message));
    QSqlQuery q(db.sql());
    QVERIFY(q.exec(QStringLiteral("SELECT value FROM telemetry WHERE device_id='dev1'")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toDouble(), 23.5);
}

void TestDatabase::rejectsHigherVersion()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    {
        Database db;
        QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);
        QSqlQuery q(db.sql());
        QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 2")));
    }
    Database db;
    const auto result = db.initialize(path, 500, {});
    QCOMPARE(result.ok, false);
    QCOMPARE(result.code, ErrorCode::DbSchemaUnsupported);
    QVERIFY(!result.message.isEmpty());
    QCOMPARE(db.isOpen(), false);
}

void TestDatabase::rejectsV0WithUnknownTables()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    {
        // 手工造一个 0 版本、含未知表的旧库
        FixtureDb fx(path);
        QVERIFY(fx.isOpen());
        QVERIFY(fx.exec(QStringLiteral("CREATE TABLE legacy_thing(x INTEGER)")));
        QVERIFY(fx.exec(QStringLiteral("PRAGMA user_version = 0")));
    }
    Database db;
    const auto result = db.initialize(path, 500, {});
    QCOMPARE(result.ok, false);
    QCOMPARE(result.code, ErrorCode::DbSchemaUnsupported);
    // 原库不动:未知表仍在,不能删了重建(docs/06 §4)
    FixtureDb fx(path);
    QCOMPARE(fx.scalar(QStringLiteral(
                  "SELECT COUNT(*) FROM sqlite_master WHERE name='legacy_thing'")).toInt(), 1);
}

void TestDatabase::emptyV0FileInitializes()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    {
        // 空文件 = 合法空库:打开过但没有业务表
        FixtureDb fx(path);
        QVERIFY(fx.isOpen());
        QCOMPARE(fx.scalar(QStringLiteral("PRAGMA user_version")).toInt(), 0);
    }
    Database db;
    const auto result = db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))});
    QVERIFY2(result.ok, qPrintable(result.message));
    QSqlQuery q(db.sql());
    QVERIFY(q.exec(QStringLiteral("PRAGMA user_version")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
}

void TestDatabase::openFailureIsReported()
{
    QTemporaryDir dir;
    // 数据库路径本身是已存在目录:SQLite 打不开,必须显式报错而不是崩或静默
    const QString path = dir.filePath(QStringLiteral("occupied"));
    QDir().mkpath(path);
    Database db;
    const auto result = db.initialize(path, 500, {});
    QCOMPARE(result.ok, false);
    QCOMPARE(result.code, ErrorCode::DbWriteFailed);
    QVERIFY(!result.message.isEmpty());
    QCOMPARE(db.isOpen(), false);
}

void TestDatabase::recoveryInterruptsLegacyCommands()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        Database db;
        QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);
    }
    {
        FixtureDb fx(path);
        QVERIFY(insertCommand(fx, QStringLiteral("dev1"), "Sent", now - 5000));
        QVERIFY(insertCommand(fx, QStringLiteral("dev1"), "Pending", now - 4000));
        QVERIFY(insertCommand(fx, QStringLiteral("dev1"), "Succeeded", now - 3000,
                              now - 2900));
    }
    Database db;
    QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);

    QSqlQuery q(db.sql());
    // Sent/Pending → Interrupted 且 finished_at 落在 created_at 之后(CHECK 语义)
    QVERIFY(q.exec(QStringLiteral(
        "SELECT result, finished_at >= created_at FROM commands"
        " WHERE result='Interrupted' ORDER BY created_at")));
    QVERIFY(q.next());
    QCOMPARE(q.value(1).toInt(), 1);
    QVERIFY(q.next());
    QCOMPARE(q.value(1).toInt(), 1);
    QVERIFY(!q.next());
    // 已终态的命令不被重启覆盖
    QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM commands WHERE result='Succeeded'")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
}

void TestDatabase::recoveryClosesLegacyAlarms()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    {
        Database db;
        QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);
    }
    {
        FixtureDb fx(path);
        QVERIFY(fx.exec(QStringLiteral(
            "INSERT INTO alarms(device_id,metric_id,level,code,message,ts,acknowledged)"
            " VALUES('dev1',1,3,'TH_HIGH','over range',100,0)")));
    }
    Database db;
    QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);

    QSqlQuery q(db.sql());
    QVERIFY(q.exec(QStringLiteral(
        "SELECT cleared_at, clear_reason, acknowledged FROM alarms")));
    QVERIFY(q.next());
    QVERIFY(q.value(0).toLongLong() > 0);
    QCOMPARE(q.value(1).toString(), QStringLiteral("restart"));
    QCOMPARE(q.value(2).toInt(), 0); // 确认位独立于关闭,不被重启动
}

void TestDatabase::deviceUpsertMirrorsConfig()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    const QList<DeviceRow> first = {device(QStringLiteral("dev1"), QStringLiteral("旧名")),
                                    device(QStringLiteral("dev2"), QStringLiteral("d2"), false)};
    qint64 dev1Created = 0;
    {
        Database db;
        QVERIFY(db.initialize(path, 500, first).ok);
        QSqlQuery q(db.sql());
        QVERIFY(q.exec(QStringLiteral("SELECT created_at FROM devices WHERE id='dev1'")));
        QVERIFY(q.next());
        dev1Created = q.value(0).toLongLong();
    }
    // 重启:dev1 改名并停用,dev2 已从配置移除;镜像更新但 created_at 保留,
    // dev2 行不删(历史外键需要,docs/06 §1"不级联删除历史"),保留最后一次镜像值
    Database db;
    QVERIFY(db.initialize(path, 500,
                          {device(QStringLiteral("dev1"), QStringLiteral("新名"), false)})
                .ok);
    QSqlQuery q(db.sql());
    QVERIFY(q.exec(QStringLiteral(
        "SELECT id, name, enabled, created_at FROM devices ORDER BY id")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("dev1"));
    QCOMPARE(q.value(1).toString(), QStringLiteral("新名"));
    QCOMPARE(q.value(2).toInt(), 0);
    QCOMPARE(q.value(3).toLongLong(), dev1Created); // 首见时刻不被覆盖
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("dev2"));
    QCOMPARE(q.value(1).toString(), QStringLiteral("d2"));
    QCOMPARE(q.value(2).toInt(), 0); // 停用镜像原样保留
    QVERIFY(!q.next());
}

void TestDatabase::telemetryForeignKeyEnforced()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    Database db;
    QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);

    QSqlQuery q(db.sql());
    QVERIFY(!q.exec(QStringLiteral(
        "INSERT INTO telemetry(device_id,session_id,seq,metric_id,value,ts,received_at)"
        " VALUES('ghost','s1',1,1,1.0,0,0)")));
}

void TestDatabase::telemetryUniqueKeyEnforced()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    Database db;
    QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);

    QSqlQuery q(db.sql());
    const QString sql = QStringLiteral(
        "INSERT INTO telemetry(device_id,session_id,seq,metric_id,value,ts,received_at)"
        " VALUES('dev1','s1',9,2,1.0,0,0)");
    QVERIFY(q.exec(sql));
    QVERIFY(!q.exec(sql)); // 同 device/session/seq/metric 唯一键
}

void TestDatabase::commandCheckConstraintsEnforced()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    Database db;
    QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);

    QSqlQuery q(db.sql());
    // 非法 result 枚举
    QVERIFY(!q.exec(QStringLiteral(
        "INSERT INTO commands(device_id,session_id,seq,command_type,request_json,"
        "result,created_at) VALUES('dev1','s1',1,'Reset','[]','Bogus',100)")));
    // 终态必须带 finished_at
    QVERIFY(!q.exec(QStringLiteral(
        "INSERT INTO commands(device_id,session_id,seq,command_type,request_json,"
        "result,created_at) VALUES('dev1','s2',1,'Reset','[]','Succeeded',100)")));
    // finished_at 早于 created_at
    QVERIFY(!q.exec(QStringLiteral(
        "INSERT INTO commands(device_id,session_id,seq,command_type,request_json,"
        "result,created_at,finished_at) VALUES('dev1','s3',1,'Reset','[]',"
        "'Succeeded',200,100)")));
    // 非法 command_type 枚举
    QVERIFY(!q.exec(QStringLiteral(
        "INSERT INTO commands(device_id,session_id,seq,command_type,request_json,"
        "result,created_at,finished_at) VALUES('dev1','s4',1,'Reboot','[]',"
        "'Succeeded',200,300)")));
}

void TestDatabase::alarmCheckConstraintsEnforced()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    Database db;
    QVERIFY(db.initialize(path, 500, {device(QStringLiteral("dev1"), QStringLiteral("d1"))}).ok);

    QSqlQuery q(db.sql());
    // acknowledged=1 必须带 acknowledged_at
    QVERIFY(!q.exec(QStringLiteral(
        "INSERT INTO alarms(device_id,metric_id,level,code,message,ts,acknowledged)"
        " VALUES('dev1',1,2,'X','m',100,1)")));
    // cleared_at 早于 ts
    QVERIFY(!q.exec(QStringLiteral(
        "INSERT INTO alarms(device_id,metric_id,level,code,message,ts,acknowledged,"
        "cleared_at) VALUES('dev1',1,2,'X','m',100,0,50)")));
    // 同设备同 code+metric 只允许一个活跃事件(部分唯一索引)
    const QString active = QStringLiteral(
        "INSERT INTO alarms(device_id,metric_id,level,code,message,ts,acknowledged)"
        " VALUES('dev1',1,2,'TH_HIGH','m',100,0)");
    QVERIFY(q.exec(active));
    QVERIFY(!q.exec(active));
}

QTEST_MAIN(TestDatabase)
#include "tst_database.moc"
