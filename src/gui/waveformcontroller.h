#pragma once

#include "waveformbuffer.h"

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantList>

namespace akrion::gui {

class WaveformController;

class WaveformChannelModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        LabelRole,
        UnitRole,
        ChannelRole,
        DescriptionRole,
        GroupRole,
        ColorRole,
        VisibleRole,
        LatestValueRole,
        HasValueRole,
    };

    explicit WaveformChannelModel(WaveformController* controller);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reset();
    void channelChanged(int row, const QVector<int>& roles);

private:
    WaveformController* m_controller = nullptr;
};

struct WaveformRenderSeries {
    WaveformChannel channel;
    QVector<WaveformEnvelope> envelopes;
};

struct WaveformRenderGroup {
    QString key;
    QString label;
    QString unit;
    double minimum = -1.0;
    double maximum = 1.0;
    QVector<WaveformRenderSeries> series;
};

struct WaveformRenderSnapshot {
    qint64 startUs = 0;
    qint64 endUs = 0;
    WaveformTimeAxis timeAxis = WaveformTimeAxis::Device;
    QVector<WaveformRenderGroup> groups;
    QVector<WaveformTimeSpan> disabledSpans;
    QVector<WaveformEvent> events;
    bool cursorVisible = false;
    qint64 cursorTimeUs = 0;
    QString cursorGroupKey;
    double cursorValue = 0.0;
    bool cursorHasPoint = false;
    bool cursorSnapped = false;
};

class WaveformController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* channelModel READ channelModel CONSTANT)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY groupsChanged)
    Q_PROPERTY(QStringList groupKeys READ groupKeys NOTIFY groupsChanged)
    Q_PROPERTY(QVariantList groupInfo READ groupInfo NOTIFY groupsChanged)
    Q_PROPERTY(double windowSeconds READ windowSeconds WRITE setWindowSeconds NOTIFY viewportChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(bool follow READ follow WRITE setFollow NOTIFY followChanged)
    Q_PROPERTY(QString timeAxis READ timeAxis WRITE setTimeAxis NOTIFY timeAxisChanged)
    Q_PROPERTY(bool includeDisabledSamples READ includeDisabledSamples WRITE setIncludeDisabledSamples NOTIFY includeDisabledSamplesChanged)
    Q_PROPERTY(qulonglong totalFrames READ totalFrames NOTIFY frameStatsChanged)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames NOTIFY frameStatsChanged)
    Q_PROPERTY(int bufferedFrames READ bufferedFrames NOTIFY frameStatsChanged)
    Q_PROPERTY(qint64 viewStartUs READ viewStartUs NOTIFY viewportChanged)
    Q_PROPERTY(qint64 viewEndUs READ viewEndUs NOTIFY viewportChanged)
    Q_PROPERTY(bool cursorVisible READ cursorVisible NOTIFY cursorChanged)
    Q_PROPERTY(double cursorPosition READ cursorPosition NOTIFY cursorChanged)
    Q_PROPERTY(QString cursorTimeLabel READ cursorTimeLabel NOTIFY cursorChanged)
    Q_PROPERTY(QVariantList cursorValues READ cursorValues NOTIFY cursorChanged)
    Q_PROPERTY(double cursorYPosition READ cursorYPosition NOTIFY cursorChanged)
    Q_PROPERTY(bool cursorSnapped READ cursorSnapped NOTIFY cursorChanged)
    Q_PROPERTY(QString cursorSnapLabel READ cursorSnapLabel NOTIFY cursorChanged)

public:
    explicit WaveformController(QObject* parent = nullptr);

    QAbstractItemModel* channelModel() { return &m_channelModel; }
    int channelCount() const { return m_channels.size(); }
    QStringList groupKeys() const { return m_groupKeys; }
    QVariantList groupInfo() const;
    double windowSeconds() const { return m_windowSeconds; }
    bool paused() const { return m_paused; }
    bool follow() const { return m_follow; }
    QString timeAxis() const;
    bool includeDisabledSamples() const { return m_includeDisabledSamples; }
    qulonglong totalFrames() const { return m_buffer.totalFrames(); }
    qulonglong droppedFrames() const { return m_buffer.droppedFrames(); }
    int bufferedFrames() const { return m_buffer.size(); }
    qint64 viewStartUs() const { return m_viewStartUs; }
    qint64 viewEndUs() const { return m_viewEndUs; }
    bool cursorVisible() const { return m_cursorVisible; }
    double cursorPosition() const { return m_cursorPosition; }
    QString cursorTimeLabel() const { return m_cursorTimeLabel; }
    QVariantList cursorValues() const { return m_cursorValues; }
    double cursorYPosition() const { return m_cursorYPosition; }
    bool cursorSnapped() const { return m_cursorSnapped; }
    QString cursorSnapLabel() const { return m_cursorSnapLabel; }

    void setWindowSeconds(double seconds);
    void setPaused(bool paused);
    void setFollow(bool follow);
    void setTimeAxis(const QString& axis);
    void setIncludeDisabledSamples(bool include);

    void appendFrame(const WaveformFrame& frame);
    void appendEvent(const WaveformEvent& event);
    WaveformRenderSnapshot renderSnapshot(int pixelWidth) const;
    const QVector<WaveformChannel>& channelDefinitions() const { return m_channels; }
    bool hasChannel(const QString& key) const { return channelIndex(key) >= 0; }

    Q_INVOKABLE int defineChannel(
        const QString& key,
        const QString& label,
        const QString& unit,
        const QString& role,
        const QString& description,
        const QString& groupKey = QString(),
        const QString& color = QString());
    Q_INVOKABLE void appendFrameValues(
        qint64 deviceTimeUs,
        qint64 hostReceiveTimeUs,
        qulonglong sequence,
        int algorithmId,
        bool algorithmEnabled,
        const QStringList& valueKeys,
        const QVariantList& values);
    Q_INVOKABLE void addEvent(
        qint64 deviceTimeUs,
        qint64 hostReceiveTimeUs,
        const QString& type,
        const QString& label,
        int severity = 0);
    Q_INVOKABLE void setChannelVisible(const QString& key, bool visible);
    Q_INVOKABLE void setChannelLabel(const QString& key, const QString& label);
    Q_INVOKABLE void setChannelColor(const QString& key, const QString& color);
    Q_INVOKABLE void setChannelGroup(const QString& key, const QString& groupKey);
    Q_INVOKABLE void setGroupAutoRange(const QString& groupKey, bool automatic);
    Q_INVOKABLE void setGroupRange(const QString& groupKey, double minimum, double maximum);
    Q_INVOKABLE void resetGroupRange(const QString& groupKey);
    Q_INVOKABLE void zoomAt(double normalizedPosition, double factor);
    Q_INVOKABLE void zoomGroupAt(const QString& groupKey, double normalizedPosition, double factor);
    Q_INVOKABLE void panBy(double normalizedDistance);
    Q_INVOKABLE void panGroupBy(const QString& groupKey, double normalizedDistance);
    Q_INVOKABLE QVariantList timeTicks(int maxTicks = 7) const;
    Q_INVOKABLE QVariantList valueTicks(double minimum, double maximum, int maxTicks = 6) const;
    Q_INVOKABLE void setCursorPosition(double normalizedPosition);
    Q_INVOKABLE void setCursorAt(double normalizedX, const QString& groupKey,
                                 double normalizedY, double pixelWidth, double pixelHeight);
    Q_INVOKABLE void clearCursor();
    Q_INVOKABLE void clear();
    void resetChannels();

    static QVector<double> numericTicks(double minimum, double maximum, int maxTicks);

signals:
    void groupsChanged();
    void viewportChanged();
    void pausedChanged();
    void followChanged();
    void timeAxisChanged();
    void includeDisabledSamplesChanged();
    void frameStatsChanged();
    void cursorChanged();
    void renderNeeded();

private:
    friend class WaveformChannelModel;

    struct GroupScale {
        bool automatic = true;
        bool initialized = false;
        double minimum = -1.0;
        double maximum = 1.0;
    };

    int channelIndex(const QString& key) const;
    QString normalizedGroupKey(const QString& requested, const QString& unit) const;
    QColor defaultColor(int index) const;
    void rebuildGroups();
    bool expandAutomaticRange(const QString& groupKey, double value);
    void fitAutomaticRange(const QString& groupKey);
    void fitAllAutomaticRanges();
    static bool setPaddedRange(GroupScale& scale, double minimum, double maximum);
    void updateViewportForLatest();
    void clampViewport();
    void scheduleUpdate();
    void updateCursorValues();
    void setCursorSample(const WaveformCursorSample& sample, const QVector<double>* values = nullptr,
                         const QBitArray* valid = nullptr);
    qint64 eventTime(const WaveformEvent& event) const;

    WaveformBuffer m_buffer;
    QVector<WaveformChannel> m_channels;
    WaveformChannelModel m_channelModel;
    QStringList m_groupKeys;
    QHash<QString, GroupScale> m_groupScales;
    QTimer m_updateTimer;
    WaveformTimeAxis m_timeAxis = WaveformTimeAxis::Device;
    double m_windowSeconds = 10.0;
    qint64 m_viewStartUs = 0;
    qint64 m_viewEndUs = 10000000;
    bool m_viewInitialized = false;
    bool m_paused = false;
    bool m_follow = true;
    bool m_includeDisabledSamples = true;
    bool m_pendingUpdate = false;
    bool m_pendingGroupsChanged = false;
    bool m_cursorVisible = false;
    double m_cursorPosition = 0.0;
    qint64 m_cursorTimeUs = 0;
    QString m_cursorTimeLabel;
    QVariantList m_cursorValues;
    QVector<double> m_latestValues;
    QBitArray m_latestValueValid;
    double m_cursorYPosition = 0.5;
    QString m_cursorGroupKey;
    double m_cursorValue = 0.0;
    bool m_cursorHasPoint = false;
    bool m_cursorSnapped = false;
    QString m_cursorSnapLabel;
    bool m_haveAlgorithmState = false;
    qint32 m_lastAlgorithmId = 0;
    bool m_lastAlgorithmEnabled = true;
};

} // namespace akrion::gui
