#pragma once

#include <QList>
#include <QString>

#include "common/Logging.h"

class QSqlDatabase;

namespace dms::persistence {

// devices 表一行(启动 upsert 用,docs/06 §1):
// JSON 是连接配置唯一来源,DB 只镜像不反向覆盖;config_json 存原始对象文本
struct DeviceRow {
    QString id;
    QString name;
    QString transportType; // "tcp" | "udp" | "serial"
    QString configJson;    // UTF-8 紧凑 JSON(devices.json 原始对象)
    bool enabled = true;
};

// 初始化结果:code 供日志/上报区分 DB_SCHEMA_UNSUPPORTED 与其他失败
struct DbInitResult {
    bool ok = false;
    dms::ErrorCode code = dms::ErrorCode::None;
    QString message;
};

// QSqlDatabase 连接的 RAII 封装(docs/06 §2):
//   - 连接名全局唯一,连接在所属线程创建/使用/关闭;
//   - 查询对象全部是方法内局部变量,析构先 close 再 removeDatabase,
//     不持有长生命周期 QSqlQuery(规避 removeDatabase 的悬空警告);
//   - 事务用显式 BEGIN IMMEDIATE 而非 QSqlDatabase::transaction():
//     Qt 只有 deferred BEGIN,读升写锁遇 BUSY_SNAPSHOT 不受 busy_timeout
//     重试保护;IMMEDIATE 一开始就取写锁,锁忙由 busy_timeout 兜住。
// schema v1 职责(docs/06 §4):首次建库 / user_version 检查 /
// 启动恢复(遗留命令 Interrupted、活跃告警 restart 关闭)/ 设备 upsert。
// openExisting 是只读打开(docs/06 §3):查询/导出连接专用,不建库不恢复不镜像,
// query_only 在引擎层挡住误写;与写连接(DatabaseWorker)并发由 WAL 保证
class Database
{
public:
    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    // 打开(必要时创建父目录与文件)+ PRAGMA + schema 初始化 + 启动恢复 +
    // 设备 upsert。失败时保留错误与磁盘原状,不删除重建(docs/06 §4)
    DbInitResult initialize(const QString &absoluteFilePath, int busyTimeoutMs,
                            const QList<DeviceRow> &devices);

    // 只读打开既有库:文件必须已存在(不静默创建),版本必须匹配;
    // 只设 busy_timeout 与 query_only,不改库内容
    DbInitResult openExisting(const QString &absoluteFilePath, int busyTimeoutMs);

    void close();
    bool isOpen() const { return open_; }

    // 当前线程内使用;返回轻量句柄,调用方存局部变量
    QSqlDatabase sql() const;

    // 引擎版本(SELECT sqlite_version()):部署证据与诊断用,须在已打开连接上调用
    QString sqliteVersion() const;

    // 显式事务控制(见类注释);失败时 *error 带 SQLite 错误文本
    bool beginImmediate(QString *error = nullptr);
    bool commit(QString *error = nullptr);
    bool rollback(QString *error = nullptr);

    static constexpr int schemaVersion = 1; // PRAGMA user_version

private:
    DbInitResult openConnection(const QString &absoluteFilePath, int busyTimeoutMs);
    DbInitResult initSchema();     // user_version=0 且无业务表时建库
    DbInitResult recoverOnStart(); // 遗留 Pending/Sent 命令与活跃告警
    DbInitResult upsertDevices(const QList<DeviceRow> &devices);
    void releaseConnection();      // 关闭并注销连接注册(打开失败也要注销)

    QString connectionName_;
    bool open_ = false;
};

} // namespace dms::persistence

Q_DECLARE_METATYPE(dms::persistence::DeviceRow)
