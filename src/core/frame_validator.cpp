#include "frame_validator.h"

#include <QDateTime>

#include <cmath>
#include <limits>

namespace akrion::core {
namespace {

quint64 absoluteDifference(quint64 left, quint64 right) {
    return left > right ? left - right : right - left;
}

std::optional<quint64> checkedProduct(quint64 left, quint64 right) {
    if (left != 0 && right > std::numeric_limits<quint64>::max() / left) return std::nullopt;
    return left * right;
}

ValidationIssue timingIssue(const QString& code, const QString& counter, quint64 actual,
                            quint64 expected, quint64 deviation) {
    return {code,
            QStringLiteral("%1 timing deviation is %2 us (actual %3 us, expected %4 us)")
                .arg(counter)
                .arg(deviation)
                .arg(actual)
                .arg(expected),
            ValidationSeverity::Warning,
            1,
            {{QStringLiteral("counter"), counter},
             {QStringLiteral("actual_delta_us"), static_cast<qint64>(actual)},
             {QStringLiteral("expected_delta_us"), static_cast<qint64>(expected)},
             {QStringLiteral("deviation_us"), static_cast<qint64>(deviation)}}};
}

} // namespace

FrameValidator::FrameValidator(const RunConfig& config) : m_config(config) {
    for (const auto& algorithm : config.algorithms) m_algorithmIds.insert(algorithm.id);
    for (const auto& channel : config.channels) m_channelKeys.insert(channel.key);
}

QVector<ValidationIssue> FrameValidator::inspect(const Frame& frame) {
    QVector<ValidationIssue> issues;
    if (!m_algorithmIds.contains(frame.algoId)) {
        issues.append({QStringLiteral("unknown_algorithm"),
                       QStringLiteral("frame refers to undefined algorithm id %1").arg(frame.algoId),
                       ValidationSeverity::Warning, 1,
                       {{QStringLiteral("algo_id"), static_cast<qint64>(frame.algoId)}}});
    }
    for (auto it = frame.values.constBegin(); it != frame.values.constEnd(); ++it) {
        if (!m_channelKeys.contains(it.key()) && !m_reportedUnknownChannels.contains(it.key())) {
            m_reportedUnknownChannels.insert(it.key());
            issues.append({QStringLiteral("unknown_channel"),
                           QStringLiteral("frame contains undefined channel '%1'").arg(it.key()),
                           ValidationSeverity::Warning, 1,
                           {{QStringLiteral("channel"), it.key()}}});
        }
    }
    if (!m_previous) {
        m_previous = frame;
        return issues;
    }

    const auto& previous = *m_previous;
    if (frame.algoId != previous.algoId) {
        issues.append({QStringLiteral("algorithm_changed"),
                       QStringLiteral("algorithm changed from %1 to %2")
                           .arg(previous.algoId)
                           .arg(frame.algoId),
                       ValidationSeverity::Info, 1,
                       {{QStringLiteral("previous_algo_id"), static_cast<qint64>(previous.algoId)},
                        {QStringLiteral("current_algo_id"), static_cast<qint64>(frame.algoId)}}});
    }
    if (frame.algoEnabled != previous.algoEnabled) {
        issues.append({QStringLiteral("algorithm_enabled_changed"),
                       frame.algoEnabled ? QStringLiteral("algorithm enabled")
                                         : QStringLiteral("algorithm disabled"),
                       ValidationSeverity::Info, 1,
                       {{QStringLiteral("enabled"), frame.algoEnabled}}});
    }
    if (frame.seq == previous.seq) {
        issues.append({QStringLiteral("duplicate_sequence"),
                       QStringLiteral("duplicate frame sequence %1").arg(frame.seq),
                       ValidationSeverity::Warning, 1, {}});
    } else if (frame.seq < previous.seq) {
        issues.append({QStringLiteral("out_of_order_sequence"),
                       QStringLiteral("frame sequence moved backwards from %1 to %2")
                           .arg(previous.seq)
                           .arg(frame.seq),
                       ValidationSeverity::Warning, 1, {}});
    } else if (frame.seq > previous.seq + 1) {
        const auto missing = frame.seq - previous.seq - 1;
        issues.append({QStringLiteral("sequence_gap"),
                       QStringLiteral("%1 frame sequence value(s) are missing").arg(missing),
                       ValidationSeverity::Warning, missing,
                       {{QStringLiteral("previous_seq"), static_cast<qint64>(previous.seq)},
                        {QStringLiteral("current_seq"), static_cast<qint64>(frame.seq)},
                        {QStringLiteral("missing"), static_cast<qint64>(missing)}}});
    }
    if (frame.deviceTimeUs < previous.deviceTimeUs) {
        issues.append({QStringLiteral("device_time_backwards"),
                       QStringLiteral("device time moved backwards from %1 to %2 us")
                           .arg(previous.deviceTimeUs)
                           .arg(frame.deviceTimeUs),
                       ValidationSeverity::Warning, 1, {}});
    }
    if (frame.algoTick < previous.algoTick) {
        issues.append({QStringLiteral("algo_tick_backwards"),
                       QStringLiteral("algorithm tick moved backwards"),
                       ValidationSeverity::Warning, 1, {}});
    }
    if (frame.emitTick < previous.emitTick) {
        issues.append({QStringLiteral("emit_tick_backwards"),
                       QStringLiteral("emit tick moved backwards"),
                       ValidationSeverity::Warning, 1, {}});
    }

    if (frame.deviceTimeUs >= previous.deviceTimeUs) {
        const auto actual = frame.deviceTimeUs - previous.deviceTimeUs;
        if (frame.algoTick >= previous.algoTick) {
            const auto expected = checkedProduct(frame.algoTick - previous.algoTick,
                                                 m_config.timing.algorithmPeriodUs);
            if (expected) {
                const auto deviation = absoluteDifference(actual, *expected);
                if (deviation > m_config.timing.maxDeviationUs)
                    issues.append(timingIssue(QStringLiteral("algorithm_timing_violation"),
                                              QStringLiteral("algorithm"), actual, *expected,
                                              deviation));
            }
        }
        if (frame.emitTick >= previous.emitTick) {
            const auto expected = checkedProduct(frame.emitTick - previous.emitTick,
                                                 m_config.timing.emitPeriodUs);
            if (expected) {
                const auto deviation = absoluteDifference(actual, *expected);
                if (deviation > m_config.timing.maxDeviationUs)
                    issues.append(timingIssue(QStringLiteral("emit_timing_violation"),
                                              QStringLiteral("emit"), actual, *expected,
                                              deviation));
            }
        }
    }
    m_previous = frame;
    return issues;
}

void FrameValidator::reset() {
    m_previous.reset();
    m_reportedUnknownChannels.clear();
}

RunEvent validationIssueEvent(const ValidationIssue& issue, const Frame& frame) {
    RunEvent event;
    event.type = issue.code;
    event.message = issue.message;
    event.hostTimeUs = frame.hostReceiveTimeUs.value_or(
        static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000ULL);
    event.deviceTimeUs = frame.deviceTimeUs;
    event.seq = frame.seq;
    event.details = issue.details;
    event.details.insert(QStringLiteral("count"), static_cast<qint64>(issue.count));
    switch (issue.severity) {
    case ValidationSeverity::Info: event.severity = EventSeverity::Info; break;
    case ValidationSeverity::Warning: event.severity = EventSeverity::Warning; break;
    case ValidationSeverity::Error: event.severity = EventSeverity::Error; break;
    }
    return event;
}

} // namespace akrion::core
