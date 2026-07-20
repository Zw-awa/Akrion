#include "capture_session.h"

#include "frame_decoder.h"
#include "frame_validator.h"
#include "statistics.h"

#include <QDateTime>
#include <QElapsedTimer>

namespace akrion::core {
namespace {

quint64 epochMicroseconds(const QElapsedTimer& monotonic, quint64 epochAtStart) {
    return epochAtStart + static_cast<quint64>(monotonic.nsecsElapsed() / 1000);
}

RunEvent decodeErrorEvent(const DecodeError& error, quint64 hostTimeUs) {
    RunEvent event;
    event.type = error.code;
    event.severity = EventSeverity::Error;
    event.message = error.message;
    event.hostTimeUs = hostTimeUs;
    event.details = {{QStringLiteral("line_number"), static_cast<qint64>(error.lineNumber)},
                     {QStringLiteral("byte_offset"), static_cast<qint64>(error.byteOffset)},
                     {QStringLiteral("raw_excerpt_base64"),
                      QString::fromLatin1(error.rawExcerpt.toBase64())}};
    return event;
}

RunEvent sourceEvent(const QString& type, EventSeverity severity, const QString& message,
                     quint64 hostTimeUs) {
    RunEvent event;
    event.type = type;
    event.severity = severity;
    event.message = message;
    event.hostTimeUs = hostTimeUs;
    return event;
}

} // namespace

CaptureResult CaptureSession::run(ByteSource& source, const RunConfig& config,
                                  const RunWriterOptions& writerOptions,
                                  const CaptureOptions& options,
                                  const CaptureCallbacks& callbacks) {
    RunWriter writer;
    return run(source, writer, config, writerOptions, options, callbacks);
}

CaptureResult CaptureSession::run(ByteSource& source, RunWriter& writer, const RunConfig& config,
                                  const RunWriterOptions& writerOptions,
                                  const CaptureOptions& options,
                                  const CaptureCallbacks& callbacks) {
    CaptureResult result;
    QString error;
    if (!source.open(&error)) {
        result.error = error;
        return result;
    }
    auto effectiveWriterOptions = writerOptions;
    if (effectiveWriterOptions.source.isEmpty())
        effectiveWriterOptions.source = source.description();
    if (!writer.start(config, effectiveWriterOptions, &error)) {
        source.close();
        result.error = error;
        return result;
    }
    result.runId = writer.runId();
    result.runDirectory = writer.runDirectory();

    NdjsonFrameDecoder decoder;
    FrameValidator validator(config);
    RunStatistics statistics(config);
    QElapsedTimer duration;
    QElapsedTimer hostClock;
    duration.start();
    hostClock.start();
    const auto hostEpochUs =
        static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000ULL;
    bool ioFailure = false;
    bool sourceEnded = false;
    bool interrupted = false;

    const auto appendEvent = [&](const RunEvent& event) {
        if (!writer.appendEvent(event, &error)) {
            ioFailure = true;
            return false;
        }
        if (callbacks.eventReceived) callbacks.eventReceived(event);
        return true;
    };
    appendEvent(sourceEvent(QStringLiteral("source_started"), EventSeverity::Info,
                            source.description(), epochMicroseconds(hostClock, hostEpochUs)));

    while (!ioFailure) {
        if (options.stopRequested && options.stopRequested->load(std::memory_order_relaxed)) {
            interrupted = true;
            break;
        }
        if (options.maximumDurationMs > 0 && duration.elapsed() >= options.maximumDurationMs)
            break;
        if (options.maximumFrames > 0 && statistics.frameCount() >= options.maximumFrames) break;

        const auto read = source.read(qMax(0, options.readTimeoutMs));
        const auto hostTimeUs = epochMicroseconds(hostClock, hostEpochUs);
        if (read.status == SourceReadStatus::Timeout) continue;
        if (read.status == SourceReadStatus::End) {
            sourceEnded = true;
            break;
        }
        if (read.status == SourceReadStatus::Error) {
            appendEvent(sourceEvent(QStringLiteral("source_error"), EventSeverity::Error,
                                    read.error, hostTimeUs));
            error = read.error;
            ioFailure = true;
            break;
        }
        if (!writer.appendRaw(read.bytes, &error)) {
            ioFailure = true;
            break;
        }
        auto batch = decoder.feed(read.bytes, hostTimeUs);
        for (const auto& decodeError : batch.errors) {
            statistics.observeDecodeError(decodeError);
            if (!appendEvent(decodeErrorEvent(decodeError, hostTimeUs))) break;
        }
        for (const auto& frame : batch.frames) {
            if (ioFailure) break;
            if (options.maximumFrames > 0 && statistics.frameCount() >= options.maximumFrames)
                break;
            const auto issues = validator.inspect(frame);
            statistics.observeFrame(frame, issues);
            if (!writer.appendFrame(frame, &error)) {
                ioFailure = true;
                break;
            }
            if (callbacks.frameReceived) callbacks.frameReceived(frame);
            for (const auto& issue : issues) {
                if (!appendEvent(validationIssueEvent(issue, frame))) break;
            }
        }
        if (callbacks.progress)
            callbacks.progress(statistics.frameCount(), writer.manifest().rawByteCount);
    }

    const auto finalHostTimeUs = epochMicroseconds(hostClock, hostEpochUs);
    auto finalBatch = decoder.finish(finalHostTimeUs);
    for (const auto& decodeError : finalBatch.errors) {
        statistics.observeDecodeError(decodeError);
        if (!ioFailure) appendEvent(decodeErrorEvent(decodeError, finalHostTimeUs));
    }
    for (const auto& frame : finalBatch.frames) {
        const auto issues = validator.inspect(frame);
        statistics.observeFrame(frame, issues);
        if (!ioFailure && !writer.appendFrame(frame, &error)) ioFailure = true;
    }
    source.close();

    const auto terminalStatus = interrupted || ioFailure ? RunStatus::Interrupted
                                                          : RunStatus::Completed;
    if (!ioFailure) {
        appendEvent(sourceEvent(QStringLiteral("source_stopped"), EventSeverity::Info,
                                sourceEnded ? QStringLiteral("source reached end")
                                            : QStringLiteral("capture stopped"),
                                finalHostTimeUs));
    }
    result.summary = statistics.summary(terminalStatus);
    QString finishError;
    const auto finalized = writer.finish(terminalStatus, result.summary, &finishError);
    if (!finalized && error.isEmpty()) error = finishError;
    result.success = finalized && !ioFailure;
    result.interrupted = interrupted;
    result.status = terminalStatus;
    result.error = error;
    return result;
}

} // namespace akrion::core
