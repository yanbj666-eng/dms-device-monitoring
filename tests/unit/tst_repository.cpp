// tst_repository:历史查询与 CSV 导出(docs/06 §3,REQ-DB-002,TC-DB-007)
// 覆盖:只读打开(缺文件/错版本/query_only 拒写)、半开区间+稳定排序、
// 指标过滤、(received_at,id) 键集游标翻页、首查 MAX(id) 边界隔离
// 翻页期间新写入、每页 1000 行上限、CSV 与查询结果一致、转义与公式防护、
// 取消与中途失败不留伪成功文件
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <memory>
#include <optional>

#include "persistence/Repository.h"

using dms::ErrorCode;
using dms::persistence::Database;
using dms::persistence::DbInitResult;
using dms::persistence::DeviceRow;
using dms::persistence::Repository;
using dms::persistence::TelemetryCursor;
using dms::persistence::TelemetryQuery;
using dms::persistence::TelemetryRow;

class TestRepository : public QObject
{
    Q_OBJECT

private slots:
    void openExistingRejectsMissingFile();
    void openExistingRejectsWrongVersion();
    void openExistingIsReadOnly();
    void queryFiltersHalfOpenRangeAndOrders();
    void queryMetricFilter();
    void paginationCursorWalksAllRows();
    void boundaryExcludesRowsWrittenDuringPaging();
    void maxRowsClampedToPageCap();
    void csvExportMatchesQuery();
    void csvEscapesAndGuardsFormula();
    void csvExportCancelledLeavesNoFile();
    void csvExportFailureKeepsOldFile();
    void csvExportToInvalidPathFails();
    void queryOnClosedDatabaseFails();

private:
    // 写连接(建库+造数)与被测只读连接分离,贴近真实部署形态(WAL 读写并发)。
    // 成员按声明逆序析构:repo/reader/writer 先关,dir 最后清,才能删掉库文件
    struct Env {
        std::unique_ptr<QTemporaryDir> dir;
        QString path;
        Database writer;
        Database reader;
        std::unique_ptr<Repository> repo; // 非拥有,指向 reader
    };

    bool makeEnv(Env *env, const QStringList &deviceIds, QString *error);
    bool insertRow(const Env &env, const QString &device, int seq, int metric,
                   double value, qint64 ts, qint64 receivedAt, QString *error);
    bool insertBulk(const Env &env, const QString &device, int firstSeq, int count,
                    qint64 firstReceivedAt, QString *error);
    static QStringList readCsvLines(const QString &path);
};

bool TestRepository::makeEnv(Env *env, const QStringList &deviceIds, QString *error)
{
    env->dir = std::make_unique<QTemporaryDir>();
    env->path = env->dir->filePath(QStringLiteral("dms.db"));
    QList<DeviceRow> rows;
    for (const QString &id : deviceIds) {
        DeviceRow row;
        row.id = id;
        row.name = QStringLiteral("设备%1").arg(id);
        row.transportType = QStringLiteral("tcp");
        row.configJson = QStringLiteral("{\"id\":\"%1\"}").arg(id);
        rows.append(row);
    }
    const DbInitResult init = env->writer.initialize(env->path, 50, rows);
    if (!init.ok) {
        *error = init.message;
        return false;
    }
    const DbInitResult opened = env->reader.openExisting(env->path, 50);
    if (!opened.ok) {
        *error = opened.message;
        return false;
    }
    env->repo = std::make_unique<Repository>(&env->reader);
    return true;
}

bool TestRepository::insertRow(const Env &env, const QString &device, int seq,
                               int metric, double value, qint64 ts, qint64 receivedAt,
                               QString *error)
{
    QSqlQuery query(env.writer.sql());
    if (!query.prepare(QStringLiteral(
            "INSERT INTO telemetry(device_id,session_id,seq,metric_id,value,ts,received_at)"
            " VALUES(?,?,?,?,?,?,?)"))) {
        *error = query.lastError().text();
        return false;
    }
    query.addBindValue(device);
    query.addBindValue(QStringLiteral("s1"));
    query.addBindValue(seq);
    query.addBindValue(metric);
    query.addBindValue(value);
    query.addBindValue(ts);
    query.addBindValue(receivedAt);
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    return true;
}

bool TestRepository::insertBulk(const Env &env, const QString &device, int firstSeq,
                                int count, qint64 firstReceivedAt, QString *error)
{
    // 造大量行:一个事务一条预编译语句,测试别在造数上花时间
    QSqlQuery control(env.writer.sql());
    if (!control.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        *error = control.lastError().text();
        return false;
    }
    QSqlQuery query(env.writer.sql());
    if (!query.prepare(QStringLiteral(
            "INSERT INTO telemetry(device_id,session_id,seq,metric_id,value,ts,received_at)"
            " VALUES(?,?,?,?,?,?,?)"))) {
        *error = query.lastError().text();
        return false;
    }
    for (int i = 0; i < count; ++i) {
        query.addBindValue(device);
        query.addBindValue(QStringLiteral("s1"));
        query.addBindValue(firstSeq + i);
        query.addBindValue(1);
        query.addBindValue(20.0 + i * 0.5);
        query.addBindValue(qint64(1000));
        query.addBindValue(firstReceivedAt + i);
        if (!query.exec()) {
            const QString text = query.lastError().text();
            control.exec(QStringLiteral("ROLLBACK"));
            *error = text;
            return false;
        }
    }
    if (!control.exec(QStringLiteral("COMMIT"))) {
        *error = control.lastError().text();
        return false;
    }
    return true;
}

QStringList TestRepository::readCsvLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF))) // UTF-8 BOM(Excel 识别用)
        text.remove(0, 1);
    while (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    return text.split(QLatin1Char('\n'));
}

void TestRepository::openExistingRejectsMissingFile()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("absent.db"));
    Database reader;
    const DbInitResult opened = reader.openExisting(path, 50);
    QVERIFY(!opened.ok);
    QCOMPARE(opened.code, ErrorCode::DbReadFailed);
    QVERIFY(!QFileInfo::exists(path)); // 只读打开不得静默创建文件
}

void TestRepository::openExistingRejectsWrongVersion()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));
    Database writer; // 声明序保证 writer 先于 dir 析构
    QVERIFY2(writer.initialize(path, 50, {}).ok, "建库失败");
    QSqlQuery bump(writer.sql());
    QVERIFY2(bump.exec(QStringLiteral("PRAGMA user_version = 2")), "改版本失败");
    writer.close();

    Database reader;
    const DbInitResult opened = reader.openExisting(path, 50);
    QVERIFY(!opened.ok);
    QCOMPARE(opened.code, ErrorCode::DbSchemaUnsupported);
}

void TestRepository::openExistingIsReadOnly()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertRow(env, QStringLiteral("dev1"), 1, 1, 20.5, 1000, 100, &err),
             qPrintable(err));

    // query_only 在引擎层拒绝一切写:误用读者连接也改不了库
    QSqlQuery write(env.reader.sql());
    QVERIFY(!write.exec(QStringLiteral(
        "INSERT INTO devices(id,name,transport_type,config_json,enabled,created_at)"
        " VALUES('x','x','tcp','{}',1,1)")));

    // 读不受影响
    TelemetryCursor cursor;
    const auto page = env.repo->queryPage(TelemetryQuery{}, &cursor, 10, &err);
    QVERIFY2(page.has_value(), qPrintable(err));
    QCOMPARE(page->rows.size(), 1);
    QCOMPARE(page->rows.first().value, 20.5);
}

void TestRepository::queryFiltersHalfOpenRangeAndOrders()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1"), QStringLiteral("dev2")}, &err),
             qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 1, 1, 20.5, 1000, 100, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 2, 2, 101.25, 1000, 200, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 3, 1, 21.0, 1000, 200, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 4, 3, 5.5, 1000, 300, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev2", 5, 1, 30.0, 1000, 150, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev2", 6, 4, 250.0, 1000, 250, &err), qPrintable(err));

    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    query.startMs = 100;
    query.endMs = 300; // 半开:含 100,不含 300

    TelemetryCursor cursor;
    const auto page = env.repo->queryPage(query, &cursor, 100, &err);
    QVERIFY2(page.has_value(), qPrintable(err));
    QVERIFY(!page->hasMore);
    QCOMPARE(page->rows.size(), 3);
    QCOMPARE(page->rows[0].receivedAt, qint64(100)); // start 包含
    QCOMPARE(page->rows[0].deviceId, QStringLiteral("dev1"));
    QCOMPARE(page->rows[0].value, 20.5);
    QCOMPARE(page->rows[0].ts, qint64(1000));
    QCOMPARE(page->rows[1].receivedAt, qint64(200));
    QCOMPARE(page->rows[2].receivedAt, qint64(200)); // 同 received_at 靠 id 稳定排序
    QVERIFY(page->rows[1].id < page->rows[2].id);
    QCOMPARE(page->rows[2].seq, quint32(3));

    // end <= start 的空区间:空结果不是错误
    TelemetryQuery empty;
    empty.deviceId = QStringLiteral("dev1");
    empty.startMs = 300;
    empty.endMs = 100;
    TelemetryCursor emptyCursor;
    const auto emptyPage = env.repo->queryPage(empty, &emptyCursor, 100, &err);
    QVERIFY2(emptyPage.has_value(), qPrintable(err));
    QVERIFY(emptyPage->rows.isEmpty());
}

void TestRepository::queryMetricFilter()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 1, 1, 20.5, 1000, 100, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 2, 2, 101.25, 1000, 110, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 3, 1, 21.0, 1000, 120, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 4, 4, 4294967295.0, 1000, 130, &err),
             qPrintable(err));

    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    query.metricId = 1;
    TelemetryCursor cursor;
    const auto page = env.repo->queryPage(query, &cursor, 100, &err);
    QVERIFY2(page.has_value(), qPrintable(err));
    QCOMPARE(page->rows.size(), 2); // 仅指标 1
    QCOMPARE(page->rows[0].seq, quint32(1));
    QCOMPARE(page->rows[1].seq, quint32(3));

    TelemetryQuery rpm;
    rpm.deviceId = QStringLiteral("dev1");
    rpm.metricId = 4;
    TelemetryCursor rpmCursor;
    const auto rpmPage = env.repo->queryPage(rpm, &rpmCursor, 100, &err);
    QVERIFY2(rpmPage.has_value(), qPrintable(err));
    QCOMPARE(rpmPage->rows.size(), 1);
    QCOMPARE(rpmPage->rows.first().metricId, quint16(4));
    QCOMPARE(rpmPage->rows.first().value, 4294967295.0); // uint32 值域完整入 REAL
}

void TestRepository::paginationCursorWalksAllRows()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertBulk(env, "dev1", 1, 10, 100, &err), qPrintable(err));

    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    TelemetryCursor cursor;
    QList<qint64> ids;
    int pages = 0;
    forever {
        const auto page = env.repo->queryPage(query, &cursor, 3, &err);
        QVERIFY2(page.has_value(), qPrintable(err));
        ++pages;
        for (const TelemetryRow &row : page->rows)
            ids.append(row.id);
        if (!page->hasMore) {
            QCOMPARE(page->rows.size(), 1); // 10 = 3+3+3+1
            break;
        }
        QCOMPARE(page->rows.size(), 3);
    }
    QCOMPARE(pages, 4);
    QCOMPARE(ids.size(), 10);
    for (int i = 1; i < ids.size(); ++i)
        QVERIFY(ids[i - 1] < ids[i]); // (received_at,id) 全序无重复

    // 新查询会话重走一遍:结果一致
    TelemetryCursor again;
    const auto full = env.repo->queryPage(query, &again, 100, &err);
    QVERIFY2(full.has_value(), qPrintable(err));
    QCOMPARE(full->rows.size(), 10);
}

void TestRepository::boundaryExcludesRowsWrittenDuringPaging()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertBulk(env, "dev1", 1, 5, 100, &err), qPrintable(err));

    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    TelemetryCursor cursor;
    const auto first = env.repo->queryPage(query, &cursor, 2, &err);
    QVERIFY2(first.has_value(), qPrintable(err));
    QVERIFY(first->hasMore);

    // 翻页期间新写入:received_at 落在未翻区域,id 更大。
    // 没有首查边界它会挤进后续页,造成同一次查询结果漂移
    QVERIFY2(insertRow(env, "dev1", 6, 1, 99.0, 1000, 100, &err), qPrintable(err));

    QList<double> walked;
    for (const TelemetryRow &row : first->rows)
        walked.append(row.value);
    forever {
        const auto page = env.repo->queryPage(query, &cursor, 2, &err);
        QVERIFY2(page.has_value(), qPrintable(err));
        for (const TelemetryRow &row : page->rows)
            walked.append(row.value);
        if (!page->hasMore)
            break;
    }
    QCOMPARE(walked.size(), 5); // 本次查询会话仍是首查时的 5 行
    QVERIFY(!walked.contains(99.0));

    // 新会话(新游标)能看到已提交的新行
    TelemetryCursor fresh;
    const auto full = env.repo->queryPage(query, &fresh, 100, &err);
    QVERIFY2(full.has_value(), qPrintable(err));
    QCOMPARE(full->rows.size(), 6);
    bool sawNewRow = false; // 新行 received_at=100,按 (received_at,id) 排第二
    for (const TelemetryRow &row : full->rows)
        sawNewRow = sawNewRow || row.value == 99.0;
    QVERIFY(sawNewRow);
}

void TestRepository::maxRowsClampedToPageCap()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertBulk(env, "dev1", 1, 1005, 100, &err), qPrintable(err));

    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    TelemetryCursor cursor;
    // 请求 5000 行:钳到每页上限 1000(docs/06 §3)
    const auto page = env.repo->queryPage(query, &cursor, 5000, &err);
    QVERIFY2(page.has_value(), qPrintable(err));
    QCOMPARE(page->rows.size(), 1000);
    QVERIFY(page->hasMore);

    const auto rest = env.repo->queryPage(query, &cursor, 5000, &err);
    QVERIFY2(rest.has_value(), qPrintable(err));
    QCOMPARE(rest->rows.size(), 5);
    QVERIFY(!rest->hasMore);
}

void TestRepository::csvExportMatchesQuery()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1"), QStringLiteral("dev2")}, &err),
             qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 1, 1, 20.5, 1000, 100, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 2, 2, 101.25, 1000, 150, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 3, 1, -12.5, 1000, 200, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 4, 4, 4294967295.0, 1000, 250, &err),
             qPrintable(err));
    QVERIFY2(insertRow(env, "dev2", 5, 1, 30.0, 1000, 120, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev2", 6, 3, 5.5, 1000, 260, &err), qPrintable(err));

    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    query.startMs = 100;
    query.endMs = 300;

    const QString target = env.dir->filePath(QStringLiteral("out.csv"));
    const qint64 written = env.repo->exportCsv(query, target, {}, &err);
    QCOMPARE(written, qint64(4));

    const QStringList lines = readCsvLines(target);
    QCOMPARE(lines.size(), 5);
    QCOMPARE(lines[0], QStringLiteral("device_id,metric_id,value,unit,ts,received_at"));
    QCOMPARE(lines[1], QStringLiteral("dev1,1,20.5,°C,1000,100"));
    QCOMPARE(lines[2], QStringLiteral("dev1,2,101.25,kPa,1000,150"));
    // 负值是数值列:不加公式防护单引号(docs/06 §3)
    QCOMPARE(lines[3], QStringLiteral("dev1,1,-12.5,°C,1000,200"));
    // uint32 值域 10 位有效数字:不丢精度不成科学计数
    QCOMPARE(lines[4], QStringLiteral("dev1,4,4294967295,r/min,1000,250"));

    // CSV 与同一查询的分页结果一致(REQ-DB-002):逐行逐列比对
    TelemetryCursor cursor;
    const auto page = env.repo->queryPage(query, &cursor, 2, &err);
    QVERIFY2(page.has_value(), qPrintable(err));
    QCOMPARE(page->rows.size(), 2);
    QCOMPARE(page->hasMore, true);
    for (int i = 0; i < page->rows.size(); ++i) {
        const TelemetryRow &row = page->rows[i];
        const QStringList fields = lines[i + 1].split(QLatin1Char(','));
        QCOMPARE(fields.size(), 6);
        QCOMPARE(fields[0], row.deviceId);
        QCOMPARE(fields[1].toUInt(), uint(row.metricId));
        QCOMPARE(fields[2].toDouble(), row.value);
        QCOMPARE(fields[3], Repository::metricUnit(row.metricId));
        QCOMPARE(fields[4].toLongLong(), row.ts);
        QCOMPARE(fields[5].toLongLong(), row.receivedAt);
    }
}

void TestRepository::csvEscapesAndGuardsFormula()
{
    // 设备 ID 是用户可控文本:逗号/引号转义,= + - @ 起头加公式防护
    const QStringList devices = {QStringLiteral("=1+1"), QStringLiteral("a,b"),
                                 QStringLiteral("q\"x"),  QStringLiteral("+ok"),
                                 QStringLiteral("@ref"), QStringLiteral("plain")};
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, devices, &err), qPrintable(err));
    for (int i = 0; i < devices.size(); ++i)
        QVERIFY2(insertRow(env, devices[i], 1, 1, 1.5, 1000, 100, &err),
                 qPrintable(err));

    const QString target = env.dir->filePath(QStringLiteral("out.csv"));
    const qint64 written = env.repo->exportCsv(TelemetryQuery{}, target, {}, &err);
    QCOMPARE(written, qint64(devices.size()));

    const QStringList lines = readCsvLines(target);
    QCOMPARE(lines.size(), devices.size() + 1);
    // 同 received_at 按 id(插入序)稳定排序,断言可确定
    QCOMPARE(lines[1], QStringLiteral("'=1+1,1,1.5,°C,1000,100"));
    QCOMPARE(lines[2], QStringLiteral("\"a,b\",1,1.5,°C,1000,100"));
    QCOMPARE(lines[3], QStringLiteral("\"q\"\"x\",1,1.5,°C,1000,100"));
    QCOMPARE(lines[4], QStringLiteral("'+ok,1,1.5,°C,1000,100"));
    QCOMPARE(lines[5], QStringLiteral("'@ref,1,1.5,°C,1000,100"));
    QCOMPARE(lines[6], QStringLiteral("plain,1,1.5,°C,1000,100"));
}

void TestRepository::csvExportCancelledLeavesNoFile()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertBulk(env, "dev1", 1, 1005, 100, &err), qPrintable(err));

    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    const QString target = env.dir->filePath(QStringLiteral("out.csv"));

    // 第一页(1000 行)写完后取消:不算成功,不落文件
    qint64 written = env.repo->exportCsv(
        query, target, [](qint64 rows) { return rows < 1000; }, &err);
    QCOMPARE(written, qint64(-1));
    QVERIFY(!err.isEmpty());
    QVERIFY(!QFileInfo::exists(target));
    // QSaveFile 临时文件也要清干净
    QVERIFY(QDir(env.dir->path())
                .entryList({QStringLiteral("out.csv*")}, QDir::Files)
                .isEmpty());

    // 已有目标文件时取消:旧内容保留,不被半成品替换
    err.clear();
    QFile old(target);
    QVERIFY(old.open(QIODevice::WriteOnly));
    QVERIFY(old.write("OLD") > 0);
    old.close();
    written = env.repo->exportCsv(query, target, [](qint64) { return false; }, &err);
    QCOMPARE(written, qint64(-1));
    QVERIFY(!err.isEmpty());
    QCOMPARE(readCsvLines(target), QStringList{QStringLiteral("OLD")});
}

void TestRepository::csvExportFailureKeepsOldFile()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertBulk(env, "dev1", 1, 1005, 100, &err), qPrintable(err));

    const QString target = env.dir->filePath(QStringLiteral("out.csv"));
    QFile old(target);
    QVERIFY(old.open(QIODevice::WriteOnly));
    QVERIFY(old.write("OLD") > 0);
    old.close();

    // 第一页(1000 行)后经写连接抽走表:第二页查询失败,导出中途失败
    TelemetryQuery query;
    query.deviceId = QStringLiteral("dev1");
    bool dropped = false;
    const qint64 written = env.repo->exportCsv(
        query, target,
        [&env, &dropped](qint64) {
            if (dropped)
                return true;
            dropped = true;
            QSqlQuery drop(env.writer.sql());
            drop.exec(QStringLiteral("DROP TABLE telemetry"));
            return true;
        },
        &err);
    QCOMPARE(written, qint64(-1));
    QVERIFY(!err.isEmpty());
    // 旧文件原样保留,目录里也不留导出临时文件
    QCOMPARE(readCsvLines(target), QStringList{QStringLiteral("OLD")});
    QCOMPARE(QDir(env.dir->path()).entryList({QStringLiteral("out.csv*")}, QDir::Files),
             QStringList{QStringLiteral("out.csv")});
}

void TestRepository::csvExportToInvalidPathFails()
{
    Env env;
    QString err;
    QVERIFY2(makeEnv(&env, {QStringLiteral("dev1")}, &err), qPrintable(err));
    QVERIFY2(insertRow(env, "dev1", 1, 1, 20.5, 1000, 100, &err), qPrintable(err));

    // 目标是目录:打开必败,明确报错不落任何文件
    const qint64 written =
        env.repo->exportCsv(TelemetryQuery{}, env.dir->path(), {}, &err);
    QCOMPARE(written, qint64(-1));
    QVERIFY(!err.isEmpty());
}

void TestRepository::queryOnClosedDatabaseFails()
{
    Database closed;
    Repository repo(&closed);
    TelemetryCursor cursor;
    QString err;
    const auto page = repo.queryPage(TelemetryQuery{}, &cursor, 10, &err);
    QVERIFY(!page.has_value());
    QVERIFY(!err.isEmpty());
}

QTEST_MAIN(TestRepository)
#include "tst_repository.moc"
