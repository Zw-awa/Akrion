#pragma once

#include "waveformcontroller.h"

#include <QPointer>
#include <QQuickItem>

namespace akrion::gui {

class WaveformView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(WaveformController* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(qreal leftInset READ leftInset CONSTANT)
    Q_PROPERTY(qreal rightInset READ rightInset CONSTANT)
    Q_PROPERTY(qreal topInset READ topInset CONSTANT)
    Q_PROPERTY(qreal bottomInset READ bottomInset CONSTANT)

public:
    explicit WaveformView(QQuickItem* parent = nullptr);

    WaveformController* controller() const { return m_controller; }
    void setController(WaveformController* controller);

    qreal leftInset() const { return 76.0; }
    qreal rightInset() const { return 12.0; }
    qreal topInset() const { return 10.0; }
    qreal bottomInset() const { return 24.0; }

signals:
    void controllerChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class DragMode {
        None,
        Both,
        Vertical,
    };

    QRectF plotRect() const;
    double normalizedX(qreal x) const;
    QString groupAtY(qreal y, double* normalizedPosition, qreal* groupHeight = nullptr) const;

    QPointer<WaveformController> m_controller;
    QPointF m_lastDragPosition;
    QString m_dragGroupKey;
    qreal m_dragGroupHeight = 1.0;
    DragMode m_dragMode = DragMode::None;
};

} // namespace akrion::gui
