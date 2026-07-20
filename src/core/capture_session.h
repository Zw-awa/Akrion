#pragma once

#include "data_source.h"
#include "run_storage.h"

#include <QJsonObject>

#include <atomic>
#include <functional>

namespace akrion::core {

struct CaptureOptions {
    quint64 maximumFrames = 0;
    qint64 maximumDurationMs = 0;
    qint32 readTimeoutMs = 100;
    std::atomic_bool* stopRequested = nullptr;
};

struct CaptureCallbacks {
    std::function<void(const Frame&)> frameReceived;
    std::function<void(const RunEvent&)> eventReceived;
    std::function<void(quint64 frameCount, quint64 rawByteCount)> progress;
};

struct CaptureResult {
    bool success = false;
    bool interrupted = false;
    RunStatus status = RunStatus::Interrupted;
    QString runId;
    QString runDirectory;
    QString error;
    QJsonObject summary;
};

class CaptureSession final {
public:
    CaptureResult run(ByteSource& source, const RunConfig& config,
                      const RunWriterOptions& writerOptions,
                      const CaptureOptions& options = {},
                      const CaptureCallbacks& callbacks = {});
    CaptureResult run(ByteSource& source, RunWriter& writer, const RunConfig& config,
                      const RunWriterOptions& writerOptions,
                      const CaptureOptions& options = {},
                      const CaptureCallbacks& callbacks = {});
};

} // namespace akrion::core
