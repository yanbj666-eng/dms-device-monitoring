#include "Repository.h"

#include <QFile>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "common/Logging.h"

namespace dms::persistence {

namespace {

// 读路径失败统一出口:调用方拿 *error,日志带 DB_READ_FAILED 结构化字段
void failQuery(QString *error, const QString &what, const QString &detail)
{
    const QString message = QStringLiteral("%1: %2").arg(what, detail);
    if (error)
        *error = message;
    dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                    {QString(), {}, dms::ErrorCode::DbReadFailed}, message);
}

// CSV 文本字段转义(docs/06 §3):
//   - 以 = + - @ 开头的文本前置单引号,防电子表格当公式执行(数值列不经此函数,
//     仍按数值输出,负数不被误伤);
//   - 含逗号/引号/换行的字段按 CSV 引号规则转义,引号翻倍
QString csvTextField(const QString &field)
{
    QString out = field;
    if (out.startsWith(QLatin1Char('=')) || out.startsWith(QLatin1Char('+'))
        || out.startsWith(QLatin1Char('-')) || out.startsWith(QLatin1Char('@')))
        out.prepend(QLatin1Char('\''));
    if (out.contains(QLatin1Char(',')) || out.contains(QLatin1Char('"'))
        || out.contains(QLatin1Char('\n')) || out.contains(QLatin1Char('\r')))
        out = QLatin1Char('"')
              + QString(out).replace(QLatin1Char('"'), QStringLiteral("\"\""))
              + QLatin1Char('"');
    return out;
}

// 数值列按可往返精度输出:float32 指标 9 位有效数字(FLT_DECIMAL_DIG)足够还原;
// uint32 指标(4=rpm)值域到 4294967295,需 10 位才不丢精度。
// 默认 'g' 6 位会截断遥测精度,不能用
QString csvValueField(quint16 metricId, double value)
{
    return QString::number(value, 'g', metricId == 4 ? 10 : 9);
}

} // namespace

Repository::Repository(Database *database)
    : database_(database)
{
}

QString Repository::metricUnit(quint16 metricId)
{
    switch (metricId) {
    case 1: return QStringLiteral("°C");
    case 2: return QStringLiteral("kPa");
    case 3: return QStringLiteral("V");
    case 4: return QStringLiteral("r/min");
    }
    return QString();
}

bool Repository::fetchBoundary(TelemetryCursor *cursor, QString *error)
{
    QSqlQuery query(database_->sql());
    if (!query.exec(QStringLiteral("SELECT MAX(id) FROM telemetry"))) {
        failQuery(error, QStringLiteral("读取查询边界失败"), query.lastError().text());
        return false;
    }
    // 空表 MAX 为 NULL → 0:id<=0 匹配不到任何行,自然返回空结果
    cursor->boundaryId = query.next() ? query.value(0).toLongLong() : 0;
    return true;
}

std::optional<TelemetryPage> Repository::queryPage(const TelemetryQuery &query,
                                                   TelemetryCursor *cursor, int maxRows,
                                                   QString *error)
{
    if (!database_ || !database_->isOpen()) {
        failQuery(error, QStringLiteral("查询失败"), QStringLiteral("数据库未打开"));
        return std::nullopt;
    }
    if (cursor->boundaryId < 0 && !fetchBoundary(cursor, error))
        return std::nullopt;

    // LIMIT+1 探测 hasMore:多取一行,有第 limit 行说明还有下一页
    const int limit = qBound(1, maxRows, maxPageRows) + 1;

    // 可选过滤拼的是语句结构,值全部走绑定参数(docs/06 §3 不拼值);
    // ":param IS NULL OR ..." 的写法会让 SQLite 放弃 device/metric 索引
    QString sql = QStringLiteral(
        "SELECT id,device_id,seq,metric_id,value,ts,received_at FROM telemetry"
        " WHERE received_at >= :start AND id <= :boundary"
        " AND (received_at > :curRa OR (received_at = :curRa AND id > :curId))");
    if (query.endMs.has_value())
        sql += QStringLiteral(" AND received_at < :end");
    if (!query.deviceId.isEmpty())
        sql += QStringLiteral(" AND device_id = :device");
    if (query.metricId != 0)
        sql += QStringLiteral(" AND metric_id = :metric");
    sql += QStringLiteral(" ORDER BY received_at, id LIMIT :limit");

    QSqlQuery statement(database_->sql());
    if (!statement.prepare(sql)) {
        failQuery(error, QStringLiteral("准备查询失败"), statement.lastError().text());
        return std::nullopt;
    }
    statement.bindValue(QStringLiteral(":start"), query.startMs);
    statement.bindValue(QStringLiteral(":boundary"), cursor->boundaryId);
    statement.bindValue(QStringLiteral(":curRa"), cursor->lastReceivedAt);
    statement.bindValue(QStringLiteral(":curId"), cursor->lastId);
    if (query.endMs.has_value())
        statement.bindValue(QStringLiteral(":end"), *query.endMs);
    if (!query.deviceId.isEmpty())
        statement.bindValue(QStringLiteral(":device"), query.deviceId);
    if (query.metricId != 0)
        statement.bindValue(QStringLiteral(":metric"), query.metricId);
    statement.bindValue(QStringLiteral(":limit"), limit);

    if (!statement.exec()) {
        failQuery(error, QStringLiteral("执行查询失败"), statement.lastError().text());
        return std::nullopt;
    }

    TelemetryPage page;
    while (statement.next()) {
        TelemetryRow row;
        row.id = statement.value(0).toLongLong();
        row.deviceId = statement.value(1).toString();
        row.seq = statement.value(2).toUInt();
        row.metricId = statement.value(3).toUInt();
        row.value = statement.value(4).toDouble();
        row.ts = statement.value(5).toLongLong();
        row.receivedAt = statement.value(6).toLongLong();
        page.rows.append(row);
    }
    if (page.rows.size() >= limit) {
        page.hasMore = true;
        page.rows.removeLast(); // 探测行留给下一页
    }
    if (!page.rows.isEmpty()) {
        const TelemetryRow &last = page.rows.last();
        cursor->lastReceivedAt = last.receivedAt;
        cursor->lastId = last.id;
    }
    return page;
}

qint64 Repository::exportCsv(const TelemetryQuery &query, const QString &targetPath,
                             const std::function<bool(qint64)> &shouldContinue,
                             QString *error)
{
    // QSaveFile:先写同目录临时文件,commit 才原子替换目标;
    // 任何失败/取消显式 cancel 丢弃,不留下伪成功文件(docs/06 §3)
    QSaveFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        failQuery(error, QStringLiteral("打开导出文件失败"), file.errorString());
        return -1;
    }
    // UTF-8 BOM:Excel 依赖 BOM 才按 UTF-8 解码,否则 °C 单位列乱码
    file.write(QByteArrayLiteral("\xEF\xBB\xBF"));
    file.write("device_id,metric_id,value,unit,ts,received_at\n");

    TelemetryCursor cursor; // 导出会话自己的边界:从导出开始的快照续读
    qint64 written = 0;
    forever {
        const std::optional<TelemetryPage> page =
            queryPage(query, &cursor, maxPageRows, error);
        if (!page.has_value()) {
            file.cancelWriting();
            return -1;
        }
        for (const TelemetryRow &row : page->rows) {
            // 文本列过转义/公式防护;数值列按数值输出(docs/06 §3)
            const QString line =
                QStringLiteral("%1,%2,%3,%4,%5,%6\n")
                    .arg(csvTextField(row.deviceId), QString::number(row.metricId),
                         csvValueField(row.metricId, row.value),
                         csvTextField(metricUnit(row.metricId)),
                         QString::number(row.ts), QString::number(row.receivedAt));
            if (file.write(line.toUtf8()) < 0) {
                failQuery(error, QStringLiteral("写入导出文件失败"), file.errorString());
                file.cancelWriting();
                return -1;
            }
            ++written;
        }
        if (shouldContinue && !shouldContinue(written)) {
            if (error)
                *error = QStringLiteral("导出已取消");
            dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Db, {},
                            QStringLiteral("CSV 导出已取消,已写 %1 行").arg(written));
            file.cancelWriting();
            return -1;
        }
        if (!page->hasMore)
            break;
    }
    if (!file.commit()) {
        failQuery(error, QStringLiteral("导出提交失败"), file.errorString());
        return -1;
    }
    return written;
}

} // namespace dms::persistence
