#pragma once

#include <QString>
#include <QWidget>

#include <utility>
#include <vector>

namespace dms::ui {

// 四指标曲线控件:QPainter 自绘折线(S4 图表后端选定记录于 HLD §5:
// 本机 Qt 无 QtCharts 模块,自绘避免额外依赖,600 点重绘开销可忽略)。
// 数据由外部 100ms 合并定时器统一喂入(setSeries),不逐帧重绘(LLD §5)
class CurveWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CurveWidget(const QString &title, const QString &unit, QColor color,
                         QWidget *parent = nullptr);

    void setSeries(const std::vector<std::pair<qint64, double>> &points);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString title_;
    QString unit_;
    QColor color_;
    std::vector<std::pair<qint64, double>> points_;
};

} // namespace dms::ui
