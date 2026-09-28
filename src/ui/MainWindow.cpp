#include "MainWindow.h"

#include <QDateTime>
#include <QDockWidget>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListView>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QtEndian> // qToBigEndian:MSVC/Qt6.8 经传递包含可见,g++/Qt6.4.2 必须显式包含
#include <QTableWidget>
#include <QToolBar>
#include <QTimer>

#include "CurveWidget.h"
#include "TelemetryBuffer.h"

namespace dms::ui {

namespace {

// 指标呈现元数据(docs/05 §4.1 指标表);颜色仅显示层约定
struct MetricViewInfo {
    quint16 metricId;
    const char *name;
    const char *unit;
    QColor color;
};

const MetricViewInfo kMetrics[4] = {
    {1, "temperature", "°C", QColor(0xc0, 0x39, 0x2b)},
    {2, "pressure",    "kPa", QColor(0x35, 0x74, 0xc4)},
    {3, "voltage",     "V",   QColor(0xd9, 0x8b, 0x1e)},
    {4, "rpm",         "r/min", QColor(0x2e, 0xa0, 0x43)},
};

// ---- 告警面板显示约定(LLD §8;文案只在 UI 层,不回写域对象)----

constexpr int kMaxAlarmRows = 500; // 面板是活动视图不是历史存档,DB 才是
enum AlarmColumn { ColTime = 0, ColDevice, ColMetric, ColCode, ColLevel,
                   ColStatus, ColAck, ColCount };

// 行键:与引擎/DB 的 (device_id, code, metric_id) 同口径;
// 控制字符分隔避免设备 id 与 code 含常见分隔符时拼错
QString alarmKey(const QString &deviceId, const QString &code, quint16 metricId)
{
    return deviceId + QChar(0x1f) + code + QChar(0x1f) + QString::number(metricId);
}

QString alarmMetricName(quint16 metricId)
{
    switch (metricId) {
    case 0: return MainWindow::tr("设备级");
    case 1: return MainWindow::tr("温度");
    case 2: return MainWindow::tr("压力");
    case 3: return MainWindow::tr("电压");
    case 4: return MainWindow::tr("转速");
    }
    return QString::number(metricId);
}

QString alarmLevelName(quint8 level)
{
    switch (level) {
    case dms::domain::AlarmLevel::Info:  return MainWindow::tr("信息");
    case dms::domain::AlarmLevel::Warn:  return MainWindow::tr("警告");
    case dms::domain::AlarmLevel::Error: return MainWindow::tr("错误");
    }
    return QString::number(level);
}

QString alarmStatusActive() { return MainWindow::tr("活跃"); }
QString alarmStatusCleared() { return MainWindow::tr("已恢复"); }
QString alarmAckPending() { return MainWindow::tr("待确认"); }
QString alarmAckDone() { return MainWindow::tr("已确认"); }

} // namespace

MainWindow::MainWindow(int refreshIntervalMs, int curvePoints,
                       const QVector<QPair<QString, QString>> &deviceIdAndNames,
                       QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("DMS 设备监控"));
    resize(1100, 700);

    // ---- 左侧:设备列表 ----
    model_ = new DeviceListModel(this);
    model_->setDevices(deviceIdAndNames);
    deviceList_ = new QListView(this);
    deviceList_->setModel(model_);
    deviceList_->setMinimumWidth(240);
    connect(deviceList_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &MainWindow::onSelectionChanged);
    for (const auto &pair : deviceIdAndNames)
        buffers_.insert(pair.first, new TelemetryBuffer(curvePoints));

    // ---- 右侧:四指标曲线 2×2 ----
    auto *curveGrid = new QWidget(this);
    auto *grid = new QGridLayout(curveGrid);
    grid->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 4; ++i) {
        curves_[i] = new CurveWidget(QString::fromUtf8(kMetrics[i].name),
                                     QString::fromUtf8(kMetrics[i].unit),
                                     kMetrics[i].color, curveGrid);
        grid->addWidget(curves_[i], i / 2, i % 2);
    }

    auto *splitter = new QSplitter(this);
    splitter->addWidget(deviceList_);
    splitter->addWidget(curveGrid);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    // ---- 底部:告警面板(S6,可停靠/可关闭;DB 才是历史,面板只管眼前)----
    alarmTable_ = new QTableWidget(0, ColCount, this);
    alarmTable_->setHorizontalHeaderLabels(
        {tr("时间"), tr("设备"), tr("指标"), tr("代码"), tr("等级"),
         tr("状态"), tr("确认")});
    alarmTable_->verticalHeader()->setVisible(false);
    alarmTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    alarmTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    alarmTable_->setAlternatingRowColors(true);
    alarmTable_->setColumnWidth(ColTime, 90);
    alarmTable_->setColumnWidth(ColDevice, 110);
    alarmTable_->setColumnWidth(ColMetric, 70);
    alarmTable_->setColumnWidth(ColCode, 130);
    alarmTable_->setColumnWidth(ColLevel, 60);
    alarmTable_->setColumnWidth(ColStatus, 70);
    alarmTable_->horizontalHeader()->setStretchLastSection(true);
    auto *alarmDock = new QDockWidget(tr("告警"), this);
    alarmDock->setWidget(alarmTable_);
    alarmDock->setFeatures(QDockWidget::DockWidgetClosable
                           | QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::BottomDockWidgetArea, alarmDock);

    // ---- 工具栏:启停与命令 ----
    auto *toolbar = addToolBar(tr("控制"));
    toolbar->setMovable(false);
    auto *connectAll = toolbar->addAction(tr("全部连接"));
    auto *disconnectAll = toolbar->addAction(tr("全部断开"));
    toolbar->addSeparator();
    auto *resetAction = toolbar->addAction(tr("Reset"));
    toolbar->addWidget(new QLabel(tr(" 采样周期(ms):"), toolbar));
    periodSpin_ = new QSpinBox(toolbar);
    periodSpin_->setRange(100, 1000);
    periodSpin_->setSingleStep(50);
    periodSpin_->setValue(100);
    toolbar->addWidget(periodSpin_);
    auto *applyPeriod = toolbar->addAction(tr("应用周期"));
    toolbar->addSeparator();
    auto *ackAction = toolbar->addAction(tr("确认告警")); // 选中面板行 → 引擎裁决

    connect(connectAll, &QAction::triggered, this, &MainWindow::startAllRequested);
    connect(disconnectAll, &QAction::triggered, this, &MainWindow::stopAllRequested);
    connect(resetAction, &QAction::triggered, this, &MainWindow::sendReset);
    connect(applyPeriod, &QAction::triggered, this, &MainWindow::sendSetPeriod);
    connect(ackAction, &QAction::triggered, this, &MainWindow::acknowledgeSelectedAlarm);

    // ---- 100ms 合并刷新 ----
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(refreshIntervalMs);
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::refreshCurves);
    refreshTimer_->start();

    statusBar()->showMessage(tr("就绪"));
    alarmLabel_ = new QLabel(statusBar());
    statusBar()->addPermanentWidget(alarmLabel_);
    onlineLabel_ = new QLabel(statusBar());
    statusBar()->addPermanentWidget(onlineLabel_);
    updateStatusBar();
    refreshAlarmCount();

    if (model_->rowCount() > 0)
        deviceList_->setCurrentIndex(model_->index(0));
}

void MainWindow::onDeviceStateChanged(QString deviceId, dms::domain::SessionState state)
{
    model_->updateState(deviceId, state);
    updateStatusBar();
}

void MainWindow::onDeviceTelemetry(QString deviceId, dms::domain::TelemetryBatch batch)
{
    TelemetryBuffer *buffer = buffers_.value(deviceId, nullptr);
    if (!buffer)
        return; // 未配置设备(理论上不会:装配时已建)
    buffer->append(batch.samples);
    model_->touchTelemetry(deviceId);
}

void MainWindow::onDeviceCommandFinished(QString deviceId,
                                         dms::domain::CommandOutcome outcome)
{
    statusBar()->showMessage(tr("%1 命令 %2 #%3:%4")
                                 .arg(deviceId, QString::number(outcome.commandId),
                                      QString::number(outcome.seq),
                                      commandStateName(outcome.state)),
                             5000);
}

void MainWindow::onCommandRejected(QString deviceId, quint16 commandId, QString reason)
{
    statusBar()->showMessage(
        tr("%1 命令 %2 被拒:%3").arg(deviceId, QString::number(commandId), reason), 5000);
}

// 引擎出口消费:新事件插顶,恢复/确认改锚定行;行数封顶丢弃最旧
// (面板是活动视图,DB 保留全史;跨行乱序安全:恢复先于确认也能各自命中)
void MainWindow::onAlarmEvent(dms::domain::AlarmEvent event)
{
    upsertAlarmRow(event);
    while (alarmTable_->rowCount() > kMaxAlarmRows)
        alarmTable_->removeRow(alarmTable_->rowCount() - 1);
    refreshAlarmCount();
}

void MainWindow::upsertAlarmRow(const dms::domain::AlarmEvent &event)
{
    const QString key = alarmKey(event.deviceId, event.code, event.metricId);
    switch (event.kind) {
    case dms::domain::AlarmEvent::Kind::Triggered: {
        alarmTable_->insertRow(0);
        auto *timeItem = new QTableWidgetItem(
            QDateTime::fromMSecsSinceEpoch(event.atMs).toString(QStringLiteral(
                "MM-dd hh:mm:ss")));
        timeItem->setData(Qt::UserRole, key); // 行键:确认请求原样带回
        alarmTable_->setItem(0, ColTime, timeItem);
        alarmTable_->setItem(0, ColDevice,
                             new QTableWidgetItem(event.deviceId));
        alarmTable_->setItem(0, ColMetric,
                             new QTableWidgetItem(alarmMetricName(event.metricId)));
        alarmTable_->setItem(0, ColCode, new QTableWidgetItem(event.code));
        alarmTable_->setItem(0, ColLevel,
                             new QTableWidgetItem(alarmLevelName(event.level)));
        alarmTable_->setItem(0, ColStatus,
                             new QTableWidgetItem(alarmStatusActive()));
        alarmTable_->setItem(0, ColAck,
                             new QTableWidgetItem(alarmAckPending()));
        break;
    }
    case dms::domain::AlarmEvent::Kind::Cleared: {
        const int row = findAlarmRow(key, ColStatus, alarmStatusActive());
        if (row >= 0)
            alarmTable_->item(row, ColStatus)->setText(alarmStatusCleared());
        break;
    }
    case dms::domain::AlarmEvent::Kind::Acknowledged: {
        // 引擎裁决只认一条,UI 同步显示;未命中 = 行被顶出面板,不必追问
        const int row = findAlarmRow(key, ColAck, alarmAckPending());
        if (row >= 0)
            alarmTable_->item(row, ColAck)->setText(alarmAckDone());
        break;
    }
    }
}

int MainWindow::findAlarmRow(const QString &key, int anchorColumn,
                             const QString &expected) const
{
    for (int row = 0; row < alarmTable_->rowCount(); ++row) {
        const QTableWidgetItem *timeItem = alarmTable_->item(row, ColTime);
        const QTableWidgetItem *anchor = alarmTable_->item(row, anchorColumn);
        if (!timeItem || !anchor)
            continue;
        if (timeItem->data(Qt::UserRole).toString() != key)
            continue;
        if (expected.isEmpty() || anchor->text() == expected)
            return row; // 自顶向下:最近一条先命中
    }
    return -1;
}

// 确认走引擎裁决而非本地置位:键命中与否、确认哪条记录都由引擎定,
// 结果经 alarmEvent 回流再改表格(同键 Reconnecting 重复触发也能对上)
void MainWindow::acknowledgeSelectedAlarm()
{
    const int row = alarmTable_->currentRow();
    if (row < 0) {
        statusBar()->showMessage(tr("请先在告警面板中选择一行"), 3000);
        return;
    }
    const QTableWidgetItem *timeItem = alarmTable_->item(row, ColTime);
    if (!timeItem)
        return;
    const QStringList key = timeItem->data(Qt::UserRole).toString()
                                .split(QChar(0x1f));
    if (key.size() != 3)
        return;
    emit alarmAckRequested(key.at(0), key.at(1),
                           quint16(key.at(2).toUInt()));
}

void MainWindow::refreshAlarmCount()
{
    // 只数活跃:已恢复行是历史注脚,不参与"还有多少要处理"
    int active = 0;
    for (int row = 0; row < alarmTable_->rowCount(); ++row) {
        const QTableWidgetItem *status = alarmTable_->item(row, ColStatus);
        if (status && status->text() == alarmStatusActive())
            ++active;
    }
    alarmLabel_->setText(tr("活跃告警 %1").arg(active));
}

void MainWindow::refreshCurves()
{
    // 只重绘当前选中设备的曲线;其他设备缓冲照常积累(HLD §3 隔离)
    if (!selectedDevice_.isEmpty()) {
        TelemetryBuffer *buffer = buffers_.value(selectedDevice_, nullptr);
        if (buffer) {
            for (int i = 0; i < 4; ++i)
                curves_[i]->setSeries(buffer->series(kMetrics[i].metricId));
        }
    }
    model_->refreshTimes();
}

void MainWindow::onSelectionChanged()
{
    const QModelIndex current = deviceList_->currentIndex();
    selectedDevice_ = current.isValid() ? model_->deviceIdAt(current.row()) : QString();
    refreshCurves();
}

void MainWindow::sendReset()
{
    if (selectedDevice_.isEmpty())
        return;
    emit sendCommandRequested(selectedDevice_, 3, {});
}

void MainWindow::sendSetPeriod()
{
    if (selectedDevice_.isEmpty())
        return;
    dms::protocol::CommandParam param;
    param.id = 1;
    param.valueType = dms::protocol::kValueTypeUint32;
    param.value = QByteArray(4, Qt::Uninitialized);
    qToBigEndian<quint32>(quint32(periodSpin_->value()),
                          param.value.data()); // TLV 值大端(协议 §4.1)
    emit sendCommandRequested(selectedDevice_, 4, {param});
}

void MainWindow::updateStatusBar()
{
    // Online/Degraded 都算"在传数据",Connecting/Reconnecting 算离线过渡态
    onlineCount_ = model_->onlineCount();
    onlineLabel_->setText(tr("在线 %1/%2").arg(onlineCount_).arg(model_->rowCount()));
}

} // namespace dms::ui
