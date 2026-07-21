#include "component_pipeline.h"

#include <QJsonDocument>

namespace akrion::core {
namespace {

void setError(QString* error, const QString& message) {
    if (error) *error = message;
}

QString referenceId(const QJsonObject& reference, const QString& fallback) {
    const QString component = reference.value(QStringLiteral("component")).toString();
    if (!component.isEmpty()) return component;
    const QString type = reference.value(QStringLiteral("type")).toString();
    return type.isEmpty() ? fallback : type;
}

QJsonObject normalizedParameters(const QJsonObject& reference, const QString& id,
                                 const RunConfig& config, uint32_t kind) {
    QJsonObject result = reference.value(QStringLiteral("parameters")).toObject();
    if (result.isEmpty()) result = reference;
    result.remove(QStringLiteral("component"));
    result.remove(QStringLiteral("type"));
    result.remove(QStringLiteral("version"));
    result.remove(QStringLiteral("parameters"));
    result.insert(QStringLiteral("profile"), id);
    if (kind == AKRION_COMPONENT_INPUT) {
        if (reference.contains(QStringLiteral("initial")))
            result.insert(QStringLiteral("offset"), reference.value(QStringLiteral("initial")));
        if (reference.contains(QStringLiteral("value"))) {
            const double initial = reference.value(QStringLiteral("initial")).toDouble();
            result.insert(QStringLiteral("amplitude"),
                          reference.value(QStringLiteral("value")).toDouble(1.0) - initial);
        }
        if (reference.contains(QStringLiteral("at_s")))
            result.insert(QStringLiteral("step_at_s"), reference.value(QStringLiteral("at_s")));
    } else if (kind == AKRION_COMPONENT_NOISE) {
        if (reference.contains(QStringLiteral("standard_deviation")))
            result.insert(QStringLiteral("amplitude"),
                          reference.value(QStringLiteral("standard_deviation")));
        if (!result.contains(QStringLiteral("seed")))
            result.insert(QStringLiteral("seed"), config.scenario.value(QStringLiteral("seed")));
    } else if (kind == AKRION_COMPONENT_ALGORITHM && !config.algorithms.isEmpty()) {
        for (auto it = config.algorithms.first().parameters.begin();
             it != config.algorithms.first().parameters.end(); ++it) {
            if (!result.contains(it.key())) result.insert(it.key(), it.value());
        }
    }
    return result;
}

QString statusName(int status) {
    switch (status) {
    case AKRION_COMPONENT_INVALID_ARGUMENT: return QStringLiteral("invalid argument");
    case AKRION_COMPONENT_INVALID_PARAMETERS: return QStringLiteral("invalid parameters");
    case AKRION_COMPONENT_OUTPUT_FULL: return QStringLiteral("output buffer full");
    case AKRION_COMPONENT_MISSING_INPUT: return QStringLiteral("missing input");
    default: return QStringLiteral("runtime error");
    }
}
} // namespace

ComponentPipeline::ComponentPipeline(RunConfig config, ComponentRegistry registry)
    : m_config(std::move(config)), m_registry(std::move(registry)) {}

ComponentPipeline::~ComponentPipeline() { close(); }

QJsonObject ComponentPipeline::componentReference(const QString& key) const {
    return m_config.scenario.value(key).toObject();
}

bool ComponentPipeline::createInstance(uint32_t kind, const QJsonObject& reference,
                                       const QString& fallbackId, Instance* instance,
                                       QString* error) {
    const QString id = referenceId(reference, fallbackId);
    if (kind == AKRION_COMPONENT_ALGORITHM && id == QStringLiteral("disabled")) {
        m_algorithmEnabled = false;
        return true;
    }
    const auto* descriptor = m_registry.find(id);
    if (!descriptor || descriptor->kind != kind) {
        setError(error, QStringLiteral("component '%1' is missing or has the wrong kind").arg(id));
        return false;
    }
    const QJsonObject object = normalizedParameters(reference, id, m_config, kind);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    char componentError[256] = {};
    void* state = descriptor->create(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                     static_cast<size_t>(bytes.size()), componentError,
                                     sizeof(componentError));
    if (!state) {
        setError(error, QStringLiteral("cannot create component '%1': %2")
                            .arg(id, QString::fromUtf8(componentError)));
        return false;
    }
    instance->descriptor = descriptor;
    instance->state = state;
    for (size_t index = 0; index < descriptor->output_count; ++index) {
        m_channelKeys.insert(descriptor->outputs[index].id,
                             QString::fromUtf8(descriptor->outputs[index].key));
    }
    return true;
}

bool ComponentPipeline::open(QString* error) {
    close();
    if (m_config.timing.algorithmPeriodUs == 0 || m_config.timing.emitPeriodUs == 0) {
        setError(error, QStringLiteral("algorithm and emit periods must be positive"));
        return false;
    }
    if (!createInstance(AKRION_COMPONENT_INPUT, componentReference(QStringLiteral("input")),
                        QStringLiteral("step"), &m_input, error)
        || !createInstance(AKRION_COMPONENT_NOISE, componentReference(QStringLiteral("noise")),
                           QStringLiteral("none"), &m_noise, error)
        || !createInstance(AKRION_COMPONENT_ALGORITHM,
                           componentReference(QStringLiteral("algorithm")),
                           QStringLiteral("p_control"), &m_algorithm, error)) {
        close();
        return false;
    }
    m_values.clear();
    m_nextAlgorithmTimeUs = 0;
    m_algorithmTick = 0;
    m_open = true;
    return true;
}

void ComponentPipeline::close() {
    for (Instance* instance : {&m_algorithm, &m_noise, &m_input}) {
        if (instance->descriptor && instance->state)
            instance->descriptor->destroy(instance->state);
        *instance = {};
    }
    m_values.clear();
    m_channelKeys.clear();
    m_algorithmEnabled = true;
    m_open = false;
}

void ComponentPipeline::mergeOutputs(
    const Instance& instance, const QVector<akrion_value_v1>& outputs) {
    Q_UNUSED(instance)
    for (const auto& value : outputs) m_values.insert(value.channel_id, value.value);
}

bool ComponentPipeline::process(Instance& instance, quint64 deviceTimeUs,
                                quint64 algorithmTick, quint64 emitTick,
                                quint64 sequence, QString* error) {
    if (!instance.descriptor) return true;
    QVector<akrion_value_v1> inputs;
    inputs.reserve(m_values.size());
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it)
        inputs.append({it.key(), it.value()});
    QVector<akrion_value_v1> outputs(static_cast<qsizetype>(instance.descriptor->output_count));
    akrion_input_v1 input{};
    input.device_time_us = deviceTimeUs;
    input.algorithm_tick = algorithmTick;
    input.emit_tick = emitTick;
    input.sequence = sequence;
    input.dt_seconds = static_cast<double>(m_config.timing.algorithmPeriodUs) / 1e6;
    input.values = inputs.constData();
    input.value_count = static_cast<size_t>(inputs.size());
    akrion_output_v1 output{outputs.data(), 0, static_cast<size_t>(outputs.size())};
    const int status = instance.descriptor->process(instance.state, &input, &output);
    if (status != AKRION_COMPONENT_OK) {
        setError(error, QStringLiteral("component '%1' failed: %2")
                            .arg(QString::fromUtf8(instance.descriptor->id), statusName(status)));
        return false;
    }
    outputs.resize(static_cast<qsizetype>(output.value_count));
    for (const auto& value : outputs) {
        bool declared = false;
        for (size_t index = 0; index < instance.descriptor->output_count; ++index) {
            if (instance.descriptor->outputs[index].id == value.channel_id) {
                declared = true;
                break;
            }
        }
        if (!declared || !qIsFinite(value.value)) {
            setError(error, QStringLiteral("component '%1' emitted an invalid or undeclared value")
                                .arg(QString::fromUtf8(instance.descriptor->id)));
            return false;
        }
    }
    mergeOutputs(instance, outputs);
    return true;
}

bool ComponentPipeline::nextFrame(quint64 emitTick, Frame* frame, QString* error) {
    if (!m_open || !frame) {
        setError(error, QStringLiteral("component pipeline is not open"));
        return false;
    }
    const quint64 emitTimeUs = emitTick * m_config.timing.emitPeriodUs;
    while (m_nextAlgorithmTimeUs <= emitTimeUs) {
        m_values.clear();
        if (!process(m_input, m_nextAlgorithmTimeUs, m_algorithmTick, emitTick, emitTick, error)
            || !process(m_noise, m_nextAlgorithmTimeUs, m_algorithmTick, emitTick, emitTick, error)
            || (m_algorithmEnabled
                && !process(m_algorithm, m_nextAlgorithmTimeUs, m_algorithmTick,
                            emitTick, emitTick, error)))
            return false;
        ++m_algorithmTick;
        m_nextAlgorithmTimeUs += m_config.timing.algorithmPeriodUs;
    }

    frame->deviceTimeUs = emitTimeUs;
    frame->algoTick = m_algorithmTick == 0 ? 0 : m_algorithmTick - 1;
    frame->emitTick = emitTick;
    frame->seq = emitTick;
    frame->algoId = m_config.algorithms.isEmpty() ? 0 : m_config.algorithms.first().id;
    frame->algoEnabled = m_algorithmEnabled;
    frame->values.clear();
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it) {
        const QString key = m_channelKeys.value(it.key());
        if (!key.isEmpty()) frame->values.insert(key, it.value());
    }
    return true;
}

} // namespace akrion::core
