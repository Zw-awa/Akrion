#include "statistics.h"

#include <QJsonArray>

#include <cmath>

namespace akrion::core {

void ScalarStatistics::observe(double value) {
    ++count;
    if (count == 1) {
        minimum = maximum = mean = value;
        m2 = 0.0;
        return;
    }
    minimum = qMin(minimum, value);
    maximum = qMax(maximum, value);
    const auto delta = value - mean;
    mean += delta / static_cast<double>(count);
    m2 += delta * (value - mean);
}

double ScalarStatistics::standardDeviation() const {
    return count == 0 ? 0.0 : std::sqrt(m2 / static_cast<double>(count));
}

QJsonObject ScalarStatistics::toJson() const {
    QJsonObject object{{QStringLiteral("count"), static_cast<qint64>(count)}};
    if (count > 0) {
        object.insert(QStringLiteral("min"), minimum);
        object.insert(QStringLiteral("max"), maximum);
        object.insert(QStringLiteral("mean"), mean);
        object.insert(QStringLiteral("stddev"), standardDeviation());
    }
    return object;
}

void MetricStatistics::observe(const Frame& frame) {
    if (!frame.algoEnabled) {
        ++skippedDisabledCount;
        return;
    }
    const auto reference = frame.values.constFind(definition.referenceChannel);
    const auto observed = frame.values.constFind(definition.observedChannel);
    if (reference == frame.values.constEnd() || observed == frame.values.constEnd()) return;
    const auto error = static_cast<long double>(*observed) - static_cast<long double>(*reference);
    ++count;
    absoluteErrorSum += std::abs(error);
    squaredErrorSum += error * error;
}

QJsonObject MetricStatistics::toJson() const {
    QJsonObject object{{QStringLiteral("reference"), definition.referenceChannel},
                       {QStringLiteral("observed"), definition.observedChannel},
                       {QStringLiteral("count"), static_cast<qint64>(count)},
                       {QStringLiteral("skipped_disabled_count"),
                        static_cast<qint64>(skippedDisabledCount)}};
    if (count > 0) {
        object.insert(QStringLiteral("mae"),
                      static_cast<double>(absoluteErrorSum / static_cast<long double>(count)));
        object.insert(QStringLiteral("rmse"),
                      static_cast<double>(std::sqrt(squaredErrorSum /
                                                    static_cast<long double>(count))));
    }
    return object;
}

RunStatistics::RunStatistics(const RunConfig& config) : m_config(config) {
    for (const auto& channel : config.channels) m_channels.insert(channel.key, {});
    for (const auto& metric : config.metrics)
        m_metrics.insert(metric.name, MetricStatistics{metric});
    for (const auto& algorithm : config.algorithms) m_algorithms.insert(algorithm.id, {});
}

void RunStatistics::observeDecodeError(const DecodeError& error) {
    Q_UNUSED(error)
    ++m_parseErrorCount;
}

void RunStatistics::observeFrame(const Frame& frame, const QVector<ValidationIssue>& issues) {
    ++m_frameCount;
    if (!m_firstDeviceTimeUs) m_firstDeviceTimeUs = frame.deviceTimeUs;
    m_lastDeviceTimeUs = frame.deviceTimeUs;
    if (m_previousDeviceTimeUs && frame.deviceTimeUs >= *m_previousDeviceTimeUs)
        m_deviceIntervalUs.observe(static_cast<double>(frame.deviceTimeUs - *m_previousDeviceTimeUs));
    m_previousDeviceTimeUs = frame.deviceTimeUs;
    if (frame.hostReceiveTimeUs) {
        if (!m_firstHostTimeUs) m_firstHostTimeUs = frame.hostReceiveTimeUs;
        m_lastHostTimeUs = frame.hostReceiveTimeUs;
        if (m_previousHostTimeUs && *frame.hostReceiveTimeUs >= *m_previousHostTimeUs)
            m_hostIntervalUs.observe(
                static_cast<double>(*frame.hostReceiveTimeUs - *m_previousHostTimeUs));
        m_previousHostTimeUs = frame.hostReceiveTimeUs;
    }
    for (auto it = frame.values.constBegin(); it != frame.values.constEnd(); ++it)
        m_channels[it.key()].observe(it.value());
    for (auto it = m_metrics.begin(); it != m_metrics.end(); ++it) it->observe(frame);
    auto& algorithm = m_algorithms[frame.algoId];
    ++algorithm.frameCount;
    if (frame.algoEnabled)
        ++algorithm.enabledFrameCount;
    else
        ++algorithm.disabledFrameCount;

    for (const auto& issue : issues) {
        if (issue.severity == ValidationSeverity::Warning) ++m_validationWarningCount;
        if (issue.severity == ValidationSeverity::Error) ++m_validationErrorCount;
        if (issue.code == QStringLiteral("sequence_gap")) m_droppedFrameCount += issue.count;
        if (issue.code == QStringLiteral("duplicate_sequence")) ++m_duplicateFrameCount;
        if (issue.code == QStringLiteral("out_of_order_sequence")) ++m_outOfOrderFrameCount;
        if (issue.code == QStringLiteral("algorithm_timing_violation") ||
            issue.code == QStringLiteral("emit_timing_violation"))
            ++m_timingViolationCount;
    }
}

QJsonObject RunStatistics::summary(RunStatus status) const {
    QJsonObject transport{{QStringLiteral("frame_count"), static_cast<qint64>(m_frameCount)},
                          {QStringLiteral("parse_error_count"),
                           static_cast<qint64>(m_parseErrorCount)},
                          {QStringLiteral("dropped_frame_count"),
                           static_cast<qint64>(m_droppedFrameCount)},
                          {QStringLiteral("duplicate_frame_count"),
                           static_cast<qint64>(m_duplicateFrameCount)},
                          {QStringLiteral("out_of_order_frame_count"),
                           static_cast<qint64>(m_outOfOrderFrameCount)},
                          {QStringLiteral("timing_violation_count"),
                           static_cast<qint64>(m_timingViolationCount)},
                          {QStringLiteral("validation_warning_count"),
                           static_cast<qint64>(m_validationWarningCount)},
                          {QStringLiteral("validation_error_count"),
                           static_cast<qint64>(m_validationErrorCount)}};
    if (m_firstDeviceTimeUs) {
        transport.insert(QStringLiteral("first_device_time_us"),
                         static_cast<qint64>(*m_firstDeviceTimeUs));
        transport.insert(QStringLiteral("last_device_time_us"),
                         static_cast<qint64>(*m_lastDeviceTimeUs));
        const auto duration = *m_lastDeviceTimeUs >= *m_firstDeviceTimeUs
                                  ? *m_lastDeviceTimeUs - *m_firstDeviceTimeUs
                                  : 0;
        transport.insert(QStringLiteral("device_duration_us"), static_cast<qint64>(duration));
        if (duration > 0 && m_frameCount > 1)
            transport.insert(QStringLiteral("average_frame_rate_hz"),
                             static_cast<double>(m_frameCount - 1) * 1000000.0 /
                                 static_cast<double>(duration));
    }
    if (m_firstHostTimeUs) {
        transport.insert(QStringLiteral("first_host_time_us"),
                         static_cast<qint64>(*m_firstHostTimeUs));
        transport.insert(QStringLiteral("last_host_time_us"),
                         static_cast<qint64>(*m_lastHostTimeUs));
    }
    transport.insert(QStringLiteral("device_interarrival_us"), m_deviceIntervalUs.toJson());
    transport.insert(QStringLiteral("host_interarrival_us"), m_hostIntervalUs.toJson());

    QJsonObject channels;
    for (auto it = m_channels.constBegin(); it != m_channels.constEnd(); ++it)
        channels.insert(it.key(), it.value().toJson());
    QJsonObject metrics;
    for (auto it = m_metrics.constBegin(); it != m_metrics.constEnd(); ++it)
        metrics.insert(it.key(), it.value().toJson());
    QJsonObject algorithms;
    for (auto it = m_algorithms.constBegin(); it != m_algorithms.constEnd(); ++it) {
        const QJsonObject algorithm{{QStringLiteral("frame_count"),
                                     static_cast<qint64>(it->frameCount)},
                                    {QStringLiteral("enabled_frame_count"),
                                     static_cast<qint64>(it->enabledFrameCount)},
                                    {QStringLiteral("disabled_frame_count"),
                                     static_cast<qint64>(it->disabledFrameCount)}};
        algorithms.insert(QString::number(it.key()), algorithm);
    }
    return {{QStringLiteral("schema"), QString::fromLatin1(kSummarySchema)},
            {QStringLiteral("status"), runStatusName(status)},
            {QStringLiteral("transport"), transport},
            {QStringLiteral("channels"), channels},
            {QStringLiteral("metrics"), metrics},
            {QStringLiteral("algorithms"), algorithms}};
}

} // namespace akrion::core
