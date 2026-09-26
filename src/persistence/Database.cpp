#include "Database.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <QVariant>

namespace dms::persistence {

namespace {

// schema v1 首次建库 DDL,与 docs/06 §2 逐字一致;
// BEGIN/COMMIT 由调用方管理,PRAGMA 必须在事务外(docs/06 §2)
const char kSchemaDdl[] = R"SQL(
CREATE TABLE devices (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(id) BETWEEN 1 AND 64),
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),
    transport_type TEXT NOT NULL CHECK(transport_type IN ('tcp','udp','serial')),
    config_json TEXT NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
    created_at INTEGER NOT NULL CHECK(created_at >= 0)
);
CREATE TABLE telemetry (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    session_id TEXT NOT NULL,
    seq INTEGER NOT NULL CHECK(seq BETWEEN 0 AND 4294967295),
    metric_id INTEGER NOT NULL CHECK(metric_id BETWEEN 1 AND 4),
    value REAL NOT NULL,
    ts INTEGER NOT NULL CHECK(ts >= 0),
    received_at INTEGER NOT NULL CHECK(received_at >= 0),
    UNIQUE(device_id, session_id, seq, metric_id)
);
CREATE INDEX idx_telemetry_device_time ON telemetry(device_id, received_at, id);
CREATE INDEX idx_telemetry_metric_time ON telemetry(device_id, metric_id, received_at, id);
CREATE TABLE alarms (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    metric_id INTEGER NOT NULL DEFAULT 0 CHECK(metric_id BETWEEN 0 AND 4),
    level INTEGER NOT NULL CHECK(level BETWEEN 1 AND 3),
    code TEXT NOT NULL,
    message TEXT NOT NULL,
    ts INTEGER NOT NULL CHECK(ts >= 0),
    acknowledged INTEGER NOT NULL DEFAULT 0 CHECK(acknowledged IN (0,1)),
    acknowledged_at INTEGER,
    cleared_at INTEGER,
    clear_reason TEXT,
    CHECK((acknowledged = 0 AND acknowledged_at IS NULL)
       OR (acknowledged = 1 AND acknowledged_at IS NOT NULL AND acknowledged_at >= ts)),
    CHECK(cleared_at IS NULL OR cleared_at >= ts)
);
CREATE UNIQUE INDEX idx_alarm_active
    ON alarms(device_id, code, metric_id) WHERE cleared_at IS NULL;
CREATE INDEX idx_alarm_time ON alarms(device_id, ts, id);
CREATE TABLE commands (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    session_id TEXT NOT NULL,
    seq INTEGER NOT NULL CHECK(seq BETWEEN 0 AND 4294967295),
    command_type TEXT NOT NULL CHECK(command_type IN ('Start','Stop','Reset','SetParameter')),
    request_json TEXT NOT NULL,
    result TEXT NOT NULL CHECK(result IN
        ('Pending','Sent','Succeeded','Rejected','TimedOut','Cancelled','Failed','Interrupted')),
    ack_status INTEGER CHECK(ack_status BETWEEN 0 AND 4),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    finished_at INTEGER,
    UNIQUE(device_id, session_id, seq),
    CHECK((result IN ('Pending','Sent') AND finished_at IS NULL)
       OR (result NOT IN ('Pending','Sent') AND finished_at IS NOT NULL AND finished_at >= created_at))
);
CREATE INDEX idx_command_time ON commands(device_id, created_at, id);
)SQL";

// 执行无结果语句;失败时带 SQLite 错误文本
bool execStatement(QSqlDatabase db, const QString &sql, QString *error = nullptr)
{
    QSqlQuery query(db);
    if (!query.exec(sql)) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(query.lastError().text(), sql);
        return false;
    }
    return true;
}

// 读取单值 PRAGMA(如 user_version / foreign_keys);失败返回 invalid
QVariant readPragma(QSqlDatabase db, const QString &sql)
{
    QSqlQuery query(db);
    if (!query.exec(sql) || !query.next())
        return QVariant();
    return query.value(0);
}

} // namespace

Database::Database()
    // 连接名全局唯一:测试与生产可能同时开多个连接指向同一文件(docs/06 §2)
    : connectionName_(QStringLiteral("dms_db_")
                      + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

Database::~Database()
{
    close();
}

QSqlDatabase Database::sql() const
{
    return QSqlDatabase::database(connectionName_);
}

bool Database::beginImmediate(QString *error)
{
    return execStatement(sql(), QStringLiteral("BEGIN IMMEDIATE"), error);
}

bool Database::commit(QString *error)
{
    return execStatement(sql(), QStringLiteral("COMMIT"), error);
}

bool Database::rollback(QString *error)
{
    return execStatement(sql(), QStringLiteral("ROLLBACK"), error);
}

DbInitResult Database::initialize(const QString &absoluteFilePath, int busyTimeoutMs,
                                  const QList<DeviceRow> &devices)
{
    close();

    DbInitResult opened = openConnection(absoluteFilePath, busyTimeoutMs);
    if (!opened.ok) {
        // openConnection 失败时连接名已登记(addDatabase 无条件登记),补注销
        releaseConnection();
        return opened;
    }
    open_ = true;

    const DbInitResult schema = initSchema();
    if (!schema.ok) {
        close();
        return schema;
    }
    const DbInitResult recovery = recoverOnStart();
    if (!recovery.ok) {
        close();
        return recovery;
    }
    const DbInitResult upserted = upsertDevices(devices);
    if (!upserted.ok) {
        close();
        return upserted;
    }
    return DbInitResult{true, dms::ErrorCode::None, QString()};
}

DbInitResult Database::openConnection(const QString &absoluteFilePath, int busyTimeoutMs)
{
    // 首次建库允许补父目录(data/);路径合法性由装配层校验(docs/15 §4)
    QDir().mkpath(QFileInfo(absoluteFilePath).absolutePath());

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db.setDatabaseName(absoluteFilePath);
    if (!db.open())
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("打开数据库失败: %1")
                                .arg(db.lastError().text())};

    QString error;
    // 外键按连接启用且必须检查实际值;journal/同步等级按 docs/06 §2(docs/06 §6)
    if (!execStatement(db, QStringLiteral("PRAGMA foreign_keys = ON"), &error)
        || readPragma(db, QStringLiteral("PRAGMA foreign_keys")).toInt() != 1) {
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("启用外键失败: %1").arg(error)};
    }
    // WAL 返回新模式的单行结果,读走并核对
    QSqlQuery journal(db);
    if (!journal.exec(QStringLiteral("PRAGMA journal_mode = WAL")) || !journal.next()
        || journal.value(0).toString().compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0) {
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("启用 WAL 失败")};
    }
    if (!execStatement(db,
                       QStringLiteral("PRAGMA busy_timeout = %1").arg(busyTimeoutMs), &error)
        || readPragma(db, QStringLiteral("PRAGMA busy_timeout")).toInt() != busyTimeoutMs) {
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("设置 busy_timeout 失败: %1").arg(error)};
    }
    if (!execStatement(db, QStringLiteral("PRAGMA synchronous = FULL"), &error)) {
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("设置 synchronous 失败: %1").arg(error)};
    }
    return DbInitResult{true, dms::ErrorCode::None, QString()};
}

DbInitResult Database::initSchema()
{
    const QSqlDatabase db = sql();
    const int version = readPragma(db, QStringLiteral("PRAGMA user_version")).toInt();

    if (version > schemaVersion)
        return DbInitResult{false, dms::ErrorCode::DbSchemaUnsupported,
                            QStringLiteral("user_version=%1 超过支持的版本 %2,拒绝写入")
                                .arg(version).arg(schemaVersion)};

    if (version == schemaVersion)
        return DbInitResult{true, dms::ErrorCode::None, QString()};

    // version==0:仅无业务表才可初始化;存在任何表视为未识别旧库(docs/06 §4)
    QSqlQuery tables(db);
    if (!tables.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"))) {
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("读取 sqlite_master 失败: %1")
                                .arg(tables.lastError().text())};
    }
    if (!tables.next() || tables.value(0).toInt() > 0) {
        return DbInitResult{false, dms::ErrorCode::DbSchemaUnsupported,
                            QStringLiteral("0 版本库存在未识别表,需明确迁移方案,拒绝初始化")};
    }

    // DDL 与 user_version 同事务,失败整体回滚保留原状(docs/06 §4)。
    // QSqlQuery 一次只准备一条语句(sqlite3_prepare 的 tail 被静默忽略),
    // 多语句 DDL 必须按 ';' 拆分逐条执行;本 DDL 无内嵌分号,拆分安全
    QString error;
    if (!beginImmediate(&error)) {
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed, error};
    }
    const QStringList statements =
        QString::fromUtf8(kSchemaDdl).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &statement : statements) {
        // 末尾换行等空白片段不是语句,别拼成 ";" 去执行
        const QString sql = statement.trimmed();
        if (sql.isEmpty())
            continue;
        if (!execStatement(db, sql + QLatin1Char(';'), &error)) {
            rollback();
            return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                                QStringLiteral("建库事务失败已回滚: %1").arg(error)};
        }
    }
    if (!execStatement(db, QStringLiteral("PRAGMA user_version = %1").arg(schemaVersion),
                       &error)
        || !commit(&error)) {
        rollback();
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("建库事务失败已回滚: %1").arg(error)};
    }
    return DbInitResult{true, dms::ErrorCode::None, QString()};
}

DbInitResult Database::recoverOnStart()
{
    // 重启不是正常恢复:遗留未决命令标 Interrupted、旧活跃告警按 restart 关闭,
    // 新会话重新评价(docs/06 §4)。MAX(?,created_at) 防 CHECK 在时钟回拨时翻车
    const QSqlDatabase db = sql();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString error;
    if (!beginImmediate(&error))
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed, error};

    QSqlQuery commandQuery(db);
    commandQuery.prepare(QStringLiteral(
        "UPDATE commands SET result='Interrupted',"
        " finished_at=MAX(:now, created_at) WHERE result IN ('Pending','Sent')"));
    commandQuery.bindValue(QStringLiteral(":now"), now);
    QSqlQuery alarmQuery(db);
    alarmQuery.prepare(QStringLiteral(
        "UPDATE alarms SET cleared_at=MAX(:now, ts), clear_reason='restart'"
        " WHERE cleared_at IS NULL"));
    alarmQuery.bindValue(QStringLiteral(":now"), now);

    if (!commandQuery.exec() || !alarmQuery.exec() || !commit(&error)) {
        const QString detail = error.isEmpty()
                                   ? commandQuery.lastError().text() + alarmQuery.lastError().text()
                                   : error;
        rollback();
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                            QStringLiteral("启动恢复事务失败已回滚: %1").arg(detail)};
    }
    return DbInitResult{true, dms::ErrorCode::None, QString()};
}

DbInitResult Database::upsertDevices(const QList<DeviceRow> &devices)
{
    // JSON 是唯一来源,DB 镜像当前配置;冲突时保留首次 created_at(docs/06 §1)
    const QSqlDatabase db = sql();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString error;
    if (!beginImmediate(&error))
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed, error};

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO devices(id,name,transport_type,config_json,enabled,created_at)"
        " VALUES(:id,:name,:transport,:config,:enabled,:created)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,"
        " transport_type=excluded.transport_type, config_json=excluded.config_json,"
        " enabled=excluded.enabled"));
    for (const DeviceRow &row : devices) {
        query.bindValue(QStringLiteral(":id"), row.id);
        query.bindValue(QStringLiteral(":name"), row.name);
        query.bindValue(QStringLiteral(":transport"), row.transportType);
        query.bindValue(QStringLiteral(":config"), row.configJson);
        query.bindValue(QStringLiteral(":enabled"), row.enabled ? 1 : 0);
        query.bindValue(QStringLiteral(":created"), now);
        if (!query.exec()) {
            const QString detail = query.lastError().text();
            rollback();
            return DbInitResult{false, dms::ErrorCode::DbWriteFailed,
                                QStringLiteral("设备 %1 upsert 失败已回滚: %2")
                                    .arg(row.id, detail)};
        }
    }
    if (!commit(&error)) {
        rollback();
        return DbInitResult{false, dms::ErrorCode::DbWriteFailed, error};
    }
    return DbInitResult{true, dms::ErrorCode::None, QString()};
}

void Database::close()
{
    if (!open_)
        return;
    open_ = false;
    releaseConnection();
}

void Database::releaseConnection()
{
    // 先让局部句柄析构再 removeDatabase:查询对象都是方法局部变量,
    // 违反此序会触发"connection is still in use"悬空警告(docs/06 §2)
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
        if (db.isValid())
            db.close();
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

// 只读打开既有库(查询/导出连接,docs/06 §3):
//   - 读者不得静默创建空库:SQLite 默认会建文件,先做存在性检查;
//   - 只设 busy_timeout(读也可能撞上写事务/checkpoint)与 query_only
//     (引擎级挡住误写);不动 journal/synchronous,不做建库/恢复/镜像;
//   - 版本与写路径同规则:user_version 不等于支持的版本拒绝解释(docs/06 §4)
DbInitResult Database::openExisting(const QString &absoluteFilePath, int busyTimeoutMs)
{
    close();

    if (!QFileInfo::exists(absoluteFilePath))
        return DbInitResult{false, dms::ErrorCode::DbReadFailed,
                            QStringLiteral("数据库文件不存在: %1").arg(absoluteFilePath)};

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
        db.setDatabaseName(absoluteFilePath);
        if (!db.open()) {
            const QString text = db.lastError().text();
            releaseConnection();
            return DbInitResult{false, dms::ErrorCode::DbReadFailed,
                                QStringLiteral("打开数据库失败: %1").arg(text)};
        }
    } // db 句柄离开作用域析构,后续 removeDatabase 才无悬空警告

    QString error;
    if (!execStatement(sql(), QStringLiteral("PRAGMA busy_timeout = %1").arg(busyTimeoutMs),
                       &error)
        || readPragma(sql(), QStringLiteral("PRAGMA busy_timeout")).toInt() != busyTimeoutMs) {
        releaseConnection();
        return DbInitResult{false, dms::ErrorCode::DbReadFailed,
                            QStringLiteral("设置 busy_timeout 失败: %1").arg(error)};
    }
    if (!execStatement(sql(), QStringLiteral("PRAGMA query_only = ON"), &error)
        || readPragma(sql(), QStringLiteral("PRAGMA query_only")).toInt() != 1) {
        releaseConnection();
        return DbInitResult{false, dms::ErrorCode::DbReadFailed,
                            QStringLiteral("启用 query_only 失败: %1").arg(error)};
    }

    const int version = readPragma(sql(), QStringLiteral("PRAGMA user_version")).toInt();
    if (version != schemaVersion) {
        releaseConnection();
        return DbInitResult{false, dms::ErrorCode::DbSchemaUnsupported,
                            QStringLiteral("user_version=%1 与支持的版本 %2 不符,拒绝读取")
                                .arg(version).arg(schemaVersion)};
    }
    open_ = true;
    return DbInitResult{true, dms::ErrorCode::None, QString()};
}

} // namespace dms::persistence
