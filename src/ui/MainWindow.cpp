#include "MainWindow.h"

#include <QGridLayout>
#include <QLabel>
#include <QListView>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QTimer>

#include <QDateTime>

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

    connect(connectAll, &QAction::triggered, this, &MainWindow::startAllRequested);
    connect(disconnectAll, &QAction::triggered, this, &MainWindow::stopAllRequested);
    connect(resetAction, &QAction::triggered, this, &MainWindow::sendReset);
    connect(applyPeriod, &QAction::triggered, this, &MainWindow::sendSetPeriod);

    // ---- 100ms 合并刷新 ----
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(refreshIntervalMs);
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::refreshCurves);
    refreshTimer_->start();

    statusBar()->showMessage(tr("就绪"));
    onlineLabel_ = new QLabel(statusBar());
    statusBar()->addPermanentWidget(onlineLabel_);
    updateStatusBar();

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
