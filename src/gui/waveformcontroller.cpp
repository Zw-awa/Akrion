#include "waveformcontroller.h"

#include <QVariantMap>
#include <QThread>
#include <QtMath>

#include <algorithm>

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
        if (!m_pendingUpdate) return;
        m_pendingUpdate = false;
        emit frameStatsChanged();
        if (!m_paused) {
            if (m_cursorVisible) updateCursorValues();
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
    emit includeDisabledSamplesChanged();
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
    emit groupsChanged();
    updateCursorValues();
    emit renderNeeded();
}

void WaveformController::setChannelGroup(const QString& key, const QString& groupKey) {
    const int index = channelIndex(key);
    if (index < 0) return;
    const QString normalized = normalizedGroupKey(groupKey, m_channels[index].unit);
    if (m_channels[index].groupKey == normalized) return;
    m_channels[index].groupKey = normalized;
    m_buffer.updateChannel(index, m_channels[index]);
    m_channelModel.channelChanged(index, { WaveformChannelModel::GroupRole });
    rebuildGroups();
    emit renderNeeded();
}

void WaveformController::setGroupAutoRange(const QString& groupKey, bool automatic) {
    GroupScale& scale = m_groupScales[groupKey];
    if (scale.automatic == automatic) return;
    scale.automatic = automatic;
    emit groupsChanged();
    emit renderNeeded();
}

void WaveformController::setGroupRange(const QString& groupKey, double minimum, double maximum) {
    if (!qIsFinite(minimum) || !qIsFinite(maximum) || maximum <= minimum) return;
    GroupScale& scale = m_groupScales[groupKey];
    scale.automatic = false;
    scale.minimum = minimum;
    scale.maximum = maximum;
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

void WaveformController::setCursorPosition(double normalizedPosition) {
    if (!m_viewInitialized) return;
    m_cursorVisible = true;
    m_cursorPosition = qBound(0.0, normalizedPosition, 1.0);
    m_cursorTimeUs = m_viewStartUs + qRound64((m_viewEndUs - m_viewStartUs) * m_cursorPosition);
    updateCursorValues();
    emit cursorChanged();
    emit renderNeeded();
}

void WaveformController::clearCursor() {
    if (!m_cursorVisible) return;
    m_cursorVisible = false;
    m_cursorValues.clear();
    m_cursorTimeLabel.clear();
    emit cursorChanged();
    emit renderNeeded();
}

void WaveformController::clear() {
    m_buffer.clear();
    m_viewInitialized = false;
    m_viewStartUs = 0;
    m_viewEndUs = qRound64(m_windowSeconds * 1000000.0);
    m_haveAlgorithmState = false;
    clearCursor();
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
        if (!scale.automatic) {
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

    m_cursorTimeUs = sample.timeUs;
    m_cursorTimeLabel = QStringLiteral("%1 %2 s · seq %3 · algo %4%5")
        .arg(m_timeAxis == WaveformTimeAxis::Device ? QStringLiteral("Device") : QStringLiteral("Host"))
        .arg(sample.timeUs / 1000000.0, 0, 'f', 6)
        .arg(sample.sequence)
        .arg(sample.algorithmId)
        .arg(sample.algorithmEnabled ? QString() : QStringLiteral(" · disabled"));
    for (int index = 0; index < m_channels.size() && index < sample.values.size(); ++index) {
        const auto& channel = m_channels[index];
        if (!channel.visible || !sample.valueValid.testBit(index)) continue;
        QVariantMap value;
        value.insert(QStringLiteral("key"), channel.key);
        value.insert(QStringLiteral("label"), channel.label);
        value.insert(QStringLiteral("unit"), channel.unit);
        value.insert(QStringLiteral("group"), channel.groupKey);
        value.insert(QStringLiteral("color"), channel.color);
        value.insert(QStringLiteral("value"), sample.values[index]);
        m_cursorValues.append(value);
    }
    emit cursorChanged();
}

qint64 WaveformController::eventTime(const WaveformEvent& event) const {
    return m_timeAxis == WaveformTimeAxis::Device ? event.deviceTimeUs : event.hostReceiveTimeUs;
}

} // namespace akrion::gui
