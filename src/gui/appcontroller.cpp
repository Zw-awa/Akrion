#include "appcontroller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSerialPortInfo>
#include <QVariantMap>
#include <QtMath>

namespace akrion::gui {

namespace {
QString localizedComponentLabel(const QString& id, const QString& fallback) {
    static const QHash<QString, QString> labels {
        {QStringLiteral("step"), QStringLiteral("阶跃")},
        {QStringLiteral("sine"), QStringLiteral("正弦")},
        {QStringLiteral("sweep"), QStringLiteral("扫频")},
        {QStringLiteral("constant"), QStringLiteral("常量")},
        {QStringLiteral("gaussian"), QStringLiteral("高斯噪声")},
        {QStringLiteral("uniform"), QStringLiteral("均匀噪声")},
        {QStringLiteral("none"), QStringLiteral("无噪声")},
        {QStringLiteral("p_control"), QStringLiteral("P 控制")},
        {QStringLiteral("pid"), QStringLiteral("PID")},
        {QStringLiteral("passthrough"), QStringLiteral("直通")},
        {QStringLiteral("median_filter"), QStringLiteral("中值滤波")},
    };
    return labels.value(id, fallback);
}

QVariantList componentList(const core::ComponentRegistry& registry, uint32_t kind) {
    QVariantList result;
    for (const auto* descriptor : registry.byKind(kind)) {
        const QString id = QString::fromUtf8(descriptor->id);
        result.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("label"), localizedComponentLabel(id, QString::fromUtf8(descriptor->name))},
            {QStringLiteral("version"), QString::fromUtf8(descriptor->version)},
        });
    }
    return result;
}
} // namespace

QVariantList AppController::inputComponents() const {
    return componentList(m_componentRegistry, AKRION_COMPONENT_INPUT);
}

QVariantList AppController::noiseComponents() const {
    return componentList(m_componentRegistry, AKRION_COMPONENT_NOISE);
}

QVariantList AppController::algorithmComponents() const {
    QVariantList result = componentList(m_componentRegistry, AKRION_COMPONENT_ALGORITHM);
    result.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("disabled")},
                              {QStringLiteral("label"), QStringLiteral("不启用算法")},
                              {QStringLiteral("version"), QStringLiteral("host")}});
    return result;
}

AppController::AppController(QObject* parent)
    : QObject(parent),
      m_store(core::RunStore::defaultRoot()),
      m_componentRegistry(core::ComponentRegistry::withBuiltins()) {
    m_config = baseConfig();
    m_validator = std::make_unique<core::FrameValidator>(m_config);
    ensureChannels(m_config);
    m_demoTimer.setTimerType(Qt::PreciseTimer);
    m_demoTimer.setInterval(10);
    connect(&m_demoTimer, &QTimer::timeout, this, &AppController::emitDemoFrame);
    connect(&m_serial, &QSerialPort::readyRead, this, &AppController::readSerial);
    connect(&m_serial, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError error) {
                if (error == QSerialPort::NoError || error == QSerialPort::TimeoutError) return;
                reportError(m_serial.errorString());
                if (error == QSerialPort::ResourceError ||
                    error == QSerialPort::DeviceNotFoundError)
                    disconnectSource();
            });
    setStatus(QStringLiteral("连接串口或启动演示源"));
    refreshPorts();
    refreshHistory();
}

AppController::~AppController() {
    if (recording()) stopRecordingWithStatus(core::RunStatus::Interrupted,
                                             QStringLiteral("application_closed"));
}

core::RunConfig AppController::baseConfig() const {
    core::RunConfig config;
    config.name = QStringLiteral("gui-session");
    config.deviceId = QStringLiteral("device-01");
    config.timing.algorithmPeriodUs = 1000;
    config.timing.emitPeriodUs = 10000;
    config.timing.maxDeviationUs = 1000;
    config.channels = {
        {QStringLiteral("target"), QStringLiteral("目标"), QString(),
         core::ChannelRole::Reference, QStringLiteral("目标输入"), {}},
        {QStringLiteral("actual"), QStringLiteral("实际"), QString(),
         core::ChannelRole::Measurement, QStringLiteral("实际测量"), {}},
        {QStringLiteral("control"), QStringLiteral("控制量"), QString(),
         core::ChannelRole::Control, QStringLiteral("算法输出"), {}},
        {QStringLiteral("noise"), QStringLiteral("扰动"), QString(),
         core::ChannelRole::Disturbance, QStringLiteral("扰动或噪声"), {}},
        {QStringLiteral("error"), QStringLiteral("误差"), QString(),
         core::ChannelRole::Diagnostic, QStringLiteral("参考与测量的差值"), {}},
    };
    core::AlgorithmDefinition algorithm;
    algorithm.id = 1;
    algorithm.displayName = QStringLiteral("GUI algorithm");
    algorithm.implementationLanguage = core::ImplementationLanguage::Other;
    algorithm.sourceReference = QStringLiteral("device");
    algorithm.revision = QStringLiteral("unknown");
    algorithm.buildHash = QStringLiteral("unknown");
    algorithm.license = QStringLiteral("NOASSERTION");
    config.algorithms = {algorithm};
    config.metrics = {{QStringLiteral("tracking_error"), QStringLiteral("target"),
                       QStringLiteral("actual"), {}}};
    config.scenario = {{QStringLiteral("source"), QStringLiteral("gui")}};
    return config;
}

void AppController::setStatus(const QString& status) {
    if (m_statusMessage == status) return;
    m_statusMessage = status;
    emit statusChanged();
}

void AppController::reportError(const QString& message) {
    setStatus(message);
    emit errorOccurred(message);
}

void AppController::refreshPorts() {
    QStringList next;
    for (const auto& info : QSerialPortInfo::availablePorts()) next.append(info.portName());
    if (next == m_ports) return;
    m_ports = next;
    emit portsChanged();
}

void AppController::resetLiveState() {
    m_decoder.reset();
    m_config = baseConfig();
    m_validator = std::make_unique<core::FrameValidator>(m_config);
    m_waveform.resetChannels();
    ensureChannels(m_config);
    m_receivedFrames = 0;
    m_parseErrors = 0;
    m_framesSinceRateUpdate = 0;
    m_frameRate = 0.0;
    m_demoSequence = 0;
    m_hostEpochUs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000ULL;
    m_hostClock.restart();
    m_rateClock.restart();
    emit statisticsChanged();
}

bool AppController::connectSerial(const QString& portName, int baudRate) {
    disconnectSource();
    resetLiveState();
    m_waveform.resetChannels();
    m_config.serial.portName = portName;
    m_config.serial.baudRate = baudRate;
    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);
    if (!m_serial.open(QIODevice::ReadOnly)) {
        reportError(QStringLiteral("无法打开串口 %1: %2").arg(portName, m_serial.errorString()));
        emit sourceChanged();
        return false;
    }
    setStatus(QStringLiteral("串口 %1 已连接").arg(portName));
    emit sourceChanged();
    return true;
}

void AppController::startDemo(
    const QString& inputComponent,
    const QString& noiseComponent,
    const QString& algorithmComponent) {
    const QString input = inputComponent.trimmed().toLower();
    const QString noise = noiseComponent.trimmed().toLower();
    const QString algorithm = algorithmComponent.trimmed().toLower();
    const auto* inputDescriptor = m_componentRegistry.find(input);
    if (!inputDescriptor || inputDescriptor->kind != AKRION_COMPONENT_INPUT) {
        reportError(QStringLiteral("未知输入组件：%1").arg(inputComponent));
        return;
    }
    const auto* noiseDescriptor = m_componentRegistry.find(noise);
    if (!noiseDescriptor || noiseDescriptor->kind != AKRION_COMPONENT_NOISE) {
        reportError(QStringLiteral("未知噪声组件：%1").arg(noiseComponent));
        return;
    }
    const auto* algorithmDescriptor = m_componentRegistry.find(algorithm);
    if (algorithm != QStringLiteral("disabled")
        && (!algorithmDescriptor || algorithmDescriptor->kind != AKRION_COMPONENT_ALGORITHM)) {
        reportError(QStringLiteral("未知算法组件：%1").arg(algorithmComponent));
        return;
    }

    disconnectSource();
    resetLiveState();
    m_demoInputComponent = input;
    m_demoNoiseComponent = noise;
    m_demoAlgorithmComponent = algorithm;
    const QString inputLabel = input == QStringLiteral("step") ? QStringLiteral("阶跃")
        : input == QStringLiteral("sine") ? QStringLiteral("正弦")
        : input == QStringLiteral("sweep") ? QStringLiteral("扫频") : QStringLiteral("常量");
    const QString noiseLabel = noise == QStringLiteral("gaussian") ? QStringLiteral("高斯噪声")
        : noise == QStringLiteral("uniform") ? QStringLiteral("均匀噪声") : QStringLiteral("无噪声");
    const QString algorithmLabel = algorithm == QStringLiteral("disabled")
        ? QStringLiteral("不启用算法")
        : localizedComponentLabel(algorithm, QString::fromUtf8(algorithmDescriptor->name));
    m_config.name = QStringLiteral("gui-demo-%1-%2-%3").arg(input, noise, algorithm);
    m_config.deviceId = QStringLiteral("demo-device");
    auto& definition = m_config.algorithms.first();
    definition.displayName = algorithmLabel;
    definition.implementationLanguage = core::ImplementationLanguage::Cpp;
    definition.sourceReference = QStringLiteral("builtin-demo");
    definition.revision = algorithmDescriptor
        ? QString::fromUtf8(algorithmDescriptor->version) : QStringLiteral("host");
    definition.buildHash = QStringLiteral("builtin");
    definition.license = QStringLiteral("Apache-2.0");
    definition.parameters = algorithm == QStringLiteral("p_control")
        ? QJsonObject{{QStringLiteral("kp"), 1.2},
                      {QStringLiteral("plant_time_constant_s"), 0.25}}
        : algorithm == QStringLiteral("pid")
            ? QJsonObject{{QStringLiteral("kp"), 1.2},
                          {QStringLiteral("ki"), 0.08},
                          {QStringLiteral("kd"), 0.02}}
        : algorithm == QStringLiteral("median_filter")
            ? QJsonObject{{QStringLiteral("window"), 5}}
        : QJsonObject{};
    m_config.scenario = {
        {QStringLiteral("source"), QStringLiteral("demo")},
        {QStringLiteral("input"), QJsonObject{
            {QStringLiteral("component"), input},
            {QStringLiteral("version"), QString::fromUtf8(inputDescriptor->version)},
            {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("profile"), input}}}}},
        {QStringLiteral("noise"), QJsonObject{{QStringLiteral("component"), noise},
                                               {QStringLiteral("version"), QString::fromUtf8(noiseDescriptor->version)},
                                               {QStringLiteral("parameters"), QJsonObject{
                                                   {QStringLiteral("profile"), noise},
                                                   {QStringLiteral("amplitude"), 0.02},
                                                   {QStringLiteral("seed"), 1}}}}},
        {QStringLiteral("algorithm"), QJsonObject{
            {QStringLiteral("component"), algorithm},
            {QStringLiteral("version"), algorithmDescriptor
                ? QString::fromUtf8(algorithmDescriptor->version) : QStringLiteral("host")},
            {QStringLiteral("parameters"), definition.parameters}}},
    };
    m_demoPipeline = std::make_unique<core::ComponentPipeline>(m_config);
    QString pipelineError;
    if (!m_demoPipeline->open(&pipelineError)) {
        m_demoPipeline.reset();
        reportError(pipelineError);
        return;
    }
    m_validator = std::make_unique<core::FrameValidator>(m_config);
    m_demoTimer.setInterval(qMax(1, static_cast<int>(m_config.timing.emitPeriodUs / 1000)));
    m_demoTimer.start();
    setStatus(QStringLiteral("演示：%1 / %2 / %3")
                  .arg(inputLabel, noiseLabel, algorithmLabel));
    emit sourceChanged();
}
void AppController::disconnectSource() {
    const bool wasConnected = connected();
    m_demoTimer.stop();
    m_demoPipeline.reset();
    if (m_serial.isOpen()) m_serial.close();
    if (m_decoder.bufferedBytes() > 0) {
        const auto hostTimeUs = this->hostTimeUs();
        processBatch(m_decoder.finish(hostTimeUs), hostTimeUs);
    }
    if (recording()) stopRecordingWithStatus(core::RunStatus::Interrupted,
                                             QStringLiteral("source_disconnected"));
    m_decoder.reset();
    if (wasConnected) {
        setStatus(QStringLiteral("数据源已断开"));
        emit sourceChanged();
    }
}

void AppController::readSerial() {
    const auto bytes = m_serial.readAll();
    if (!bytes.isEmpty()) consumeBytes(bytes);
}

void AppController::emitDemoFrame() {
    if (!m_demoPipeline) return;
    core::Frame frame;
    QString error;
    if (!m_demoPipeline->nextFrame(m_demoSequence, &frame, &error)) {
        reportError(error);
        disconnectSource();
        return;
    }
    auto bytes = QJsonDocument(core::toJson(frame)).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    ++m_demoSequence;
    consumeBytes(bytes);
}
void AppController::consumeBytes(const QByteArray& bytes) {
    const auto hostTimeUs = this->hostTimeUs();
    if (recording()) {
        QString error;
        if (!m_writer->appendRaw(bytes, &error)) {
            reportError(error);
            stopRecordingWithStatus(core::RunStatus::Interrupted, QStringLiteral("write_failed"));
        }
    }
    processBatch(m_decoder.feed(bytes, hostTimeUs), hostTimeUs);
}

void AppController::processBatch(const core::DecodeBatch& batch, quint64 hostTimeUs) {
    for (const auto& decodeError : batch.errors) {
        ++m_parseErrors;
        if (m_statistics) m_statistics->observeDecodeError(decodeError);
        core::RunEvent event;
        event.type = QStringLiteral("parse_error");
        event.severity = core::EventSeverity::Error;
        event.message = decodeError.message;
        event.hostTimeUs = hostTimeUs;
        event.details = {{QStringLiteral("code"), decodeError.code},
                         {QStringLiteral("line"), static_cast<qint64>(decodeError.lineNumber)},
                         {QStringLiteral("byte_offset"), static_cast<qint64>(decodeError.byteOffset)}};
        appendEvent(event);
        publishEvent(event);
    }
    for (const auto& frame : batch.frames) {
        ensureFrameChannels(frame);
        const auto issues = m_validator ? m_validator->inspect(frame)
                                        : QVector<core::ValidationIssue>{};
        if (m_statistics) m_statistics->observeFrame(frame, issues);
        if (recording()) {
            QString error;
            if (!m_writer->appendFrame(frame, &error)) {
                reportError(error);
                stopRecordingWithStatus(core::RunStatus::Interrupted, QStringLiteral("write_failed"));
            }
        }
        for (const auto& issue : issues) {
            const auto event = core::validationIssueEvent(issue, frame);
            appendEvent(event);
            publishEvent(event);
        }
        publishFrame(frame);
        ++m_receivedFrames;
        ++m_framesSinceRateUpdate;
    }
    updateRate();
    emit statisticsChanged();
}

void AppController::publishFrame(const core::Frame& frame) {
    WaveformFrame waveformFrame;
    waveformFrame.deviceTimeUs = static_cast<qint64>(frame.deviceTimeUs);
    waveformFrame.hostReceiveTimeUs = static_cast<qint64>(frame.hostReceiveTimeUs.value_or(0));
    waveformFrame.sequence = frame.seq;
    waveformFrame.algorithmId = static_cast<qint32>(frame.algoId);
    waveformFrame.algorithmEnabled = frame.algoEnabled;
    waveformFrame.values.reserve(frame.values.size());
    for (auto it = frame.values.cbegin(); it != frame.values.cend(); ++it)
        waveformFrame.values.append({it.key(), it.value()});
    m_waveform.appendFrame(waveformFrame);
}

void AppController::publishEvent(const core::RunEvent& event) {
    WaveformEvent waveformEvent;
    waveformEvent.deviceTimeUs = static_cast<qint64>(event.deviceTimeUs.value_or(0));
    waveformEvent.hostReceiveTimeUs = static_cast<qint64>(event.hostTimeUs);
    waveformEvent.type = event.type;
    waveformEvent.label = event.message;
    waveformEvent.severity = event.severity == core::EventSeverity::Error ? 2
        : event.severity == core::EventSeverity::Warning ? 1
                                                         : 0;
    m_waveform.appendEvent(waveformEvent);
}

void AppController::ensureChannels(const core::RunConfig& config) {
    for (const auto& channel : config.channels) {
        const QString color = channel.extraFields.value(QStringLiteral("color")).toString();
        m_waveform.defineChannel(channel.key, channel.label, channel.unit,
                                 core::channelRoleName(channel.role), channel.description,
                                 QString(), color);
    }
}

void AppController::ensureFrameChannels(const core::Frame& frame) {
    for (auto it = frame.values.cbegin(); it != frame.values.cend(); ++it) {
        if (m_waveform.hasChannel(it.key())) continue;
        m_waveform.defineChannel(it.key(), it.key(), QString(), QStringLiteral("other"),
                                 QStringLiteral("Discovered from the input stream"));
    }
}

bool AppController::appendEvent(const core::RunEvent& event) {
    if (!recording()) return true;
    QString error;
    if (m_writer->appendEvent(event, &error)) return true;
    reportError(error);
    return false;
}

void AppController::updateRate() {
    const auto elapsed = m_rateClock.elapsed();
    if (elapsed < 500) return;
    m_frameRate = m_framesSinceRateUpdate * 1000.0 / qMax<qint64>(1, elapsed);
    m_framesSinceRateUpdate = 0;
    m_rateClock.restart();
}

quint64 AppController::hostTimeUs() const {
    return m_hostEpochUs + static_cast<quint64>(m_hostClock.nsecsElapsed() / 1000);
}

bool AppController::startRecording(const QString& runName,
                                   const QString& deviceId,
                                   const QString& algorithmName,
                                   const QString& parametersJson,
                                   int algorithmPeriodUs,
                                   int emitPeriodUs) {
    if (!connected()) {
        reportError(QStringLiteral("请先连接串口或启动演示源"));
        return false;
    }
    if (recording()) return true;
    if (m_waveform.channelDefinitions().isEmpty()) {
        reportError(QStringLiteral("尚未收到通道数据，请等待首帧后再开始录制"));
        return false;
    }
    QJsonObject parameters;
    if (!parametersJson.trimmed().isEmpty()) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(parametersJson.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            reportError(QStringLiteral("算法参数不是有效 JSON 对象"));
            return false;
        }
        parameters = document.object();
    }
    if (algorithmPeriodUs <= 0 || emitPeriodUs <= 0) {
        reportError(QStringLiteral("算法周期和发送周期必须大于 0"));
        return false;
    }
    m_config.name = runName.trimmed().isEmpty() ? QStringLiteral("gui-session") : runName.trimmed();
    m_config.deviceId = deviceId.trimmed().isEmpty() ? QStringLiteral("unknown-device")
                                                     : deviceId.trimmed();
    m_config.timing.algorithmPeriodUs = static_cast<quint64>(algorithmPeriodUs);
    m_config.timing.emitPeriodUs = static_cast<quint64>(emitPeriodUs);
    if (m_config.algorithms.isEmpty()) m_config.algorithms.append(core::AlgorithmDefinition{});
    m_config.algorithms.first().id = 1;
    m_config.algorithms.first().displayName = algorithmName.trimmed().isEmpty()
        ? QStringLiteral("Algorithm 1") : algorithmName.trimmed();
    m_config.algorithms.first().parameters = parameters;
    auto algorithmScenario = m_config.scenario.value(QStringLiteral("algorithm")).toObject();
    algorithmScenario.insert(QStringLiteral("parameters"), parameters);
    m_config.scenario.insert(QStringLiteral("algorithm"), algorithmScenario);
    m_config.channels.clear();
    for (const auto& channel : m_waveform.channelDefinitions()) {
        core::ChannelDefinition definition;
        definition.key = channel.key;
        definition.label = channel.label;
        definition.unit = channel.unit;
        definition.role = core::channelRoleFromName(channel.role).value_or(core::ChannelRole::Other);
        definition.description = channel.description;
        definition.extraFields.insert(QStringLiteral("color"), channel.color.name());
        m_config.channels.append(definition);
    }
    m_config.scenario.insert(QStringLiteral("source"), demo() ? QStringLiteral("demo")
                                                              : QStringLiteral("serial"));
    m_validator = std::make_unique<core::FrameValidator>(m_config);
    m_statistics = std::make_unique<core::RunStatistics>(m_config);
    m_decoder.reset();
    const auto duplicates = m_store.findByExperimentSignature(core::experimentSignature(m_config));
    m_writer = std::make_unique<core::RunWriter>();
    core::RunWriterOptions options;
    options.storeRoot = m_store.root();
    options.source = demo() ? QStringLiteral("demo") : QStringLiteral("serial");
    QString error;
    if (!m_writer->start(m_config, options, &error)) {
        m_writer.reset();
        m_statistics.reset();
        reportError(error);
        return false;
    }
    if (demo())
        m_demoTimer.setInterval(qMax(1, static_cast<int>(m_config.timing.emitPeriodUs / 1000)));
    core::RunEvent startEvent;
    startEvent.type = QStringLiteral("capture_start");
    startEvent.message = QStringLiteral("Recording started");
    startEvent.hostTimeUs = hostTimeUs();
    appendEvent(startEvent);
    if (!duplicates.isEmpty())
        setStatus(QStringLiteral("开始录制；相同配置已有运行 %1").arg(duplicates.first().manifest.runId));
    else
        setStatus(QStringLiteral("正在录制 %1").arg(m_writer->runId()));
    emit recordingChanged();
    return true;
}

bool AppController::stopRecording() {
    if (!recording()) return false;
    stopRecordingWithStatus(core::RunStatus::Completed);
    return true;
}

void AppController::stopRecordingWithStatus(core::RunStatus status, const QString& reason) {
    if (!recording()) return;
    const auto runId = m_writer->runId();
    const auto path = m_writer->runDirectory();
    core::RunEvent stopEvent;
    stopEvent.type = QStringLiteral("capture_stop");
    stopEvent.message = reason.isEmpty() ? QStringLiteral("Recording stopped") : reason;
    stopEvent.hostTimeUs = hostTimeUs();
    appendEvent(stopEvent);
    auto summary = m_statistics ? m_statistics->summary(status)
                                : QJsonObject{{QStringLiteral("schema"),
                                               QString::fromLatin1(core::kSummarySchema)}};
    QString error;
    if (!m_writer->finish(status, summary, &error)) reportError(error);
    m_writer.reset();
    m_statistics.reset();
    refreshHistory();
    setStatus(QStringLiteral("运行 %1 已保存到 %2").arg(runId, path));
    emit recordingChanged();
}

void AppController::refreshHistory() {
    QVariantList next;
    for (const auto& info : m_store.list()) {
        QVariantMap item;
        item.insert(QStringLiteral("run_id"), info.manifest.runId);
        item.insert(QStringLiteral("name"), info.manifest.name);
        item.insert(QStringLiteral("status"), core::runStatusName(info.manifest.status));
        item.insert(QStringLiteral("frames"), QVariant::fromValue<qulonglong>(info.manifest.frameCount));
        item.insert(QStringLiteral("created_at"), info.manifest.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        item.insert(QStringLiteral("path"), info.path);
        next.append(item);
    }
    m_history = next;
    emit historyChanged();
}

bool AppController::replayRun(const QString& runId) {
    disconnectSource();
    const auto path = m_store.resolve(runId);
    if (path.isEmpty()) {
        reportError(QStringLiteral("找不到运行 %1").arg(runId));
        return false;
    }
    core::RunReader reader;
    QString error;
    if (!reader.open(path, &error)) {
        reportError(error);
        return false;
    }
    m_waveform.resetChannels();
    m_config = reader.config();
    ensureChannels(m_config);
    m_receivedFrames = 0;
    m_parseErrors = 0;
    if (!reader.forEachFrame([this](const core::Frame& frame) {
            ensureFrameChannels(frame);
            publishFrame(frame);
            ++m_receivedFrames;
            if ((m_receivedFrames % 2000) == 0)
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            return true;
        }, &error)) {
        reportError(error);
        return false;
    }
    reader.forEachEvent([this](const core::RunEvent& event) {
        publishEvent(event);
        return true;
    }, nullptr);
    m_frameRate = 0.0;
    emit statisticsChanged();
    setStatus(QStringLiteral("已载入运行 %1，共 %2 帧").arg(runId).arg(m_receivedFrames));
    return true;
}

void AppController::clearWaveform() {
    m_waveform.clear();
    m_receivedFrames = 0;
    m_parseErrors = 0;
    m_frameRate = 0.0;
    emit statisticsChanged();
}

} // namespace akrion::gui
