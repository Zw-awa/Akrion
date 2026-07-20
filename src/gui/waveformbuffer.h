#pragma once

#include <QBitArray>
#include <QColor>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

#include <limits>

namespace akrion::gui {

enum class WaveformTimeAxis {
    Device,
    Host,
};

struct WaveformChannel {
    QString key;
    QString label;
    QString unit;
    QString role;
    QString description;
    QString groupKey;
    QColor color;
    bool visible = true;
};

struct WaveformValue {
    QString key;
    double value = std::numeric_limits<double>::quiet_NaN();
};

struct WaveformFrame {
    qint64 deviceTimeUs = 0;
    qint64 hostReceiveTimeUs = 0;
    quint64 sequence = 0;
    qint32 algorithmId = 0;
    bool algorithmEnabled = true;
    QVector<WaveformValue> values;
};

struct WaveformEvent {
    qint64 deviceTimeUs = 0;
    qint64 hostReceiveTimeUs = 0;
    QString type;
    QString label;
    int severity = 0;
};

struct WaveformEnvelope {
    qint64 firstTimeUs = 0;
    qint64 lastTimeUs = 0;
    double first = 0.0;
    double last = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    bool startsSegment = false;
};

struct WaveformSeriesSnapshot {
    int channelIndex = -1;
    QVector<WaveformEnvelope> envelopes;
};

struct WaveformTimeSpan {
    qint64 startUs = 0;
    qint64 endUs = 0;
};

struct WaveformQueryResult {
    qint64 startUs = 0;
    qint64 endUs = 0;
    QVector<WaveformSeriesSnapshot> series;
    QVector<WaveformTimeSpan> disabledSpans;
    QVector<WaveformEvent> events;
};

struct WaveformCursorSample {
    bool valid = false;
    qint64 timeUs = 0;
    qint64 deviceTimeUs = 0;
    qint64 hostReceiveTimeUs = 0;
    quint64 sequence = 0;
    qint32 algorithmId = 0;
    bool algorithmEnabled = true;
    QVector<double> values;
    QBitArray valueValid;
};

class WaveformBuffer final {
public:
    explicit WaveformBuffer(int capacity = 120000);

    int capacity() const;
    int size() const;
    quint64 totalFrames() const;
    quint64 droppedFrames() const;

    void setCapacity(int capacity);
    void clear();

    int ensureChannel(const WaveformChannel& channel);
    bool updateChannel(int index, const WaveformChannel& channel);
    QVector<WaveformChannel> channels() const;

    void append(const WaveformFrame& frame);
    void appendEvent(const WaveformEvent& event);

    qint64 earliestTimeUs(WaveformTimeAxis axis) const;
    qint64 latestTimeUs(WaveformTimeAxis axis) const;
    WaveformQueryResult query(
        WaveformTimeAxis axis,
        qint64 startUs,
        qint64 endUs,
        int pixelWidth,
        const QVector<int>& channelIndices,
        bool includeDisabledSamples) const;
    WaveformCursorSample nearest(WaveformTimeAxis axis, qint64 timeUs) const;

private:
    struct ChannelStorage {
        WaveformChannel channel;
        QVector<double> values;
        QBitArray valid;
    };

    int physicalIndex(int logicalIndex) const;
    qint64 timeAt(int logicalIndex, WaveformTimeAxis axis) const;
    int lowerBound(qint64 timeUs, WaveformTimeAxis axis) const;
    int upperBound(qint64 timeUs, WaveformTimeAxis axis) const;
    int channelIndexForKey(const QString& key) const;

    mutable QReadWriteLock m_lock;
    int m_capacity = 0;
    int m_count = 0;
    int m_head = 0;
    QVector<qint64> m_deviceTimes;
    QVector<qint64> m_hostTimes;
    QVector<quint64> m_sequences;
    QVector<qint32> m_algorithmIds;
    QBitArray m_algorithmEnabled;
    QVector<ChannelStorage> m_channels;
    QVector<WaveformEvent> m_events;
    quint64 m_totalFrames = 0;
    quint64 m_droppedFrames = 0;
    quint64 m_lastSequence = 0;
    bool m_hasLastSequence = false;
    bool m_deviceTimeMonotonic = true;
    bool m_hostTimeMonotonic = true;
};

} // namespace akrion::gui
