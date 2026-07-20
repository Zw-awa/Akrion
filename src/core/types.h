#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

#include <optional>

namespace akrion::core {

inline constexpr auto kFrameSchema = "akrion.frame/1";
inline constexpr auto kConfigSchema = "akrion.config/1";
inline constexpr auto kManifestSchema = "akrion.manifest/1";
inline constexpr auto kEventSchema = "akrion.event/1";
inline constexpr auto kSummarySchema = "akrion.summary/1";

enum class ChannelRole {
    Reference,
    Input,
    Measurement,
    Output,
    Control,
    Disturbance,
    Estimate,
    State,
    Diagnostic,
    Other,
};

enum class ImplementationLanguage { C, Cpp, Rust, Other };
enum class RunStatus { Recording, Completed, Interrupted, Recovered };
enum class EventSeverity { Info, Warning, Error };

struct ChannelDefinition {
    QString key;
    QString label;
    QString unit;
    ChannelRole role = ChannelRole::Other;
    QString description;
    QJsonObject extraFields;
};

struct AlgorithmDefinition {
    quint32 id = 0;
    QString displayName;
    ImplementationLanguage implementationLanguage = ImplementationLanguage::Other;
    QString repository;
    QString sourceReference;
    QString revision;
    QString buildHash;
    QString author;
    QString organization;
    QString license;
    QJsonObject parameters;
    QJsonObject extraFields;
};

struct MetricDefinition {
    QString name;
    QString referenceChannel;
    QString observedChannel;
    QJsonObject extraFields;
};

struct SerialSettings {
    QString portName;
    qint32 baudRate = 115200;
    qint32 dataBits = 8;
    QString parity = QStringLiteral("none");
    QString stopBits = QStringLiteral("one");
    QString flowControl = QStringLiteral("none");
    qint32 readTimeoutMs = 100;
    QJsonObject extraFields;
};

struct TimingSettings {
    quint64 algorithmPeriodUs = 1000;
    quint64 emitPeriodUs = 10000;
    quint64 maxDeviationUs = 1000;
    QJsonObject extraFields;
};

struct RunConfig {
    QString schema = QString::fromLatin1(kConfigSchema);
    QString name;
    QString deviceId;
    SerialSettings serial;
    TimingSettings timing;
    QVector<ChannelDefinition> channels;
    QVector<AlgorithmDefinition> algorithms;
    QVector<MetricDefinition> metrics;
    QJsonObject scenario;
    QJsonObject extraFields;
};

struct Frame {
    QString schema = QString::fromLatin1(kFrameSchema);
    quint64 deviceTimeUs = 0;
    std::optional<quint64> hostReceiveTimeUs;
    quint64 algoTick = 0;
    quint64 emitTick = 0;
    quint64 seq = 0;
    quint32 algoId = 0;
    bool algoEnabled = false;
    QMap<QString, double> values;
    QJsonObject extraFields;
};

struct RunEvent {
    QString schema = QString::fromLatin1(kEventSchema);
    QString type;
    EventSeverity severity = EventSeverity::Info;
    QString message;
    quint64 hostTimeUs = 0;
    std::optional<quint64> deviceTimeUs;
    std::optional<quint64> seq;
    QJsonObject details;
    QJsonObject extraFields;
};

struct RunManifest {
    QString schema = QString::fromLatin1(kManifestSchema);
    QString runId;
    QString name;
    RunStatus status = RunStatus::Recording;
    QDateTime createdAt;
    QDateTime updatedAt;
    std::optional<QDateTime> completedAt;
    QString source;
    QString configSignature;
    QString experimentSignature;
    QMap<quint32, QString> algorithmSignatures;
    quint64 frameCount = 0;
    quint64 eventCount = 0;
    quint64 rawByteCount = 0;
    QJsonObject extraFields;
};

QString channelRoleName(ChannelRole role);
std::optional<ChannelRole> channelRoleFromName(const QString& name);
QString implementationLanguageName(ImplementationLanguage language);
std::optional<ImplementationLanguage> implementationLanguageFromName(const QString& name);
QString runStatusName(RunStatus status);
std::optional<RunStatus> runStatusFromName(const QString& name);
QString eventSeverityName(EventSeverity severity);
std::optional<EventSeverity> eventSeverityFromName(const QString& name);

QJsonObject toJson(const ChannelDefinition& definition);
QJsonObject toJson(const AlgorithmDefinition& definition);
QJsonObject toJson(const MetricDefinition& definition);
QJsonObject toJson(const SerialSettings& settings);
QJsonObject toJson(const TimingSettings& settings);
QJsonObject toJson(const RunConfig& config);
QJsonObject toJson(const Frame& frame);
QJsonObject toJson(const RunEvent& event);
QJsonObject toJson(const RunManifest& manifest);

bool runConfigFromJson(const QJsonObject& object, RunConfig* config, QString* error = nullptr);
bool frameFromJson(const QJsonObject& object, Frame* frame, QString* error = nullptr);
bool runEventFromJson(const QJsonObject& object, RunEvent* event, QString* error = nullptr);
bool runManifestFromJson(const QJsonObject& object, RunManifest* manifest, QString* error = nullptr);

bool hasSupportedSchemaMajor(const QString& schema, const QString& family, int supportedMajor = 1);
QByteArray canonicalJson(const QJsonValue& value);
QString sha256Canonical(const QJsonValue& value);
QString algorithmSignature(const AlgorithmDefinition& algorithm);
QString configSignature(const RunConfig& config);
QString experimentSignature(const RunConfig& config);

} // namespace akrion::core
