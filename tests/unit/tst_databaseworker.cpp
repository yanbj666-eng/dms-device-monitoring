// tst_databaseworker:LLD §7 DB 队列与退出测试
// 覆盖(TC-DB-002/003/004):批量双触发(数量/时间)、失败整批回滚保留重试、
// 重试不改写入队时打点的 received_at、队列满丢新计数、同键 DO NOTHING 保留原值、
// 命令事件 FIFO 生命周期与直写、事件队列满丢弃、未入库设备/未知命令防毒丸、
// 退出排空与超时未提交数上报、致命初始化后的显式丢弃
#include <QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <QtEndian>

#include <functional>
#include <memory>

#include "persistence/DatabaseWorker.h"

using dms::domain::CommandOutcome;
using dms::domain::CommandState;
using dms::domain::CommandSubmission;
using dms::domain::TelemetryBatch;
using dms::persistence::DatabaseWorker;
using dms::persistence::DeviceRow;
using dms::protocol::CommandParam;
using dms::protocol::TelemetrySample;
using dms::protocol::kValueTypeFloat32;
using dms::protocol::kValueTypeUint32;

namespace {

// 独立第二连接:断言行数/造锁;析构先 close 再 removeDatabase(docs/06 §2)
class FixtureDb
{
public:
    explicit FixtureDb(const QString &path)
        : name_(QStringLiteral("fixture_")
                + QUuid::createUuid().toString(QUuid::WithoutBraces))
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
    FixtureDb(const FixtureDb &) = delete;
    FixtureDb &operator=(const FixtureDb &) = delete;

    bool isOpen() const { return opened_; }
    QString name() const { return name_; }
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

DeviceRow deviceRow()
{
    DeviceRow row;
    row.id = QStringLiteral("dev1");
    row.name = QStringLiteral("设备一");
    row.transportType = QStringLiteral("tcp");
    row.configJson = QStringLiteral("{\"id\":\"dev1\"}");
    row.enabled = true;
    return row;
}

TelemetryBatch makeBatch(quint32 seq, int sampleCount, float value = 20.0f,
                         const QUuid &sessionId = QUuid())
{
    TelemetryBatch batch;
    batch.seq = seq;
    batch.timestampMs = 1000 + seq;
    batch.sessionId = sessionId.isNull() ? QUuid::createUuid() : sessionId;
    for (int i = 0; i < sampleCount; ++i) {
        TelemetrySample sample;
        sample.metricId = quint16(1 + (i % 4));
        sample.valueType = kValueTypeFloat32;
        sample.asFloat = value;
        batch.samples.append(sample);
    }
    return batch;
}

CommandSubmission makeSubmission(const QUuid &sessionId, quint32 seq, quint16 commandId = 4)
{
    CommandSubmission submission;
    submission.deviceId = QStringLiteral("dev1");
    submission.sessionId = sessionId;
    submission.seq = seq;
    submission.commandId = commandId;
    if (commandId == 4) {
        CommandParam param;
        param.id = 1;
        param.valueType = kValueTypeUint32;
        param.value = QByteArray(4, Qt::Uninitialized);
        qToBigEndian<quint32>(250, param.value.data());
        submission.params.append(param);
    }
    return submission;
}

CommandOutcome makeOutcome(const QUuid &sessionId, quint32 seq,
                           CommandState state = CommandState::Succeeded)
{
    CommandOutcome outcome;
    outcome.seq = seq;
    outcome.commandId = 4;
    outcome.state = state;
    outcome.ackStatus = 0;
    outcome.sessionId = sessionId;
    return outcome;
}

} // namespace

class TestDatabaseWorker : public QObject
{
    Q_OBJECT

private slots:
    void telemetryFlushedByCount();
    void telemetryFlushedByTimer();
    void queueOverflowDropsNewAndCounts();
    void duplicateKeyKeepsOriginalValue();
    void transactionFailureRetriesWholeBatch();
    void retryKeepsEnqueueTimestamp();
    void commandLifecycleRecorded();
    void commandFinishedWithoutSubmissionIsReported();
    void eventQueueOverflowDrops();
    void drainFlushesAndCloses();
    void drainReportsUncommittedOnPersistentFailure();
    void fatalInitDropsEverything();
    void unknownDeviceTelemetryDropped();
    void unknownCommandIdDropped();

private:
    // 每用例独立:假时钟 + 小参数 worker + 临时库。
    // 假时钟用 shared_ptr:Harness 按值返回发生移动,lambda 捕获不悬空
    struct Harness {
        std::unique_ptr<QTemporaryDir> dir;
        std::shared_ptr<qint64> fakeNow = std::make_shared<qint64>(0);
        std::unique_ptr<DatabaseWorker> worker;
        QString path;
    };

    Harness makeHarness(int batchSamples = 4, int queueSamples = 8, int eventQueueSize = 4,
                        int flushIntervalMs = 100, int shutdownFlushMs = 500)
    {
        Harness h;
        *h.fakeNow = 100000;
        h.dir = std::make_unique<QTemporaryDir>();
        h.path = h.dir->filePath(QStringLiteral("dms.db"));

        dms::persistence::DbWorkerSettings settings;
        settings.databasePath = h.path;
        settings.batchSamples = batchSamples;
        settings.queueSamples = queueSamples;
        settings.eventQueueSize = eventQueueSize;
        settings.flushIntervalMs = flushIntervalMs;
        settings.busyTimeoutMs = 50; // 锁测试里失败要快
        settings.shutdownFlushMs = shutdownFlushMs;

        const std::shared_ptr<qint64> now = h.fakeNow;
        h.worker = std::make_unique<DatabaseWorker>(settings, [now] { return *now; },
                                                    [](int) { /* 测试不真睡 */ });
        h.worker->initialize({deviceRow()});
        return h;
    }

    static void tick(Harness &h, int ms) { *h.fakeNow += ms; }
};

void TestDatabaseWorker::telemetryFlushedByCount()
{
    Harness h = makeHarness();
    QSignalSpy flushSpy(h.worker.get(), &DatabaseWorker::flushCompleted);

    // 4 样本 = batchSamples:数量先到即提交,不必等定时器
    h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(1, 4));
    QCOMPARE(h.worker->pendingSamples(), 0);

    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 4);
    QCOMPARE(flushSpy.count(), 1);
    QCOMPARE(flushSpy.first().at(0).toBool(), true);
    // received_at 用入队时打点;session_id 落库
    QSqlQuery q(QSqlDatabase::database(fx.name()));
    QVERIFY(q.exec(QStringLiteral("SELECT received_at, session_id FROM telemetry LIMIT 1")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toLongLong(), *h.fakeNow);
    QVERIFY(!q.value(1).toString().isEmpty());
}

void TestDatabaseWorker::telemetryFlushedByTimer()
{
    Harness h = makeHarness();
    h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(1, 2)); // 低于批阈值

    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 0);

    tick(h, 100); // 时间触发到点
    h.worker->pollFlush();
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 2);
    QCOMPARE(h.worker->pendingSamples(), 0);

    // 不到点不重复写
    h.worker->pollFlush();
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 2);
}

void TestDatabaseWorker::queueOverflowDropsNewAndCounts()
{
    Harness h = makeHarness(/*batch=*/4, /*queue=*/8);
    // 队列上限只在写入受阻时才起作用:持锁让批写失败,两帧(8 样本)积压满队列,
    // 第三帧撞满丢新 4(TC-DB-004)
    {
        FixtureDb lock(h.path);
        QVERIFY(lock.exec(QStringLiteral("BEGIN EXCLUSIVE")));
        h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(1, 4));
        h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(2, 4));
        QCOMPARE(h.worker->pendingSamples(), 8);
        h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(3, 4));
        QCOMPARE(h.worker->droppedSamples(), quint64(4));
        QCOMPARE(h.worker->pendingSamples(), 8); // 丢新不丢旧
        QCOMPARE(lock.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 0);
    } // 释放锁

    // 恢复:按批节奏分两轮把积压写空
    tick(h, 100);
    h.worker->pollFlush();
    QCOMPARE(h.worker->pendingSamples(), 4);
    tick(h, 100);
    h.worker->pollFlush();
    QCOMPARE(h.worker->pendingSamples(), 0);

    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 8);
    QCOMPARE(h.worker->droppedSamples(), quint64(4));

    // 溢出后队列恢复可用:后续正常入队写出
    h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(4, 2));
    tick(h, 100);
    h.worker->pollFlush();
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 10);
    QCOMPARE(h.worker->droppedSamples(), quint64(4));
}

void TestDatabaseWorker::duplicateKeyKeepsOriginalValue()
{
    Harness h = makeHarness();
    const QUuid session = QUuid::createUuid();
    h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(7, 4, 10.0f, session));
    // 同 device/session/seq/metric 不同值:DO NOTHING,原值不覆盖
    h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(7, 4, 99.0f, session));

    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 4);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT MIN(value) FROM telemetry")).toDouble(), 10.0);
}

void TestDatabaseWorker::transactionFailureRetriesWholeBatch()
{
    Harness h = makeHarness();
    // 外部连接持 EXCLUSIVE 锁:模拟锁库超过 busy_timeout(TC-DB-003)
    {
        FixtureDb lock(h.path);
        QVERIFY(lock.exec(QStringLiteral("BEGIN EXCLUSIVE")));
        h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(1, 4));
        // 整批回滚保留:样本不丢也不落库,采集侧不受影响
        QCOMPARE(h.worker->pendingSamples(), 4);
        QCOMPARE(lock.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 0);
    } // 析构释放锁

    tick(h, 100);
    h.worker->pollFlush();
    QCOMPARE(h.worker->pendingSamples(), 0);
    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 4);
}

void TestDatabaseWorker::retryKeepsEnqueueTimestamp()
{
    Harness h = makeHarness();
    {
        FixtureDb lock(h.path);
        QVERIFY(lock.exec(QStringLiteral("BEGIN EXCLUSIVE")));
        h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(1, 4));
    }
    // 重试发生在很晚:received_at 仍是入队时刻,不是重写时刻
    tick(h, 5000);
    h.worker->pollFlush();
    FixtureDb fx(h.path);
    QSqlQuery q(QSqlDatabase::database(fx.name()));
    QVERIFY(q.exec(QStringLiteral("SELECT received_at FROM telemetry")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toLongLong(), qint64(100000));
}

void TestDatabaseWorker::commandLifecycleRecorded()
{
    Harness h = makeHarness();
    const QUuid session = QUuid::createUuid();
    h.worker->onCommandSubmitted(makeSubmission(session, 1));
    h.worker->onCommandFinished(QStringLiteral("dev1"), makeOutcome(session, 1));

    FixtureDb fx(h.path);
    QSqlQuery q(QSqlDatabase::database(fx.name()));
    QVERIFY(q.exec(QStringLiteral(
        "SELECT command_type, request_json, result, ack_status,"
        " finished_at >= created_at FROM commands")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("SetParameter"));
    QVERIFY(q.value(1).toString().contains(QStringLiteral("\"value\":250")));
    QCOMPARE(q.value(2).toString(), QStringLiteral("Succeeded"));
    QCOMPARE(q.value(3).toInt(), 0); // AckStatus::Success
    QCOMPARE(q.value(4).toInt(), 1);
    QVERIFY(!q.next());
}

void TestDatabaseWorker::commandFinishedWithoutSubmissionIsReported()
{
    Harness h = makeHarness();
    // 终态先于受理(受理被丢过):更新不命中,不崩不造假行
    h.worker->onCommandFinished(QStringLiteral("dev1"),
                                makeOutcome(QUuid::createUuid(), 42));
    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM commands")).toInt(), 0);
    QCOMPARE(h.worker->pendingEvents(), 0);
}

void TestDatabaseWorker::eventQueueOverflowDrops()
{
    Harness h = makeHarness(/*batch=*/4, /*queue=*/8, /*events=*/4);
    const QUuid session = QUuid::createUuid();
    {
        // 锁住使直写失败,事件积压在 FIFO
        FixtureDb lock(h.path);
        QVERIFY(lock.exec(QStringLiteral("BEGIN EXCLUSIVE")));
        for (quint32 seq = 1; seq <= 4; ++seq)
            h.worker->onCommandSubmitted(makeSubmission(session, seq));
        QCOMPARE(h.worker->pendingEvents(), 4);
        // 第 5 条:事件队列满,丢新并计数
        h.worker->onCommandSubmitted(makeSubmission(session, 5));
        QCOMPARE(h.worker->droppedEvents(), quint64(1));
        QCOMPARE(h.worker->pendingEvents(), 4);
    }
    tick(h, 100);
    h.worker->pollFlush();
    QCOMPARE(h.worker->pendingEvents(), 0);
    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM commands")).toInt(), 4);
}

void TestDatabaseWorker::drainFlushesAndCloses()
{
    Harness h = makeHarness();
    QSignalSpy closedSpy(h.worker.get(), &DatabaseWorker::closed);
    h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(1, 2)); // 未到批阈值

    h.worker->drainAndClose();
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(closedSpy.first().at(0).toLongLong(), qint64(0)); // 全部提交
    QCOMPARE(closedSpy.first().at(1).toLongLong(), qint64(0));

    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 2);

    // 关闭后到达:计数丢弃,不伪报成功
    h.worker->onTelemetry(QStringLiteral("dev1"), makeBatch(2, 3));
    QCOMPARE(h.worker->droppedSamples(), quint64(3));
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 2);
}

void TestDatabaseWorker::drainReportsUncommittedOnPersistentFailure()
{
    // 排空预算 300ms/重试节奏 100ms:锁持续持有 → 尝试到预算耗尽后如实上报
    const std::shared_ptr<qint64> fakeNow = std::make_shared<qint64>(qint64(100000));
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dms.db"));

    dms::persistence::DbWorkerSettings settings;
    settings.databasePath = path;
    settings.batchSamples = 4;
    settings.queueSamples = 8;
    settings.eventQueueSize = 4;
    settings.flushIntervalMs = 100;
    settings.busyTimeoutMs = 50;
    settings.shutdownFlushMs = 300;

    DatabaseWorker worker(settings, [fakeNow] { return *fakeNow; },
                          [fakeNow](int ms) { *fakeNow += ms; });
    worker.initialize({deviceRow()});
    QSignalSpy closedSpy(&worker, &DatabaseWorker::closed);

    {
        FixtureDb lock(path);
        QVERIFY(lock.exec(QStringLiteral("BEGIN EXCLUSIVE")));
        worker.onTelemetry(QStringLiteral("dev1"), makeBatch(1, 4));
        worker.drainAndClose();
    }
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(closedSpy.first().at(0).toLongLong(), qint64(4)); // 未提交样本如实上报
    QCOMPARE(closedSpy.first().at(1).toLongLong(), qint64(0));
}

void TestDatabaseWorker::fatalInitDropsEverything()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("occupied"));
    QDir().mkpath(path); // 路径是目录:初始化必败

    dms::persistence::DbWorkerSettings settings;
    settings.databasePath = path;
    settings.busyTimeoutMs = 50;

    DatabaseWorker worker(settings);
    QSignalSpy fatalSpy(&worker, &DatabaseWorker::dbFatal);
    QSignalSpy closedSpy(&worker, &DatabaseWorker::closed);
    worker.initialize({deviceRow()});
    QCOMPARE(fatalSpy.count(), 1);

    // 拒绝一切写入但显式计数:通信/UI 不受影响(docs/06 §5)
    worker.onTelemetry(QStringLiteral("dev1"), makeBatch(1, 4));
    QCOMPARE(worker.droppedSamples(), quint64(4));
    worker.onCommandSubmitted(makeSubmission(QUuid::createUuid(), 1));
    QCOMPARE(worker.droppedEvents(), quint64(1));

    worker.drainAndClose();
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(closedSpy.first().at(0).toLongLong(), qint64(0)); // 没有可提交的
    QCOMPARE(closedSpy.first().at(2).toULongLong(), quint64(4)); // 丢弃数随行上报
}

void TestDatabaseWorker::unknownDeviceTelemetryDropped()
{
    Harness h = makeHarness();
    // 防 FK 毒丸:未 upsert 设备的遥测丢弃计数,不进队堵死重试
    h.worker->onTelemetry(QStringLiteral("ghost"), makeBatch(1, 4));
    QCOMPARE(h.worker->droppedSamples(), quint64(4));
    QCOMPARE(h.worker->pendingSamples(), 0);
    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM telemetry")).toInt(), 0);
}

void TestDatabaseWorker::unknownCommandIdDropped()
{
    Harness h = makeHarness();
    // CHECK 枚举外的命令不落库(会毒化整批重试)
    h.worker->onCommandSubmitted(makeSubmission(QUuid::createUuid(), 1, /*commandId=*/9));
    QCOMPARE(h.worker->droppedEvents(), quint64(1));
    QCOMPARE(h.worker->pendingEvents(), 0);
    FixtureDb fx(h.path);
    QCOMPARE(fx.scalar(QStringLiteral("SELECT COUNT(*) FROM commands")).toInt(), 0);
}

QTEST_MAIN(TestDatabaseWorker)
#include "tst_databaseworker.moc"
