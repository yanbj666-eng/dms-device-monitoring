#include "DatabaseWorker.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>
#include <QVariant>

#include <QtEndian>

#include <algorithm>
#include <cstring>

#include "common/Logging.h"
#include "protocol/MessageCodec.h"

namespace dms::persistence {

namespace {

// 协议 §4.2 命令表:commands 表 CHECK 枚举之外的命令不落库(日志报告)
QString commandTypeName(quint16 commandId)
{
    switch (commandId) {
    case 1: return QStringLiteral("Start");
    case 2: return QStringLiteral("Stop");
    case 3: return QStringLiteral("Reset");
    case 4: return QStringLiteral("SetParameter");
    }
    return QString();
}

// request_json:参数数组 [{id,type,value}];float32/uint32 解码为数值,
// 未知类型退化为十六进制文本,不虚构语义(docs/06 §3)
QString commandRequestJson(const QList<dms::protocol::CommandParam> &params)
{
    QJsonArray array;
    for (const dms::protocol::CommandParam &param : params) {
        QJsonObject item;
        item.insert(QStringLiteral("id"), param.id);
        if (param.valueType == dms::protocol::kValueTypeFloat32
            && param.value.size() >= 4) {
            const quint32 bits = qFromBigEndian<quint32>(param.value.constData());
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            item.insert(QStringLiteral("type"), QStringLiteral("float32"));
            item.insert(QStringLiteral("value"), double(value));
        } else if (param.valueType == dms::protocol::kValueTypeUint32
                   && param.value.size() >= 4) {
            item.insert(QStringLiteral("type"), QStringLiteral("uint32"));
            // QJsonValue 无 quint32 重载,uint32 值域用 qint64 精确承载
            item.insert(QStringLiteral("value"),
                        qint64(qFromBigEndian<quint32>(param.value.constData())));
        } else {
            item.insert(QStringLiteral("type"),
                        QStringLiteral("type_0x%1").arg(param.valueType, 2, 16));
            item.insert(QStringLiteral("value"),
                        QString::fromLatin1(param.value.toHex()));
        }
        array.append(item);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

// 终态列值:Sent 之外的 CommandState 名与表 CHECK 枚举一致;
// 仅 ACK 终态带 ack_status,其余 NULL(0 也是合法 AckStatus,不能混用)
QString terminalResult(dms::domain::CommandState state)
{
    return dms::domain::commandStateName(state);
}

bool isAckTerminal(dms::domain::CommandState state)
{
    return state == dms::domain::CommandState::Succeeded
           || state == dms::domain::CommandState::Rejected;
}

QString sessionKey(const QUuid &sessionId)
{
    return sessionId.toString(QUuid::WithoutBraces);
}

} // namespace

DatabaseWorker::DatabaseWorker(DbWorkerSettings settings, Clock clock, Sleeper sleeper,
                               QObject *parent)
    : QObject(parent)
    , settings_(std::move(settings))
    , clock_(std::move(clock))
    , sleeper_(std::move(sleeper))
{
}

qint64 DatabaseWorker::defaultClock()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void DatabaseWorker::defaultSleeper(int ms)
{
    QThread::msleep(ms);
}

void DatabaseWorker::initialize(QList<dms::persistence::DeviceRow> devices)
{
    const DbInitResult result = database_.initialize(settings_.databasePath,
                                                     settings_.busyTimeoutMs, devices);
    if (!result.ok) {
        // 建库失败保留错误,不反复删除重建;后续数据显式计数丢弃(docs/06 §4)
        fatal_ = true;
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                        {QString(), {}, result.code},
                        QStringLiteral("数据库初始化失败:%1").arg(result.message));
        emit dbFatal(result.code, result.message);
        return;
    }

    for (const DeviceRow &row : devices)
        knownDevices_.insert(row.id);

    // 定时器在本线程创建(与 DeviceSession pollTimer_ 同一懒创建纪律)
    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(settings_.flushIntervalMs);
    connect(flushTimer_, &QTimer::timeout, this, &DatabaseWorker::pollFlush);
    flushTimer_->start();
    lastFlushAtMs_ = clock_();

    dms::logMessage(dms::LogLevel::Info, dms::LogCategory::Db, {},
                    QStringLiteral("数据库就绪:%1(批 %2/队列 %3/事件 %4)")
                        .arg(settings_.databasePath)
                        .arg(settings_.batchSamples)
                        .arg(settings_.queueSamples)
                        .arg(settings_.eventQueueSize));
}

void DatabaseWorker::onTelemetry(QString deviceId, dms::domain::TelemetryBatch batch)
{
    if (stopped_ || fatal_) {
        // 关闭/致命后到达:显式计数,不静默(LLD §7"不伪报成功")
        droppedSamples_ += quint64(batch.samples.size());
        return;
    }
    if (!knownDevices_.contains(deviceId)) {
        // 防 FK 毒丸:未 upsert 的设备写库必违反外键,整批保留会永久堵死
        // 队列;丢弃并大声报告比堵死所有设备的历史更好排查
        dropSample(deviceId, batch.samples.size(), dms::ErrorCode::DbWriteFailed,
                   QStringLiteral("设备未入库,遥测丢弃"));
        return;
    }

    const qint64 receivedAt = clock_();
    int droppedHere = 0;
    for (const dms::protocol::TelemetrySample &sample : batch.samples) {
        // 值对象契约是"仅已知指标 1..4",防御越界防 CHECK 毒丸
        if (sample.metricId < 1 || sample.metricId > 4)
            continue;
        if (qint64(pending_.size()) >= settings_.queueSamples) {
            // 队列满丢新样本(LLD §7):保留已积压的,丢弃后到的
            ++droppedHere;
            continue;
        }
        PendingSample row;
        row.deviceId = deviceId;
        row.sessionId = batch.sessionId;
        row.seq = batch.seq;
        row.metricId = sample.metricId;
        row.value = sample.valueType == dms::protocol::kValueTypeFloat32
                        ? double(sample.asFloat)
                        : double(sample.asUint);
        row.ts = qint64(batch.timestampMs);
        row.receivedAt = receivedAt;
        pending_.push_back(std::move(row));
    }
    if (droppedHere > 0)
        dropSample(deviceId, droppedHere, dms::ErrorCode::DbQueueFull,
                   QStringLiteral("遥测队列满,丢新样本"));

    // 数量触发:先到者触发;失败保留整批等下一轮(LLD §7)。
    // 走 flushOnce:发 flushCompleted 作测试/诊断同步点,并刷新时间锚点
    while (int(pending_.size()) >= settings_.batchSamples) {
        if (!flushOnce())
            break;
    }
}

void DatabaseWorker::onCommandSubmitted(dms::domain::CommandSubmission submission)
{
    if (stopped_ || fatal_) {
        ++droppedEvents_;
        return;
    }
    const QString type = commandTypeName(submission.commandId);
    if (type.isEmpty() || !knownDevices_.contains(submission.deviceId)) {
        // CHECK/FK 之外的命令不落库:显式报告,不进队列毒化重试
        ++droppedEvents_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Db,
                        {submission.deviceId, submission.sessionId},
                        QStringLiteral("命令受理事件无法持久化(commandId=%1)")
                            .arg(submission.commandId));
        return;
    }
    if (int(events_.size()) >= settings_.eventQueueSize) {
        // 事件队列满:拒绝新事件并明确报告未持久化,不无限增长(LLD §7)
        ++droppedEvents_;
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                        {submission.deviceId, submission.sessionId,
                         dms::ErrorCode::DbQueueFull},
                        QStringLiteral("命令事件未持久化(事件队列满)"));
        return;
    }

    CommandEvent event;
    event.submitted = true;
    event.deviceId = submission.deviceId;
    event.sessionId = submission.sessionId;
    event.seq = submission.seq;
    event.atMs = clock_();
    event.commandId = submission.commandId;
    event.params = submission.params;
    events_.push_back(std::move(event));
    tryFlushEventsIfIdle();
}

void DatabaseWorker::onCommandFinished(QString deviceId,
                                        dms::domain::CommandOutcome outcome)
{
    if (stopped_ || fatal_) {
        ++droppedEvents_;
        return;
    }
    if (!knownDevices_.contains(deviceId)) {
        ++droppedEvents_;
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Db,
                        {deviceId, outcome.sessionId},
                        QStringLiteral("命令终态事件无法持久化(设备未入库)"));
        return;
    }
    if (int(events_.size()) >= settings_.eventQueueSize) {
        ++droppedEvents_;
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                        {deviceId, outcome.sessionId, dms::ErrorCode::DbQueueFull},
                        QStringLiteral("命令终态事件未持久化(事件队列满)"));
        return;
    }

    CommandEvent event;
    event.submitted = false;
    event.deviceId = deviceId;
    event.sessionId = outcome.sessionId;
    event.seq = outcome.seq;
    event.atMs = clock_();
    event.finalState = outcome.state;
    event.ackStatus = outcome.ackStatus;
    events_.push_back(std::move(event));
    tryFlushEventsIfIdle();
}

void DatabaseWorker::pollFlush()
{
    if (stopped_ || fatal_ || !database_.isOpen())
        return;
    const qint64 now = clock_();
    if (pending_.empty() && events_.empty()) {
        lastFlushAtMs_ = now;
        return;
    }
    if (now - lastFlushAtMs_ >= settings_.flushIntervalMs)
        flushOnce();
}

bool DatabaseWorker::flushOnce()
{
    const bool telemetryOk = flushTelemetryChunk();
    const bool eventsOk = flushCommandEvents();
    lastFlushAtMs_ = clock_();
    emit flushCompleted(telemetryOk && eventsOk, int(pending_.size()),
                        int(events_.size()));
    return telemetryOk && eventsOk;
}

bool DatabaseWorker::flushTelemetryChunk()
{
    if (pending_.empty())
        return true;
    if (!database_.isOpen())
        return false;

    const int count = std::min(int(pending_.size()), std::max(1, settings_.batchSamples));
    QString error;
    if (!database_.beginImmediate(&error)) {
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                        {QString(), {}, dms::ErrorCode::DbWriteFailed},
                        QStringLiteral("遥测批开事务失败,整批保留重试:%1").arg(error));
        return false;
    }

    QSqlQuery insert(database_.sql());
    insert.prepare(QStringLiteral(
        "INSERT INTO telemetry(device_id,session_id,seq,metric_id,value,ts,received_at)"
        " VALUES(?,?,?,?,?,?,?)"
        " ON CONFLICT(device_id,session_id,seq,metric_id) DO NOTHING"));
    int written = 0;
    for (auto it = pending_.begin(); it != pending_.begin() + count; ++it) {
        insert.bindValue(0, it->deviceId);
        insert.bindValue(1, sessionKey(it->sessionId));
        insert.bindValue(2, it->seq);
        insert.bindValue(3, it->metricId);
        insert.bindValue(4, it->value);
        insert.bindValue(5, it->ts);
        insert.bindValue(6, it->receivedAt);
        if (!insert.exec()) {
            // 任一失败整批回滚保留(docs/06 §3);重复键不算失败
            const QString detail = insert.lastError().text();
            database_.rollback();
            dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                            {it->deviceId, it->sessionId, dms::ErrorCode::DbWriteFailed},
                            QStringLiteral("遥测批写入失败已回滚,整批保留重试:%1")
                                .arg(detail));
            return false;
        }
        if (insert.numRowsAffected() == 0)
            reportDuplicate(*it); // 罕见路径:同键重交付
        ++written;
    }
    if (!database_.commit(&error)) {
        database_.rollback();
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                        {QString(), {}, dms::ErrorCode::DbWriteFailed},
                        QStringLiteral("遥测批提交失败已回滚:%1").arg(error));
        return false;
    }
    pending_.erase(pending_.begin(), pending_.begin() + written);
    return true;
}

void DatabaseWorker::reportDuplicate(const PendingSample &sample)
{
    // 只比 value 与 ts:received_at 是到达时刻,重交付天然不同,不参与"内容"
    QSqlQuery query(database_.sql());
    query.prepare(QStringLiteral(
        "SELECT value, ts FROM telemetry WHERE device_id=? AND session_id=?"
        " AND seq=? AND metric_id=?"));
    query.bindValue(0, sample.deviceId);
    query.bindValue(1, sessionKey(sample.sessionId));
    query.bindValue(2, sample.seq);
    query.bindValue(3, sample.metricId);
    if (!query.exec() || !query.next())
        return;
    const bool sameContent = query.value(0).toDouble() == sample.value
                             && query.value(1).toLongLong() == sample.ts;
    if (!sameContent) {
        // 同键不同内容:日志报告,不覆盖原值(docs/06 §3)
        dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Db,
                        {sample.deviceId, sample.sessionId},
                        QStringLiteral("遥测重复键内容不同,保留原值:"
                                       "seq=%1 metric=%2 新值=%3 旧值=%4")
                            .arg(sample.seq)
                            .arg(sample.metricId)
                            .arg(sample.value)
                            .arg(query.value(0).toDouble()));
    }
}

bool DatabaseWorker::flushCommandEvents()
{
    if (events_.empty())
        return true;
    if (!database_.isOpen())
        return false;

    QString error;
    if (!database_.beginImmediate(&error)) {
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                        {QString(), {}, dms::ErrorCode::DbWriteFailed},
                        QStringLiteral("命令事件开事务失败,整批保留重试:%1").arg(error));
        return false;
    }

    QSqlQuery insert(database_.sql());
    insert.prepare(QStringLiteral(
        "INSERT INTO commands(device_id,session_id,seq,command_type,request_json,"
        "result,ack_status,created_at,finished_at)"
        " VALUES(?,?,?,?,?,'Sent',NULL,?,NULL)"
        " ON CONFLICT(device_id,session_id,seq) DO NOTHING"));
    QSqlQuery update(database_.sql());
    update.prepare(QStringLiteral(
        "UPDATE commands SET result=:result, ack_status=:ack,"
        " finished_at=MAX(:finished, created_at)"
        " WHERE device_id=:dev AND session_id=:session AND seq=:seq"));

    for (const CommandEvent &event : events_) {
        bool ok = false;
        if (event.submitted) {
            insert.bindValue(0, event.deviceId);
            insert.bindValue(1, sessionKey(event.sessionId));
            insert.bindValue(2, event.seq);
            insert.bindValue(3, commandTypeName(event.commandId));
            insert.bindValue(4, commandRequestJson(event.params));
            insert.bindValue(5, event.atMs);
            ok = insert.exec();
        } else {
            update.bindValue(QStringLiteral(":result"), terminalResult(event.finalState));
            update.bindValue(QStringLiteral(":ack"),
                             isAckTerminal(event.finalState)
                                 ? QVariant::fromValue(event.ackStatus)
                                 : QVariant());
            update.bindValue(QStringLiteral(":finished"), event.atMs);
            update.bindValue(QStringLiteral(":dev"), event.deviceId);
            update.bindValue(QStringLiteral(":session"), sessionKey(event.sessionId));
            update.bindValue(QStringLiteral(":seq"), event.seq);
            ok = update.exec();
            if (ok && update.numRowsAffected() == 0) {
                // 受理行被丢过(队列满)或库重开过:诚实报告,不算失败
                dms::logMessage(dms::LogLevel::Warn, dms::LogCategory::Db,
                                {event.deviceId, event.sessionId},
                                QStringLiteral("命令终态更新未命中:seq=%1")
                                    .arg(event.seq));
            }
        }
        if (!ok) {
            const QString detail = event.submitted ? insert.lastError().text()
                                                   : update.lastError().text();
            database_.rollback();
            dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                            {event.deviceId, event.sessionId,
                             dms::ErrorCode::DbWriteFailed},
                            QStringLiteral("命令事件写入失败已回滚,整批保留重试:%1")
                                .arg(detail));
            return false;
        }
    }
    if (!database_.commit(&error)) {
        database_.rollback();
        dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                        {QString(), {}, dms::ErrorCode::DbWriteFailed},
                        QStringLiteral("命令事件提交失败已回滚:%1").arg(error));
        return false;
    }
    events_.clear();
    return true;
}

void DatabaseWorker::tryFlushEventsIfIdle()
{
    // 直写低延迟路径:仅当 FIFO 里只有刚到的这一条(此前无积压)。
    // 积压非空说明上一轮失败等重试,只追加保持顺序
    if (int(events_.size()) == 1 && !stopped_ && !fatal_ && database_.isOpen())
        flushCommandEvents();
}

void DatabaseWorker::dropSample(const QString &deviceId, int count,
                                dms::ErrorCode code, const QString &reason)
{
    droppedSamples_ += quint64(count);
    dms::logMessage(dms::LogLevel::Error, dms::LogCategory::Db,
                    {deviceId, {}, code},
                    QStringLiteral("%1:%2 个样本(累计 %3)")
                        .arg(reason)
                        .arg(count)
                        .arg(droppedSamples_));
}

void DatabaseWorker::drainAndClose()
{
    if (stopped_)
        return;
    stopped_ = true; // 先立标志:此后到达的数据只计数丢弃

    if (!fatal_ && database_.isOpen()) {
        const qint64 deadline = clock_() + settings_.shutdownFlushMs;
        while ((!pending_.empty() || !events_.empty()) && clock_() < deadline) {
            flushOnce();
            if (pending_.empty() && events_.empty())
                break;
            // 失败按 flush 节奏重试,不超预算(LLD §7:最多等 shutdown_flush_ms)
            const qint64 remaining = deadline - clock_();
            if (remaining > 0)
                sleeper_(int(std::min<qint64>(settings_.flushIntervalMs, remaining)));
        }
    }
    // 未提交数显式上报:超时报告而不是伪报成功(LLD §7)
    emit closed(qint64(pending_.size()), qint64(events_.size()),
                droppedSamples_, droppedEvents_);
    closeDatabase();
}

void DatabaseWorker::closeDatabase()
{
    if (flushTimer_) {
        // 本线程直接删:drainAndClose 之后线程事件循环可能随即退出,
        // deleteLater 不保证执行
        flushTimer_->stop();
        delete flushTimer_;
        flushTimer_ = nullptr;
    }
    database_.close();
}

} // namespace dms::persistence
