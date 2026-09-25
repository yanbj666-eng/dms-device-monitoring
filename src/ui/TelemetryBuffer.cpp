#include "TelemetryBuffer.h"

#include <QDateTime>
#include <QtMath>

namespace dms::ui {

TelemetryBuffer::TelemetryBuffer(int capacityPoints)
    : capacity_(qMax(10, capacityPoints))
{
}

void TelemetryBuffer::append(const QList<dms::protocol::TelemetrySample> &samples)
{
    appendAt(QDateTime::currentMSecsSinceEpoch(), samples);
}

void TelemetryBuffer::appendAt(qint64 xMs,
                               const QList<dms::protocol::TelemetrySample> &samples)
{
    for (const dms::protocol::TelemetrySample &s : samples) {
        if (s.metricId < 1 || s.metricId > 4)
            continue; // 未知指标已在会话层计数,曲线只画已知四指标
        const double value =
            (s.valueType == dms::protocol::kValueTypeUint32) ? double(s.asUint) : s.asFloat;
        push(int(s.metricId) - 1, xMs, value);
    }
}

std::vector<std::pair<qint64, double>> TelemetryBuffer::series(quint16 metricId) const
{
    std::vector<std::pair<qint64, double>> out;
    if (metricId < 1 || metricId > 4)
        return out;
    const auto &src = series_[metricId - 1];
    out.reserve(src.size());
    out.assign(src.begin(), src.end());
    return out;
}

int TelemetryBuffer::count(quint16 metricId) const
{
    if (metricId < 1 || metricId > 4)
        return 0;
    return int(series_[metricId - 1].size());
}

double TelemetryBuffer::latest(quint16 metricId) const
{
    if (metricId < 1 || metricId > 4)
        return qQNaN();
    const auto &src = series_[metricId - 1];
    return src.empty() ? qQNaN() : src.back().second;
}

void TelemetryBuffer::push(int index, qint64 xMs, double value)
{
    auto &s = series_[index];
    if (int(s.size()) >= capacity_)
        s.pop_front();
    s.emplace_back(xMs, value);
}

} // namespace dms::ui
