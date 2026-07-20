#pragma once

#include "frame_decoder.h"
#include "types.h"

#include <QByteArrayView>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace akrion::core {

struct RunWriterOptions {
    QString storeRoot;
    QString requestedRunId;
    QString source;
    qint64 flushByteThreshold = 1024 * 1024;
    qint64 flushIntervalMs = 1000;
};

class RunWriter final {
public:
    RunWriter() = default;
    ~RunWriter();
    RunWriter(const RunWriter&) = delete;
    RunWriter& operator=(const RunWriter&) = delete;

    bool start(const RunConfig& config, const RunWriterOptions& options, QString* error = nullptr);
    bool appendRaw(QByteArrayView bytes, QString* error = nullptr);
    bool appendFrame(const Frame& frame, QString* error = nullptr);
    bool appendEvent(const RunEvent& event, QString* error = nullptr);
    bool flush(QString* error = nullptr);
    bool finish(RunStatus status, const QJsonObject& summary, QString* error = nullptr);

    bool isActive() const { return m_active; }
    QString runId() const { return m_manifest.runId; }
    QString runDirectory() const { return m_runDirectory; }
    const RunManifest& manifest() const { return m_manifest; }

private:
    bool appendJsonLine(QFile* file, const QJsonObject& object, QString* error);
    bool maybeFlush(QString* error);
    bool writeManifest(QString* error);

    bool m_active = false;
    QString m_runDirectory;
    RunManifest m_manifest;
    QFile m_rawFile;
    QFile m_framesFile;
    QFile m_eventsFile;
    QElapsedTimer m_flushTimer;
    qint64 m_flushByteThreshold = 1024 * 1024;
    qint64 m_flushIntervalMs = 1000;
    qint64 m_bytesSinceFlush = 0;
};

struct RunValidationReport {
    bool valid = true;
    bool repaired = false;
    quint64 frameCount = 0;
    quint64 eventCount = 0;
    quint64 rawByteCount = 0;
    QVector<QString> errors;
    QVector<QString> warnings;
};

class RunReader final {
public:
    bool open(const QString& runDirectory, QString* error = nullptr);

    QString runDirectory() const { return m_runDirectory; }
    const RunManifest& manifest() const { return m_manifest; }
    const RunConfig& config() const { return m_config; }
    const QJsonObject& summary() const { return m_summary; }

    bool forEachFrame(const std::function<bool(const Frame&)>& callback, QString* error = nullptr,
                      QVector<DecodeError>* decodeErrors = nullptr) const;
    bool forEachEvent(const std::function<bool(const RunEvent&)>& callback,
                      QString* error = nullptr) const;
    RunValidationReport validate(bool repairTrailingLines = false);

private:
    QString m_runDirectory;
    RunManifest m_manifest;
    RunConfig m_config;
    QJsonObject m_summary;
};

struct RunInfo {
    QString path;
    RunManifest manifest;
};

class RunStore final {
public:
    explicit RunStore(QString root = {});

    static QString defaultRoot();
    QString root() const { return m_root; }
    QVector<RunInfo> list(QString* error = nullptr) const;
    std::optional<RunInfo> find(const QString& runId, QString* error = nullptr) const;
    QVector<RunInfo> findByExperimentSignature(const QString& signature,
                                               QString* error = nullptr) const;
    QString resolve(const QString& runIdOrPath) const;
    bool recover(const QString& runIdOrPath, QString* error = nullptr) const;

private:
    QString m_root;
};

bool writeJsonObjectAtomic(const QString& path, const QJsonObject& object,
                           QString* error = nullptr);
bool readJsonObject(const QString& path, QJsonObject* object, QString* error = nullptr);
QString generateRunId();

} // namespace akrion::core
