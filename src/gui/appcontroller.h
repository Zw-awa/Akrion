#pragma once

#include "../core/frame_decoder.h"
#include "../core/component_registry.h"
#include "../core/component_pipeline.h"
#include "../core/frame_validator.h"
#include "../core/run_storage.h"
#include "../core/statistics.h"
#include "waveformcontroller.h"

#include <QElapsedTimer>
#include <QObject>
#include <QRandomGenerator>
#include <QSerialPort>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <memory>

namespace akrion::gui {

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(WaveformController* waveform READ waveform CONSTANT)
    Q_PROPERTY(QStringList ports READ ports NOTIFY portsChanged)
    Q_PROPERTY(QVariantList inputComponents READ inputComponents CONSTANT)
    Q_PROPERTY(QVariantList noiseComponents READ noiseComponents CONSTANT)
    Q_PROPERTY(QVariantList algorithmComponents READ algorithmComponents CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY sourceChanged)
    Q_PROPERTY(bool demo READ demo NOTIFY sourceChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(QString currentRunId READ currentRunId NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY statisticsChanged)
    Q_PROPERTY(qulonglong receivedFrames READ receivedFrames NOTIFY statisticsChanged)
    Q_PROPERTY(qulonglong parseErrors READ parseErrors NOTIFY statisticsChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    WaveformController* waveform() { return &m_waveform; }
    QStringList ports() const { return m_ports; }
    QVariantList inputComponents() const;
    QVariantList noiseComponents() const;
    QVariantList algorithmComponents() const;
    bool connected() const { return m_serial.isOpen() || m_demoTimer.isActive(); }
    bool demo() const { return m_demoTimer.isActive(); }
    bool recording() const { return m_writer && m_writer->isActive(); }
    QString statusMessage() const { return m_statusMessage; }
    QString currentRunId() const { return recording() ? m_writer->runId() : QString(); }
    QVariantList history() const { return m_history; }
    double frameRate() const { return m_frameRate; }
    qulonglong receivedFrames() const { return m_receivedFrames; }
    qulonglong parseErrors() const { return m_parseErrors; }

    Q_INVOKABLE void refreshPorts();
    Q_INVOKABLE bool connectSerial(const QString& portName, int baudRate);
    Q_INVOKABLE void startDemo(const QString& inputComponent = QStringLiteral("step"),
                               const QString& noiseComponent = QStringLiteral("gaussian"),
                               const QString& algorithmComponent = QStringLiteral("p_control"));
    Q_INVOKABLE void disconnectSource();
    Q_INVOKABLE bool startRecording(const QString& runName,
                                    const QString& deviceId,
                                    const QString& algorithmName,
                                    const QString& parametersJson,
                                    int algorithmPeriodUs,
                                    int emitPeriodUs);
    Q_INVOKABLE bool stopRecording();
    Q_INVOKABLE void refreshHistory();
    Q_INVOKABLE bool replayRun(const QString& runId);
    Q_INVOKABLE void clearWaveform();

signals:
    void portsChanged();
    void sourceChanged();
    void recordingChanged();
    void statusChanged();
    void historyChanged();
    void statisticsChanged();
    void errorOccurred(const QString& message);

private slots:
    void readSerial();
    void emitDemoFrame();

private:
    core::RunConfig baseConfig() const;
    void setStatus(const QString& status);
    void resetLiveState();
    void consumeBytes(const QByteArray& bytes);
    void processBatch(const core::DecodeBatch& batch, quint64 hostTimeUs);
    void publishFrame(const core::Frame& frame);
    void publishEvent(const core::RunEvent& event);
    void ensureChannels(const core::RunConfig& config);
    void ensureFrameChannels(const core::Frame& frame);
    bool appendEvent(const core::RunEvent& event);
    void stopRecordingWithStatus(core::RunStatus status, const QString& reason = {});
    void reportError(const QString& message);
    void updateRate();
    quint64 hostTimeUs() const;

    WaveformController m_waveform;
    QSerialPort m_serial;
    QTimer m_demoTimer;
    QElapsedTimer m_hostClock;
    QElapsedTimer m_rateClock;
    core::NdjsonFrameDecoder m_decoder;
    core::RunConfig m_config;
    std::unique_ptr<core::FrameValidator> m_validator;
    std::unique_ptr<core::RunStatistics> m_statistics;
    std::unique_ptr<core::RunWriter> m_writer;
    std::unique_ptr<core::ComponentPipeline> m_demoPipeline;
    core::RunStore m_store;
    core::ComponentRegistry m_componentRegistry;
    QStringList m_ports;
    QVariantList m_history;
    QString m_statusMessage;
    double m_frameRate = 0.0;
    quint64 m_receivedFrames = 0;
    quint64 m_parseErrors = 0;
    quint64 m_framesSinceRateUpdate = 0;
    quint64 m_demoSequence = 0;
    quint64 m_hostEpochUs = 0;
    QString m_demoInputComponent = QStringLiteral("step");
    QString m_demoNoiseComponent = QStringLiteral("gaussian");
    QString m_demoAlgorithmComponent = QStringLiteral("p_control");
};

} // namespace akrion::gui
