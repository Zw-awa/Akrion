#include "data_source.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace akrion::core {
namespace {

void setError(QString* error, const QString& message) {
    if (error) *error = message;
}

std::optional<QSerialPort::Parity> parseParity(const QString& value) {
    const auto name = value.trimmed().toLower();
    if (name == QStringLiteral("none")) return QSerialPort::NoParity;
    if (name == QStringLiteral("even")) return QSerialPort::EvenParity;
    if (name == QStringLiteral("odd")) return QSerialPort::OddParity;
    if (name == QStringLiteral("space")) return QSerialPort::SpaceParity;
    if (name == QStringLiteral("mark")) return QSerialPort::MarkParity;
    return std::nullopt;
}

std::optional<QSerialPort::StopBits> parseStopBits(const QString& value) {
    const auto name = value.trimmed().toLower();
    if (name == QStringLiteral("one") || name == QStringLiteral("1"))
        return QSerialPort::OneStop;
    if (name == QStringLiteral("one_and_half") || name == QStringLiteral("1.5"))
        return QSerialPort::OneAndHalfStop;
    if (name == QStringLiteral("two") || name == QStringLiteral("2"))
        return QSerialPort::TwoStop;
    return std::nullopt;
}

std::optional<QSerialPort::FlowControl> parseFlowControl(const QString& value) {
    const auto name = value.trimmed().toLower();
    if (name == QStringLiteral("none")) return QSerialPort::NoFlowControl;
    if (name == QStringLiteral("hardware")) return QSerialPort::HardwareControl;
    if (name == QStringLiteral("software")) return QSerialPort::SoftwareControl;
    return std::nullopt;
}

double scenarioDouble(const QJsonObject& object, const QString& key, double fallback) {
    const auto value = object.value(key);
    return value.isDouble() && std::isfinite(value.toDouble()) ? value.toDouble() : fallback;
}

quint64 scenarioUnsigned(const QJsonObject& object, const QString& key, quint64 fallback) {
    const auto integer = object.value(key).toInteger(-1);
    return integer >= 0 ? static_cast<quint64>(integer) : fallback;
}

QString scenarioString(const QJsonObject& object, const QString& key, const QString& fallback) {
    const auto value = object.value(key);
    return value.isString() && !value.toString().isEmpty() ? value.toString() : fallback;
}

} // namespace

QVector<SerialPortDescriptor> availableSerialPorts() {
    QVector<SerialPortDescriptor> result;
    for (const auto& info : QSerialPortInfo::availablePorts()) {
        SerialPortDescriptor descriptor;
        descriptor.name = info.portName();
        descriptor.systemLocation = info.systemLocation();
        descriptor.description = info.description();
        descriptor.manufacturer = info.manufacturer();
        descriptor.serialNumber = info.serialNumber();
        if (info.hasVendorIdentifier()) descriptor.vendorId = info.vendorIdentifier();
        if (info.hasProductIdentifier()) descriptor.productId = info.productIdentifier();
        result.append(std::move(descriptor));
    }
    return result;
}

struct SerialSource::Impl {
    explicit Impl(SerialSettings sourceSettings) : settings(std::move(sourceSettings)) {}
    SerialSettings settings;
    QSerialPort port;
};

SerialSource::SerialSource(SerialSettings settings)
    : m_impl(std::make_unique<Impl>(std::move(settings))) {}

SerialSource::~SerialSource() = default;

bool SerialSource::open(QString* error) {
    close();
    const auto parity = parseParity(m_impl->settings.parity);
    const auto stopBits = parseStopBits(m_impl->settings.stopBits);
    const auto flowControl = parseFlowControl(m_impl->settings.flowControl);
    if (!parity || !stopBits || !flowControl || m_impl->settings.dataBits < 5 ||
        m_impl->settings.dataBits > 8) {
        setError(error, QStringLiteral("invalid serial format settings"));
        return false;
    }
    m_impl->port.setPortName(m_impl->settings.portName);
    m_impl->port.setBaudRate(m_impl->settings.baudRate);
    m_impl->port.setDataBits(static_cast<QSerialPort::DataBits>(m_impl->settings.dataBits));
    m_impl->port.setParity(*parity);
    m_impl->port.setStopBits(*stopBits);
    m_impl->port.setFlowControl(*flowControl);
    if (!m_impl->port.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot open serial port %1: %2")
                            .arg(m_impl->settings.portName, m_impl->port.errorString()));
        return false;
    }
    return true;
}

SourceReadResult SerialSource::read(int timeoutMs) {
    if (!m_impl->port.isOpen())
        return {SourceReadStatus::Error, {}, QStringLiteral("serial port is not open")};
    if (m_impl->port.bytesAvailable() == 0 && !m_impl->port.waitForReadyRead(qMax(0, timeoutMs))) {
        const auto portError = m_impl->port.error();
        if (portError == QSerialPort::NoError || portError == QSerialPort::TimeoutError)
            return {SourceReadStatus::Timeout, {}, {}};
        return {SourceReadStatus::Error, {}, m_impl->port.errorString()};
    }
    const auto bytes = m_impl->port.readAll();
    if (bytes.isEmpty() && m_impl->port.error() != QSerialPort::NoError)
        return {SourceReadStatus::Error, {}, m_impl->port.errorString()};
    return bytes.isEmpty() ? SourceReadResult{SourceReadStatus::Timeout, {}, {}}
                           : SourceReadResult{SourceReadStatus::Data, bytes, {}};
}

void SerialSource::close() {
    if (m_impl->port.isOpen()) m_impl->port.close();
}

bool SerialSource::isOpen() const { return m_impl->port.isOpen(); }

QString SerialSource::description() const {
    return QStringLiteral("serial:%1@%2")
        .arg(m_impl->settings.portName)
        .arg(m_impl->settings.baudRate);
}

DemoOptions DemoOptions::fromConfig(const RunConfig& config) {
    DemoOptions options;
    const auto& scenario = config.scenario;
    options.inputProfile = scenarioString(scenario, QStringLiteral("input_profile"),
                                          options.inputProfile);
    options.noiseProfile = scenarioString(scenario, QStringLiteral("noise_profile"),
                                          options.noiseProfile);
    options.sampleCount = scenarioUnsigned(scenario, QStringLiteral("sample_count"),
                                           options.sampleCount);
    options.seed = scenarioUnsigned(scenario, QStringLiteral("seed"), options.seed);
    if (scenario.value(QStringLiteral("realtime")).isBool())
        options.realtime = scenario.value(QStringLiteral("realtime")).toBool();
    options.offset = scenarioDouble(scenario, QStringLiteral("offset"), options.offset);
    options.amplitude = scenarioDouble(scenario, QStringLiteral("amplitude"), options.amplitude);
    options.frequencyHz = scenarioDouble(scenario, QStringLiteral("frequency_hz"),
                                         options.frequencyHz);
    options.sweepEndFrequencyHz = scenarioDouble(
        scenario, QStringLiteral("sweep_end_frequency_hz"), options.sweepEndFrequencyHz);
    options.stepTimeSeconds = scenarioDouble(scenario, QStringLiteral("step_time_seconds"),
                                             options.stepTimeSeconds);
    options.noiseAmplitude = scenarioDouble(scenario, QStringLiteral("noise_amplitude"),
                                            options.noiseAmplitude);
    options.plantTimeConstantSeconds = scenarioDouble(
        scenario, QStringLiteral("plant_time_constant_seconds"),
        options.plantTimeConstantSeconds);
    options.controlGain = scenarioDouble(scenario, QStringLiteral("control_gain"),
                                         options.controlGain);
    options.disabledEveryFrames = scenarioUnsigned(
        scenario, QStringLiteral("disabled_every_frames"), options.disabledEveryFrames);
    options.referenceChannel = scenarioString(scenario, QStringLiteral("reference_channel"),
                                              options.referenceChannel);
    options.measurementChannel = scenarioString(scenario, QStringLiteral("measurement_channel"),
                                                options.measurementChannel);
    options.theoreticalChannel = scenarioString(scenario, QStringLiteral("theoretical_channel"),
                                                options.theoreticalChannel);
    options.controlChannel = scenarioString(scenario, QStringLiteral("control_channel"),
                                            options.controlChannel);
    options.disturbanceChannel = scenarioString(scenario, QStringLiteral("disturbance_channel"),
                                                options.disturbanceChannel);
    const auto algorithmId = scenario.value(QStringLiteral("algorithm_id")).toInteger(-1);
    if (algorithmId >= 0 && algorithmId <= std::numeric_limits<quint32>::max())
        options.algorithmId = static_cast<quint32>(algorithmId);
    return options;
}

struct DemoSource::Impl {
    Impl(RunConfig sourceConfig, DemoOptions sourceOptions)
        : config(std::move(sourceConfig)), options(std::move(sourceOptions)), random(options.seed) {}

    double referenceAt(double seconds) const {
        const auto profile = options.inputProfile.trimmed().toLower();
        if (profile == QStringLiteral("constant")) return options.offset + options.amplitude;
        if (profile == QStringLiteral("sine"))
            return options.offset + options.amplitude *
                                        std::sin(2.0 * std::numbers::pi * options.frequencyHz * seconds);
        if (profile == QStringLiteral("sweep")) {
            const auto duration = options.sampleCount > 1
                                      ? static_cast<double>(options.sampleCount - 1) *
                                            static_cast<double>(config.timing.emitPeriodUs) / 1e6
                                      : 1.0;
            const auto slope = (options.sweepEndFrequencyHz - options.frequencyHz) /
                               qMax(1e-9, duration);
            const auto phase = 2.0 * std::numbers::pi *
                               (options.frequencyHz * seconds + 0.5 * slope * seconds * seconds);
            return options.offset + options.amplitude * std::sin(phase);
        }
        return seconds < options.stepTimeSeconds ? options.offset
                                                 : options.offset + options.amplitude;
    }

    double noise() {
        const auto profile = options.noiseProfile.trimmed().toLower();
        if (profile == QStringLiteral("none")) return 0.0;
        if (profile == QStringLiteral("uniform"))
            return (random.generateDouble() * 2.0 - 1.0) * options.noiseAmplitude;
        const auto first = qMax(random.generateDouble(), std::numeric_limits<double>::min());
        const auto second = random.generateDouble();
        return std::sqrt(-2.0 * std::log(first)) *
               std::cos(2.0 * std::numbers::pi * second) * options.noiseAmplitude;
    }

    Frame nextFrame() {
        Frame frame;
        frame.deviceTimeUs = sequence * config.timing.emitPeriodUs;
        frame.algoTick = config.timing.algorithmPeriodUs == 0
                             ? sequence
                             : frame.deviceTimeUs / config.timing.algorithmPeriodUs;
        frame.emitTick = sequence;
        frame.seq = sequence;
        frame.algoId = options.algorithmId.value_or(
            config.algorithms.isEmpty() ? 0U : config.algorithms.constFirst().id);
        frame.algoEnabled = options.disabledEveryFrames == 0 ||
                            ((sequence / options.disabledEveryFrames) % 2 == 0);
        const auto seconds = static_cast<double>(frame.deviceTimeUs) / 1e6;
        const auto reference = referenceAt(seconds);
        const auto dt = static_cast<double>(config.timing.emitPeriodUs) / 1e6;
        const auto tau = qMax(options.plantTimeConstantSeconds, 1e-9);
        theoretical += (reference - theoretical) * qBound(0.0, dt / tau, 1.0);
        const auto disturbance = noise();
        const auto measurement = theoretical + disturbance;
        const auto control = qBound(-1.0, (reference - measurement) * options.controlGain, 1.0);
        frame.values.insert(options.referenceChannel, reference);
        frame.values.insert(options.measurementChannel, measurement);
        frame.values.insert(options.theoreticalChannel, theoretical);
        frame.values.insert(options.controlChannel, control);
        frame.values.insert(options.disturbanceChannel, disturbance);
        ++sequence;
        return frame;
    }

    RunConfig config;
    DemoOptions options;
    QRandomGenerator64 random;
    QElapsedTimer clock;
    bool opened = false;
    quint64 sequence = 0;
    double theoretical = 0.0;
};

DemoSource::DemoSource(RunConfig config, DemoOptions options)
    : m_impl(std::make_unique<Impl>(std::move(config), std::move(options))) {}

DemoSource::~DemoSource() = default;

bool DemoSource::open(QString* error) {
    const auto input = m_impl->options.inputProfile.trimmed().toLower();
    const auto noise = m_impl->options.noiseProfile.trimmed().toLower();
    if (!QStringList{QStringLiteral("constant"), QStringLiteral("step"), QStringLiteral("sine"),
                     QStringLiteral("sweep")}.contains(input)) {
        setError(error, QStringLiteral("unknown demo input profile '%1'").arg(input));
        return false;
    }
    if (!QStringList{QStringLiteral("none"), QStringLiteral("uniform"),
                     QStringLiteral("gaussian")}.contains(noise)) {
        setError(error, QStringLiteral("unknown demo noise profile '%1'").arg(noise));
        return false;
    }
    m_impl->sequence = 0;
    m_impl->theoretical = m_impl->options.offset;
    m_impl->random.seed(m_impl->options.seed);
    m_impl->clock.start();
    m_impl->opened = true;
    return true;
}

SourceReadResult DemoSource::read(int timeoutMs) {
    if (!m_impl->opened)
        return {SourceReadStatus::Error, {}, QStringLiteral("demo source is not open")};
    if (m_impl->options.sampleCount > 0 &&
        m_impl->sequence >= m_impl->options.sampleCount)
        return {SourceReadStatus::End, {}, {}};

    if (m_impl->options.realtime) {
        const auto dueUs = m_impl->sequence * m_impl->config.timing.emitPeriodUs;
        const auto elapsedUs = static_cast<quint64>(m_impl->clock.nsecsElapsed() / 1000);
        if (dueUs > elapsedUs) {
            const auto waitUs = dueUs - elapsedUs;
            const auto allowedUs = static_cast<quint64>(qMax(0, timeoutMs)) * 1000ULL;
            if (waitUs > allowedUs) {
                if (allowedUs > 0) QThread::usleep(static_cast<unsigned long>(allowedUs));
                return {SourceReadStatus::Timeout, {}, {}};
            }
            QThread::usleep(static_cast<unsigned long>(waitUs));
        }
    }

    const auto maximum = m_impl->options.realtime
                             ? qsizetype{1}
                             : qMax<qsizetype>(1, m_impl->options.maximumBatchFrames);
    QByteArray bytes;
    for (qsizetype index = 0; index < maximum; ++index) {
        if (m_impl->options.sampleCount > 0 &&
            m_impl->sequence >= m_impl->options.sampleCount) break;
        bytes.append(QJsonDocument(toJson(m_impl->nextFrame())).toJson(QJsonDocument::Compact));
        bytes.append('\n');
    }
    return {SourceReadStatus::Data, std::move(bytes), {}};
}

void DemoSource::close() { m_impl->opened = false; }

bool DemoSource::isOpen() const { return m_impl->opened; }

QString DemoSource::description() const {
    return QStringLiteral("demo:%1/%2")
        .arg(m_impl->options.inputProfile, m_impl->options.noiseProfile);
}

} // namespace akrion::core
