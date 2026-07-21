#include "core/capture_session.h"
#include "core/frame_decoder.h"
#include "core/run_storage.h"
#include "core/types.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <cmath>
#include <iostream>

namespace {

using namespace akrion::core;

int failures = 0;

void check(bool condition, const QString& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
        ++failures;
    }
}

class TestDirectory final {
public:
    TestDirectory() {
        QString root = qEnvironmentVariable("AKRION_TEST_TMP");
        if (root.isEmpty()) {
            root = QDir::tempPath();
        }
        m_path = QDir(root).filePath(QStringLiteral("akrion-core-test-%1")
                                         .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        m_valid = QDir().mkpath(m_path);
    }

    ~TestDirectory() {
        if (m_valid && !qEnvironmentVariableIsSet("AKRION_KEEP_TEST_ARTIFACTS")) {
            QDir(m_path).removeRecursively();
        }
    }

    [[nodiscard]] bool isValid() const noexcept { return m_valid; }
    [[nodiscard]] QString path() const { return m_path; }

private:
    QString m_path;
    bool m_valid = false;
};

ChannelDefinition channel(const QString& key, ChannelRole role, const QString& unit = {}) {
    ChannelDefinition definition;
    definition.key = key;
    definition.label = key;
    definition.unit = unit;
    definition.role = role;
    definition.description = QStringLiteral("test channel");
    return definition;
}

RunConfig testConfig() {
    RunConfig config;
    config.name = QStringLiteral("Core integration test");
    config.deviceId = QStringLiteral("demo-device");
    config.timing.algorithmPeriodUs = 1000;
    config.timing.emitPeriodUs = 10000;
    config.timing.maxDeviationUs = 100;
    config.channels = {
        channel(QStringLiteral("reference"), ChannelRole::Reference, QStringLiteral("unit")),
        channel(QStringLiteral("measurement"), ChannelRole::Measurement, QStringLiteral("unit")),
        channel(QStringLiteral("theoretical"), ChannelRole::Estimate, QStringLiteral("unit")),
        channel(QStringLiteral("control"), ChannelRole::Control),
        channel(QStringLiteral("disturbance"), ChannelRole::Disturbance, QStringLiteral("unit")),
    };
    AlgorithmDefinition algorithm;
    algorithm.id = 7;
    algorithm.displayName = QStringLiteral("Test controller");
    algorithm.implementationLanguage = ImplementationLanguage::Cpp;
    algorithm.repository = QStringLiteral("https://example.invalid/controller");
    algorithm.revision = QStringLiteral("deadbeef");
    algorithm.license = QStringLiteral("MIT");
    algorithm.parameters = {{QStringLiteral("gain"), 1.5},
                            {QStringLiteral("window"), 5}};
    config.algorithms.append(algorithm);
    MetricDefinition metric;
    metric.name = QStringLiteral("tracking");
    metric.referenceChannel = QStringLiteral("reference");
    metric.observedChannel = QStringLiteral("measurement");
    config.metrics.append(metric);
    config.scenario = {{QStringLiteral("input_profile"), QStringLiteral("constant")},
                       {QStringLiteral("noise_profile"), QStringLiteral("none")},
                       {QStringLiteral("seed"), 42}};
    return config;
}

Frame testFrame(quint64 sequence) {
    Frame frame;
    frame.deviceTimeUs = sequence * 10000;
    frame.algoTick = sequence * 10;
    frame.emitTick = sequence;
    frame.seq = sequence;
    frame.algoId = 7;
    frame.algoEnabled = true;
    frame.values = {{QStringLiteral("reference"), 1.0 + static_cast<double>(sequence)},
                    {QStringLiteral("measurement"), 0.75 + static_cast<double>(sequence)}};
    frame.extraFields = {
        {QStringLiteral("vendor_extension"),
         QJsonObject{{QStringLiteral("quality"), QStringLiteral("simulated")}}},
    };
    return frame;
}

QByteArray frameLine(const Frame& frame) {
    QByteArray bytes = QJsonDocument(toJson(frame)).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    return bytes;
}

bool appendFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Append)
        && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void testNdjsonDecoder() {
    const Frame source = testFrame(3);
    QByteArray encoded = frameLine(source);
    encoded.chop(1);
    encoded.append("\r\n");

    NdjsonFrameDecoder decoder;
    DecodeBatch combined;
    const QList<qsizetype> chunks {1, 2, 5, 3, 11, 7};
    qsizetype offset = 0;
    qsizetype chunkIndex = 0;
    while (offset < encoded.size()) {
        const qsizetype count = qMin(chunks.at(chunkIndex % chunks.size()), encoded.size() - offset);
        const auto batch = decoder.feed(QByteArrayView(encoded).sliced(offset, count),
                                        static_cast<quint64>(1000 + offset));
        combined.frames += batch.frames;
        combined.errors += batch.errors;
        offset += count;
        ++chunkIndex;
    }
    check(combined.errors.isEmpty(), QStringLiteral("chunked CRLF frame has no decode errors"));
    check(combined.frames.size() == 1, QStringLiteral("chunked input emits exactly one frame"));
    if (combined.frames.size() == 1) {
        const Frame& decoded = combined.frames.constFirst();
        check(decoded.seq == source.seq && decoded.deviceTimeUs == source.deviceTimeUs,
              QStringLiteral("chunked frame preserves fixed fields"));
        check(decoded.values == source.values, QStringLiteral("chunked frame preserves values"));
        check(decoded.hostReceiveTimeUs.has_value(),
              QStringLiteral("decoder attaches host receive time"));
        check(decoded.extraFields.value(QStringLiteral("vendor_extension"))
                      == source.extraFields.value(QStringLiteral("vendor_extension")),
              QStringLiteral("unknown frame fields survive decoding"));
        check(toJson(decoded).contains(QStringLiteral("vendor_extension")),
              QStringLiteral("unknown frame fields survive serialization"));
    }

    QJsonObject unknownVersion = toJson(testFrame(4));
    unknownVersion.insert(QStringLiteral("schema"), QStringLiteral("akrion.frame/2"));
    QByteArray badInput = QByteArrayLiteral("{not-json}\n");
    badInput += QJsonDocument(unknownVersion).toJson(QJsonDocument::Compact) + '\n';
    badInput += frameLine(testFrame(5));
    const DecodeBatch badBatch = decoder.feed(badInput, 5000);
    check(badBatch.errors.size() == 2, QStringLiteral("bad JSON and unknown major are rejected"));
    check(badBatch.frames.size() == 1 && badBatch.frames.constFirst().seq == 5,
          QStringLiteral("decoder continues after malformed frames"));
    if (badBatch.errors.size() == 2) {
        check(badBatch.errors.at(0).code == QStringLiteral("invalid_frame")
                  && badBatch.errors.at(1).code == QStringLiteral("invalid_frame"),
              QStringLiteral("malformed frames use stable error code"));
        check(badBatch.errors.at(1).message.contains(QStringLiteral("unsupported frame schema")),
              QStringLiteral("unknown major error identifies the schema"));
    }

    NdjsonFrameDecoder truncated;
    truncated.feed(QByteArrayLiteral("{\"schema\":"));
    const DecodeBatch finalBatch = truncated.finish();
    check(finalBatch.errors.size() == 1
              && finalBatch.errors.constFirst().code == QStringLiteral("truncated_line"),
          QStringLiteral("unterminated final line is reported"));
}

void testCanonicalSignatures() {
    QJsonObject nestedLeft;
    nestedLeft.insert(QStringLiteral("z"), 3);
    nestedLeft.insert(QStringLiteral("a"), 1);
    QJsonObject left;
    left.insert(QStringLiteral("beta"), nestedLeft);
    left.insert(QStringLiteral("alpha"), 2);

    QJsonObject nestedRight;
    nestedRight.insert(QStringLiteral("a"), 1);
    nestedRight.insert(QStringLiteral("z"), 3);
    QJsonObject right;
    right.insert(QStringLiteral("alpha"), 2);
    right.insert(QStringLiteral("beta"), nestedRight);
    check(canonicalJson(left) == canonicalJson(right),
          QStringLiteral("canonical JSON ignores object key insertion order"));
    check(sha256Canonical(left) == sha256Canonical(right),
          QStringLiteral("canonical SHA-256 ignores object key insertion order"));

    AlgorithmDefinition first = testConfig().algorithms.constFirst();
    AlgorithmDefinition second = first;
    first.parameters = {{QStringLiteral("zeta"), 9}, {QStringLiteral("alpha"), 1}};
    second.parameters = {{QStringLiteral("alpha"), 1}, {QStringLiteral("zeta"), 9}};
    check(algorithmSignature(first) == algorithmSignature(second),
          QStringLiteral("algorithm signature ignores parameter key insertion order"));

    RunConfig firstConfig = testConfig();
    RunConfig secondConfig = firstConfig;
    firstConfig.scenario = {{QStringLiteral("seed"), 42},
                            {QStringLiteral("noise_profile"), QStringLiteral("none")},
                            {QStringLiteral("input_profile"), QStringLiteral("constant")}};
    secondConfig.scenario = {{QStringLiteral("input_profile"), QStringLiteral("constant")},
                             {QStringLiteral("noise_profile"), QStringLiteral("none")},
                             {QStringLiteral("seed"), 42}};
    check(configSignature(firstConfig) == configSignature(secondConfig),
          QStringLiteral("config signature ignores scenario key insertion order"));
    check(experimentSignature(firstConfig) == experimentSignature(secondConfig),
          QStringLiteral("experiment signature ignores scenario key insertion order"));
}

void testRunStorageAndRepair() {
    TestDirectory temporary;
    check(temporary.isValid(), QStringLiteral("storage test directory is available"));
    const RunConfig config = testConfig();
    RunWriterOptions options;
    options.storeRoot = temporary.path();
    options.requestedRunId = QStringLiteral("writer-roundtrip");
    options.source = QStringLiteral("test:memory");
    options.flushByteThreshold = 4096;
    options.flushIntervalMs = 100;

    RunWriter writer;
    QString error;
    check(writer.start(config, options, &error),
          QStringLiteral("RunWriter starts: %1").arg(error));
    const QString runDirectory = writer.runDirectory();
    const QByteArray raw = frameLine(testFrame(0)) + frameLine(testFrame(1));
    check(writer.appendRaw(raw, &error), QStringLiteral("raw bytes stream: %1").arg(error));
    check(writer.appendFrame(testFrame(0), &error),
          QStringLiteral("first normalized frame streams: %1").arg(error));
    check(writer.appendFrame(testFrame(1), &error),
          QStringLiteral("second normalized frame streams: %1").arg(error));
    RunEvent event;
    event.type = QStringLiteral("test_event");
    event.message = QStringLiteral("streaming test");
    event.hostTimeUs = 1234;
    event.details = {{QStringLiteral("kind"), QStringLiteral("fixture")}};
    check(writer.appendEvent(event, &error), QStringLiteral("event streams: %1").arg(error));
    check(writer.flush(&error), QStringLiteral("stream flush succeeds: %1").arg(error));

    const QStringList required {
        QStringLiteral("manifest.json"), QStringLiteral("config.json"),
        QStringLiteral("serial.raw"), QStringLiteral("frames.ndjson"),
        QStringLiteral("events.ndjson"), QStringLiteral("summary.json"),
    };
    for (const QString& file : required) {
        check(QFileInfo(QDir(runDirectory).filePath(file)).isFile(),
              QStringLiteral("active run exposes %1").arg(file));
    }
    QJsonObject activeManifest;
    check(readJsonObject(QDir(runDirectory).filePath(QStringLiteral("manifest.json")),
                         &activeManifest, &error),
          QStringLiteral("active manifest is readable: %1").arg(error));
    check(activeManifest.value(QStringLiteral("status")).toString() == QStringLiteral("recording")
              && activeManifest.value(QStringLiteral("frame_count")).toInteger() == 2,
          QStringLiteral("active manifest reflects flushed streaming counts"));
    check(readFile(QDir(runDirectory).filePath(QStringLiteral("serial.raw"))) == raw,
          QStringLiteral("serial.raw preserves exact input bytes"));

    QJsonObject summary{{QStringLiteral("transport"),
                         QJsonObject{{QStringLiteral("frame_count"), 2}}}};
    check(writer.finish(RunStatus::Completed, summary, &error),
          QStringLiteral("RunWriter completes: %1").arg(error));
    check(!writer.isActive(), QStringLiteral("finished writer is inactive"));

    RunReader reader;
    check(reader.open(runDirectory, &error), QStringLiteral("RunReader opens run: %1").arg(error));
    check(reader.manifest().status == RunStatus::Completed
              && reader.manifest().frameCount == 2
              && reader.manifest().eventCount == 1
              && reader.manifest().rawByteCount == static_cast<quint64>(raw.size()),
          QStringLiteral("RunReader restores manifest counters"));
    QVector<quint64> sequences;
    check(reader.forEachFrame([&sequences](const Frame& frame) {
              sequences.append(frame.seq);
              return true;
          }, &error),
          QStringLiteral("RunReader iterates frames: %1").arg(error));
    check(sequences == QVector<quint64>({0, 1}),
          QStringLiteral("RunReader returns frames in append order"));
    quint64 eventCount = 0;
    check(reader.forEachEvent([&eventCount](const RunEvent& restored) {
              ++eventCount;
              return restored.type == QStringLiteral("test_event");
          }, &error),
          QStringLiteral("RunReader iterates events: %1").arg(error));
    check(eventCount == 1, QStringLiteral("RunReader restores event count"));

    const QString framesPath = QDir(runDirectory).filePath(QStringLiteral("frames.ndjson"));
    const QString eventsPath = QDir(runDirectory).filePath(QStringLiteral("events.ndjson"));
    check(appendFile(framesPath, QByteArrayLiteral("{\"schema\":"))
              && appendFile(eventsPath, QByteArrayLiteral("{\"schema\":")),
          QStringLiteral("partial trailing records can be injected"));
    const RunValidationReport broken = reader.validate(false);
    check(!broken.valid, QStringLiteral("validation rejects partial trailing records"));
    const RunValidationReport repaired = reader.validate(true);
    check(repaired.valid && repaired.repaired && repaired.frameCount == 2
              && repaired.eventCount == 1,
          QStringLiteral("repair truncates only incomplete trailing records"));
    check(readFile(framesPath).endsWith('\n') && readFile(eventsPath).endsWith('\n'),
          QStringLiteral("repaired streams end on complete NDJSON lines"));

    check(appendFile(framesPath, QByteArrayLiteral("partial"))
              && appendFile(eventsPath, QByteArrayLiteral("partial")),
          QStringLiteral("recovery fixture is corrupted at the tail"));
    RunStore store(temporary.path());
    check(store.recover(QStringLiteral("writer-roundtrip"), &error),
          QStringLiteral("RunStore recovery succeeds: %1").arg(error));
    RunReader recovered;
    check(recovered.open(runDirectory, &error),
          QStringLiteral("recovered run can be reopened: %1").arg(error));
    check(recovered.manifest().status == RunStatus::Recovered
              && recovered.manifest().frameCount == 2
              && recovered.summary().value(QStringLiteral("status")).toString()
                     == QStringLiteral("recovered"),
          QStringLiteral("recovery rebuilds terminal manifest and summary"));
}

void testCaptureSessionDemo() {
    TestDirectory temporary;
    check(temporary.isValid(), QStringLiteral("capture test directory is available"));
    const RunConfig config = testConfig();
    DemoOptions demo;
    demo.inputProfile = QStringLiteral("constant");
    demo.noiseProfile = QStringLiteral("none");
    demo.sampleCount = 25;
    demo.seed = 42;
    demo.realtime = false;
    demo.maximumBatchFrames = 7;
    demo.offset = 0.0;
    demo.amplitude = 1.0;
    demo.algorithmId = 7;
    DemoSource source(config, demo);

    RunWriterOptions writerOptions;
    writerOptions.storeRoot = temporary.path();
    writerOptions.requestedRunId = QStringLiteral("capture-demo");
    CaptureOptions captureOptions;
    captureOptions.readTimeoutMs = 0;
    quint64 callbackFrames = 0;
    quint64 callbackEvents = 0;
    quint64 progressFrames = 0;
    CaptureCallbacks callbacks;
    callbacks.frameReceived = [&callbackFrames](const Frame&) { ++callbackFrames; };
    callbacks.eventReceived = [&callbackEvents](const RunEvent&) { ++callbackEvents; };
    callbacks.progress = [&progressFrames](quint64 frames, quint64) { progressFrames = frames; };

    CaptureSession session;
    const CaptureResult result = session.run(source, config, writerOptions, captureOptions, callbacks);
    check(result.success && !result.interrupted && result.status == RunStatus::Completed,
          QStringLiteral("Demo capture completes: %1").arg(result.error));
    check(result.runId == QStringLiteral("capture-demo") && QFileInfo(result.runDirectory).isDir(),
          QStringLiteral("capture reports its persistent run directory"));
    check(callbackFrames == demo.sampleCount && progressFrames == demo.sampleCount,
          QStringLiteral("capture callbacks observe every frame"));
    check(callbackEvents == 2, QStringLiteral("capture emits source start and stop events"));

    const QJsonObject transport = result.summary.value(QStringLiteral("transport")).toObject();
    check(transport.value(QStringLiteral("frame_count")).toInteger() == 25
              && transport.value(QStringLiteral("parse_error_count")).toInteger() == 0
              && transport.value(QStringLiteral("dropped_frame_count")).toInteger() == 0
              && transport.value(QStringLiteral("timing_violation_count")).toInteger() == 0,
          QStringLiteral("capture summary contains clean transport statistics"));
    const QJsonObject tracking = result.summary.value(QStringLiteral("metrics")).toObject()
                                     .value(QStringLiteral("tracking")).toObject();
    check(tracking.value(QStringLiteral("count")).toInteger() == 25
              && tracking.contains(QStringLiteral("mae"))
              && tracking.contains(QStringLiteral("rmse"))
              && std::isfinite(tracking.value(QStringLiteral("rmse")).toDouble()),
          QStringLiteral("explicit metric mapping produces MAE and RMSE"));
    const QJsonObject algorithm = result.summary.value(QStringLiteral("algorithms")).toObject()
                                      .value(QStringLiteral("7")).toObject();
    check(algorithm.value(QStringLiteral("frame_count")).toInteger() == 25
              && algorithm.value(QStringLiteral("enabled_frame_count")).toInteger() == 25,
          QStringLiteral("capture summary tracks algorithm state"));

    RunReader reader;
    QString error;
    check(reader.open(result.runDirectory, &error),
          QStringLiteral("captured run reopens: %1").arg(error));
    const RunValidationReport report = reader.validate(false);
    check(report.valid && report.frameCount == 25 && report.eventCount == 2
              && report.rawByteCount > 0,
          QStringLiteral("captured run validates against streamed files"));
    quint64 replayedFrames = 0;
    quint64 lastSequence = 0;
    bool allHaveHostTime = true;
    check(reader.forEachFrame([&](const Frame& frame) {
              lastSequence = frame.seq;
              allHaveHostTime = allHaveHostTime && frame.hostReceiveTimeUs.has_value();
              ++replayedFrames;
              return true;
          }, &error),
          QStringLiteral("captured frames replay: %1").arg(error));
    check(replayedFrames == 25 && lastSequence == 24 && allHaveHostTime,
          QStringLiteral("captured frame order and host timestamps persist"));
}

void testRunStoreEnvironmentOverride() {
    const QByteArray previous = qgetenv("AKRION_STORE");
    const QString configured = QDir(QDir::tempPath()).filePath(QStringLiteral("akrion-env-store"));
    qputenv("AKRION_STORE", configured.toUtf8());
    check(RunStore::defaultRoot() == QDir(configured).absolutePath(),
          QStringLiteral("AKRION_STORE overrides the default run location"));
    if (previous.isNull()) {
        qunsetenv("AKRION_STORE");
    } else {
        qputenv("AKRION_STORE", previous);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    testNdjsonDecoder();
    testCanonicalSignatures();
    testRunStorageAndRepair();
    testCaptureSessionDemo();
    testRunStoreEnvironmentOverride();
    if (failures == 0) {
        std::cout << "All core tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
