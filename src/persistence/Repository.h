#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <optional>

#include "persistence/Database.h"

namespace dms::persistence {

// 历史查询条件(REQ-DB-002):按设备/指标过滤 + received_at 半开区间 [startMs, endMs)。
// endMs 为 nullopt 表示上界不限(仍受首查 id 边界约束,见 TelemetryCursor);
// 设备时钟不可信,查询一律依据接收时间(docs/06 §1)
struct TelemetryQuery {
    QString deviceId;      // 空 = 全部设备
    quint16 metricId = 0;  // 0 = 全部指标;1..4 过滤单指标
    qint64 startMs = 0;
    std::optional<qint64> endMs;
};

// 一行遥测(查询/导出共用形状;session_id 不在导出列,不取)
struct TelemetryRow {
    qint64 id = 0;
    QString deviceId;
    quint32 seq = 0;
    quint16 metricId = 0;
    double value = 0.0;
    qint64 ts = 0;         // 设备时间,0=未知
    qint64 receivedAt = 0; // 客户端接收时间(查询排序键)
};

// 分页游标:一个值对象代表一次查询会话(docs/06 §3)。默认构造 = 首次查询。
//   - 首查捕获当时 MAX(id) 作为本次会话边界:翻页期间新写入的行 id 更大,
//     不会混入后续页,避免重复/漏行;
//   - 之后每页用上页末行 (received_at,id) 做键集游标续读,不使用 OFFSET
//     (深翻页时 OFFSET 每页都要扫过并丢弃前面所有行);
//   - id 单调依赖"本项目不清历史"(docs/06 §5):若未来引入删除,
//     需同时改为 AUTOINCREMENT 或换边界策略
struct TelemetryCursor {
    qint64 boundaryId = -1;     // <0 = 尚未首查
    qint64 lastReceivedAt = -1; // 上页末行;received_at>=0,-1 先于一切行
    qint64 lastId = 0;
};

struct TelemetryPage {
    QList<TelemetryRow> rows;
    bool hasMore = false;
};

// 历史查询与 CSV 导出(docs/06 §3)。非拥有 Database 引用,在单个线程内使用
// (通常是发起查询的线程);与写线程的 DatabaseWorker 并发由 WAL 读写不互斥保证。
// 线程策略(REQ-THREAD-001/002)由装配层决定,本类不建线程不发信号
class Repository
{
public:
    static constexpr int maxPageRows = 1000; // 每页行数上限(docs/06 §3)

    explicit Repository(Database *database);

    // 分页查询。失败返回 nullopt 且 *error 非空(已记结构化日志);
    // 成功可返回空页。maxRows 钳制到 [1, maxPageRows]
    std::optional<TelemetryPage> queryPage(const TelemetryQuery &query,
                                           TelemetryCursor *cursor, int maxRows,
                                           QString *error = nullptr);

    // CSV 导出:与查询同一过滤/排序/首查边界,按页写出,不长期持有读事务
    // (docs/06 §5)。返回写出行数(不含表头);失败/取消返回 -1 且 *error 非空,
    // 目标文件保持原状(QSaveFile 临时文件 + commit 原子替换,docs/06 §3)。
    // shouldContinue 每页回调一次,返回 false 取消导出
    qint64 exportCsv(const TelemetryQuery &query, const QString &targetPath,
                     const std::function<bool(qint64 rowsWritten)> &shouldContinue = {},
                     QString *error = nullptr);

    // 指标单位(导出 unit 列;与 UI 展示同一映射,docs/09 §4),未知指标返回空
    static QString metricUnit(quint16 metricId);

private:
    bool fetchBoundary(TelemetryCursor *cursor, QString *error);

    Database *database_; // 非拥有
};

} // namespace dms::persistence
