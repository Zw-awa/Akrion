#include "waveformbuffer.h"

#include <QtMath>

#include <algorithm>

namespace akrion::gui {
namespace {

constexpr int kMinimumCapacity = 1024;
constexpr int kMaximumEvents = 8192;

struct Bucket {
    bool used = false;
    bool startsSegment = false;
    qint64 firstTimeUs = 0;
    qint64 lastTimeUs = 0;
    double first = 0.0;
    double last = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
};

} // namespace

WaveformBuffer::WaveformBuffer(int capacity) {
    setCapacity(capacity);
}

int WaveformBuffer::capacity() const {
    QReadLocker locker(&m_lock);
    return m_capacity;
}

int WaveformBuffer::size() const {
    QReadLocker locker(&m_lock);
    return m_count;
}

quint64 WaveformBuffer::totalFrames() const {
    QReadLocker locker(&m_lock);
    return m_totalFrames;
}

quint64 WaveformBuffer::droppedFrames() const {
    QReadLocker locker(&m_lock);
    return m_droppedFrames;
}

void WaveformBuffer::setCapacity(int capacity) {
    capacity = qMax(kMinimumCapacity, capacity);
    QWriteLocker locker(&m_lock);
    if (capacity == m_capacity) return;

    const int keep = qMin(m_count, capacity);
    const int firstToKeep = m_count - keep;
    QVector<qint64> deviceTimes(capacity);
    QVector<qint64> hostTimes(capacity);
    QVector<quint64> sequences(capacity);
    QVector<qint32> algorithmIds(capacity);
    QBitArray algorithmEnabled(capacity);

    QVector<ChannelStorage> channels = m_channels;
    for (auto& channel : channels) {
        channel.values = QVector<double>(capacity);
        channel.valid = QBitArray(capacity);
    }

    for (int target = 0; target < keep; ++target) {
        const int source = physicalIndex(firstToKeep + target);
        deviceTimes[target] = m_deviceTimes.value(source);
        hostTimes[target] = m_hostTimes.value(source);
        sequences[target] = m_sequences.value(source);
        algorithmIds[target] = m_algorithmIds.value(source);
        algorithmEnabled.setBit(target, m_algorithmEnabled.testBit(source));
        for (int channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
            const bool valid = m_channels[channelIndex].valid.testBit(source);
            channels[channelIndex].valid.setBit(target, valid);
            if (valid) channels[channelIndex].values[target] = m_channels[channelIndex].values[source];
        }
    }

    m_capacity = capacity;
    m_count = keep;
    m_head = keep % capacity;
    m_deviceTimes = std::move(deviceTimes);
    m_hostTimes = std::move(hostTimes);
    m_sequences = std::move(sequences);
    m_algorithmIds = std::move(algorithmIds);
    m_algorithmEnabled = std::move(algorithmEnabled);
    m_channels = std::move(channels);
}

void WaveformBuffer::clear() {
    QWriteLocker locker(&m_lock);
    m_count = 0;
    m_head = 0;
    m_events.clear();
    m_totalFrames = 0;
    m_droppedFrames = 0;
    m_lastSequence = 0;
    m_hasLastSequence = false;
    m_deviceTimeMonotonic = true;
    m_hostTimeMonotonic = true;
    m_algorithmEnabled.fill(false);
    for (auto& channel : m_channels) channel.valid.fill(false);
}

int WaveformBuffer::ensureChannel(const WaveformChannel& channel) {
    if (channel.key.isEmpty()) return -1;
    QWriteLocker locker(&m_lock);
    const int existing = channelIndexForKey(channel.key);
    if (existing >= 0) {
        m_channels[existing].channel = channel;
        return existing;
    }

    ChannelStorage storage;
    storage.channel = channel;
    storage.values = QVector<double>(m_capacity);
    storage.valid = QBitArray(m_capacity);
    m_channels.append(std::move(storage));
    return m_channels.size() - 1;
}

bool WaveformBuffer::updateChannel(int index, const WaveformChannel& channel) {
    QWriteLocker locker(&m_lock);
    if (index < 0 || index >= m_channels.size() || channel.key.isEmpty()) return false;
    if (channel.key != m_channels[index].channel.key && channelIndexForKey(channel.key) >= 0) return false;
    m_channels[index].channel = channel;
    return true;
}

QVector<WaveformChannel> WaveformBuffer::channels() const {
    QReadLocker locker(&m_lock);
    QVector<WaveformChannel> result;
    result.reserve(m_channels.size());
    for (const auto& channel : m_channels) result.append(channel.channel);
    return result;
}

void WaveformBuffer::append(const WaveformFrame& frame) {
    QWriteLocker locker(&m_lock);
    if (m_capacity <= 0) return;

    if (m_count > 0) {
        const int previous = physicalIndex(m_count - 1);
        m_deviceTimeMonotonic = m_deviceTimeMonotonic && frame.deviceTimeUs >= m_deviceTimes[previous];
        m_hostTimeMonotonic = m_hostTimeMonotonic && frame.hostReceiveTimeUs >= m_hostTimes[previous];
    }
    if (m_hasLastSequence && frame.sequence > m_lastSequence + 1) {
        m_droppedFrames += frame.sequence - m_lastSequence - 1;
    }
    m_lastSequence = frame.sequence;
    m_hasLastSequence = true;

    const int target = m_head;
    m_deviceTimes[target] = frame.deviceTimeUs;
    m_hostTimes[target] = frame.hostReceiveTimeUs;
    m_sequences[target] = frame.sequence;
    m_algorithmIds[target] = frame.algorithmId;
    m_algorithmEnabled.setBit(target, frame.algorithmEnabled);
    for (auto& channel : m_channels) channel.valid.setBit(target, false);

    for (const auto& value : frame.values) {
        const int channelIndex = channelIndexForKey(value.key);
        if (channelIndex < 0 || !qIsFinite(value.value)) continue;
        m_channels[channelIndex].values[target] = value.value;
        m_channels[channelIndex].valid.setBit(target, true);
    }

    m_head = (m_head + 1) % m_capacity;
    m_count = qMin(m_count + 1, m_capacity);
    ++m_totalFrames;
}

void WaveformBuffer::appendEvent(const WaveformEvent& event) {
    QWriteLocker locker(&m_lock);
    m_events.append(event);
    if (m_events.size() > kMaximumEvents) m_events.remove(0, m_events.size() - kMaximumEvents);
}

qint64 WaveformBuffer::earliestTimeUs(WaveformTimeAxis axis) const {
    QReadLocker locker(&m_lock);
    return m_count == 0 ? 0 : timeAt(0, axis);
}

qint64 WaveformBuffer::latestTimeUs(WaveformTimeAxis axis) const {
    QReadLocker locker(&m_lock);
    return m_count == 0 ? 0 : timeAt(m_count - 1, axis);
}

WaveformQueryResult WaveformBuffer::query(
    WaveformTimeAxis axis,
    qint64 startUs,
    qint64 endUs,
    int pixelWidth,
    const QVector<int>& channelIndices,
    bool includeDisabledSamples) const {
    QReadLocker locker(&m_lock);
    WaveformQueryResult result;
    result.startUs = startUs;
    result.endUs = endUs;
    if (m_count == 0 || endUs <= startUs) return result;

    pixelWidth = qBound(1, pixelWidth, 16384);
    QVector<QVector<Bucket>> buckets;
    buckets.reserve(channelIndices.size());
    for (int ignored : channelIndices) {
        Q_UNUSED(ignored);
        buckets.append(QVector<Bucket>(pixelWidth));
    }
    QVector<bool> previousValid(channelIndices.size(), false);

    const bool monotonic = axis == WaveformTimeAxis::Device ? m_deviceTimeMonotonic : m_hostTimeMonotonic;
    const int first = monotonic ? lowerBound(startUs, axis) : 0;
    const int last = monotonic ? upperBound(endUs, axis) : m_count;
    bool inDisabledSpan = first < last && first > 0 && !m_algorithmEnabled.testBit(physicalIndex(first - 1));
    qint64 disabledStart = inDisabledSpan ? startUs : 0;

    for (int logical = first; logical < last; ++logical) {
        const int physical = physicalIndex(logical);
        const qint64 timeUs = axis == WaveformTimeAxis::Device ? m_deviceTimes[physical] : m_hostTimes[physical];
        if (timeUs < startUs || timeUs > endUs) {
            previousValid.fill(false);
            continue;
        }
        const bool enabled = m_algorithmEnabled.testBit(physical);
        if (!enabled && !inDisabledSpan) {
            inDisabledSpan = true;
            disabledStart = timeUs;
        } else if (enabled && inDisabledSpan) {
            result.disabledSpans.append({ disabledStart, timeUs });
            inDisabledSpan = false;
        }

        const qint64 relative = qBound<qint64>(0, timeUs - startUs, endUs - startUs);
        const int bucketIndex = qMin(pixelWidth - 1,
            static_cast<int>((static_cast<long double>(relative) * pixelWidth) / (endUs - startUs)));

        for (int requested = 0; requested < channelIndices.size(); ++requested) {
            const int channelIndex = channelIndices[requested];
            const bool validChannel = channelIndex >= 0 && channelIndex < m_channels.size();
            const bool valid = validChannel && m_channels[channelIndex].valid.testBit(physical)
                && (includeDisabledSamples || enabled);
            if (!valid) {
                previousValid[requested] = false;
                continue;
            }

            const double value = m_channels[channelIndex].values[physical];
            Bucket& bucket = buckets[requested][bucketIndex];
            if (!bucket.used) {
                bucket.used = true;
                bucket.startsSegment = !previousValid[requested];
                bucket.firstTimeUs = bucket.lastTimeUs = timeUs;
                bucket.first = bucket.last = bucket.minimum = bucket.maximum = value;
            } else {
                bucket.lastTimeUs = timeUs;
                bucket.last = value;
                bucket.minimum = qMin(bucket.minimum, value);
                bucket.maximum = qMax(bucket.maximum, value);
            }
            previousValid[requested] = true;
        }
    }
    if (inDisabledSpan) result.disabledSpans.append({ disabledStart, endUs });

    result.series.reserve(channelIndices.size());
    for (int requested = 0; requested < channelIndices.size(); ++requested) {
        WaveformSeriesSnapshot series;
        series.channelIndex = channelIndices[requested];
        for (const Bucket& bucket : buckets[requested]) {
            if (!bucket.used) continue;
            series.envelopes.append({
                bucket.firstTimeUs,
                bucket.lastTimeUs,
                bucket.first,
                bucket.last,
                bucket.minimum,
                bucket.maximum,
                bucket.startsSegment,
            });
        }
        result.series.append(std::move(series));
    }

    for (const auto& event : m_events) {
        const qint64 timeUs = axis == WaveformTimeAxis::Device ? event.deviceTimeUs : event.hostReceiveTimeUs;
        if (timeUs >= startUs && timeUs <= endUs) result.events.append(event);
    }
    return result;
}

WaveformCursorSample WaveformBuffer::nearest(WaveformTimeAxis axis, qint64 timeUs) const {
    QReadLocker locker(&m_lock);
    WaveformCursorSample result;
    if (m_count == 0) return result;

    const bool monotonic = axis == WaveformTimeAxis::Device ? m_deviceTimeMonotonic : m_hostTimeMonotonic;
    int logical = 0;
    if (monotonic) {
        logical = lowerBound(timeUs, axis);
        if (logical >= m_count) logical = m_count - 1;
        if (logical > 0) {
            const qint64 currentDistance = qAbs(timeAt(logical, axis) - timeUs);
            const qint64 previousDistance = qAbs(timeAt(logical - 1, axis) - timeUs);
            if (previousDistance <= currentDistance) --logical;
        }
    } else {
        qint64 bestDistance = qAbs(timeAt(0, axis) - timeUs);
        for (int candidate = 1; candidate < m_count; ++candidate) {
            const qint64 distance = qAbs(timeAt(candidate, axis) - timeUs);
            if (distance < bestDistance) {
                bestDistance = distance;
                logical = candidate;
            }
        }
    }

    const int physical = physicalIndex(logical);
    result.valid = true;
    result.timeUs = timeAt(logical, axis);
    result.deviceTimeUs = m_deviceTimes[physical];
    result.hostReceiveTimeUs = m_hostTimes[physical];
    result.sequence = m_sequences[physical];
    result.algorithmId = m_algorithmIds[physical];
    result.algorithmEnabled = m_algorithmEnabled.testBit(physical);
    result.values.resize(m_channels.size());
    result.valueValid.resize(m_channels.size());
    for (int channelIndex = 0; channelIndex < m_channels.size(); ++channelIndex) {
        const bool valid = m_channels[channelIndex].valid.testBit(physical);
        result.valueValid.setBit(channelIndex, valid);
        if (valid) result.values[channelIndex] = m_channels[channelIndex].values[physical];
    }
    return result;
}

int WaveformBuffer::physicalIndex(int logicalIndex) const {
    if (m_capacity <= 0) return 0;
    const int oldest = (m_head - m_count + m_capacity) % m_capacity;
    return (oldest + logicalIndex) % m_capacity;
}

qint64 WaveformBuffer::timeAt(int logicalIndex, WaveformTimeAxis axis) const {
    const int physical = physicalIndex(logicalIndex);
    return axis == WaveformTimeAxis::Device ? m_deviceTimes[physical] : m_hostTimes[physical];
}

int WaveformBuffer::lowerBound(qint64 timeUs, WaveformTimeAxis axis) const {
    const bool monotonic = axis == WaveformTimeAxis::Device ? m_deviceTimeMonotonic : m_hostTimeMonotonic;
    if (!monotonic) {
        for (int logical = 0; logical < m_count; ++logical) {
            if (timeAt(logical, axis) >= timeUs) return logical;
        }
        return m_count;
    }
    int low = 0;
    int high = m_count;
    while (low < high) {
        const int middle = low + (high - low) / 2;
        if (timeAt(middle, axis) < timeUs) low = middle + 1;
        else high = middle;
    }
    return low;
}

int WaveformBuffer::upperBound(qint64 timeUs, WaveformTimeAxis axis) const {
    const bool monotonic = axis == WaveformTimeAxis::Device ? m_deviceTimeMonotonic : m_hostTimeMonotonic;
    if (!monotonic) {
        int last = 0;
        for (int logical = 0; logical < m_count; ++logical) {
            if (timeAt(logical, axis) <= timeUs) last = logical + 1;
        }
        return last;
    }
    int low = 0;
    int high = m_count;
    while (low < high) {
        const int middle = low + (high - low) / 2;
        if (timeAt(middle, axis) <= timeUs) low = middle + 1;
        else high = middle;
    }
    return low;
}

int WaveformBuffer::channelIndexForKey(const QString& key) const {
    for (int index = 0; index < m_channels.size(); ++index) {
        if (m_channels[index].channel.key == key) return index;
    }
    return -1;
}

} // namespace akrion::gui
