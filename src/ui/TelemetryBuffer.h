#pragma once

#include <QList>

#include <deque>
#include <utility>
#include <vector>

#include "MessageCodec.h"

namespace dms::ui {

// GUI 侧单设备四指标曲线缓冲(LLD §5):
//   - 有界环形:每指标最多 capacity 点,满了丢最旧,内存常数可控;
//   - 只做显示合并,告警/落盘的完整采样语义不受此处降采样影响(HLD §3);
//   - X 轴用本地接收时刻(单调,不依赖发送端时钟质量)。
// 仅 GUI 线程使用(经 queued 信号喂数据),无需加锁
class TelemetryBuffer
{
public:
    explicit TelemetryBuffer(int capacityPoints = 600);

    void append(const QList<dms::protocol::TelemetrySample> &samples);
    // 测试注入:显式指定接收时刻
    void appendAt(qint64 xMs, const QList<dms::protocol::TelemetrySample> &samples);

    // metricId 1..4 的序列副本(拷贝给 CurveWidget,避免迭代期间被新数据改动)
    std::vector<std::pair<qint64, double>> series(quint16 metricId) const;
    int count(quint16 metricId) const;
    double latest(quint16 metricId) const; // 空序列返回 NaN

private:
    void push(int index, qint64 xMs, double value);

    std::deque<std::pair<qint64, double>> series_[4]; // index = metricId-1
    int capacity_;
};

} // namespace dms::ui
