#include "types.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace akrion::core {
namespace {

void setError(QString* error, const QString& message) {
    if (error) *error = message;
}

QJsonObject withExtra(QJsonObject object, const QJsonObject& extra) {
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
        if (!object.contains(it.key())) object.insert(it.key(), it.value());
    }
    return object;
}

QJsonObject unknownFields(QJsonObject object, const QStringList& known) {
    for (const auto& key : known) object.remove(key);
    return object;
}

bool readString(const QJsonObject& object, const QString& key, QString* value, QString* error,
                bool allowEmpty = false) {
    const auto field = object.value(key);
    if (!field.isString() || (!allowEmpty && field.toString().trimmed().isEmpty())) {
        setError(error, QStringLiteral("'%1' must be a %2 string")
                            .arg(key, allowEmpty ? QStringLiteral("valid")
                                                : QStringLiteral("non-empty")));
        return false;
    }
    *value = field.toString();
    return true;
}

bool readUnsigned(const QJsonObject& object, const QString& key, quint64 maximum, quint64* value,
                  QString* error) {
    const auto field = object.value(key);
    if (!field.isDouble()) {
        setError(error, QStringLiteral("'%1' must be a non-negative integer").arg(key));
        return false;
    }
    const auto integer = field.toInteger(-1);
    if (integer < 0 || static_cast<quint64>(integer) > maximum) {
        setError(error, QStringLiteral("'%1' is outside its supported integer range").arg(key));
        return false;
    }
    *value = static_cast<quint64>(integer);
    return true;
}

bool readPositive(const QJsonObject& object, const QString& key, quint64* value, QString* error) {
    if (!readUnsigned(object, key, static_cast<quint64>(std::numeric_limits<qint64>::max()), value,
                      error)) return false;
    if (*value == 0) {
        setError(error, QStringLiteral("'%1' must be greater than zero").arg(key));
        return false;
    }
    return true;
}

QByteArray quoteJsonString(const QString& value) {
    const auto encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return encoded.mid(1, encoded.size() - 2);
}

QJsonObject algorithmIdentityJson(const AlgorithmDefinition& algorithm) {
    auto object = toJson(algorithm);
    object.remove(QStringLiteral("display_name"));
    object.remove(QStringLiteral("author"));
    object.remove(QStringLiteral("organization"));
    return object;
}

} // namespace

QString channelRoleName(ChannelRole role) {
    switch (role) {
    case ChannelRole::Reference: return QStringLiteral("reference");
    case ChannelRole::Input: return QStringLiteral("input");
    case ChannelRole::Measurement: return QStringLiteral("measurement");
    case ChannelRole::Output: return QStringLiteral("output");
    case ChannelRole::Control: return QStringLiteral("control");
    case ChannelRole::Disturbance: return QStringLiteral("disturbance");
    case ChannelRole::Estimate: return QStringLiteral("estimate");
    case ChannelRole::State: return QStringLiteral("state");
    case ChannelRole::Diagnostic: return QStringLiteral("diagnostic");
    case ChannelRole::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

std::optional<ChannelRole> channelRoleFromName(const QString& name) {
    const auto normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("reference")) return ChannelRole::Reference;
    if (normalized == QStringLiteral("input")) return ChannelRole::Input;
    if (normalized == QStringLiteral("measurement")) return ChannelRole::Measurement;
    if (normalized == QStringLiteral("output")) return ChannelRole::Output;
    if (normalized == QStringLiteral("control")) return ChannelRole::Control;
    if (normalized == QStringLiteral("disturbance")) return ChannelRole::Disturbance;
    if (normalized == QStringLiteral("estimate")) return ChannelRole::Estimate;
    if (normalized == QStringLiteral("state")) return ChannelRole::State;
    if (normalized == QStringLiteral("diagnostic")) return ChannelRole::Diagnostic;
    if (normalized == QStringLiteral("other")) return ChannelRole::Other;
    return std::nullopt;
}

QString implementationLanguageName(ImplementationLanguage language) {
    switch (language) {
    case ImplementationLanguage::C: return QStringLiteral("c");
    case ImplementationLanguage::Cpp: return QStringLiteral("cpp");
    case ImplementationLanguage::Rust: return QStringLiteral("rust");
    case ImplementationLanguage::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

std::optional<ImplementationLanguage> implementationLanguageFromName(const QString& name) {
    const auto normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("c")) return ImplementationLanguage::C;
    if (normalized == QStringLiteral("cpp") || normalized == QStringLiteral("c++"))
        return ImplementationLanguage::Cpp;
    if (normalized == QStringLiteral("rust")) return ImplementationLanguage::Rust;
    if (normalized == QStringLiteral("other")) return ImplementationLanguage::Other;
    return std::nullopt;
}

QString runStatusName(RunStatus status) {
    switch (status) {
    case RunStatus::Recording: return QStringLiteral("recording");
    case RunStatus::Completed: return QStringLiteral("completed");
    case RunStatus::Interrupted: return QStringLiteral("interrupted");
    case RunStatus::Recovered: return QStringLiteral("recovered");
    }
    return QStringLiteral("interrupted");
}

std::optional<RunStatus> runStatusFromName(const QString& name) {
    if (name == QStringLiteral("recording")) return RunStatus::Recording;
    if (name == QStringLiteral("completed")) return RunStatus::Completed;
    if (name == QStringLiteral("interrupted")) return RunStatus::Interrupted;
    if (name == QStringLiteral("recovered")) return RunStatus::Recovered;
    return std::nullopt;
}

QString eventSeverityName(EventSeverity severity) {
    switch (severity) {
    case EventSeverity::Info: return QStringLiteral("info");
    case EventSeverity::Warning: return QStringLiteral("warning");
    case EventSeverity::Error: return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

std::optional<EventSeverity> eventSeverityFromName(const QString& name) {
    if (name == QStringLiteral("info")) return EventSeverity::Info;
    if (name == QStringLiteral("warning")) return EventSeverity::Warning;
    if (name == QStringLiteral("error")) return EventSeverity::Error;
    return std::nullopt;
}

QJsonObject toJson(const ChannelDefinition& definition) {
    return withExtra({{QStringLiteral("key"), definition.key},
                      {QStringLiteral("label"), definition.label},
                      {QStringLiteral("unit"), definition.unit},
                      {QStringLiteral("role"), channelRoleName(definition.role)},
                      {QStringLiteral("description"), definition.description}},
                     definition.extraFields);
}

QJsonObject toJson(const AlgorithmDefinition& definition) {
    return withExtra({{QStringLiteral("id"), static_cast<qint64>(definition.id)},
                      {QStringLiteral("display_name"), definition.displayName},
                      {QStringLiteral("implementation_language"),
                       implementationLanguageName(definition.implementationLanguage)},
                      {QStringLiteral("repository"), definition.repository},
                      {QStringLiteral("source_reference"), definition.sourceReference},
                      {QStringLiteral("revision"), definition.revision},
                      {QStringLiteral("build_hash"), definition.buildHash},
                      {QStringLiteral("author"), definition.author},
                      {QStringLiteral("organization"), definition.organization},
                      {QStringLiteral("license"), definition.license},
                      {QStringLiteral("parameters"), definition.parameters}},
                     definition.extraFields);
}

QJsonObject toJson(const MetricDefinition& definition) {
    return withExtra({{QStringLiteral("name"), definition.name},
                      {QStringLiteral("reference"), definition.referenceChannel},
                      {QStringLiteral("observed"), definition.observedChannel}},
                     definition.extraFields);
}

QJsonObject toJson(const SerialSettings& settings) {
    return withExtra({{QStringLiteral("port"), settings.portName},
                      {QStringLiteral("baud_rate"), settings.baudRate},
                      {QStringLiteral("data_bits"), settings.dataBits},
                      {QStringLiteral("parity"), settings.parity},
                      {QStringLiteral("stop_bits"), settings.stopBits},
                      {QStringLiteral("flow_control"), settings.flowControl},
                      {QStringLiteral("read_timeout_ms"), settings.readTimeoutMs}},
                     settings.extraFields);
}

QJsonObject toJson(const TimingSettings& settings) {
    return withExtra({{QStringLiteral("algorithm_period_us"),
                       static_cast<qint64>(settings.algorithmPeriodUs)},
                      {QStringLiteral("emit_period_us"), static_cast<qint64>(settings.emitPeriodUs)},
                      {QStringLiteral("max_deviation_us"),
                       static_cast<qint64>(settings.maxDeviationUs)}},
                     settings.extraFields);
}

QJsonObject toJson(const RunConfig& config) {
    QJsonArray channels;
    for (const auto& channel : config.channels) channels.append(toJson(channel));
    QJsonArray algorithms;
    for (const auto& algorithm : config.algorithms) algorithms.append(toJson(algorithm));
    QJsonArray metrics;
    for (const auto& metric : config.metrics) metrics.append(toJson(metric));
    return withExtra({{QStringLiteral("schema"), config.schema},
                      {QStringLiteral("name"), config.name},
                      {QStringLiteral("device_id"), config.deviceId},
                      {QStringLiteral("serial"), toJson(config.serial)},
                      {QStringLiteral("timing"), toJson(config.timing)},
                      {QStringLiteral("channels"), channels},
                      {QStringLiteral("algorithms"), algorithms},
                      {QStringLiteral("metrics"), metrics},
                      {QStringLiteral("scenario"), config.scenario}},
                     config.extraFields);
}

QJsonObject toJson(const Frame& frame) {
    QJsonObject values;
    for (auto it = frame.values.constBegin(); it != frame.values.constEnd(); ++it)
        values.insert(it.key(), it.value());
    QJsonObject object{{QStringLiteral("schema"), frame.schema},
                       {QStringLiteral("device_time_us"), static_cast<qint64>(frame.deviceTimeUs)},
                       {QStringLiteral("algo_tick"), static_cast<qint64>(frame.algoTick)},
                       {QStringLiteral("emit_tick"), static_cast<qint64>(frame.emitTick)},
                       {QStringLiteral("seq"), static_cast<qint64>(frame.seq)},
                       {QStringLiteral("algo_id"), static_cast<qint64>(frame.algoId)},
                       {QStringLiteral("algo_enabled"), frame.algoEnabled},
                       {QStringLiteral("values"), values}};
    if (frame.hostReceiveTimeUs)
        object.insert(QStringLiteral("host_receive_time_us"),
                      static_cast<qint64>(*frame.hostReceiveTimeUs));
    return withExtra(object, frame.extraFields);
}

QJsonObject toJson(const RunEvent& event) {
    QJsonObject object{{QStringLiteral("schema"), event.schema},
                       {QStringLiteral("type"), event.type},
                       {QStringLiteral("severity"), eventSeverityName(event.severity)},
                       {QStringLiteral("message"), event.message},
                       {QStringLiteral("host_time_us"), static_cast<qint64>(event.hostTimeUs)},
                       {QStringLiteral("details"), event.details}};
    if (event.deviceTimeUs)
        object.insert(QStringLiteral("device_time_us"), static_cast<qint64>(*event.deviceTimeUs));
    if (event.seq) object.insert(QStringLiteral("seq"), static_cast<qint64>(*event.seq));
    return withExtra(object, event.extraFields);
}

QJsonObject toJson(const RunManifest& manifest) {
    QJsonObject signatures;
    for (auto it = manifest.algorithmSignatures.constBegin();
         it != manifest.algorithmSignatures.constEnd(); ++it)
        signatures.insert(QString::number(it.key()), it.value());
    QJsonObject object{{QStringLiteral("schema"), manifest.schema},
                       {QStringLiteral("run_id"), manifest.runId},
                       {QStringLiteral("name"), manifest.name},
                       {QStringLiteral("status"), runStatusName(manifest.status)},
                       {QStringLiteral("created_at"),
                        manifest.createdAt.toUTC().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("updated_at"),
                        manifest.updatedAt.toUTC().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("source"), manifest.source},
                       {QStringLiteral("config_signature"), manifest.configSignature},
                       {QStringLiteral("experiment_signature"), manifest.experimentSignature},
                       {QStringLiteral("algorithm_signatures"), signatures},
                       {QStringLiteral("frame_count"), static_cast<qint64>(manifest.frameCount)},
                       {QStringLiteral("event_count"), static_cast<qint64>(manifest.eventCount)},
                       {QStringLiteral("raw_byte_count"), static_cast<qint64>(manifest.rawByteCount)}};
    if (manifest.completedAt)
        object.insert(QStringLiteral("completed_at"),
                      manifest.completedAt->toUTC().toString(Qt::ISODateWithMs));
    return withExtra(object, manifest.extraFields);
}

bool runConfigFromJson(const QJsonObject& object, RunConfig* config, QString* error) {
    if (!config) {
        setError(error, QStringLiteral("output RunConfig is null"));
        return false;
    }
    RunConfig result;
    if (!readString(object, QStringLiteral("schema"), &result.schema, error) ||
        !hasSupportedSchemaMajor(result.schema, QStringLiteral("akrion.config"))) {
        setError(error, QStringLiteral("unsupported config schema '%1'").arg(result.schema));
        return false;
    }
    if (!readString(object, QStringLiteral("name"), &result.name, error, true) ||
        !readString(object, QStringLiteral("device_id"), &result.deviceId, error, true))
        return false;

    const auto serialValue = object.value(QStringLiteral("serial"));
    if (!serialValue.isObject()) {
        setError(error, QStringLiteral("'serial' must be an object"));
        return false;
    }
    const auto serial = serialValue.toObject();
    quint64 numeric = 0;
    if (!readString(serial, QStringLiteral("port"), &result.serial.portName, error, true) ||
        !readPositive(serial, QStringLiteral("baud_rate"), &numeric, error) ||
        numeric > static_cast<quint64>(std::numeric_limits<qint32>::max())) return false;
    result.serial.baudRate = static_cast<qint32>(numeric);
    if (!readPositive(serial, QStringLiteral("data_bits"), &numeric, error) || numeric > 8)
        return false;
    result.serial.dataBits = static_cast<qint32>(numeric);
    if (!readString(serial, QStringLiteral("parity"), &result.serial.parity, error) ||
        !readString(serial, QStringLiteral("stop_bits"), &result.serial.stopBits, error) ||
        !readString(serial, QStringLiteral("flow_control"), &result.serial.flowControl, error) ||
        !readUnsigned(serial, QStringLiteral("read_timeout_ms"),
                      static_cast<quint64>(std::numeric_limits<qint32>::max()), &numeric, error))
        return false;
    result.serial.readTimeoutMs = static_cast<qint32>(numeric);
    result.serial.extraFields = unknownFields(
        serial, {QStringLiteral("port"), QStringLiteral("baud_rate"),
                 QStringLiteral("data_bits"), QStringLiteral("parity"),
                 QStringLiteral("stop_bits"), QStringLiteral("flow_control"),
                 QStringLiteral("read_timeout_ms")});

    const auto timingValue = object.value(QStringLiteral("timing"));
    if (!timingValue.isObject()) {
        setError(error, QStringLiteral("'timing' must be an object"));
        return false;
    }
    const auto timing = timingValue.toObject();
    if (!readPositive(timing, QStringLiteral("algorithm_period_us"),
                      &result.timing.algorithmPeriodUs, error) ||
        !readPositive(timing, QStringLiteral("emit_period_us"), &result.timing.emitPeriodUs,
                      error) ||
        !readUnsigned(timing, QStringLiteral("max_deviation_us"),
                      static_cast<quint64>(std::numeric_limits<qint64>::max()),
                      &result.timing.maxDeviationUs, error)) return false;
    result.timing.extraFields = unknownFields(
        timing, {QStringLiteral("algorithm_period_us"), QStringLiteral("emit_period_us"),
                 QStringLiteral("max_deviation_us")});

    const auto channelValue = object.value(QStringLiteral("channels"));
    if (!channelValue.isArray() || channelValue.toArray().isEmpty()) {
        setError(error, QStringLiteral("'channels' must be a non-empty array"));
        return false;
    }
    QSet<QString> channelKeys;
    for (const auto& value : channelValue.toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("each channel definition must be an object"));
            return false;
        }
        const auto source = value.toObject();
        ChannelDefinition channel;
        QString roleName;
        if (!readString(source, QStringLiteral("key"), &channel.key, error) ||
            !readString(source, QStringLiteral("label"), &channel.label, error, true) ||
            !readString(source, QStringLiteral("unit"), &channel.unit, error, true) ||
            !readString(source, QStringLiteral("role"), &roleName, error) ||
            !readString(source, QStringLiteral("description"), &channel.description, error, true))
            return false;
        const auto role = channelRoleFromName(roleName);
        if (!role) {
            setError(error, QStringLiteral("unknown channel role '%1'").arg(roleName));
            return false;
        }
        channel.role = *role;
        if (channelKeys.contains(channel.key)) {
            setError(error, QStringLiteral("duplicate channel key '%1'").arg(channel.key));
            return false;
        }
        channelKeys.insert(channel.key);
        channel.extraFields = unknownFields(
            source, {QStringLiteral("key"), QStringLiteral("label"), QStringLiteral("unit"),
                     QStringLiteral("role"), QStringLiteral("description")});
        result.channels.append(channel);
    }

    const auto algorithmValue = object.value(QStringLiteral("algorithms"));
    if (!algorithmValue.isArray() || algorithmValue.toArray().isEmpty()) {
        setError(error, QStringLiteral("'algorithms' must be a non-empty array"));
        return false;
    }
    QSet<quint32> algorithmIds;
    for (const auto& value : algorithmValue.toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("each algorithm definition must be an object"));
            return false;
        }
        const auto source = value.toObject();
        AlgorithmDefinition algorithm;
        if (!readUnsigned(source, QStringLiteral("id"), std::numeric_limits<quint32>::max(),
                          &numeric, error)) return false;
        algorithm.id = static_cast<quint32>(numeric);
        QString languageName;
        if (!readString(source, QStringLiteral("display_name"), &algorithm.displayName, error) ||
            !readString(source, QStringLiteral("implementation_language"), &languageName, error))
            return false;
        const auto language = implementationLanguageFromName(languageName);
        if (!language) {
            setError(error,
                     QStringLiteral("unknown implementation language '%1'").arg(languageName));
            return false;
        }
        algorithm.implementationLanguage = *language;
        const auto optionalString = [&source, error](const QString& key, QString* output) {
            return !source.contains(key) || readString(source, key, output, error, true);
        };
        if (!optionalString(QStringLiteral("repository"), &algorithm.repository) ||
            !optionalString(QStringLiteral("source_reference"), &algorithm.sourceReference) ||
            !optionalString(QStringLiteral("revision"), &algorithm.revision) ||
            !optionalString(QStringLiteral("build_hash"), &algorithm.buildHash) ||
            !optionalString(QStringLiteral("author"), &algorithm.author) ||
            !optionalString(QStringLiteral("organization"), &algorithm.organization) ||
            !optionalString(QStringLiteral("license"), &algorithm.license)) return false;
        const auto parameters = source.value(QStringLiteral("parameters"));
        if (!parameters.isObject()) {
            setError(error, QStringLiteral("algorithm 'parameters' must be an object"));
            return false;
        }
        algorithm.parameters = parameters.toObject();
        if (algorithmIds.contains(algorithm.id)) {
            setError(error, QStringLiteral("duplicate algorithm id %1").arg(algorithm.id));
            return false;
        }
        algorithmIds.insert(algorithm.id);
        algorithm.extraFields = unknownFields(
            source, {QStringLiteral("id"), QStringLiteral("display_name"),
                     QStringLiteral("implementation_language"), QStringLiteral("repository"),
                     QStringLiteral("source_reference"), QStringLiteral("revision"),
                     QStringLiteral("build_hash"), QStringLiteral("author"),
                     QStringLiteral("organization"), QStringLiteral("license"),
                     QStringLiteral("parameters")});
        result.algorithms.append(algorithm);
    }

    const auto metricValue = object.value(QStringLiteral("metrics"));
    if (!metricValue.isArray()) {
        setError(error, QStringLiteral("'metrics' must be an array"));
        return false;
    }
    QSet<QString> metricNames;
    for (const auto& value : metricValue.toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("each metric definition must be an object"));
            return false;
        }
        const auto source = value.toObject();
        MetricDefinition metric;
        if (!readString(source, QStringLiteral("name"), &metric.name, error) ||
            !readString(source, QStringLiteral("reference"), &metric.referenceChannel, error) ||
            !readString(source, QStringLiteral("observed"), &metric.observedChannel, error))
            return false;
        if (!channelKeys.contains(metric.referenceChannel) ||
            !channelKeys.contains(metric.observedChannel)) {
            setError(error,
                     QStringLiteral("metric '%1' refers to an undefined channel").arg(metric.name));
            return false;
        }
        if (metricNames.contains(metric.name)) {
            setError(error, QStringLiteral("duplicate metric name '%1'").arg(metric.name));
            return false;
        }
        metricNames.insert(metric.name);
        metric.extraFields = unknownFields(
            source, {QStringLiteral("name"), QStringLiteral("reference"),
                     QStringLiteral("observed")});
        result.metrics.append(metric);
    }

    const auto scenarioValue = object.value(QStringLiteral("scenario"));
    if (!scenarioValue.isObject()) {
        setError(error, QStringLiteral("'scenario' must be an object"));
        return false;
    }
    result.scenario = scenarioValue.toObject();
    result.extraFields = unknownFields(
        object, {QStringLiteral("schema"), QStringLiteral("name"), QStringLiteral("device_id"),
                 QStringLiteral("serial"), QStringLiteral("timing"), QStringLiteral("channels"),
                 QStringLiteral("algorithms"), QStringLiteral("metrics"),
                 QStringLiteral("scenario")});
    *config = std::move(result);
    return true;
}

bool frameFromJson(const QJsonObject& object, Frame* frame, QString* error) {
    if (!frame) {
        setError(error, QStringLiteral("output Frame is null"));
        return false;
    }
    Frame result;
    if (!readString(object, QStringLiteral("schema"), &result.schema, error) ||
        !hasSupportedSchemaMajor(result.schema, QStringLiteral("akrion.frame"))) {
        setError(error, QStringLiteral("unsupported frame schema '%1'").arg(result.schema));
        return false;
    }
    const auto integerMaximum = static_cast<quint64>(std::numeric_limits<qint64>::max());
    if (!readUnsigned(object, QStringLiteral("device_time_us"), integerMaximum,
                      &result.deviceTimeUs, error) ||
        !readUnsigned(object, QStringLiteral("algo_tick"), integerMaximum, &result.algoTick,
                      error) ||
        !readUnsigned(object, QStringLiteral("emit_tick"), integerMaximum, &result.emitTick,
                      error) ||
        !readUnsigned(object, QStringLiteral("seq"), integerMaximum, &result.seq, error))
        return false;
    quint64 algorithmId = 0;
    if (!readUnsigned(object, QStringLiteral("algo_id"), std::numeric_limits<quint32>::max(),
                      &algorithmId, error)) return false;
    result.algoId = static_cast<quint32>(algorithmId);
    const auto enabled = object.value(QStringLiteral("algo_enabled"));
    if (!enabled.isBool()) {
        setError(error, QStringLiteral("'algo_enabled' must be a boolean"));
        return false;
    }
    result.algoEnabled = enabled.toBool();
    if (object.contains(QStringLiteral("host_receive_time_us"))) {
        quint64 hostTime = 0;
        if (!readUnsigned(object, QStringLiteral("host_receive_time_us"), integerMaximum,
                          &hostTime, error)) return false;
        result.hostReceiveTimeUs = hostTime;
    }
    const auto values = object.value(QStringLiteral("values"));
    if (!values.isObject()) {
        setError(error, QStringLiteral("'values' must be an object"));
        return false;
    }
    const auto valuesObject = values.toObject();
    for (auto it = valuesObject.constBegin(); it != valuesObject.constEnd(); ++it) {
        if (it.key().isEmpty() || !it.value().isDouble() || !std::isfinite(it.value().toDouble())) {
            setError(error, QStringLiteral("values.%1 must be a finite number").arg(it.key()));
            return false;
        }
        result.values.insert(it.key(), it.value().toDouble());
    }
    result.extraFields = unknownFields(
        object, {QStringLiteral("schema"), QStringLiteral("device_time_us"),
                 QStringLiteral("host_receive_time_us"), QStringLiteral("algo_tick"),
                 QStringLiteral("emit_tick"), QStringLiteral("seq"), QStringLiteral("algo_id"),
                 QStringLiteral("algo_enabled"), QStringLiteral("values")});
    *frame = std::move(result);
    return true;
}

bool runEventFromJson(const QJsonObject& object, RunEvent* event, QString* error) {
    if (!event) {
        setError(error, QStringLiteral("output RunEvent is null"));
        return false;
    }
    RunEvent result;
    if (!readString(object, QStringLiteral("schema"), &result.schema, error) ||
        !hasSupportedSchemaMajor(result.schema, QStringLiteral("akrion.event")) ||
        !readString(object, QStringLiteral("type"), &result.type, error) ||
        !readString(object, QStringLiteral("message"), &result.message, error, true) ||
        !readUnsigned(object, QStringLiteral("host_time_us"),
                      static_cast<quint64>(std::numeric_limits<qint64>::max()), &result.hostTimeUs,
                      error)) return false;
    QString severityName;
    if (!readString(object, QStringLiteral("severity"), &severityName, error)) return false;
    const auto severity = eventSeverityFromName(severityName);
    if (!severity) {
        setError(error, QStringLiteral("unknown event severity '%1'").arg(severityName));
        return false;
    }
    result.severity = *severity;
    quint64 numeric = 0;
    if (object.contains(QStringLiteral("device_time_us"))) {
        if (!readUnsigned(object, QStringLiteral("device_time_us"),
                          static_cast<quint64>(std::numeric_limits<qint64>::max()), &numeric,
                          error)) return false;
        result.deviceTimeUs = numeric;
    }
    if (object.contains(QStringLiteral("seq"))) {
        if (!readUnsigned(object, QStringLiteral("seq"),
                          static_cast<quint64>(std::numeric_limits<qint64>::max()), &numeric,
                          error)) return false;
        result.seq = numeric;
    }
    const auto details = object.value(QStringLiteral("details"));
    if (!details.isObject()) {
        setError(error, QStringLiteral("event 'details' must be an object"));
        return false;
    }
    result.details = details.toObject();
    result.extraFields = unknownFields(
        object, {QStringLiteral("schema"), QStringLiteral("type"), QStringLiteral("severity"),
                 QStringLiteral("message"), QStringLiteral("host_time_us"),
                 QStringLiteral("device_time_us"), QStringLiteral("seq"),
                 QStringLiteral("details")});
    *event = std::move(result);
    return true;
}

bool runManifestFromJson(const QJsonObject& object, RunManifest* manifest, QString* error) {
    if (!manifest) {
        setError(error, QStringLiteral("output RunManifest is null"));
        return false;
    }
    RunManifest result;
    if (!readString(object, QStringLiteral("schema"), &result.schema, error) ||
        !hasSupportedSchemaMajor(result.schema, QStringLiteral("akrion.manifest")) ||
        !readString(object, QStringLiteral("run_id"), &result.runId, error) ||
        !readString(object, QStringLiteral("name"), &result.name, error, true) ||
        !readString(object, QStringLiteral("source"), &result.source, error, true) ||
        !readString(object, QStringLiteral("config_signature"), &result.configSignature, error) ||
        !readString(object, QStringLiteral("experiment_signature"), &result.experimentSignature,
                    error)) return false;
    QString statusName;
    if (!readString(object, QStringLiteral("status"), &statusName, error)) return false;
    const auto status = runStatusFromName(statusName);
    if (!status) {
        setError(error, QStringLiteral("unknown run status '%1'").arg(statusName));
        return false;
    }
    result.status = *status;
    result.createdAt = QDateTime::fromString(object.value(QStringLiteral("created_at")).toString(),
                                             Qt::ISODateWithMs);
    result.updatedAt = QDateTime::fromString(object.value(QStringLiteral("updated_at")).toString(),
                                             Qt::ISODateWithMs);
    if (!result.createdAt.isValid() || !result.updatedAt.isValid()) {
        setError(error, QStringLiteral("manifest timestamps must be ISO 8601 date-times"));
        return false;
    }
    if (object.contains(QStringLiteral("completed_at"))) {
        auto completed = QDateTime::fromString(object.value(QStringLiteral("completed_at")).toString(),
                                               Qt::ISODateWithMs);
        if (!completed.isValid()) {
            setError(error, QStringLiteral("'completed_at' must be an ISO 8601 date-time"));
            return false;
        }
        result.completedAt = completed;
    }
    const auto integerMaximum = static_cast<quint64>(std::numeric_limits<qint64>::max());
    if (!readUnsigned(object, QStringLiteral("frame_count"), integerMaximum, &result.frameCount,
                      error) ||
        !readUnsigned(object, QStringLiteral("event_count"), integerMaximum, &result.eventCount,
                      error) ||
        !readUnsigned(object, QStringLiteral("raw_byte_count"), integerMaximum,
                      &result.rawByteCount, error)) return false;
    const auto signaturesValue = object.value(QStringLiteral("algorithm_signatures"));
    if (!signaturesValue.isObject()) {
        setError(error, QStringLiteral("'algorithm_signatures' must be an object"));
        return false;
    }
    const auto signatures = signaturesValue.toObject();
    for (auto it = signatures.constBegin(); it != signatures.constEnd(); ++it) {
        bool ok = false;
        const auto id = it.key().toUInt(&ok);
        if (!ok || !it.value().isString()) {
            setError(error, QStringLiteral("invalid algorithm signature entry '%1'").arg(it.key()));
            return false;
        }
        result.algorithmSignatures.insert(id, it.value().toString());
    }
    result.extraFields = unknownFields(
        object, {QStringLiteral("schema"), QStringLiteral("run_id"), QStringLiteral("name"),
                 QStringLiteral("status"), QStringLiteral("created_at"),
                 QStringLiteral("updated_at"), QStringLiteral("completed_at"),
                 QStringLiteral("source"), QStringLiteral("config_signature"),
                 QStringLiteral("experiment_signature"), QStringLiteral("algorithm_signatures"),
                 QStringLiteral("frame_count"), QStringLiteral("event_count"),
                 QStringLiteral("raw_byte_count")});
    *manifest = std::move(result);
    return true;
}

bool hasSupportedSchemaMajor(const QString& schema, const QString& family, int supportedMajor) {
    const auto prefix = family + QLatin1Char('/');
    if (!schema.startsWith(prefix)) return false;
    const auto version = schema.mid(prefix.size()).section(QLatin1Char('.'), 0, 0);
    bool ok = false;
    const auto major = version.toInt(&ok);
    return ok && major == supportedMajor;
}

QByteArray canonicalJson(const QJsonValue& value) {
    if (value.isObject()) {
        const auto object = value.toObject();
        auto keys = object.keys();
        keys.sort(Qt::CaseSensitive);
        QByteArray result{"{"};
        bool first = true;
        for (const auto& key : keys) {
            if (!first) result.append(',');
            first = false;
            result.append(quoteJsonString(key));
            result.append(':');
            result.append(canonicalJson(object.value(key)));
        }
        result.append('}');
        return result;
    }
    if (value.isArray()) {
        QByteArray result{"["};
        bool first = true;
        for (const auto& child : value.toArray()) {
            if (!first) result.append(',');
            first = false;
            result.append(canonicalJson(child));
        }
        result.append(']');
        return result;
    }
    return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
}

QString sha256Canonical(const QJsonValue& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(canonicalJson(value), QCryptographicHash::Sha256).toHex());
}

QString algorithmSignature(const AlgorithmDefinition& algorithm) {
    return sha256Canonical(algorithmIdentityJson(algorithm));
}

QString configSignature(const RunConfig& config) { return sha256Canonical(toJson(config)); }

QString experimentSignature(const RunConfig& config) {
    auto sortedAlgorithms = config.algorithms;
    std::sort(sortedAlgorithms.begin(), sortedAlgorithms.end(),
              [](const AlgorithmDefinition& left, const AlgorithmDefinition& right) {
                  return left.id < right.id;
              });
    QJsonArray algorithms;
    for (const auto& algorithm : sortedAlgorithms)
        algorithms.append(algorithmIdentityJson(algorithm));

    auto sortedChannels = config.channels;
    std::sort(sortedChannels.begin(), sortedChannels.end(),
              [](const ChannelDefinition& left, const ChannelDefinition& right) {
                  return left.key < right.key;
              });
    QJsonArray channels;
    for (const auto& channel : sortedChannels) channels.append(toJson(channel));

    auto sortedMetrics = config.metrics;
    std::sort(sortedMetrics.begin(), sortedMetrics.end(),
              [](const MetricDefinition& left, const MetricDefinition& right) {
                  return left.name < right.name;
              });
    QJsonArray metrics;
    for (const auto& metric : sortedMetrics) metrics.append(toJson(metric));
    const QJsonObject experiment{{QStringLiteral("algorithms"), algorithms},
                                 {QStringLiteral("timing"), toJson(config.timing)},
                                 {QStringLiteral("channels"), channels},
                                 {QStringLiteral("metrics"), metrics},
                                 {QStringLiteral("scenario"), config.scenario}};
    return sha256Canonical(experiment);
}

} // namespace akrion::core
