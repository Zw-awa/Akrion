#include "builtin_components.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QtMath>

#include <cstring>
#include <algorithm>

namespace akrion::core {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr uint32_t kReference = 1;
constexpr uint32_t kMeasurement = 2;
constexpr uint32_t kControl = 3;
constexpr uint32_t kDisturbance = 4;
constexpr uint32_t kError = 6;
constexpr uint32_t kFiltered = 7;

void setError(char* error, size_t capacity, const char* message) {
    if (!error || capacity == 0) return;
    const QByteArray bytes(message);
    const size_t count = qMin(capacity - 1, static_cast<size_t>(bytes.size()));
    std::memcpy(error, bytes.constData(), count);
    error[count] = '\0';
}

QJsonObject readParameters(const uint8_t* data, size_t size, char* error, size_t capacity) {
    if (!data || size == 0) return {};
    QJsonParseError parseError;
    const QByteArray bytes(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, capacity, "parameters must be a JSON object");
        return {};
    }
    return document.object();
}

bool emitValue(akrion_output_v1* output, uint32_t channelId, double value) {
    if (!output || output->value_count >= output->value_capacity) return false;
    output->values[output->value_count++] = {channelId, value};
    return true;
}

bool readValue(const akrion_input_v1* input, uint32_t channelId, double* value) {
    if (!input || !value) return false;
    for (size_t index = 0; index < input->value_count; ++index) {
        if (input->values[index].channel_id != channelId) continue;
        *value = input->values[index].value;
        return qIsFinite(*value);
    }
    return false;
}

struct InputState {
    QString profile = QStringLiteral("step");
    double offset = 0.0;
    double amplitude = 1.0;
    double frequencyHz = 0.5;
    double stepAtSeconds = 1.0;
};

void* createInput(const uint8_t* data, size_t size, char* error, size_t capacity) {
    const QJsonObject object = readParameters(data, size, error, capacity);
    if (data && size > 0 && object.isEmpty()) return nullptr;
    auto* state = new InputState;
    state->profile = object.value(QStringLiteral("profile")).toString(state->profile).toLower();
    state->offset = object.value(QStringLiteral("offset")).toDouble(state->offset);
    state->amplitude = object.value(QStringLiteral("amplitude")).toDouble(state->amplitude);
    state->frequencyHz = object.value(QStringLiteral("frequency_hz")).toDouble(state->frequencyHz);
    state->stepAtSeconds = object.value(QStringLiteral("step_at_s")).toDouble(state->stepAtSeconds);
    if (!QStringList{QStringLiteral("constant"), QStringLiteral("step"),
                     QStringLiteral("sine"), QStringLiteral("sweep")}.contains(state->profile)) {
        setError(error, capacity, "unknown input profile");
        delete state;
        return nullptr;
    }
    return state;
}

int processInput(void* instance, const akrion_input_v1* input, akrion_output_v1* output) {
    auto* state = static_cast<InputState*>(instance);
    if (!state || !input || !output) return AKRION_COMPONENT_INVALID_ARGUMENT;
    const double seconds = static_cast<double>(input->device_time_us) / 1e6;
    double value = state->offset + state->amplitude;
    if (state->profile == QStringLiteral("step")) {
        value = state->offset + (seconds >= state->stepAtSeconds ? state->amplitude : 0.0);
    } else if (state->profile == QStringLiteral("sine")) {
        value = state->offset + state->amplitude
            * qSin(kTwoPi * state->frequencyHz * seconds);
    } else if (state->profile == QStringLiteral("sweep")) {
        const double frequency = state->frequencyHz + 0.1 * seconds;
        value = state->offset + state->amplitude * qSin(kTwoPi * frequency * seconds);
    }
    return emitValue(output, kReference, value)
        ? AKRION_COMPONENT_OK : AKRION_COMPONENT_OUTPUT_FULL;
}

void destroyInput(void* instance) { delete static_cast<InputState*>(instance); }

struct NoiseState {
    QString profile = QStringLiteral("none");
    double amplitude = 0.02;
    QRandomGenerator64 random{1};
};

void* createNoise(const uint8_t* data, size_t size, char* error, size_t capacity) {
    const QJsonObject object = readParameters(data, size, error, capacity);
    if (data && size > 0 && object.isEmpty()) return nullptr;
    auto* state = new NoiseState;
    state->profile = object.value(QStringLiteral("profile")).toString(state->profile).toLower();
    state->amplitude = object.value(QStringLiteral("amplitude")).toDouble(state->amplitude);
    state->random.seed(static_cast<quint32>(object.value(QStringLiteral("seed")).toInteger(1)));
    if (!QStringList{QStringLiteral("none"), QStringLiteral("uniform"),
                     QStringLiteral("gaussian")}.contains(state->profile)) {
        setError(error, capacity, "unknown noise profile");
        delete state;
        return nullptr;
    }
    return state;
}

int processNoise(void* instance, const akrion_input_v1* input, akrion_output_v1* output) {
    auto* state = static_cast<NoiseState*>(instance);
    if (!state || !input || !output) return AKRION_COMPONENT_INVALID_ARGUMENT;
    double reference = 0.0;
    if (!readValue(input, kReference, &reference)) return AKRION_COMPONENT_MISSING_INPUT;
    double disturbance = 0.0;
    if (state->profile == QStringLiteral("uniform")) {
        disturbance = (state->random.generateDouble() * 2.0 - 1.0) * state->amplitude;
    } else if (state->profile == QStringLiteral("gaussian")) {
        const double first = qMax(state->random.generateDouble(), 1e-12);
        const double second = state->random.generateDouble();
        disturbance = qSqrt(-2.0 * qLn(first)) * qCos(kTwoPi * second) * state->amplitude;
    }
    if (!emitValue(output, kMeasurement, reference + disturbance)
        || !emitValue(output, kDisturbance, disturbance))
        return AKRION_COMPONENT_OUTPUT_FULL;
    return AKRION_COMPONENT_OK;
}

void destroyNoise(void* instance) { delete static_cast<NoiseState*>(instance); }

struct PidState {
    double kp = 1.2;
    double ki = 0.0;
    double kd = 0.0;
    double integral = 0.0;
    double previousError = 0.0;
    bool havePrevious = false;
};

void* createPid(const uint8_t* data, size_t size, char* error, size_t capacity) {
    const QJsonObject object = readParameters(data, size, error, capacity);
    if (data && size > 0 && object.isEmpty()) return nullptr;
    auto* state = new PidState;
    state->kp = object.value(QStringLiteral("kp")).toDouble(state->kp);
    state->ki = object.value(QStringLiteral("ki")).toDouble(state->ki);
    state->kd = object.value(QStringLiteral("kd")).toDouble(state->kd);
    return state;
}

int processPid(void* instance, const akrion_input_v1* input, akrion_output_v1* output) {
    auto* state = static_cast<PidState*>(instance);
    if (!state || !input || !output) return AKRION_COMPONENT_INVALID_ARGUMENT;
    double reference = 0.0;
    double measurement = 0.0;
    if (!readValue(input, kReference, &reference)
        || !readValue(input, kMeasurement, &measurement))
        return AKRION_COMPONENT_MISSING_INPUT;
    const double error = reference - measurement;
    const double dt = qMax(0.0, input->dt_seconds);
    state->integral += error * dt;
    const double derivative = state->havePrevious && dt > 0.0
        ? (error - state->previousError) / dt : 0.0;
    state->previousError = error;
    state->havePrevious = true;
    const double control = state->kp * error + state->ki * state->integral
        + state->kd * derivative;
    if (!emitValue(output, kControl, qBound(-1.0, control, 1.0))
        || !emitValue(output, kError, error))
        return AKRION_COMPONENT_OUTPUT_FULL;
    return AKRION_COMPONENT_OK;
}

void destroyPid(void* instance) { delete static_cast<PidState*>(instance); }

struct PassthroughState {};

void* createPassthrough(const uint8_t*, size_t, char*, size_t) {
    return new PassthroughState;
}

int processPassthrough(void* instance, const akrion_input_v1* input, akrion_output_v1* output) {
    if (!instance || !input || !output) return AKRION_COMPONENT_INVALID_ARGUMENT;
    double measurement = 0.0;
    if (!readValue(input, kMeasurement, &measurement)) return AKRION_COMPONENT_MISSING_INPUT;
    return emitValue(output, kFiltered, measurement)
        ? AKRION_COMPONENT_OK : AKRION_COMPONENT_OUTPUT_FULL;
}

void destroyPassthrough(void* instance) { delete static_cast<PassthroughState*>(instance); }

struct MedianState {
    int windowSize = 5;
    QVector<double> window;
};

void* createMedian(const uint8_t* data, size_t size, char* error, size_t capacity) {
    const QJsonObject object = readParameters(data, size, error, capacity);
    if (data && size > 0 && object.isEmpty()) return nullptr;
    auto* state = new MedianState;
    state->windowSize = object.value(QStringLiteral("window")).toInt(state->windowSize);
    if (state->windowSize < 1 || state->windowSize > 4095) {
        setError(error, capacity, "median window must be between 1 and 4095");
        delete state;
        return nullptr;
    }
    return state;
}

int processMedian(void* instance, const akrion_input_v1* input, akrion_output_v1* output) {
    auto* state = static_cast<MedianState*>(instance);
    if (!state || !input || !output) return AKRION_COMPONENT_INVALID_ARGUMENT;
    double measurement = 0.0;
    if (!readValue(input, kMeasurement, &measurement)) return AKRION_COMPONENT_MISSING_INPUT;
    state->window.append(measurement);
    if (state->window.size() > state->windowSize) state->window.removeFirst();
    QVector<double> sorted = state->window;
    std::sort(sorted.begin(), sorted.end());
    const int middle = sorted.size() / 2;
    const double median = sorted.size() % 2 == 0
        ? (sorted[middle - 1] + sorted[middle]) * 0.5 : sorted[middle];
    return emitValue(output, kFiltered, median)
        ? AKRION_COMPONENT_OK : AKRION_COMPONENT_OUTPUT_FULL;
}

void destroyMedian(void* instance) { delete static_cast<MedianState*>(instance); }

const akrion_channel_descriptor_v1 kReferenceOutput[] = {
    {kReference, "target", "Target", "", "reference"},
};
const akrion_channel_descriptor_v1 kMeasurementInput[] = {
    {kMeasurement, "actual", "Actual", "", "measurement"},
};
const akrion_channel_descriptor_v1 kNoiseInput[] = {
    {kReference, "target", "Target", "", "reference"},
};
const akrion_channel_descriptor_v1 kNoiseOutput[] = {
    {kMeasurement, "actual", "Actual", "", "measurement"},
    {kDisturbance, "noise", "Noise", "", "disturbance"},
};
const akrion_channel_descriptor_v1 kAlgorithmInput[] = {
    {kReference, "target", "Target", "", "reference"},
    {kMeasurement, "actual", "Actual", "", "measurement"},
};
const akrion_channel_descriptor_v1 kPidOutput[] = {
    {kControl, "control", "Control", "", "control"},
    {kError, "error", "Error", "", "diagnostic"},
};
const akrion_channel_descriptor_v1 kFilterOutput[] = {
    {kFiltered, "filtered", "Filtered", "", "estimate"},
};

const akrion_component_descriptor_v1 kStep = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_INPUT, "step", "Step input", "1.0.0", "{}",
    nullptr, 0, kReferenceOutput, 1, createInput, processInput, destroyInput};
const akrion_component_descriptor_v1 kSine = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_INPUT, "sine", "Sine input", "1.0.0", "{}",
    nullptr, 0, kReferenceOutput, 1, createInput, processInput, destroyInput};
const akrion_component_descriptor_v1 kSweep = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_INPUT, "sweep", "Sweep input", "1.0.0", "{}",
    nullptr, 0, kReferenceOutput, 1, createInput, processInput, destroyInput};
const akrion_component_descriptor_v1 kConstant = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_INPUT, "constant", "Constant input", "1.0.0", "{}",
    nullptr, 0, kReferenceOutput, 1, createInput, processInput, destroyInput};
const akrion_component_descriptor_v1 kNoise = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_NOISE, "gaussian", "Gaussian noise", "1.0.0", "{}",
    kNoiseInput, 1, kNoiseOutput, 2, createNoise, processNoise, destroyNoise};
const akrion_component_descriptor_v1 kUniformNoise = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_NOISE, "uniform", "Uniform noise", "1.0.0", "{}",
    kNoiseInput, 1, kNoiseOutput, 2, createNoise, processNoise, destroyNoise};
const akrion_component_descriptor_v1 kNoNoise = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_NOISE, "none", "No noise", "1.0.0", "{}",
    kNoiseInput, 1, kNoiseOutput, 2, createNoise, processNoise, destroyNoise};
const akrion_component_descriptor_v1 kPid = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_ALGORITHM, "p_control", "P controller", "1.0.0", "{}",
    kAlgorithmInput, 2, kPidOutput, 2, createPid, processPid, destroyPid};
const akrion_component_descriptor_v1 kFullPid = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_ALGORITHM, "pid", "PID controller", "1.0.0", "{}",
    kAlgorithmInput, 2, kPidOutput, 2, createPid, processPid, destroyPid};
const akrion_component_descriptor_v1 kPassthrough = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_ALGORITHM, "passthrough", "Pass through", "1.0.0", "{}",
    kMeasurementInput, 1, kFilterOutput, 1, createPassthrough, processPassthrough, destroyPassthrough};
const akrion_component_descriptor_v1 kMedian = {
    AKRION_COMPONENT_ABI_V1, AKRION_COMPONENT_ALGORITHM, "median_filter", "Median filter", "1.0.0", "{}",
    kMeasurementInput, 1, kFilterOutput, 1, createMedian, processMedian, destroyMedian};

} // namespace

QVector<const akrion_component_descriptor_v1*> builtinComponentDescriptors() {
    return {&kStep, &kSine, &kSweep, &kConstant,
            &kNoise, &kUniformNoise, &kNoNoise,
            &kPid, &kFullPid, &kPassthrough, &kMedian};
}

} // namespace akrion::core
