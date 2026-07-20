#pragma once

#include "types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

namespace akrion::core {

enum class SourceReadStatus { Data, Timeout, End, Error };

struct SourceReadResult {
    SourceReadStatus status = SourceReadStatus::Timeout;
    QByteArray bytes;
    QString error;
};

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual bool open(QString* error = nullptr) = 0;
    virtual SourceReadResult read(int timeoutMs) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QString description() const = 0;
};

struct SerialPortDescriptor {
    QString name;
    QString systemLocation;
    QString description;
    QString manufacturer;
    QString serialNumber;
    std::optional<quint16> vendorId;
    std::optional<quint16> productId;
};

QVector<SerialPortDescriptor> availableSerialPorts();

class SerialSource final : public ByteSource {
public:
    explicit SerialSource(SerialSettings settings);
    ~SerialSource() override;

    bool open(QString* error = nullptr) override;
    SourceReadResult read(int timeoutMs) override;
    void close() override;
    bool isOpen() const override;
    QString description() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

struct DemoOptions {
    QString inputProfile = QStringLiteral("step");
    QString noiseProfile = QStringLiteral("gaussian");
    quint64 sampleCount = 10000;
    quint64 seed = 1;
    bool realtime = true;
    qsizetype maximumBatchFrames = 64;
    double offset = 0.0;
    double amplitude = 1.0;
    double frequencyHz = 0.5;
    double sweepEndFrequencyHz = 5.0;
    double stepTimeSeconds = 1.0;
    double noiseAmplitude = 0.02;
    double plantTimeConstantSeconds = 0.2;
    double controlGain = 1.5;
    quint64 disabledEveryFrames = 0;
    QString referenceChannel = QStringLiteral("reference");
    QString measurementChannel = QStringLiteral("measurement");
    QString theoreticalChannel = QStringLiteral("theoretical");
    QString controlChannel = QStringLiteral("control");
    QString disturbanceChannel = QStringLiteral("disturbance");
    std::optional<quint32> algorithmId;

    static DemoOptions fromConfig(const RunConfig& config);
};

class DemoSource final : public ByteSource {
public:
    DemoSource(RunConfig config, DemoOptions options = {});
    ~DemoSource() override;

    bool open(QString* error = nullptr) override;
    SourceReadResult read(int timeoutMs) override;
    void close() override;
    bool isOpen() const override;
    QString description() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace akrion::core
