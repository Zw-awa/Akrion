#include "run_storage.h"

#include "frame_validator.h"
#include "statistics.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace akrion::core {
namespace {

constexpr auto kManifestFile = "manifest.json";
constexpr auto kConfigFile = "config.json";
constexpr auto kRawFile = "serial.raw";
constexpr auto kFramesFile = "frames.ndjson";
constexpr auto kEventsFile = "events.ndjson";
constexpr auto kSummaryFile = "summary.json";

void setError(QString* error, const QString& message) {
    if (error) *error = message;
}

QString childPath(const QString& directory, const char* name) {
    return QDir(directory).filePath(QString::fromLatin1(name));
}

bool isValidRunId(const QString& id) {
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return id.size() <= 128 && !id.contains(QStringLiteral("..")) && expression.match(id).hasMatch();
}

bool truncateTrailingPartialLine(const QString& path, bool* changed, QString* error) {
    *changed = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        setError(error, QStringLiteral("cannot open %1 for repair: %2").arg(path, file.errorString()));
        return false;
    }
    const auto size = file.size();
    if (size == 0) return true;
    if (!file.seek(size - 1)) {
        setError(error, QStringLiteral("cannot seek %1: %2").arg(path, file.errorString()));
        return false;
    }
    if (file.read(1) == QByteArrayLiteral("\n")) return true;
    constexpr qint64 blockSize = 64 * 1024;
    qint64 cursor = size;
    qint64 truncateAt = 0;
    while (cursor > 0) {
        const auto start = qMax<qint64>(0, cursor - blockSize);
        if (!file.seek(start)) break;
        const auto block = file.read(cursor - start);
        const auto newline = block.lastIndexOf('\n');
        if (newline >= 0) {
            truncateAt = start + newline + 1;
            break;
        }
        cursor = start;
    }
    if (!file.resize(truncateAt)) {
        setError(error, QStringLiteral("cannot truncate %1: %2").arg(path, file.errorString()));
        return false;
    }
    *changed = true;
    return true;
}

} // namespace

bool writeJsonObjectAtomic(const QString& path, const QJsonObject& object, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("cannot open %1: %2").arg(path, file.errorString()));
        return false;
    }
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        setError(error, QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QStringLiteral("cannot atomically replace %1: %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

bool readJsonObject(const QString& path, QJsonObject* object, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot open %1: %2").arg(path, file.errorString()));
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("invalid JSON object in %1: %2")
                            .arg(path, parseError.errorString()));
        return false;
    }
    *object = document.object();
    return true;
}

QString generateRunId() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
           QLatin1Char('-') + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

RunWriter::~RunWriter() {
    if (!m_active) return;
    QJsonObject summary{{QStringLiteral("schema"), QString::fromLatin1(kSummarySchema)},
                        {QStringLiteral("status"), QStringLiteral("interrupted")},
                        {QStringLiteral("reason"), QStringLiteral("writer_destroyed")}};
    finish(RunStatus::Interrupted, summary, nullptr);
}

bool RunWriter::start(const RunConfig& config, const RunWriterOptions& options, QString* error) {
    if (m_active) {
        setError(error, QStringLiteral("run writer is already active"));
        return false;
    }
    const auto root = options.storeRoot.isEmpty() ? RunStore::defaultRoot() : options.storeRoot;
    if (!QDir().mkpath(root)) {
        setError(error, QStringLiteral("cannot create run store %1").arg(root));
        return false;
    }
    const auto id = options.requestedRunId.isEmpty() ? generateRunId() : options.requestedRunId;
    if (!isValidRunId(id)) {
        setError(error, QStringLiteral("invalid run id '%1'").arg(id));
        return false;
    }
    const auto directory = QDir(root).filePath(id);
    if (QFileInfo::exists(directory) || !QDir().mkpath(directory)) {
        setError(error, QStringLiteral("run directory already exists or cannot be created: %1")
                            .arg(directory));
        return false;
    }

    m_runDirectory = directory;
    m_rawFile.setFileName(childPath(directory, kRawFile));
    m_framesFile.setFileName(childPath(directory, kFramesFile));
    m_eventsFile.setFileName(childPath(directory, kEventsFile));
    if (!m_rawFile.open(QIODevice::WriteOnly | QIODevice::Append) ||
        !m_framesFile.open(QIODevice::WriteOnly | QIODevice::Append) ||
        !m_eventsFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        setError(error, QStringLiteral("cannot create run stream files in %1").arg(directory));
        m_rawFile.close();
        m_framesFile.close();
        m_eventsFile.close();
        return false;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    m_manifest = {};
    m_manifest.runId = id;
    m_manifest.name = config.name;
    m_manifest.status = RunStatus::Recording;
    m_manifest.createdAt = now;
    m_manifest.updatedAt = now;
    m_manifest.source = options.source;
    m_manifest.configSignature = core::configSignature(config);
    m_manifest.experimentSignature = core::experimentSignature(config);
    for (const auto& algorithm : config.algorithms)
        m_manifest.algorithmSignatures.insert(algorithm.id, algorithmSignature(algorithm));
    m_flushByteThreshold = qMax<qint64>(4096, options.flushByteThreshold);
    m_flushIntervalMs = qMax<qint64>(100, options.flushIntervalMs);
    m_bytesSinceFlush = 0;
    m_flushTimer.start();
    m_active = true;

    if (!writeJsonObjectAtomic(childPath(directory, kConfigFile), toJson(config), error) ||
        !writeJsonObjectAtomic(childPath(directory, kSummaryFile),
                               {{QStringLiteral("schema"), QString::fromLatin1(kSummarySchema)},
                                {QStringLiteral("status"), QStringLiteral("recording")}},
                               error) ||
        !writeManifest(error)) {
        m_active = false;
        m_rawFile.close();
        m_framesFile.close();
        m_eventsFile.close();
        return false;
    }
    return true;
}

bool RunWriter::appendRaw(QByteArrayView bytes, QString* error) {
    if (!m_active) {
        setError(error, QStringLiteral("run writer is not active"));
        return false;
    }
    if (bytes.isEmpty()) return true;
    if (m_rawFile.write(bytes.data(), bytes.size()) != bytes.size()) {
        setError(error, QStringLiteral("cannot append serial.raw: %1").arg(m_rawFile.errorString()));
        return false;
    }
    m_manifest.rawByteCount += static_cast<quint64>(bytes.size());
    m_bytesSinceFlush += bytes.size();
    return maybeFlush(error);
}

bool RunWriter::appendFrame(const Frame& frame, QString* error) {
    if (!m_active) {
        setError(error, QStringLiteral("run writer is not active"));
        return false;
    }
    if (!appendJsonLine(&m_framesFile, toJson(frame), error)) return false;
    ++m_manifest.frameCount;
    return maybeFlush(error);
}

bool RunWriter::appendEvent(const RunEvent& event, QString* error) {
    if (!m_active) {
        setError(error, QStringLiteral("run writer is not active"));
        return false;
    }
    if (!appendJsonLine(&m_eventsFile, toJson(event), error)) return false;
    ++m_manifest.eventCount;
    return maybeFlush(error);
}

bool RunWriter::appendJsonLine(QFile* file, const QJsonObject& object, QString* error) {
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (file->write(bytes) != bytes.size()) {
        setError(error, QStringLiteral("cannot append %1: %2")
                            .arg(file->fileName(), file->errorString()));
        return false;
    }
    m_bytesSinceFlush += bytes.size();
    return true;
}

bool RunWriter::maybeFlush(QString* error) {
    if (m_bytesSinceFlush >= m_flushByteThreshold || m_flushTimer.elapsed() >= m_flushIntervalMs)
        return flush(error);
    return true;
}

bool RunWriter::flush(QString* error) {
    if (!m_active) return true;
    if (!m_rawFile.flush() || !m_framesFile.flush() || !m_eventsFile.flush()) {
        setError(error, QStringLiteral("cannot flush run stream files"));
        return false;
    }
    m_manifest.updatedAt = QDateTime::currentDateTimeUtc();
    if (!writeManifest(error)) return false;
    m_bytesSinceFlush = 0;
    m_flushTimer.restart();
    return true;
}

bool RunWriter::finish(RunStatus status, const QJsonObject& summary, QString* error) {
    if (!m_active) return true;
    if (status == RunStatus::Recording) {
        setError(error, QStringLiteral("finish requires a terminal run status"));
        return false;
    }
    if (!flush(error)) return false;
    m_rawFile.close();
    m_framesFile.close();
    m_eventsFile.close();
    auto finalSummary = summary;
    finalSummary.insert(QStringLiteral("schema"), QString::fromLatin1(kSummarySchema));
    finalSummary.insert(QStringLiteral("status"), runStatusName(status));
    const auto summaryWritten =
        writeJsonObjectAtomic(childPath(m_runDirectory, kSummaryFile), finalSummary, error);
    m_manifest.status = status;
    m_manifest.updatedAt = QDateTime::currentDateTimeUtc();
    m_manifest.completedAt = m_manifest.updatedAt;
    const auto manifestWritten = summaryWritten && writeManifest(error);
    m_active = false;
    return summaryWritten && manifestWritten;
}

bool RunWriter::writeManifest(QString* error) {
    return writeJsonObjectAtomic(childPath(m_runDirectory, kManifestFile), toJson(m_manifest), error);
}

bool RunReader::open(const QString& runDirectory, QString* error) {
    const QFileInfo directoryInfo(runDirectory);
    if (!directoryInfo.isDir()) {
        setError(error, QStringLiteral("run directory does not exist: %1").arg(runDirectory));
        return false;
    }
    const auto canonical = directoryInfo.canonicalFilePath();
    QJsonObject manifestObject;
    QJsonObject configObject;
    QJsonObject summaryObject;
    if (!readJsonObject(childPath(canonical, kManifestFile), &manifestObject, error) ||
        !readJsonObject(childPath(canonical, kConfigFile), &configObject, error) ||
        !readJsonObject(childPath(canonical, kSummaryFile), &summaryObject, error)) return false;
    RunManifest manifest;
    RunConfig config;
    if (!runManifestFromJson(manifestObject, &manifest, error) ||
        !runConfigFromJson(configObject, &config, error)) return false;
    m_runDirectory = canonical;
    m_manifest = std::move(manifest);
    m_config = std::move(config);
    m_summary = std::move(summaryObject);
    return true;
}

bool RunReader::forEachFrame(const std::function<bool(const Frame&)>& callback, QString* error,
                             QVector<DecodeError>* decodeErrors) const {
    if (m_runDirectory.isEmpty()) {
        setError(error, QStringLiteral("run reader is not open"));
        return false;
    }
    QFile file(childPath(m_runDirectory, kFramesFile));
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot open frames.ndjson: %1").arg(file.errorString()));
        return false;
    }
    NdjsonFrameDecoder decoder;
    bool keepGoing = true;
    while (keepGoing && !file.atEnd()) {
        auto batch = decoder.feed(file.read(64 * 1024));
        if (decodeErrors) decodeErrors->append(batch.errors);
        for (const auto& frame : batch.frames) {
            if (!callback(frame)) {
                keepGoing = false;
                break;
            }
        }
    }
    if (keepGoing) {
        auto batch = decoder.finish();
        if (decodeErrors) decodeErrors->append(batch.errors);
        for (const auto& frame : batch.frames) {
            if (!callback(frame)) break;
        }
    }
    return true;
}

bool RunReader::forEachEvent(const std::function<bool(const RunEvent&)>& callback,
                             QString* error) const {
    if (m_runDirectory.isEmpty()) {
        setError(error, QStringLiteral("run reader is not open"));
        return false;
    }
    QFile file(childPath(m_runDirectory, kEventsFile));
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot open events.ndjson: %1").arg(file.errorString()));
        return false;
    }
    quint64 lineNumber = 0;
    while (!file.atEnd()) {
        const auto line = file.readLine();
        ++lineNumber;
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(line, &parseError);
        RunEvent event;
        QString parseMessage;
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            !runEventFromJson(document.object(), &event, &parseMessage)) {
            setError(error, QStringLiteral("invalid event at line %1: %2")
                                .arg(lineNumber)
                                .arg(parseError.error == QJsonParseError::NoError
                                         ? parseMessage
                                         : parseError.errorString()));
            return false;
        }
        if (!callback(event)) break;
    }
    return true;
}

RunValidationReport RunReader::validate(bool repairTrailingLines) {
    RunValidationReport report;
    if (m_runDirectory.isEmpty()) {
        report.valid = false;
        report.errors.append(QStringLiteral("run reader is not open"));
        return report;
    }
    for (const auto* name : {kManifestFile, kConfigFile, kRawFile, kFramesFile, kEventsFile,
                             kSummaryFile}) {
        if (!QFileInfo(childPath(m_runDirectory, name)).isFile()) {
            report.valid = false;
            report.errors.append(QStringLiteral("missing required file %1").arg(name));
        }
    }
    if (!report.valid) return report;

    if (repairTrailingLines) {
        for (const auto* name : {kFramesFile, kEventsFile}) {
            bool changed = false;
            QString repairError;
            if (!truncateTrailingPartialLine(childPath(m_runDirectory, name), &changed,
                                             &repairError)) {
                report.valid = false;
                report.errors.append(repairError);
            }
            report.repaired = report.repaired || changed;
        }
    }

    QVector<DecodeError> decodeErrors;
    QString readError;
    if (!forEachFrame([&report](const Frame&) {
            ++report.frameCount;
            return true;
        }, &readError, &decodeErrors)) {
        report.valid = false;
        report.errors.append(readError);
    }
    for (const auto& decodeError : decodeErrors) {
        report.valid = false;
        report.errors.append(QStringLiteral("frames.ndjson line %1: %2")
                                 .arg(decodeError.lineNumber)
                                 .arg(decodeError.message));
    }
    if (!forEachEvent([&report](const RunEvent&) {
            ++report.eventCount;
            return true;
        }, &readError)) {
        report.valid = false;
        report.errors.append(readError);
    }
    report.rawByteCount = static_cast<quint64>(QFileInfo(childPath(m_runDirectory, kRawFile)).size());
    if (report.frameCount != m_manifest.frameCount)
        report.warnings.append(QStringLiteral("manifest frame_count is %1, file contains %2")
                                   .arg(m_manifest.frameCount)
                                   .arg(report.frameCount));
    if (report.eventCount != m_manifest.eventCount)
        report.warnings.append(QStringLiteral("manifest event_count is %1, file contains %2")
                                   .arg(m_manifest.eventCount)
                                   .arg(report.eventCount));
    if (report.rawByteCount != m_manifest.rawByteCount)
        report.warnings.append(QStringLiteral("manifest raw_byte_count is %1, file contains %2")
                                   .arg(m_manifest.rawByteCount)
                                   .arg(report.rawByteCount));
    if (core::configSignature(m_config) != m_manifest.configSignature) {
        report.valid = false;
        report.errors.append(QStringLiteral("config signature does not match manifest"));
    }
    if (core::experimentSignature(m_config) != m_manifest.experimentSignature) {
        report.valid = false;
        report.errors.append(QStringLiteral("experiment signature does not match manifest"));
    }
    if (m_manifest.status == RunStatus::Recording)
        report.warnings.append(QStringLiteral("run is still marked as recording"));
    return report;
}

RunStore::RunStore(QString root) : m_root(root.isEmpty() ? defaultRoot() : std::move(root)) {}

QString RunStore::defaultRoot() {
    const QString configured = qEnvironmentVariable("AKRION_STORE").trimmed();
    if (!configured.isEmpty()) return QDir(configured).absolutePath();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("runs"));
}

QVector<RunInfo> RunStore::list(QString* error) const {
    QVector<RunInfo> result;
    QDir directory(m_root);
    if (!directory.exists()) return result;
    const auto entries = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entries) {
        QJsonObject object;
        QString readError;
        if (!readJsonObject(childPath(entry.filePath(), kManifestFile), &object, &readError))
            continue;
        RunManifest manifest;
        if (!runManifestFromJson(object, &manifest, &readError)) continue;
        result.append({entry.canonicalFilePath(), std::move(manifest)});
    }
    std::sort(result.begin(), result.end(), [](const RunInfo& left, const RunInfo& right) {
        return left.manifest.createdAt > right.manifest.createdAt;
    });
    Q_UNUSED(error)
    return result;
}

std::optional<RunInfo> RunStore::find(const QString& runId, QString* error) const {
    if (!isValidRunId(runId)) {
        setError(error, QStringLiteral("invalid run id '%1'").arg(runId));
        return std::nullopt;
    }
    const auto path = QDir(m_root).filePath(runId);
    RunReader reader;
    if (!reader.open(path, error)) return std::nullopt;
    return RunInfo{reader.runDirectory(), reader.manifest()};
}

QVector<RunInfo> RunStore::findByExperimentSignature(const QString& signature,
                                                     QString* error) const {
    QVector<RunInfo> matches;
    for (const auto& run : list(error)) {
        if (run.manifest.experimentSignature == signature) matches.append(run);
    }
    return matches;
}

QString RunStore::resolve(const QString& runIdOrPath) const {
    const QFileInfo direct(runIdOrPath);
    if (direct.isDir()) return direct.canonicalFilePath();
    if (!isValidRunId(runIdOrPath)) return {};
    const QFileInfo stored(QDir(m_root).filePath(runIdOrPath));
    return stored.isDir() ? stored.canonicalFilePath() : QString{};
}

bool RunStore::recover(const QString& runIdOrPath, QString* error) const {
    const auto path = resolve(runIdOrPath);
    if (path.isEmpty()) {
        setError(error, QStringLiteral("run not found: %1").arg(runIdOrPath));
        return false;
    }
    RunReader reader;
    if (!reader.open(path, error)) return false;
    const auto report = reader.validate(true);
    if (!report.valid) {
        setError(error, report.errors.join(QStringLiteral("; ")));
        return false;
    }

    FrameValidator validator(reader.config());
    RunStatistics statistics(reader.config());
    if (!reader.forEachFrame([&](const Frame& frame) {
            const auto issues = validator.inspect(frame);
            statistics.observeFrame(frame, issues);
            return true;
        }, error)) return false;

    auto manifest = reader.manifest();
    manifest.status = RunStatus::Recovered;
    manifest.frameCount = report.frameCount;
    manifest.eventCount = report.eventCount;
    manifest.rawByteCount = report.rawByteCount;
    manifest.updatedAt = QDateTime::currentDateTimeUtc();
    manifest.completedAt = manifest.updatedAt;
    if (!writeJsonObjectAtomic(childPath(path, kSummaryFile),
                               statistics.summary(RunStatus::Recovered), error) ||
        !writeJsonObjectAtomic(childPath(path, kManifestFile), toJson(manifest), error))
        return false;
    return true;
}

} // namespace akrion::core
