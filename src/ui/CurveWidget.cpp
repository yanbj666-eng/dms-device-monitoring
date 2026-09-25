#include "CurveWidget.h"

#include <QPaintEvent>
#include <QPainter>

#include <QtMath>

namespace dms::ui {

CurveWidget::CurveWidget(const QString &title, const QString &unit, QColor color,
                         QWidget *parent)
    : QWidget(parent), title_(title), unit_(unit), color_(std::move(color))
{
    setMinimumSize(220, 140);
}

void CurveWidget::setSeries(const std::vector<std::pair<qint64, double>> &points)
{
    points_ = points; // 拷贝:数据源是 TelemetryBuffer 的快照
    update();         // 合并到下一次 paint,不逐帧强制重绘
}

void CurveWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF area = rect().adjusted(8, 8, -8, -8);
    const QFontMetricsF fm(font());

    // 标题行:名称 + 当前值(最新点);空数据显示等待占位
    QString headline = title_;
    if (!points_.empty())
        headline += QStringLiteral("   %1 %2")
                        .arg(points_.back().second, 0, 'f', 1)
                        .arg(unit_);
    p.setPen(palette().color(QPalette::Text));
    p.drawText(QPointF(area.left(), area.top() + fm.ascent()), headline);

    const QRectF plot = area.adjusted(0, fm.height() + 4, 0, -fm.height() - 4);
    if (points_.size() < 2) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(plot, Qt::AlignCenter, QStringLiteral("等待数据…"));
        return;
    }

    double yMin = points_.front().second, yMax = yMin;
    qint64 xMin = points_.front().first, xMax = xMin;
    for (const auto &pt : points_) {
        yMin = qMin(yMin, pt.second);
        yMax = qMax(yMax, pt.second);
        xMin = qMin<qint64>(xMin, pt.first);
        xMax = qMax<qint64>(xMax, pt.first);
    }
    if (qFuzzyCompare(yMin, yMax)) { // 平线时撑开范围,折线不至于贴边
        yMin -= 1.0;
        yMax += 1.0;
    } else {
        const double pad = (yMax - yMin) * 0.08;
        yMin -= pad;
        yMax += pad;
    }
    const double xSpan = double(qMax<qint64>(xMax - xMin, 1));

    // 三条横向网格线 + 上下界标注
    p.setPen(palette().color(QPalette::Mid));
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.8);
    p.setFont(small);
    for (int i = 0; i <= 2; ++i) {
        const double ratio = double(i) / 2.0;
        const double y = plot.bottom() - plot.height() * ratio;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const double value = yMin + (yMax - yMin) * ratio;
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QPointF(plot.left(), y - 2), QString::number(value, 'f', 1));
        p.setPen(palette().color(QPalette::Mid));
    }

    // 时间跨度标注(右下角)
    p.setPen(palette().color(QPalette::PlaceholderText));
    p.drawText(QPointF(plot.right() - fm.horizontalAdvance(QStringLiteral("9999s")),
                       plot.bottom() + fm.ascent() + 2),
               QStringLiteral("%1s").arg(qRound64(xSpan / 1000.0)));

    // 折线本体
    QPolygonF line;
    line.reserve(int(points_.size()));
    for (const auto &pt : points_) {
        const double x = plot.left() + plot.width() * double(pt.first - xMin) / xSpan;
        const double y = plot.bottom() - plot.height() * (pt.second - yMin) / (yMax - yMin);
        line.append(QPointF(x, y));
    }
    p.setPen(QPen(color_, 2.0));
    p.drawPolyline(line);
}

} // namespace dms::ui
