#pragma once

#include "frame_decoder.h"
#include "frame_validator.h"
#include "types.h"

#include <QJsonObject>
#include <QMap>

#include <optional>

namespace akrion::core {

struct ScalarStatistics {
    quint64 count = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double m2 = 0.0;

    void observe(double value);
    double standardDeviation() const;
    QJsonObject toJson() const;
};

struct MetricStatistics {
    MetricDefinition definition;
    quint64 count = 0;
    quint64 skippedDisabledCount = 0;
    long double absoluteErrorSum = 0.0;
    long double squaredErrorSum = 0.0;

    void observe(const Frame& frame);
    QJsonObject toJson() const;
};

struct AlgorithmStatistics {
    quint64 frameCount = 0;
    quint64 enabledFrameCount = 0;
    quint64 disabledFrameCount = 0;
};

class RunStatistics final {
public:
    explicit RunStatistics(const RunConfig& config);

    void observeDecodeError(const DecodeError& error);
    void observeFrame(const Frame& frame, const QVector<ValidationIssue>& issues = {});
    QJsonObject summary(RunStatus status) const;

    quint64 frameCount() const { return m_frameCount; }
    quint64 parseErrorCount() const { return m_parseErrorCount; }
    quint64 droppedFrameCount() const { return m_droppedFrameCount; }

private:
    RunConfig m_config;
    quint64 m_frameCount = 0;
    quint64 m_parseErrorCount = 0;
    quint64 m_droppedFrameCount = 0;
    quint64 m_duplicateFrameCount = 0;
    quint64 m_outOfOrderFrameCount = 0;
    quint64 m_timingViolationCount = 0;
    quint64 m_validationWarningCount = 0;
    quint64 m_validationErrorCount = 0;
    std::optional<quint64> m_firstDeviceTimeUs;
    std::optional<quint64> m_lastDeviceTimeUs;
    std::optional<quint64> m_firstHostTimeUs;
    std::optional<quint64> m_lastHostTimeUs;
    std::optional<quint64> m_previousDeviceTimeUs;
    std::optional<quint64> m_previousHostTimeUs;
    ScalarStatistics m_deviceIntervalUs;
    ScalarStatistics m_hostIntervalUs;
    QMap<QString, ScalarStatistics> m_channels;
    QMap<QString, MetricStatistics> m_metrics;
    QMap<quint32, AlgorithmStatistics> m_algorithms;
};

} // namespace akrion::core
