#include "../core/frame_decoder.h"
#include "../core/frame_validator.h"
#include "../core/component_pipeline.h"
#include "../core/component_registry.h"
#include "../core/run_storage.h"
#include "../core/statistics.h"
#include "../core/types.h"
#include "../package/runpackage.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTemporaryFile>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

using namespace akrion::core;

constexpr int kInternalError = 1;
constexpr int kCliError = 2;
constexpr int kIoError = 3;
constexpr int kValidationError = 4;
constexpr int kInterrupted = 130;

std::atomic_bool g_interrupted = false;

#ifdef Q_OS_WIN
BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_interrupted.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
void signalHandler(int) { g_interrupted.store(true); }
#endif

void installInterruptHandler() {
#ifdef Q_OS_WIN
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif
}

struct CliContext {
    QTextStream out{stdout};
    QTextStream err{stderr};
    bool json = false;
    bool verbose = false;
    QString command;
    RunStore store;
};

void writeJsonLine(QTextStream& stream, const QJsonObject& object) {
    stream << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
}

QJsonObject resultEnvelope(const CliContext& context, bool ok, const QJsonValue& data,
                           const QString& message = {}) {
    QJsonObject object{{QStringLiteral("schema"), QStringLiteral("akrion.cli/1")},
                       {QStringLiteral("ok"), ok},
                       {QStringLiteral("command"), context.command}};
    if (!data.isUndefined()) object.insert(QStringLiteral("data"), data);
    if (!message.isEmpty()) object.insert(QStringLiteral("message"), message);
    return object;
}

int fail(CliContext& context, int exitCode, const QString& message,
         const QJsonObject& details = {}) {
    if (context.json) {
        auto object = resultEnvelope(context, false, details, message);
        object.insert(QStringLiteral("exit_code"), exitCode);
        writeJsonLine(context.out, object);
    } else {
        context.err << "error: " << message << Qt::endl;
    }
    return exitCode;
}

void succeed(CliContext& context, const QJsonValue& data, const QString& human = {});

QString componentKindName(uint32_t kind) {
    if (kind == AKRION_COMPONENT_INPUT) return QStringLiteral("input");
    if (kind == AKRION_COMPONENT_NOISE) return QStringLiteral("noise");
    return QStringLiteral("algorithm");
}

int commandComponents(CliContext& context) {
    const ComponentRegistry registry = ComponentRegistry::withBuiltins();
    QJsonArray components;
    for (const auto* descriptor : registry.all()) {
        QJsonArray inputs;
        for (size_t index = 0; index < descriptor->input_count; ++index)
            inputs.append(QString::fromUtf8(descriptor->inputs[index].key));
        QJsonArray outputs;
        for (size_t index = 0; index < descriptor->output_count; ++index)
            outputs.append(QString::fromUtf8(descriptor->outputs[index].key));
        components.append(QJsonObject{
            {QStringLiteral("id"), QString::fromUtf8(descriptor->id)},
            {QStringLiteral("name"), QString::fromUtf8(descriptor->name)},
            {QStringLiteral("version"), QString::fromUtf8(descriptor->version)},
            {QStringLiteral("kind"), componentKindName(descriptor->kind)},
            {QStringLiteral("inputs"), inputs},
            {QStringLiteral("outputs"), outputs},
        });
    }
    if (context.json) {
        succeed(context, QJsonObject{{QStringLiteral("components"), components}});
    } else {
        for (const auto& value : components) {
            const auto object = value.toObject();
            context.out << object.value(QStringLiteral("kind")).toString() << "\t"
                        << object.value(QStringLiteral("id")).toString() << "\t"
                        << object.value(QStringLiteral("version")).toString() << "\t"
                        << object.value(QStringLiteral("name")).toString() << Qt::endl;
        }
    }
    return 0;
}

void succeed(CliContext& context, const QJsonValue& data, const QString& human) {
    if (context.json) writeJsonLine(context.out, resultEnvelope(context, true, data));
    else if (!human.isEmpty()) context.out << human << Qt::endl;
}

void streamEvent(CliContext& context, const QString& type, const QJsonObject& data) {
    if (context.json) {
        writeJsonLine(context.out,
                      {{QStringLiteral("schema"), QStringLiteral("akrion.cli.event/1")},
                       {QStringLiteral("command"), context.command},
                       {QStringLiteral("type"), type},
                       {QStringLiteral("data"), data}});
    } else if (type == QStringLiteral("progress")) {
        context.err << data.value(QStringLiteral("frames")).toInteger() << " frames\r";
        context.err.flush();
    } else if (type == QStringLiteral("duplicate")) {
        context.err << "matching experiment already exists: "
                    << data.value(QStringLiteral("path")).toString() << Qt::endl;
    }
}

RunConfig defaultConfig() {
    RunConfig config;
    config.name = QStringLiteral("demo-pid-step");
    config.deviceId = QStringLiteral("device-01");
    config.serial.portName = QStringLiteral("COM3");
    config.serial.baudRate = 115200;
    config.timing.algorithmPeriodUs = 1000;
    config.timing.emitPeriodUs = 10000;
    config.timing.maxDeviationUs = 1000;
    config.channels = {
        {QStringLiteral("target"), QStringLiteral("Target"), QString(),
         ChannelRole::Reference, QStringLiteral("Commanded value"), {}},
        {QStringLiteral("actual"), QStringLiteral("Actual"), QString(),
         ChannelRole::Measurement, QStringLiteral("Measured response"), {}},
        {QStringLiteral("control"), QStringLiteral("Control"), QString(),
         ChannelRole::Control, QStringLiteral("Algorithm output"), {}},
        {QStringLiteral("noise"), QStringLiteral("Noise"), QString(),
         ChannelRole::Disturbance, QStringLiteral("Injected demo disturbance"), {}},
        {QStringLiteral("error"), QStringLiteral("Error"), QString(),
         ChannelRole::Diagnostic, QStringLiteral("Reference minus measurement"), {}},
    };
    AlgorithmDefinition algorithm;
    algorithm.id = 1;
    algorithm.displayName = QStringLiteral("Demo PID");
    algorithm.implementationLanguage = ImplementationLanguage::Cpp;
    algorithm.sourceReference = QStringLiteral("builtin-demo");
    algorithm.revision = QStringLiteral("dev");
    algorithm.buildHash = QStringLiteral("local");
    algorithm.license = QStringLiteral("Apache-2.0");
    algorithm.parameters = {{QStringLiteral("kp"), 1.2},
                            {QStringLiteral("ki"), 0.08},
                            {QStringLiteral("kd"), 0.02}};
    config.algorithms = {algorithm};
    config.metrics = {{QStringLiteral("tracking_error"), QStringLiteral("target"),
                       QStringLiteral("actual"), {}}};
    config.scenario = {
        {QStringLiteral("input"),
         QJsonObject{{QStringLiteral("component"), QStringLiteral("step")},
                     {QStringLiteral("version"), QStringLiteral("1.0.0")},
                     {QStringLiteral("parameters"), QJsonObject{
                         {QStringLiteral("profile"), QStringLiteral("step")},
                         {QStringLiteral("offset"), 0.0},
                         {QStringLiteral("amplitude"), 1.0},
                         {QStringLiteral("step_at_s"), 2.0}}}}},
        {QStringLiteral("noise"),
         QJsonObject{{QStringLiteral("component"), QStringLiteral("gaussian")},
                     {QStringLiteral("version"), QStringLiteral("1.0.0")},
                     {QStringLiteral("parameters"), QJsonObject{
                         {QStringLiteral("profile"), QStringLiteral("gaussian")},
                         {QStringLiteral("amplitude"), 0.02},
                         {QStringLiteral("seed"), 1}}}}},
        {QStringLiteral("algorithm"),
         QJsonObject{{QStringLiteral("component"), QStringLiteral("p_control")},
                     {QStringLiteral("version"), QStringLiteral("1.0.0")},
                     {QStringLiteral("parameters"), algorithm.parameters}}},
        {QStringLiteral("seed"), 1},
    };
    return config;
}

bool readConfig(const QString& path, RunConfig* config, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open config %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("invalid config JSON: %1").arg(parseError.errorString());
        return false;
    }
    return runConfigFromJson(document.object(), config, error);
}

bool writeJsonFile(const QString& path, const QJsonObject& object, bool overwrite,
                   QString* error) {
    if (!overwrite && QFileInfo::exists(path)) {
        if (error) *error = QStringLiteral("file already exists: %1").arg(path);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot create %1: %2").arg(path, file.errorString());
        return false;
    }
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString csvCell(const QString& value) {
    if (!value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('"')) &&
        !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r')))
        return value;
    auto escaped = value;
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString resolvedRunPath(const CliContext& context, const QString& idOrPath) {
    return context.store.resolve(idOrPath);
}

QJsonObject manifestResult(const RunInfo& info) {
    auto object = toJson(info.manifest);
    object.insert(QStringLiteral("path"), info.path);
    return object;
}

int commandDoctor(CliContext& context) {
    QJsonArray checks;
    bool ok = true;
    const auto addCheck = [&](const QString& name, bool passed, const QString& detail) {
        checks.append(QJsonObject{{QStringLiteral("name"), name},
                                 {QStringLiteral("ok"), passed},
                                 {QStringLiteral("detail"), detail}});
        ok = ok && passed;
    };
    QDir root(context.store.root());
    const bool created = root.exists() || QDir().mkpath(root.path());
    addCheck(QStringLiteral("store_exists"), created, root.absolutePath());
    bool writable = false;
    if (created) {
        QTemporaryFile probe(root.filePath(QStringLiteral(".akrion-doctor-XXXXXX")));
        writable = probe.open();
    }
    addCheck(QStringLiteral("store_writable"), writable, root.absolutePath());
    const auto ports = QSerialPortInfo::availablePorts();
    addCheck(QStringLiteral("serial_backend"), true,
             QStringLiteral("%1 port(s) available").arg(ports.size()));
    int unfinished = 0;
    for (const auto& run : context.store.list())
        if (run.manifest.status == RunStatus::Recording) ++unfinished;
    addCheck(QStringLiteral("unfinished_runs"), true,
             QStringLiteral("%1 run(s) still marked recording").arg(unfinished));
    QJsonObject data{{QStringLiteral("checks"), checks},
                     {QStringLiteral("store"), root.absolutePath()},
                     {QStringLiteral("unfinished_runs"), unfinished}};
    if (!ok) return fail(context, kIoError, QStringLiteral("one or more checks failed"), data);
    if (context.json) succeed(context, data);
    else {
        context.out << "Akrion doctor\n";
        for (const auto& value : checks) {
            const auto check = value.toObject();
            context.out << (check.value(QStringLiteral("ok")).toBool() ? "[ok] " : "[fail] ")
                        << check.value(QStringLiteral("name")).toString() << ": "
                        << check.value(QStringLiteral("detail")).toString() << '\n';
        }
        context.out.flush();
    }
    return 0;
}

int commandPorts(CliContext& context) {
    QJsonArray result;
    for (const auto& info : QSerialPortInfo::availablePorts()) {
        result.append(QJsonObject{{QStringLiteral("port"), info.portName()},
                                 {QStringLiteral("description"), info.description()},
                                 {QStringLiteral("manufacturer"), info.manufacturer()},
                                 {QStringLiteral("serial_number"), info.serialNumber()},
                                 {QStringLiteral("system_location"), info.systemLocation()},
                                 {QStringLiteral("vendor_id"), info.hasVendorIdentifier()
                                      ? QJsonValue(static_cast<qint64>(info.vendorIdentifier()))
                                      : QJsonValue()},
                                 {QStringLiteral("product_id"), info.hasProductIdentifier()
                                      ? QJsonValue(static_cast<qint64>(info.productIdentifier()))
                                      : QJsonValue()}});
    }
    if (context.json) succeed(context, result);
    else if (result.isEmpty()) context.out << "No serial ports found.\n";
    else for (const auto& value : result) {
        const auto port = value.toObject();
        context.out << port.value(QStringLiteral("port")).toString();
        const auto description = port.value(QStringLiteral("description")).toString();
        if (!description.isEmpty()) context.out << "  " << description;
        context.out << '\n';
    }
    context.out.flush();
    return 0;
}

int commandInit(CliContext& context, const QCommandLineParser& parser,
                const QStringList& positional) {
    const auto path = positional.size() >= 2 ? positional.at(1)
                                             : QStringLiteral("akrion.config.json");
    QString error;
    if (!writeJsonFile(path, toJson(defaultConfig()), parser.isSet(QStringLiteral("force")),
                       &error))
        return fail(context, kIoError, error);
    succeed(context, QJsonObject{{QStringLiteral("path"), QFileInfo(path).absoluteFilePath()}},
            QStringLiteral("Created %1").arg(QFileInfo(path).absoluteFilePath()));
    return 0;
}

int commandRunsList(CliContext& context, const QCommandLineParser& parser) {
    const auto statusFilter = parser.value(QStringLiteral("status"));
    const auto signatureFilter = parser.value(QStringLiteral("signature"));
    QJsonArray result;
    for (const auto& info : context.store.list()) {
        if (!statusFilter.isEmpty() && runStatusName(info.manifest.status) != statusFilter) continue;
        if (!signatureFilter.isEmpty() && info.manifest.experimentSignature != signatureFilter)
            continue;
        result.append(manifestResult(info));
    }
    if (context.json) succeed(context, result);
    else if (result.isEmpty()) context.out << "No runs found in " << context.store.root() << '\n';
    else for (const auto& value : result) {
        const auto run = value.toObject();
        context.out << run.value(QStringLiteral("run_id")).toString() << "  "
                    << run.value(QStringLiteral("status")).toString() << "  "
                    << run.value(QStringLiteral("frame_count")).toInteger() << " frames  "
                    << run.value(QStringLiteral("name")).toString() << '\n';
    }
    context.out.flush();
    return 0;
}

int commandRunsShow(CliContext& context, const QStringList& positional) {
    if (positional.size() < 3)
        return fail(context, kCliError, QStringLiteral("usage: akrion runs show <run-id>"));
    const auto path = resolvedRunPath(context, positional.at(2));
    if (path.isEmpty()) return fail(context, kIoError, QStringLiteral("run not found"));
    RunReader reader;
    QString error;
    if (!reader.open(path, &error)) return fail(context, kValidationError, error);
    QJsonObject data{{QStringLiteral("path"), path},
                     {QStringLiteral("manifest"), toJson(reader.manifest())},
                     {QStringLiteral("config"), toJson(reader.config())},
                     {QStringLiteral("summary"), reader.summary()}};
    if (context.json) succeed(context, data);
    else context.out << QJsonDocument(data).toJson(QJsonDocument::Indented);
    context.out.flush();
    return 0;
}

QJsonObject packageResultJson(const akrion::PackageResult& result) {
    QJsonArray files;
    for (const auto& file : result.files) files.append(file);
    return {{QStringLiteral("output_path"), result.outputPath},
            {QStringLiteral("files"), files},
            {QStringLiteral("total_bytes"), static_cast<qint64>(result.totalBytes)},
            {QStringLiteral("message"), result.message}};
}

int commandValidate(CliContext& context, const QCommandLineParser& parser,
                    const QStringList& positional) {
    if (positional.size() < 2)
        return fail(context, kCliError, QStringLiteral("usage: akrion validate <path-or-run-id>"));
    const auto requested = positional.at(1);
    if (requested.endsWith(QStringLiteral(".akrion"), Qt::CaseInsensitive)) {
        const auto result = akrion::RunPackage::validate(requested);
        if (!result) return fail(context, kValidationError, result.message,
                                 packageResultJson(result));
        succeed(context, packageResultJson(result), QStringLiteral("Package is valid."));
        return 0;
    }
    const QFileInfo info(requested);
    if (info.isFile()) {
        RunConfig config;
        QString error;
        if (!readConfig(info.absoluteFilePath(), &config, &error))
            return fail(context, kValidationError, error);
        succeed(context,
                QJsonObject{{QStringLiteral("path"), info.absoluteFilePath()},
                            {QStringLiteral("config_signature"), configSignature(config)},
                            {QStringLiteral("experiment_signature"), experimentSignature(config)}},
                QStringLiteral("Configuration is valid."));
        return 0;
    }
    const auto path = resolvedRunPath(context, requested);
    if (path.isEmpty()) return fail(context, kIoError, QStringLiteral("run not found"));
    RunReader reader;
    QString error;
    if (!reader.open(path, &error)) return fail(context, kValidationError, error);
    if (parser.isSet(QStringLiteral("repair")) &&
        reader.manifest().status == RunStatus::Recording) {
        if (!context.store.recover(path, &error)) return fail(context, kValidationError, error);
        if (!reader.open(path, &error)) return fail(context, kValidationError, error);
    }
    const auto report = reader.validate(parser.isSet(QStringLiteral("repair")));
    QJsonArray errors;
    QJsonArray warnings;
    for (const auto& item : report.errors) errors.append(item);
    for (const auto& item : report.warnings) warnings.append(item);
    QJsonObject data{{QStringLiteral("path"), path},
                     {QStringLiteral("valid"), report.valid},
                     {QStringLiteral("repaired"), report.repaired},
                     {QStringLiteral("frame_count"), static_cast<qint64>(report.frameCount)},
                     {QStringLiteral("event_count"), static_cast<qint64>(report.eventCount)},
                     {QStringLiteral("raw_byte_count"), static_cast<qint64>(report.rawByteCount)},
                     {QStringLiteral("errors"), errors},
                     {QStringLiteral("warnings"), warnings}};
    if (!report.valid) return fail(context, kValidationError, QStringLiteral("run is invalid"), data);
    succeed(context, data, warnings.isEmpty() ? QStringLiteral("Run is valid.")
                                              : QStringLiteral("Run is valid with warnings."));
    return 0;
}

int commandPack(CliContext& context, const QCommandLineParser& parser,
                const QStringList& positional) {
    if (positional.size() < 2)
        return fail(context, kCliError, QStringLiteral("usage: akrion pack <run-id>"));
    const auto path = resolvedRunPath(context, positional.at(1));
    if (path.isEmpty()) return fail(context, kIoError, QStringLiteral("run not found"));
    RunReader reader;
    QString error;
    if (!reader.open(path, &error)) return fail(context, kValidationError, error);
    if (reader.manifest().status == RunStatus::Recording)
        return fail(context, kValidationError, QStringLiteral("cannot pack an active run"));
    auto output = parser.value(QStringLiteral("output"));
    if (output.isEmpty()) output = QDir::current().filePath(reader.manifest().runId + QStringLiteral(".akrion"));
    if (QFileInfo::exists(output) && !parser.isSet(QStringLiteral("force")))
        return fail(context, kIoError, QStringLiteral("output already exists: %1").arg(output));
    if (QFileInfo::exists(output)) QFile::remove(output);
    const auto result = akrion::RunPackage::pack(path, output);
    if (!result) return fail(context, kIoError, result.message, packageResultJson(result));
    succeed(context, packageResultJson(result), QStringLiteral("Packed %1").arg(result.outputPath));
    return 0;
}

int commandUnpack(CliContext& context, const QCommandLineParser& parser,
                  const QStringList& positional) {
    if (positional.size() < 2)
        return fail(context, kCliError, QStringLiteral("usage: akrion unpack <file.akrion>"));
    auto destination = parser.value(QStringLiteral("output"));
    if (destination.isEmpty()) {
        auto base = QFileInfo(positional.at(1)).completeBaseName();
        destination = QDir(context.store.root()).filePath(base);
    }
    if (QFileInfo::exists(destination))
        return fail(context, kIoError, QStringLiteral("destination already exists: %1").arg(destination));
    const auto result = akrion::RunPackage::unpack(positional.at(1), destination);
    if (!result) return fail(context, kValidationError, result.message,
                             packageResultJson(result));
    succeed(context, packageResultJson(result), QStringLiteral("Unpacked %1").arg(result.outputPath));
    return 0;
}

std::optional<quint64> secondsOptionUs(const QCommandLineParser& parser,
                                       const QString& name, QString* error) {
    if (!parser.isSet(name)) return std::nullopt;
    bool ok = false;
    const auto seconds = parser.value(name).toDouble(&ok);
    if (!ok || seconds < 0.0 || !std::isfinite(seconds)) {
        if (error) *error = QStringLiteral("--%1 must be a non-negative number").arg(name);
        return std::nullopt;
    }
    return static_cast<quint64>(seconds * 1000000.0);
}

int commandExport(CliContext& context, const QCommandLineParser& parser,
                  const QStringList& positional) {
    if (positional.size() < 2)
        return fail(context, kCliError, QStringLiteral("usage: akrion export <run-id>"));
    const auto path = resolvedRunPath(context, positional.at(1));
    if (path.isEmpty()) return fail(context, kIoError, QStringLiteral("run not found"));
    RunReader reader;
    QString error;
    if (!reader.open(path, &error)) return fail(context, kValidationError, error);
    auto format = parser.value(QStringLiteral("format")).toLower();
    if (format.isEmpty()) format = QStringLiteral("csv");
    if (format != QStringLiteral("csv") && format != QStringLiteral("ndjson") &&
        format != QStringLiteral("summary-json"))
        return fail(context, kCliError, QStringLiteral("unsupported export format: %1").arg(format));
    auto output = parser.value(QStringLiteral("output"));
    if (output.isEmpty()) {
        const auto suffix = format == QStringLiteral("csv") ? QStringLiteral(".csv")
            : format == QStringLiteral("ndjson") ? QStringLiteral(".ndjson")
                                                   : QStringLiteral(".summary.json");
        output = QDir::current().filePath(reader.manifest().runId + suffix);
    }
    if (QFileInfo::exists(output) && !parser.isSet(QStringLiteral("force")))
        return fail(context, kIoError, QStringLiteral("output already exists: %1").arg(output));
    if (format == QStringLiteral("summary-json")) {
        if (!writeJsonFile(output, reader.summary(), true, &error)) return fail(context, kIoError, error);
        succeed(context, QJsonObject{{QStringLiteral("path"), QFileInfo(output).absoluteFilePath()},
                                     {QStringLiteral("format"), format}},
                QStringLiteral("Exported %1").arg(QFileInfo(output).absoluteFilePath()));
        return 0;
    }

    const auto fromUs = secondsOptionUs(parser, QStringLiteral("from"), &error);
    if (parser.isSet(QStringLiteral("from")) && !fromUs) return fail(context, kCliError, error);
    const auto toUs = secondsOptionUs(parser, QStringLiteral("to"), &error);
    if (parser.isSet(QStringLiteral("to")) && !toUs) return fail(context, kCliError, error);
    QStringList channels;
    if (parser.isSet(QStringLiteral("channels"))) {
        for (auto key : parser.value(QStringLiteral("channels")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            key = key.trimmed();
            if (!key.isEmpty() && !channels.contains(key)) channels.append(key);
        }
    } else {
        for (const auto& channel : reader.config().channels) channels.append(channel.key);
    }
    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly))
        return fail(context, kIoError, QStringLiteral("cannot create %1: %2").arg(output, file.errorString()));
    bool writeOk = true;
    quint64 exported = 0;
    if (format == QStringLiteral("csv")) {
        QStringList header{QStringLiteral("device_time_us"), QStringLiteral("host_receive_time_us"),
                           QStringLiteral("algo_tick"), QStringLiteral("emit_tick"),
                           QStringLiteral("seq"), QStringLiteral("algo_id"),
                           QStringLiteral("algo_enabled")};
        for (const auto& channel : channels) header.append(channel);
        const auto bytes = header.join(QLatin1Char(',')).toUtf8() + '\n';
        writeOk = file.write(bytes) == bytes.size();
    }
    if (writeOk && !reader.forEachFrame([&](const Frame& frame) {
            if (fromUs && frame.deviceTimeUs < *fromUs) return true;
            if (toUs && frame.deviceTimeUs > *toUs) return false;
            QByteArray bytes;
            if (format == QStringLiteral("ndjson")) {
                bytes = QJsonDocument(toJson(frame)).toJson(QJsonDocument::Compact) + '\n';
            } else {
                QStringList row{QString::number(frame.deviceTimeUs),
                                frame.hostReceiveTimeUs ? QString::number(*frame.hostReceiveTimeUs) : QString(),
                                QString::number(frame.algoTick), QString::number(frame.emitTick),
                                QString::number(frame.seq), QString::number(frame.algoId),
                                frame.algoEnabled ? QStringLiteral("true") : QStringLiteral("false")};
                for (const auto& channel : channels) {
                    const auto it = frame.values.constFind(channel);
                    row.append(it == frame.values.cend() ? QString()
                                                         : QString::number(it.value(), 'g', 17));
                }
                for (auto& cell : row) cell = csvCell(cell);
                bytes = row.join(QLatin1Char(',')).toUtf8() + '\n';
            }
            if (file.write(bytes) != bytes.size()) {
                writeOk = false;
                return false;
            }
            ++exported;
            return true;
        }, &error)) {
        file.cancelWriting();
        return fail(context, kIoError, error);
    }
    if (!writeOk || !file.commit()) {
        file.cancelWriting();
        return fail(context, kIoError, QStringLiteral("cannot write export %1").arg(output));
    }
    succeed(context, QJsonObject{{QStringLiteral("path"), QFileInfo(output).absoluteFilePath()},
                                 {QStringLiteral("format"), format},
                                 {QStringLiteral("frames"), static_cast<qint64>(exported)}},
            QStringLiteral("Exported %1 frame(s) to %2").arg(exported).arg(QFileInfo(output).absoluteFilePath()));
    return 0;
}

bool interruptibleSleep(quint64 microseconds) {
    while (microseconds > 0 && !g_interrupted.load()) {
        const auto slice = qMin<quint64>(microseconds, 50000);
        QThread::usleep(static_cast<unsigned long>(slice));
        microseconds -= slice;
    }
    return !g_interrupted.load();
}

int commandReplay(CliContext& context, const QCommandLineParser& parser,
                  const QStringList& positional) {
    if (positional.size() < 2)
        return fail(context, kCliError, QStringLiteral("usage: akrion replay <run-id>"));
    const auto path = resolvedRunPath(context, positional.at(1));
    if (path.isEmpty()) return fail(context, kIoError, QStringLiteral("run not found"));
    RunReader reader;
    QString error;
    if (!reader.open(path, &error)) return fail(context, kValidationError, error);
    bool speedOk = false;
    const auto speed = parser.value(QStringLiteral("speed")).toDouble(&speedOk);
    if (!speedOk || speed <= 0.0 || !std::isfinite(speed))
        return fail(context, kCliError, QStringLiteral("--speed must be greater than zero"));
    const auto fromUs = secondsOptionUs(parser, QStringLiteral("from"), &error);
    if (parser.isSet(QStringLiteral("from")) && !fromUs) return fail(context, kCliError, error);
    const auto toUs = secondsOptionUs(parser, QStringLiteral("to"), &error);
    if (parser.isSet(QStringLiteral("to")) && !toUs) return fail(context, kCliError, error);
    const bool emitFrames = parser.isSet(QStringLiteral("frames"));
    std::optional<quint64> previousTime;
    quint64 replayed = 0;
    streamEvent(context, QStringLiteral("started"),
                {{QStringLiteral("run_id"), reader.manifest().runId},
                 {QStringLiteral("speed"), speed}});
    if (!reader.forEachFrame([&](const Frame& frame) {
            if (g_interrupted.load()) return false;
            if (fromUs && frame.deviceTimeUs < *fromUs) return true;
            if (toUs && frame.deviceTimeUs > *toUs) return false;
            if (previousTime && frame.deviceTimeUs > *previousTime) {
                const auto delay = static_cast<quint64>((frame.deviceTimeUs - *previousTime) / speed);
                if (!interruptibleSleep(delay)) return false;
            }
            previousTime = frame.deviceTimeUs;
            ++replayed;
            if (emitFrames) writeJsonLine(context.out, toJson(frame));
            return true;
        }, &error)) return fail(context, kIoError, error);
    const QJsonObject data{{QStringLiteral("run_id"), reader.manifest().runId},
                           {QStringLiteral("frames"), static_cast<qint64>(replayed)},
                           {QStringLiteral("interrupted"), g_interrupted.load()}};
    if (g_interrupted.load()) {
        streamEvent(context, QStringLiteral("interrupted"), data);
        return kInterrupted;
    }
    if (context.json) streamEvent(context, QStringLiteral("completed"), data);
    else context.out << "Replayed " << replayed << " frame(s).\n";
    context.out.flush();
    return 0;
}

class CapturePipeline final {
public:
    explicit CapturePipeline(const RunConfig& config)
        : m_config(config), m_validator(config), m_statistics(config) {}

    bool start(const RunWriterOptions& options, QString* error) {
        return m_writer.start(m_config, options, error);
    }

    bool ingest(const QByteArray& bytes, quint64 hostTimeUs, QString* error) {
        if (!m_writer.appendRaw(bytes, error)) return false;
        return process(m_decoder.feed(bytes, hostTimeUs), hostTimeUs, error);
    }

    bool finish(RunStatus status, QString* error) {
        if (!process(m_decoder.finish(), m_lastHostTimeUs, error)) return false;
        return m_writer.finish(status, m_statistics.summary(status), error);
    }

    quint64 frameCount() const { return m_statistics.frameCount(); }
    QString runId() const { return m_writer.runId(); }
    QString runDirectory() const { return m_writer.runDirectory(); }

private:
    bool process(const DecodeBatch& batch, quint64 hostTimeUs, QString* error) {
        m_lastHostTimeUs = hostTimeUs;
        for (const auto& decodeError : batch.errors) {
            m_statistics.observeDecodeError(decodeError);
            RunEvent event;
            event.type = QStringLiteral("parse_error");
            event.severity = EventSeverity::Error;
            event.message = decodeError.message;
            event.hostTimeUs = hostTimeUs;
            event.details = {{QStringLiteral("code"), decodeError.code},
                             {QStringLiteral("line"), static_cast<qint64>(decodeError.lineNumber)},
                             {QStringLiteral("byte_offset"), static_cast<qint64>(decodeError.byteOffset)},
                             {QStringLiteral("raw_excerpt"), QString::fromUtf8(decodeError.rawExcerpt)}};
            if (!m_writer.appendEvent(event, error)) return false;
        }
        for (const auto& frame : batch.frames) {
            const auto issues = m_validator.inspect(frame);
            m_statistics.observeFrame(frame, issues);
            if (!m_writer.appendFrame(frame, error)) return false;
            for (const auto& issue : issues)
                if (!m_writer.appendEvent(validationIssueEvent(issue, frame), error)) return false;
        }
        return true;
    }

    RunConfig m_config;
    NdjsonFrameDecoder m_decoder;
    FrameValidator m_validator;
    RunStatistics m_statistics;
    RunWriter m_writer;
    quint64 m_lastHostTimeUs = 0;
};

bool applySerialSettings(QSerialPort* port, const SerialSettings& settings, QString* error) {
    port->setPortName(settings.portName);
    if (!port->setBaudRate(settings.baudRate)) {
        if (error) *error = QStringLiteral("unsupported baud rate %1").arg(settings.baudRate);
        return false;
    }
    const auto dataBits = settings.dataBits == 5 ? QSerialPort::Data5
        : settings.dataBits == 6 ? QSerialPort::Data6
        : settings.dataBits == 7 ? QSerialPort::Data7
                                 : QSerialPort::Data8;
    if (!port->setDataBits(dataBits)) {
        if (error) *error = QStringLiteral("unsupported data bits %1").arg(settings.dataBits);
        return false;
    }
    const auto parity = settings.parity == QStringLiteral("even") ? QSerialPort::EvenParity
        : settings.parity == QStringLiteral("odd") ? QSerialPort::OddParity
        : settings.parity == QStringLiteral("mark") ? QSerialPort::MarkParity
        : settings.parity == QStringLiteral("space") ? QSerialPort::SpaceParity
                                                      : QSerialPort::NoParity;
    const auto stopBits = settings.stopBits == QStringLiteral("two") ? QSerialPort::TwoStop
                                                                     : QSerialPort::OneStop;
    const auto flow = settings.flowControl == QStringLiteral("hardware") ? QSerialPort::HardwareControl
        : settings.flowControl == QStringLiteral("software") ? QSerialPort::SoftwareControl
                                                              : QSerialPort::NoFlowControl;
    if (!port->setParity(parity) || !port->setStopBits(stopBits) || !port->setFlowControl(flow)) {
        if (error) *error = QStringLiteral("unsupported serial settings");
        return false;
    }
    return true;
}

std::optional<double> durationOption(const QCommandLineParser& parser, double fallback,
                                     QString* error) {
    if (!parser.isSet(QStringLiteral("duration"))) return fallback;
    bool ok = false;
    const auto value = parser.value(QStringLiteral("duration")).toDouble(&ok);
    if (!ok || value <= 0.0 || !std::isfinite(value)) {
        if (error) *error = QStringLiteral("--duration must be greater than zero");
        return std::nullopt;
    }
    return value;
}

void announceDuplicate(CliContext& context, const RunConfig& config) {
    const auto matches = context.store.findByExperimentSignature(experimentSignature(config));
    if (matches.isEmpty()) return;
    streamEvent(context, QStringLiteral("duplicate"),
                {{QStringLiteral("run_id"), matches.first().manifest.runId},
                 {QStringLiteral("path"), matches.first().path},
                 {QStringLiteral("experiment_signature"),
                  matches.first().manifest.experimentSignature}});
}

void emitCaptureStarted(CliContext& context, const CapturePipeline& pipeline,
                        const QString& source) {
    streamEvent(context, QStringLiteral("started"),
                {{QStringLiteral("run_id"), pipeline.runId()},
                 {QStringLiteral("path"), pipeline.runDirectory()},
                 {QStringLiteral("source"), source}});
    if (!context.json)
        context.out << "Recording " << pipeline.runId() << " to " << pipeline.runDirectory()
                    << Qt::endl;
}

void emitCaptureProgress(CliContext& context, const CapturePipeline& pipeline) {
    streamEvent(context, QStringLiteral("progress"),
                {{QStringLiteral("run_id"), pipeline.runId()},
                 {QStringLiteral("frames"), static_cast<qint64>(pipeline.frameCount())}});
}

int finishCapture(CliContext& context, CapturePipeline* pipeline, RunStatus status,
                  int exitCode, const QString& failure = {}) {
    QString finishError;
    if (!pipeline->finish(status, &finishError))
        return fail(context, kIoError, finishError);
    const QJsonObject data{{QStringLiteral("run_id"), pipeline->runId()},
                           {QStringLiteral("path"), pipeline->runDirectory()},
                           {QStringLiteral("status"), runStatusName(status)},
                           {QStringLiteral("frames"), static_cast<qint64>(pipeline->frameCount())}};
    if (exitCode == 0) {
        if (context.json) streamEvent(context, QStringLiteral("completed"), data);
        else context.out << "Saved " << pipeline->frameCount() << " frame(s) to "
                         << pipeline->runDirectory() << Qt::endl;
    } else if (exitCode == kInterrupted) {
        streamEvent(context, QStringLiteral("interrupted"), data);
        if (!context.json) context.err << "Capture interrupted; run was finalized safely.\n";
    } else {
        return fail(context, exitCode, failure, data);
    }
    return exitCode;
}

int commandRecord(CliContext& context, const QCommandLineParser& parser) {
    const auto configPath = parser.value(QStringLiteral("config"));
    if (configPath.isEmpty())
        return fail(context, kCliError, QStringLiteral("record requires --config <file>"));
    RunConfig config;
    QString error;
    if (!readConfig(configPath, &config, &error)) return fail(context, kCliError, error);
    if (parser.isSet(QStringLiteral("port"))) config.serial.portName = parser.value(QStringLiteral("port"));
    if (parser.isSet(QStringLiteral("baud"))) {
        bool ok = false;
        const auto baud = parser.value(QStringLiteral("baud")).toInt(&ok);
        if (!ok || baud <= 0) return fail(context, kCliError, QStringLiteral("invalid --baud value"));
        config.serial.baudRate = baud;
    }
    if (parser.isSet(QStringLiteral("name"))) config.name = parser.value(QStringLiteral("name"));
    if (config.serial.portName.isEmpty())
        return fail(context, kCliError, QStringLiteral("serial port is empty"));
    const auto duration = durationOption(parser, 0.0, &error);
    if (!duration) return fail(context, kCliError, error);

    QSerialPort serial;
    if (!applySerialSettings(&serial, config.serial, &error)) return fail(context, kCliError, error);
    if (!serial.open(QIODevice::ReadOnly))
        return fail(context, kIoError,
                    QStringLiteral("cannot open %1: %2").arg(config.serial.portName, serial.errorString()));
    announceDuplicate(context, config);
    CapturePipeline pipeline(config);
    RunWriterOptions options;
    options.storeRoot = context.store.root();
    options.source = QStringLiteral("serial");
    if (!pipeline.start(options, &error)) return fail(context, kIoError, error);
    emitCaptureStarted(context, pipeline, QStringLiteral("serial"));

    QElapsedTimer clock;
    QElapsedTimer progress;
    const auto hostEpochUs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000ULL;
    clock.start();
    progress.start();
    bool ioFailed = false;
    while (!g_interrupted.load()) {
        if (*duration > 0.0 && clock.elapsed() >= *duration * 1000.0) break;
        if (serial.waitForReadyRead(qMax(10, config.serial.readTimeoutMs))) {
            QByteArray bytes = serial.readAll();
            while (serial.waitForReadyRead(0)) bytes.append(serial.readAll());
            if (!bytes.isEmpty() &&
                !pipeline.ingest(bytes,
                                 hostEpochUs + static_cast<quint64>(clock.nsecsElapsed() / 1000),
                                 &error)) {
                ioFailed = true;
                break;
            }
        } else if (serial.error() != QSerialPort::NoError &&
                   serial.error() != QSerialPort::TimeoutError) {
            error = serial.errorString();
            ioFailed = true;
            break;
        }
        if (progress.elapsed() >= 1000) {
            emitCaptureProgress(context, pipeline);
            progress.restart();
        }
    }
    serial.close();
    if (g_interrupted.load())
        return finishCapture(context, &pipeline, RunStatus::Interrupted, kInterrupted);
    if (ioFailed)
        return finishCapture(context, &pipeline, RunStatus::Interrupted, kIoError, error);
    return finishCapture(context, &pipeline, RunStatus::Completed, 0);
}

double profileValue(const QJsonObject& profile, double timeSeconds, double durationSeconds) {
    const auto type = profile.value(QStringLiteral("type")).toString(QStringLiteral("step"));
    if (type == QStringLiteral("constant")) return profile.value(QStringLiteral("value")).toDouble(1.0);
    if (type == QStringLiteral("sine")) {
        const auto offset = profile.value(QStringLiteral("offset")).toDouble();
        const auto amplitude = profile.value(QStringLiteral("amplitude")).toDouble(1.0);
        const auto frequency = profile.value(QStringLiteral("frequency_hz")).toDouble(1.0);
        return offset + amplitude * std::sin(2.0 * std::numbers::pi * frequency * timeSeconds);
    }
    if (type == QStringLiteral("chirp")) {
        const auto amplitude = profile.value(QStringLiteral("amplitude")).toDouble(1.0);
        const auto start = profile.value(QStringLiteral("start_hz")).toDouble(0.1);
        const auto end = profile.value(QStringLiteral("end_hz")).toDouble(5.0);
        const auto span = qMax(0.001, durationSeconds);
        const auto slope = (end - start) / span;
        return amplitude * std::sin(2.0 * std::numbers::pi * (start * timeSeconds + 0.5 * slope * timeSeconds * timeSeconds));
    }
    const auto at = profile.value(QStringLiteral("at_s")).toDouble(2.0);
    return timeSeconds < at ? profile.value(QStringLiteral("initial")).toDouble()
                            : profile.value(QStringLiteral("value")).toDouble(1.0);
}

double gaussian(QRandomGenerator* random) {
    const auto u1 = qMax(random->generateDouble(), 1.0e-12);
    const auto u2 = random->generateDouble();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
}

double noiseValue(const QJsonObject& profile, QRandomGenerator* random) {
    const auto type = profile.value(QStringLiteral("type")).toString(QStringLiteral("none"));
    if (type == QStringLiteral("uniform")) {
        const auto amplitude = profile.value(QStringLiteral("amplitude")).toDouble(0.02);
        return (random->generateDouble() * 2.0 - 1.0) * amplitude;
    }
    if (type == QStringLiteral("gaussian"))
        return gaussian(random) * profile.value(QStringLiteral("standard_deviation")).toDouble(0.02);
    return 0.0;
}

int commandDemo(CliContext& context, const QCommandLineParser& parser) {
    RunConfig config = defaultConfig();
    QString error;
    if (parser.isSet(QStringLiteral("config")) &&
        !readConfig(parser.value(QStringLiteral("config")), &config, &error))
        return fail(context, kCliError, error);
    if (parser.isSet(QStringLiteral("name"))) config.name = parser.value(QStringLiteral("name"));
    const auto duration = durationOption(parser, 10.0, &error);
    if (!duration) return fail(context, kCliError, error);
    quint32 seed = static_cast<quint32>(config.scenario.value(QStringLiteral("seed")).toInteger(1));
    if (parser.isSet(QStringLiteral("seed"))) {
        bool ok = false;
        const auto value = parser.value(QStringLiteral("seed")).toUInt(&ok);
        if (!ok) return fail(context, kCliError, QStringLiteral("invalid --seed value"));
        seed = value;
        config.scenario.insert(QStringLiteral("seed"), static_cast<qint64>(seed));
        auto noise = config.scenario.value(QStringLiteral("noise")).toObject();
        auto noiseParameters = noise.value(QStringLiteral("parameters")).toObject();
        noiseParameters.insert(QStringLiteral("seed"), static_cast<qint64>(seed));
        noise.insert(QStringLiteral("parameters"), noiseParameters);
        config.scenario.insert(QStringLiteral("noise"), noise);
    }
    announceDuplicate(context, config);
    CapturePipeline pipeline(config);
    RunWriterOptions options;
    options.storeRoot = context.store.root();
    options.source = QStringLiteral("demo");
    if (!pipeline.start(options, &error)) return fail(context, kIoError, error);
    emitCaptureStarted(context, pipeline, QStringLiteral("demo"));
    ComponentPipeline componentPipeline(config);
    if (!componentPipeline.open(&error))
        return finishCapture(context, &pipeline, RunStatus::Interrupted, kCliError, error);
    const quint64 periodUs = config.timing.emitPeriodUs;
    QElapsedTimer hostClock;
    QElapsedTimer progress;
    const auto hostEpochUs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000ULL;
    hostClock.start();
    progress.start();
    quint64 sequence = 0;
    const auto totalFrames = static_cast<quint64>(*duration * 1000000.0 / periodUs);
    while (sequence < totalFrames && !g_interrupted.load()) {
        Frame frame;
        if (!componentPipeline.nextFrame(sequence, &frame, &error))
            return finishCapture(context, &pipeline, RunStatus::Interrupted, kCliError, error);
        auto bytes = QJsonDocument(toJson(frame)).toJson(QJsonDocument::Compact);
        bytes.append('\n');
        if (!pipeline.ingest(bytes,
                             hostEpochUs + static_cast<quint64>(hostClock.nsecsElapsed() / 1000),
                             &error))
            return finishCapture(context, &pipeline, RunStatus::Interrupted, kIoError, error);
        ++sequence;
        if (progress.elapsed() >= 1000) {
            emitCaptureProgress(context, pipeline);
            progress.restart();
        }
        const auto desiredUs = sequence * periodUs;
        const auto elapsedUs = static_cast<quint64>(hostClock.nsecsElapsed() / 1000);
        if (desiredUs > elapsedUs && !interruptibleSleep(desiredUs - elapsedUs)) break;
    }
    if (g_interrupted.load())
        return finishCapture(context, &pipeline, RunStatus::Interrupted, kInterrupted);
    return finishCapture(context, &pipeline, RunStatus::Completed, 0);
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("akrion"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("Akrion"));
    installInterruptHandler();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Real-device experiment recorder and replay tool\n\n"
        "Commands: doctor, ports, components, init, record, demo, validate, runs list, "
        "runs show, replay, export, pack, unpack"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Command to execute"));
    parser.addPositionalArgument(QStringLiteral("arguments"), QStringLiteral("Command arguments"),
                                 QStringLiteral("[arguments...]"));
    parser.addOptions({
        {{QStringLiteral("s"), QStringLiteral("store")}, QStringLiteral("Run store root"), QStringLiteral("path")},
        {QStringLiteral("json"), QStringLiteral("Emit machine-readable JSON or NDJSON")},
        {QStringLiteral("verbose"), QStringLiteral("Print diagnostic details")},
        {{QStringLiteral("c"), QStringLiteral("config")}, QStringLiteral("Configuration JSON"), QStringLiteral("file")},
        {QStringLiteral("force"), QStringLiteral("Overwrite an existing output")},
        {QStringLiteral("repair"), QStringLiteral("Repair trailing partial run records")},
        {QStringLiteral("port"), QStringLiteral("Override serial port"), QStringLiteral("name")},
        {QStringLiteral("baud"), QStringLiteral("Override baud rate"), QStringLiteral("rate")},
        {QStringLiteral("duration"), QStringLiteral("Capture duration in seconds"), QStringLiteral("seconds")},
        {QStringLiteral("name"), QStringLiteral("Override run name"), QStringLiteral("name")},
        {QStringLiteral("seed"), QStringLiteral("Override demo random seed"), QStringLiteral("number")},
        {QStringLiteral("speed"), QStringLiteral("Replay speed multiplier"), QStringLiteral("factor"), QStringLiteral("1")},
        {QStringLiteral("from"), QStringLiteral("Start time in seconds"), QStringLiteral("seconds")},
        {QStringLiteral("to"), QStringLiteral("End time in seconds"), QStringLiteral("seconds")},
        {QStringLiteral("frames"), QStringLiteral("Emit replayed frames")},
        {QStringLiteral("format"), QStringLiteral("Export format: csv, ndjson, summary-json"), QStringLiteral("format")},
        {{QStringLiteral("o"), QStringLiteral("output")}, QStringLiteral("Output path"), QStringLiteral("path")},
        {QStringLiteral("channels"), QStringLiteral("Comma-separated exported channels"), QStringLiteral("keys")},
        {QStringLiteral("status"), QStringLiteral("Filter runs by status"), QStringLiteral("status")},
        {QStringLiteral("signature"), QStringLiteral("Filter runs by experiment signature"), QStringLiteral("sha256")},
    });
    parser.process(app);

    const auto positional = parser.positionalArguments();
    if (positional.isEmpty()) parser.showHelp(kCliError);
    QString command = positional.first().toLower();
    if (command == QStringLiteral("runs") && positional.size() >= 2)
        command += QLatin1Char(' ') + positional.at(1).toLower();
    const auto storeRoot = parser.value(QStringLiteral("store"));
    CliContext context;
    context.json = parser.isSet(QStringLiteral("json"));
    context.verbose = parser.isSet(QStringLiteral("verbose"));
    context.command = command;
    context.store = RunStore(storeRoot);

    if (command == QStringLiteral("doctor")) return commandDoctor(context);
    if (command == QStringLiteral("ports")) return commandPorts(context);
    if (command == QStringLiteral("components")) return commandComponents(context);
    if (command == QStringLiteral("init")) return commandInit(context, parser, positional);
    if (command == QStringLiteral("record")) return commandRecord(context, parser);
    if (command == QStringLiteral("demo")) return commandDemo(context, parser);
    if (command == QStringLiteral("validate")) return commandValidate(context, parser, positional);
    if (command == QStringLiteral("runs list")) return commandRunsList(context, parser);
    if (command == QStringLiteral("runs show")) return commandRunsShow(context, positional);
    if (command == QStringLiteral("replay")) return commandReplay(context, parser, positional);
    if (command == QStringLiteral("export")) return commandExport(context, parser, positional);
    if (command == QStringLiteral("pack")) return commandPack(context, parser, positional);
    if (command == QStringLiteral("unpack")) return commandUnpack(context, parser, positional);
    return fail(context, kCliError, QStringLiteral("unknown command: %1").arg(command));
}
