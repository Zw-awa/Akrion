#pragma once

#include "types.h"

#include <QSet>
#include <QVector>

#include <optional>

namespace akrion::core {

enum class ValidationSeverity { Info, Warning, Error };

struct ValidationIssue {
    QString code;
    QString message;
    ValidationSeverity severity = ValidationSeverity::Warning;
    quint64 count = 1;
    QJsonObject details;
};

class FrameValidator final {
public:
    explicit FrameValidator(const RunConfig& config);

    QVector<ValidationIssue> inspect(const Frame& frame);
    void reset();

private:
    RunConfig m_config;
    QSet<quint32> m_algorithmIds;
    QSet<QString> m_channelKeys;
    QSet<QString> m_reportedUnknownChannels;
    std::optional<Frame> m_previous;
};

RunEvent validationIssueEvent(const ValidationIssue& issue, const Frame& frame);

} // namespace akrion::core
