#include "waveformcontroller.h"

#include <QVariantMap>
#include <QThread>
#include <QtMath>

#include <algorithm>
#include <utility>

namespace akrion::gui {

WaveformChannelModel::WaveformChannelModel(WaveformController* controller)
    : QAbstractListModel(controller), m_controller(controller) {}

int WaveformChannelModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || !m_controller ? 0 : m_controller->m_channels.size();
}

QVariant WaveformChannelModel::data(const QModelIndex& index, int role) const {
    if (!m_controller || !index.isValid() || index.row() < 0 || index.row() >= m_controller->m_channels.size()) {
        return {};
    }
    const WaveformChannel& channel = m_controller->m_channels[index.row()];
    switch (role) {
    case KeyRole: return channel.key;
    case LabelRole: return channel.label;
    case UnitRole: return channel.unit;
    case ChannelRole: return channel.role;
    case DescriptionRole: return channel.description;
    case GroupRole: return channel.groupKey;
    case ColorRole: return channel.color;
    case VisibleRole: return channel.visible;
    case LatestValueRole:
        return index.row() < m_controller->m_latestValues.size()
                && m_controller->m_latestValueValid.testBit(index.row())
            ? QVariant(m_controller->m_latestValues[index.row()]) : QVariant();
    case HasValueRole:
        return index.row() < m_controller->m_latestValues.size()
            && m_controller->m_latestValueValid.testBit(index.row());
    default: return {};
    }
}

QHash<int, QByteArray> WaveformChannelModel::roleNames() const {
    return {
        { KeyRole, "key" },
        { LabelRole, "label" },
        { UnitRole, "unit" },
        { ChannelRole, "channelRole" },
        { DescriptionRole, "description" },
        { GroupRole, "groupKey" },
        { ColorRole, "channelColor" },
        { VisibleRole, "channelVisible" },
        { LatestValueRole, "latestValue" },
        { HasValueRole, "hasValue" },
    };
}

void WaveformChannelModel::reset() {
    beginResetModel();
    endResetModel();
}

void WaveformChannelModel::channelChanged(int row, const QVector<int>& roles) {
    if (row < 0 || row >= rowCount()) return;
    emit dataChanged(index(row), index(row), roles);
}

WaveformController::WaveformController(QObject* parent)
    : QObject(parent), m_buffer(120000), m_channelModel(this) {
    m_updateTimer.setInterval(16);
    m_updateTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_updateTimer, &QTimer::timeout, this, [this] {
        if (!m_pendingUpdate && !m_pendingGroupsChanged) return;
        m_pendingUpdate = false;
        if (m_pendingGroupsChanged) {
            m_pendingGroupsChanged = false;
            emit groupsChanged();
        }
        emit frameStatsChanged();
        if (!m_channels.isEmpty()) {
            m_channelModel.channelChanged(0, { WaveformChannelModel::LatestValueRole,
                                               WaveformChannelModel::HasValueRole });
            if (m_channels.size() > 1) {
                emit m_channelModel.dataChanged(
                    m_channelModel.index(0), m_channelModel.index(m_channels.size() - 1),
                    { WaveformChannelModel::LatestValueRole, WaveformChannelModel::HasValueRole });
            }
        }
        if (!m_paused) {
            emit viewportChanged();
            emit renderNeeded();
        }
    });
    m_updateTimer.start();
}

QVariantList WaveformController::groupInfo() const {
    QVariantList result;
    for (const QString& groupKey : m_groupKeys) {
        QString unit;
        int visibleChannels = 0;
        int totalChannels = 0;
        bool haveUnit = false;
        bool mixedUnits = false;
        for (const auto& channel : m_channels) {
            if (channel.groupKey != groupKey) continue;
            ++totalChannels;
            if (channel.visible) ++visibleChannels;
            if (!haveUnit) {
                unit = channel.unit;
                haveUnit = true;
            } else if (unit != channel.unit) {
                mixedUnits = true;
            }
        }
        if (totalChannels == 0) continue;
        const GroupScale scale = m_groupScales.value(groupKey);
        QVariantMap item;
        item.insert(QStringLiteral("key"), groupKey);
        item.insert(QStringLiteral("label"), groupKey);
        item.insert(QStringLiteral("unit"), mixedUnits ? QString() : unit);
        item.insert(QStringLiteral("visibleChannels"), visibleChannels);
        item.insert(QStringLiteral("totalChannels"), totalChannels);
        item.insert(QStringLiteral("autoRange"), scale.automatic);
        item.insert(QStringLiteral("minimum"), scale.minimum);
        item.insert(QStringLiteral("maximum"), scale.maximum);
        result.append(item);
    }
    return result;
}

QString WaveformController::timeAxis() const {
    return m_timeAxis == WaveformTimeAxis::Device ? QStringLiteral("device") : QStringLiteral("host");
}

void WaveformController::setWindowSeconds(double seconds) {
    seconds = qBound(0.001, seconds, 3600.0);
    if (qFuzzyCompare(m_windowSeconds, seconds)) return;
    m_windowSeconds = seconds;
    const qint64 spanUs = qMax<qint64>(1000, qRound64(seconds * 1000000.0));
    if (m_follow) {
        updateViewportForLatest();
    } else {
        const qint64 center = m_viewStartUs + (m_viewEndUs - m_viewStartUs) / 2;
        m_viewStartUs = center - spanUs / 2;
        m_viewEndUs = m_viewStartUs + spanUs;
        clampViewport();
    }
    updateCursorValues();
    emit viewportChanged();
    emit renderNeeded();
}

void WaveformController::setPaused(bool paused) {
    if (m_paused == paused) return;
    m_paused = paused;
    if (!m_paused && m_follow) updateViewportForLatest();
    emit pausedChanged();
    emit viewportChanged();
    emit renderNeeded();
}

void WaveformController::setFollow(bool follow) {
    if (m_follow == follow && (!follow || !m_paused)) return;
    m_follow = follow;
    if (m_follow) {
        if (m_paused) {
            m_paused = false;
            emit pausedChanged();
        }
        updateViewportForLatest();
    }
    emit followChanged();
    emit viewportChanged();
    emit renderNeeded();
}

void WaveformController::setTimeAxis(const QString& axis) {
    const WaveformTimeAxis next = axis.compare(QStringLiteral("host"), Qt::CaseInsensitive) == 0
        ? WaveformTimeAxis::Host
        : WaveformTimeAxis::Device;
    if (m_timeAxis == next) return;
    m_timeAxis = next;
    m_viewInitialized = false;
    updateViewportForLatest();
    updateCursorValues();
    emit timeAxisChanged();
    emit viewportChanged();
    emit renderNeeded();
}

void WaveformController::setIncludeDisabledSamples(bool include) {
    if (m_includeDisabledSamples == include) return;
    m_includeDisabledSamples = include;
    fitAllAutomaticRanges();
    emit includeDisabledSamplesChanged();
    emit groupsChanged();
    emit renderNeeded();
}

int WaveformController::defineChannel(
    const QString& key,
    const QString& label,
    const QString& unit,
    const QString& role,
    const QString& description,
    const QString& groupKey,
    const QString& color) {
    const QString cleanKey = key.trimmed();
    if (cleanKey.isEmpty()) return -1;

    WaveformChannel channel;
    channel.key = cleanKey;
    channel.label = label.trimmed().isEmpty() ? cleanKey : label.trimmed();
    channel.unit = unit.trimmed();
    channel.role = role.trimmed().isEmpty() ? QStringLiteral("other") : role.trimmed();
    channel.description = description.trimmed();
    channel.groupKey = normalizedGroupKey(groupKey, channel.unit);

    int index = channelIndex(cleanKey);
    if (index >= 0) channel.visible = m_channels[index].visible;
    channel.color = QColor(color);
    if (!channel.color.isValid()) channel.color = index >= 0 ? m_channels[index].color : defaultColor(m_channels.size());

    if (index >= 0) {
        m_channels[index] = channel;
        m_buffer.updateChannel(index, channel);
        m_channelModel.channelChanged(index, {
            WaveformChannelModel::LabelRole,
            WaveformChannelModel::UnitRole,
            WaveformChannelModel::ChannelRole,
            WaveformChannelModel::DescriptionRole,
            WaveformChannelModel::GroupRole,
            WaveformChannelModel::ColorRole,
        });
    } else {
        index = m_channels.size();
        m_channels.append(channel);
        m_latestValues.append(0.0);
        m_latestValueValid.resize(m_channels.size());
        m_buffer.ensureChannel(channel);
        m_channelModel.reset();
    }
    rebuildGroups();
    emit renderNeeded();
    return index;
}

void WaveformController::appendFrameValues(
    qint64 deviceTimeUs,
    qint64 hostReceiveTimeUs,
    qulonglong sequence,
    int algorithmId,
    bool algorithmEnabled,
    const QStringList& valueKeys,
    const QVariantList& values) {
    WaveformFrame frame;
    frame.deviceTimeUs = deviceTimeUs;
    frame.hostReceiveTimeUs = hostReceiveTimeUs;
    frame.sequence = sequence;
    frame.algorithmId = algorithmId;
    frame.algorithmEnabled = algorithmEnabled;
    const int count = qMin(valueKeys.size(), values.size());
    frame.values.reserve(count);
    for (int index = 0; index < count; ++index) {
        bool ok = false;
        const double value = values[index].toDouble(&ok);
        if (!ok || !qIsFinite(value) || valueKeys[index].isEmpty()) continue;
        frame.values.append({ valueKeys[index], value });
    }
    appendFrame(frame);
}

void WaveformController::appendFrame(const WaveformFrame& frame) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, frame] { appendFrame(frame); }, Qt::QueuedConnection);
        return;
    }
    for (const auto& value : frame.values) {
        if (channelIndex(value.key) < 0) {
            defineChannel(value.key, value.key, QString(), QStringLiteral("other"), QString());
        }
    }

    if (m_haveAlgorithmState) {
        if (frame.algorithmId != m_lastAlgorithmId) {
            appendEvent({
                frame.deviceTimeUs,
                frame.hostReceiveTimeUs,
                QStringLiteral("algorithm_changed"),
                QStringLiteral("Algorithm changed to %1").arg(frame.algorithmId),
                0,
            });
        }
        if (frame.algorithmEnabled != m_lastAlgorithmEnabled) {
            appendEvent({
                frame.deviceTimeUs,
                frame.hostReceiveTimeUs,
                frame.algorithmEnabled ? QStringLiteral("algorithm_enabled") : QStringLiteral("algorithm_disabled"),
                frame.algorithmEnabled ? QStringLiteral("Algorithm enabled") : QStringLiteral("Algorithm disabled"),
                frame.algorithmEnabled ? 0 : 1,
            });
        }
    }
    m_haveAlgorithmState = true;
    m_lastAlgorithmId = frame.algorithmId;
    m_lastAlgorithmEnabled = frame.algorithmEnabled;

    m_buffer.append(frame);
    for (const auto& value : frame.values) {
        const int index = channelIndex(value.key);
        if (index < 0) continue;
        m_latestValues[index] = value.value;
        m_latestValueValid.setBit(index, true);
    }
    if (frame.algorithmEnabled || m_includeDisabledSamples) {
        for (const auto& value : frame.values) {
            const int index = channelIndex(value.key);
            if (index < 0 || !m_channels[index].visible) continue;
            if (expandAutomaticRange(m_channels[index].groupKey, value.value)) {
                m_pendingGroupsChanged = true;
            }
        }
    }
    if (!m_paused && m_follow) updateViewportForLatest();
    scheduleUpdate();
}

void WaveformController::addEvent(
    qint64 deviceTimeUs,
    qint64 hostReceiveTimeUs,
    const QString& type,
    const QString& label,
    int severity) {
    appendEvent({ deviceTimeUs, hostReceiveTimeUs, type, label, severity });
}

void WaveformController::appendEvent(const WaveformEvent& event) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, event] { appendEvent(event); }, Qt::QueuedConnection);
        return;
    }
    m_buffer.appendEvent(event);
    scheduleUpdate();
}

void WaveformController::setChannelVisible(const QString& key, bool visible) {
    const int index = channelIndex(key);
    if (index < 0 || m_channels[index].visible == visible) return;
    m_channels[index].visible = visible;
    m_buffer.updateChannel(index, m_channels[index]);
    m_channelModel.channelChanged(index, { WaveformChannelModel::VisibleRole });
    fitAutomaticRange(m_channels[index].groupKey);
    emit groupsChanged();
    updateCursorValues();
    emit renderNeeded();
}

void WaveformController::setChannelLabel(const QString& key, const QString& label) {
    const int index = channelIndex(key);
    if (index < 0) return;
    const QString clean = label.trimmed().isEmpty() ? key : label.trimmed();
    if (m_channels[index].label == clean) return;
    m_channels[index].label = clean;
    m_buffer.updateChannel(index, m_channels[index]);
    m_channelModel.channelChanged(index, { WaveformChannelModel::LabelRole });
    updateCursorValues();
    emit renderNeeded();
}

void WaveformController::setChannelColor(const QString& key, const QString& color) {
    const int index = channelIndex(key);
    const QColor next(color);
    if (index < 0 || !next.isValid() || m_channels[index].color == next) return;
    m_channels[index].color = next;
    m_buffer.updateChannel(index, m_channels[index]);
    m_channelModel.channelChanged(index, { WaveformChannelModel::ColorRole });
    updateCursorValues();
    emit renderNeeded();
}

void WaveformController::setChannelGroup(const QString& key, const QString& groupKey) {
    const int index = channelIndex(key);
    if (index < 0) return;
    const QString normalized = normalizedGroupKey(groupKey, m_channels[index].unit);
    if (m_channels[index].groupKey == normalized) return;
    const QString previousGroup = m_channels[index].groupKey;
    m_channels[index].groupKey = normalized;
    m_buffer.updateChannel(index, m_channels[index]);
    m_channelModel.channelChanged(index, { WaveformChannelModel::GroupRole });
    rebuildGroups();
    fitAutomaticRange(previousGroup);
    fitAutomaticRange(normalized);
    emit groupsChanged();
    emit renderNeeded();
}

void WaveformController::setGroupAutoRange(const QString& groupKey, bool automatic) {
    GroupScale& scale = m_groupScales[groupKey];
    if (scale.automatic == automatic) return;
    scale.automatic = automatic;
    if (automatic) {
        fitAutomaticRange(groupKey);
    } else {
        scale.initialized = true;
    }
    emit groupsChanged();
    emit renderNeeded();
}

void WaveformController::setGroupRange(const QString& groupKey, double minimum, double maximum) {
    if (!qIsFinite(minimum) || !qIsFinite(maximum) || maximum <= minimum) return;
    GroupScale& scale = m_groupScales[groupKey];
    scale.automatic = false;
    scale.initialized = true;
    scale.minimum = minimum;
    scale.maximum = maximum;
    emit groupsChanged();
    emit renderNeeded();
}

void WaveformController::resetGroupRange(const QString& groupKey) {
    GroupScale& scale = m_groupScales[groupKey];
    scale.automatic = true;
    scale.initialized = false;
    fitAutomaticRange(groupKey);
    emit groupsChanged();
    emit renderNeeded();
}

void WaveformController::zoomAt(double normalizedPosition, double factor) {
    if (!m_viewInitialized || !qIsFinite(factor) || factor <= 0.0) return;
    normalizedPosition = qBound(0.0, normalizedPosition, 1.0);
    const qint64 oldSpan = qMax<qint64>(1000, m_viewEndUs - m_viewStartUs);
    const qint64 newSpan = qBound<qint64>(1000, qRound64(oldSpan / factor), 3600000000LL);
    const qint64 anchor = m_viewStartUs + qRound64(oldSpan * normalizedPosition);
    m_viewStartUs = anchor - qRound64(newSpan * normalizedPosition);
    m_viewEndUs = m_viewStartUs + newSpan;
    m_windowSeconds = newSpan / 1000000.0;
    m_follow = false;
    clampViewport();
    updateCursorValues();
    emit followChanged();
    emit viewportChanged();
    emit renderNeeded();
}

void WaveformController::zoomGroupAt(
    const QString& groupKey,
    double normalizedPosition,
    double factor) {
    if (!qIsFinite(factor) || factor <= 0.0) return;
    auto scaleIt = m_groupScales.find(groupKey);
    if (scaleIt == m_groupScales.end()) return;
    if (!scaleIt->initialized) fitAutomaticRange(groupKey);

    GroupScale& scale = scaleIt.value();
    if (!scale.initialized) return;
    normalizedPosition = qBound(0.0, normalizedPosition, 1.0);
    const double oldSpan = scale.maximum - scale.minimum;
    if (!qIsFinite(oldSpan) || oldSpan <= 0.0) return;

    const double boundedFactor = qBound(0.01, factor, 100.0);
    const double minimumSpan = qMax(1e-12, qMax(qAbs(scale.minimum), qAbs(scale.maximum)) * 1e-12);
    const double newSpan = qMax(minimumSpan, oldSpan / boundedFactor);
    const double anchor = scale.maximum - oldSpan * normalizedPosition;
    const double maximum = anchor + newSpan * normalizedPosition;
    const double minimum = maximum - newSpan;
    if (!qIsFinite(minimum) || !qIsFinite(maximum) || maximum <= minimum) return;

    scale.automatic = false;
    scale.initialized = true;
    scale.minimum = minimum;
    scale.maximum = maximum;
    emit groupsChanged();
    emit renderNeeded();
}

void WaveformController::panBy(double normalizedDistance) {
    if (!m_viewInitialized || !qIsFinite(normalizedDistance)) return;
    const qint64 span = m_viewEndUs - m_viewStartUs;
    const qint64 distance = qRound64(span * normalizedDistance);
    m_viewStartUs += distance;
    m_viewEndUs += distance;
    m_follow = false;
    clampViewport();
    updateCursorValues();
    emit followChanged();
    emit viewportChanged();
    emit renderNeeded();
}

void WaveformController::panGroupBy(const QString& groupKey, double normalizedDistance) {
    if (!qIsFinite(normalizedDistance)) return;
    auto scaleIt = m_groupScales.find(groupKey);
    if (scaleIt == m_groupScales.end()) return;
    if (!scaleIt->initialized) fitAutomaticRange(groupKey);

    GroupScale& scale = scaleIt.value();
    const double span = scale.maximum - scale.minimum;
    if (!scale.initialized || !qIsFinite(span) || span <= 0.0) return;
    const double distance = span * normalizedDistance;
    if (!qIsFinite(distance)) return;
    scale.automatic = false;
    scale.minimum += distance;
    scale.maximum += distance;
    emit groupsChanged();
    emit renderNeeded();
}

QVariantList WaveformController::timeTicks(int maxTicks) const {
    QVariantList result;
    const double start = static_cast<double>(m_viewStartUs);
    const double end = static_cast<double>(m_viewEndUs);
    const double span = end - start;
    if (!m_viewInitialized || span <= 0.0) return result;
    for (double tick : numericTicks(start, end, maxTicks)) {
        QVariantMap item;
        item.insert(QStringLiteral("timeUs"), qRound64(tick));
        item.insert(QStringLiteral("position"), (tick - start) / span);
        result.append(item);
    }
    return result;
}

QVariantList WaveformController::valueTicks(double minimum, double maximum, int maxTicks) const {
    QVariantList result;
    const double span = maximum - minimum;
    if (!qIsFinite(span) || span <= 0.0) return result;
    for (double tick : numericTicks(minimum, maximum, maxTicks)) {
        QVariantMap item;
        item.insert(QStringLiteral("value"), tick);
        item.insert(QStringLiteral("position"), (maximum - tick) / span);
        item.insert(QStringLiteral("label"), QString::number(tick, 'g', 6));
        result.append(item);
    }
    return result;
}

QVector<double> WaveformController::numericTicks(double minimum, double maximum, int maxTicks) {
    QVector<double> result;
    if (!qIsFinite(minimum) || !qIsFinite(maximum) || maximum <= minimum) return result;
    maxTicks = qBound(2, maxTicks, 20);
    const double rawStep = (maximum - minimum) / static_cast<double>(maxTicks - 1);
    if (!qIsFinite(rawStep) || rawStep <= 0.0) return result;

    const double magnitude = qPow(10.0, qFloor(qLn(rawStep) / qLn(10.0)));
    const double fraction = rawStep / magnitude;
    const double niceFraction = fraction < 1.5 ? 1.0
        : fraction < 3.0 ? 2.0
        : fraction < 7.0 ? 5.0
        : 10.0;
    const double step = niceFraction * magnitude;
    if (!qIsFinite(step) || step <= 0.0) return result;

    const double epsilon = step * 1e-9;
    double tick = qCeil((minimum - epsilon) / step) * step;
    for (int guard = 0; guard < maxTicks + 2 && tick <= maximum + epsilon; ++guard, tick += step) {
        if (tick < minimum - epsilon) continue;
        result.append(qAbs(tick) < epsilon ? 0.0 : tick);
    }
    return result;
}

void WaveformController::setCursorPosition(double normalizedPosition) {
    if (!m_viewInitialized) return;
    m_cursorVisible = true;
    m_cursorPosition = qBound(0.0, normalizedPosition, 1.0);
    m_cursorTimeUs = m_viewStartUs + qRound64((m_viewEndUs - m_viewStartUs) * m_cursorPosition);
    updateCursorValues();
    emit cursorChanged();
    emit renderNeeded();
}

void WaveformController::setCursorAt(
    double normalizedX,
    const QString& groupKey,
    double normalizedY,
    double pixelWidth,
    double pixelHeight) {
    if (!m_viewInitialized) return;
    normalizedX = qBound(0.0, normalizedX, 1.0);
    normalizedY = qBound(0.0, normalizedY, 1.0);
    pixelWidth = qMax(1.0, pixelWidth);
    pixelHeight = qMax(1.0, pixelHeight);
    const qint64 requestedTime = m_viewStartUs
        + qRound64((m_viewEndUs - m_viewStartUs) * normalizedX);
    const auto scaleIt = m_groupScales.constFind(groupKey);
    if (scaleIt == m_groupScales.cend() || !scaleIt->initialized) {
        setCursorPosition(normalizedX);
        return;
    }

    const double minimum = scaleIt->minimum;
    const double maximum = scaleIt->maximum;
    const double requestedValue = maximum - normalizedY * (maximum - minimum);
    const QVector<WaveformCursorSample> samples = m_buffer.nearby(m_timeAxis, requestedTime, 5);
    if (samples.isEmpty()) return;

    struct Candidate {
        WaveformCursorSample sample;
        QVector<double> values;
        QBitArray valid;
        double value = 0.0;
        double distance = std::numeric_limits<double>::max();
        bool crossing = false;
    } bestPoint, bestCrossing;

    const double timeSpan = qMax<qint64>(1, m_viewEndUs - m_viewStartUs);
    const double valueSpan = qMax(1e-12, maximum - minimum);
    auto consider = [&](const WaveformCursorSample& sample, int channelIndex,
                              double value, bool crossing, const QVector<double>* values,
                              const QBitArray* valid) {
        if (channelIndex < 0 || channelIndex >= m_channels.size()
            || m_channels[channelIndex].groupKey != groupKey
            || !m_channels[channelIndex].visible) return;
        const double dx = qAbs(sample.timeUs - requestedTime) / timeSpan * pixelWidth;
        const double dy = qAbs(value - requestedValue) / valueSpan * pixelHeight;
        const double distance = qSqrt(dx * dx + dy * dy);
        Candidate& best = crossing ? bestCrossing : bestPoint;
        if (distance >= best.distance) return;
        best.sample = sample;
        best.values = values ? *values : sample.values;
        best.valid = valid ? *valid : sample.valueValid;
        best.value = value;
        best.distance = distance;
        best.crossing = crossing;
    };

    for (const auto& sample : samples) {
        for (int channel = 0; channel < m_channels.size() && channel < sample.values.size(); ++channel) {
            if (sample.valueValid.testBit(channel))
                consider(sample, channel, sample.values[channel], false, nullptr, nullptr);
        }
    }

    for (int sampleIndex = 1; sampleIndex < samples.size(); ++sampleIndex) {
        const auto& left = samples[sampleIndex - 1];
        const auto& right = samples[sampleIndex];
        if (right.timeUs <= left.timeUs) continue;
        for (int first = 0; first < m_channels.size(); ++first) {
            if (!left.valueValid.testBit(first) || !right.valueValid.testBit(first)
                || !m_channels[first].visible || m_channels[first].groupKey != groupKey) continue;
            for (int second = first + 1; second < m_channels.size(); ++second) {
                if (!left.valueValid.testBit(second) || !right.valueValid.testBit(second)
                    || !m_channels[second].visible || m_channels[second].groupKey != groupKey) continue;
                const double deltaLeft = left.values[first] - left.values[second];
                const double deltaRight = right.values[first] - right.values[second];
                if (deltaLeft * deltaRight > 0.0 || qFuzzyCompare(deltaLeft, deltaRight)) continue;
                const double ratio = qBound(0.0, deltaLeft / (deltaLeft - deltaRight), 1.0);
                WaveformCursorSample crossing = left;
                crossing.timeUs = left.timeUs + qRound64((right.timeUs - left.timeUs) * ratio);
                crossing.values.resize(m_channels.size());
                crossing.valueValid.resize(m_channels.size());
                for (int channel = 0; channel < m_channels.size(); ++channel) {
                    if (!left.valueValid.testBit(channel) || !right.valueValid.testBit(channel)) continue;
                    crossing.valueValid.setBit(channel, true);
                    crossing.values[channel] = left.values[channel]
                        + (right.values[channel] - left.values[channel]) * ratio;
                }
                consider(crossing, first, crossing.values[first], true,
                         &crossing.values, &crossing.valueValid);
            }
        }
    }

    constexpr double kSnapRadiusPixels = 14.0;
    const Candidate& best = bestCrossing.sample.valid && bestCrossing.distance <= kSnapRadiusPixels
        ? bestCrossing : bestPoint;
    if (!best.sample.valid) {
        setCursorPosition(normalizedX);
        m_cursorGroupKey.clear();
        m_cursorHasPoint = false;
        m_cursorSnapped = false;
        m_cursorSnapLabel.clear();
        return;
    }

    m_cursorVisible = true;
    m_cursorTimeUs = best.sample.timeUs;
    m_cursorPosition = qBound(0.0, static_cast<double>(m_cursorTimeUs - m_viewStartUs)
                                      / static_cast<double>(timeSpan), 1.0);
    m_cursorGroupKey = groupKey;
    m_cursorValue = best.value;
    m_cursorYPosition = qBound(0.0, (maximum - best.value) / valueSpan, 1.0);
    m_cursorHasPoint = true;
    m_cursorSnapped = best.crossing;
    m_cursorSnapLabel = best.crossing ? QStringLiteral("已吸附交叉点") : QString();
    setCursorSample(best.sample, &best.values, &best.valid);
    emit renderNeeded();
}

void WaveformController::clearCursor() {
    if (!m_cursorVisible) return;
    m_cursorVisible = false;
    m_cursorValues.clear();
    m_cursorTimeLabel.clear();
    m_cursorHasPoint = false;
    m_cursorSnapped = false;
    m_cursorSnapLabel.clear();
    emit cursorChanged();
    emit renderNeeded();
}

void WaveformController::clear() {
    m_buffer.clear();
    for (auto scaleIt = m_groupScales.begin(); scaleIt != m_groupScales.end(); ++scaleIt) {
        if (!scaleIt->automatic) continue;
        scaleIt->initialized = false;
        scaleIt->minimum = -1.0;
        scaleIt->maximum = 1.0;
    }
    m_viewInitialized = false;
    m_viewStartUs = 0;
    m_viewEndUs = qRound64(m_windowSeconds * 1000000.0);
    m_haveAlgorithmState = false;
    clearCursor();
    emit frameStatsChanged();
    emit groupsChanged();
    emit viewportChanged();
    emit renderNeeded();
}

void WaveformController::resetChannels() {
    m_buffer.resetChannels();
    m_channels.clear();
    m_latestValues.clear();
    m_latestValueValid.clear();
    m_groupKeys.clear();
    m_groupScales.clear();
    m_channelModel.reset();
    m_viewInitialized = false;
    clearCursor();
    emit groupsChanged();
    emit frameStatsChanged();
    emit viewportChanged();
    emit renderNeeded();
}

WaveformRenderSnapshot WaveformController::renderSnapshot(int pixelWidth) const {
    WaveformRenderSnapshot snapshot;
    snapshot.startUs = m_viewStartUs;
    snapshot.endUs = m_viewEndUs;
    snapshot.timeAxis = m_timeAxis;
    snapshot.cursorVisible = m_cursorVisible;
    snapshot.cursorTimeUs = m_cursorTimeUs;
    snapshot.cursorGroupKey = m_cursorGroupKey;
    snapshot.cursorValue = m_cursorValue;
    snapshot.cursorHasPoint = m_cursorHasPoint;
    snapshot.cursorSnapped = m_cursorSnapped;

    QVector<int> visibleIndices;
    for (int index = 0; index < m_channels.size(); ++index) {
        if (m_channels[index].visible) visibleIndices.append(index);
    }
    const WaveformQueryResult query = m_buffer.query(
        m_timeAxis,
        m_viewStartUs,
        m_viewEndUs,
        pixelWidth,
        visibleIndices,
        m_includeDisabledSamples);
    snapshot.disabledSpans = query.disabledSpans;
    snapshot.events = query.events;

    QHash<int, QVector<WaveformEnvelope>> envelopesByChannel;
    for (const auto& series : query.series) envelopesByChannel.insert(series.channelIndex, series.envelopes);

    for (const QString& groupKey : m_groupKeys) {
        WaveformRenderGroup group;
        group.key = groupKey;
        group.label = groupKey;
        bool haveRange = false;
        bool haveUnit = false;
        bool mixedUnits = false;
        for (int index = 0; index < m_channels.size(); ++index) {
            const WaveformChannel& channel = m_channels[index];
            if (!channel.visible || channel.groupKey != groupKey) continue;
            if (!haveUnit) {
                group.unit = channel.unit;
                haveUnit = true;
            } else if (group.unit != channel.unit) {
                mixedUnits = true;
            }
            WaveformRenderSeries series;
            series.channel = channel;
            series.envelopes = envelopesByChannel.value(index);
            for (const auto& envelope : series.envelopes) {
                if (!haveRange) {
                    group.minimum = envelope.minimum;
                    group.maximum = envelope.maximum;
                    haveRange = true;
                } else {
                    group.minimum = qMin(group.minimum, envelope.minimum);
                    group.maximum = qMax(group.maximum, envelope.maximum);
                }
            }
            group.series.append(std::move(series));
        }
        if (group.series.isEmpty()) continue;
        if (mixedUnits) group.unit.clear();

        const GroupScale scale = m_groupScales.value(groupKey);
        if (!scale.automatic || scale.initialized) {
            group.minimum = scale.minimum;
            group.maximum = scale.maximum;
        } else if (!haveRange) {
            group.minimum = -1.0;
            group.maximum = 1.0;
        } else if (qFuzzyCompare(group.minimum, group.maximum)) {
            const double amount = qMax(1.0, qAbs(group.minimum) * 0.1);
            group.minimum -= amount;
            group.maximum += amount;
        } else {
            const double padding = (group.maximum - group.minimum) * 0.08;
            group.minimum -= padding;
            group.maximum += padding;
        }
        snapshot.groups.append(std::move(group));
    }
    return snapshot;
}

int WaveformController::channelIndex(const QString& key) const {
    for (int index = 0; index < m_channels.size(); ++index) {
        if (m_channels[index].key == key) return index;
    }
    return -1;
}

QString WaveformController::normalizedGroupKey(const QString& requested, const QString& unit) const {
    const QString clean = requested.trimmed();
    if (!clean.isEmpty()) return clean;
    return unit.trimmed().isEmpty() ? QStringLiteral("No unit") : unit.trimmed();
}

QColor WaveformController::defaultColor(int index) const {
    static const QColor colors[] = {
        QColor(QStringLiteral("#0969da")),
        QColor(QStringLiteral("#cf222e")),
        QColor(QStringLiteral("#1a7f37")),
        QColor(QStringLiteral("#8250df")),
        QColor(QStringLiteral("#bf8700")),
        QColor(QStringLiteral("#00838f")),
        QColor(QStringLiteral("#d15704")),
        QColor(QStringLiteral("#57606a")),
    };
    return colors[index % (sizeof(colors) / sizeof(colors[0]))];
}

void WaveformController::rebuildGroups() {
    QStringList groups;
    for (const auto& channel : m_channels) {
        if (!groups.contains(channel.groupKey)) groups.append(channel.groupKey);
        if (!m_groupScales.contains(channel.groupKey)) m_groupScales.insert(channel.groupKey, {});
    }
    if (groups == m_groupKeys) {
        emit groupsChanged();
        return;
    }
    m_groupKeys = groups;
    emit groupsChanged();
}

bool WaveformController::expandAutomaticRange(const QString& groupKey, double value) {
    if (!qIsFinite(value)) return false;
    GroupScale& scale = m_groupScales[groupKey];
    if (!scale.automatic) return false;
    if (!scale.initialized) return setPaddedRange(scale, value, value);

    const double currentSpan = qMax(1e-12, scale.maximum - scale.minimum);
    const double padding = currentSpan * 0.08;
    double minimum = scale.minimum;
    double maximum = scale.maximum;
    if (value < minimum) minimum = value - padding;
    if (value > maximum) maximum = value + padding;
    if (minimum == scale.minimum && maximum == scale.maximum) return false;
    scale.minimum = minimum;
    scale.maximum = maximum;
    return true;
}

void WaveformController::fitAutomaticRange(const QString& groupKey) {
    auto scaleIt = m_groupScales.find(groupKey);
    if (scaleIt == m_groupScales.end() || !scaleIt->automatic) return;

    QVector<int> visibleIndices;
    for (int index = 0; index < m_channels.size(); ++index) {
        if (m_channels[index].visible && m_channels[index].groupKey == groupKey) {
            visibleIndices.append(index);
        }
    }

    GroupScale& scale = scaleIt.value();
    scale.initialized = false;
    scale.minimum = -1.0;
    scale.maximum = 1.0;
    if (visibleIndices.isEmpty() || m_buffer.size() == 0) return;

    const qint64 startUs = m_buffer.earliestTimeUs(m_timeAxis);
    const qint64 endUs = qMax(startUs + 1, m_buffer.latestTimeUs(m_timeAxis));
    const WaveformQueryResult query = m_buffer.query(
        m_timeAxis,
        startUs,
        endUs,
        1,
        visibleIndices,
        m_includeDisabledSamples);
    bool haveRange = false;
    double minimum = 0.0;
    double maximum = 0.0;
    for (const auto& series : query.series) {
        for (const auto& envelope : series.envelopes) {
            if (!haveRange) {
                minimum = envelope.minimum;
                maximum = envelope.maximum;
                haveRange = true;
            } else {
                minimum = qMin(minimum, envelope.minimum);
                maximum = qMax(maximum, envelope.maximum);
            }
        }
    }
    if (haveRange) setPaddedRange(scale, minimum, maximum);
}

void WaveformController::fitAllAutomaticRanges() {
    for (const QString& groupKey : std::as_const(m_groupKeys)) fitAutomaticRange(groupKey);
}

bool WaveformController::setPaddedRange(GroupScale& scale, double minimum, double maximum) {
    if (!qIsFinite(minimum) || !qIsFinite(maximum) || maximum < minimum) return false;
    double padding = 0.0;
    if (qFuzzyCompare(minimum, maximum)) {
        padding = qMax(1.0, qAbs(minimum) * 0.1);
    } else {
        padding = qMax(1e-12, (maximum - minimum) * 0.08);
    }
    const double paddedMinimum = minimum - padding;
    const double paddedMaximum = maximum + padding;
    const bool changed = !scale.initialized
        || !qFuzzyCompare(scale.minimum, paddedMinimum)
        || !qFuzzyCompare(scale.maximum, paddedMaximum);
    scale.initialized = true;
    scale.minimum = paddedMinimum;
    scale.maximum = paddedMaximum;
    return changed;
}

void WaveformController::updateViewportForLatest() {
    const qint64 latest = m_buffer.latestTimeUs(m_timeAxis);
    if (m_buffer.size() == 0) return;
    const qint64 span = qMax<qint64>(1000, qRound64(m_windowSeconds * 1000000.0));
    m_viewEndUs = latest;
    m_viewStartUs = latest - span;
    m_viewInitialized = true;
}

void WaveformController::clampViewport() {
    if (m_buffer.size() == 0) return;
    const qint64 earliest = m_buffer.earliestTimeUs(m_timeAxis);
    const qint64 latest = m_buffer.latestTimeUs(m_timeAxis);
    const qint64 span = m_viewEndUs - m_viewStartUs;
    if (span >= latest - earliest) {
        m_viewStartUs = earliest;
        m_viewEndUs = earliest + span;
        return;
    }
    if (m_viewStartUs < earliest) {
        m_viewStartUs = earliest;
        m_viewEndUs = earliest + span;
    }
    if (m_viewEndUs > latest) {
        m_viewEndUs = latest;
        m_viewStartUs = latest - span;
    }
}

void WaveformController::scheduleUpdate() {
    m_pendingUpdate = true;
}

void WaveformController::updateCursorValues() {
    if (!m_cursorVisible || !m_viewInitialized) return;
    m_cursorTimeUs = m_viewStartUs + qRound64((m_viewEndUs - m_viewStartUs) * m_cursorPosition);
    const WaveformCursorSample sample = m_buffer.nearest(m_timeAxis, m_cursorTimeUs);
    m_cursorValues.clear();
    if (!sample.valid) {
        m_cursorTimeLabel.clear();
        emit cursorChanged();
        return;
    }

    m_cursorGroupKey.clear();
    m_cursorHasPoint = false;
    m_cursorSnapped = false;
    m_cursorSnapLabel.clear();
    setCursorSample(sample);
}

void WaveformController::setCursorSample(
    const WaveformCursorSample& sample,
    const QVector<double>* suppliedValues,
    const QBitArray* suppliedValid) {
    const QVector<double>& values = suppliedValues ? *suppliedValues : sample.values;
    const QBitArray& valid = suppliedValid ? *suppliedValid : sample.valueValid;
    m_cursorTimeUs = sample.timeUs;
    m_cursorTimeLabel = QStringLiteral("%1 %2 s · seq %3 · algo %4%5")
        .arg(m_timeAxis == WaveformTimeAxis::Device ? QStringLiteral("Device") : QStringLiteral("Host"))
        .arg(sample.timeUs / 1000000.0, 0, 'f', 6)
        .arg(sample.sequence)
        .arg(sample.algorithmId)
        .arg(sample.algorithmEnabled ? QString() : QStringLiteral(" · disabled"));
    m_cursorValues.clear();
    for (int index = 0; index < m_channels.size() && index < values.size(); ++index) {
        const auto& channel = m_channels[index];
        if (!channel.visible || !valid.testBit(index)) continue;
        QVariantMap value;
        value.insert(QStringLiteral("key"), channel.key);
        value.insert(QStringLiteral("label"), channel.label);
        value.insert(QStringLiteral("unit"), channel.unit);
        value.insert(QStringLiteral("group"), channel.groupKey);
        value.insert(QStringLiteral("color"), channel.color);
        value.insert(QStringLiteral("value"), values[index]);
        m_cursorValues.append(value);
    }
    emit cursorChanged();
}

qint64 WaveformController::eventTime(const WaveformEvent& event) const {
    return m_timeAxis == WaveformTimeAxis::Device ? event.deviceTimeUs : event.hostReceiveTimeUs;
}

} // namespace akrion::gui
