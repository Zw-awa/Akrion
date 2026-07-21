#include "waveformview.h"

#include "waveformcontroller.h"

#include <QHoverEvent>
#include <QMouseEvent>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QWheelEvent>
#include <QtMath>

namespace akrion::gui {
namespace {

QSGGeometryNode* lineNode(const QVector<QPointF>& points, const QColor& color, float width = 1.0f) {
    if (points.size() < 2) return nullptr;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), points.size());
    geometry->setDrawingMode(QSGGeometry::DrawLines);
    geometry->setLineWidth(width);
    auto* vertices = geometry->vertexDataAsPoint2D();
    for (int index = 0; index < points.size(); ++index) {
        vertices[index].set(static_cast<float>(points[index].x()), static_cast<float>(points[index].y()));
    }

    auto* material = new QSGFlatColorMaterial;
    material->setColor(color);
    auto* node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

QSGGeometryNode* rectangleNode(const QRectF& rect, const QColor& color) {
    if (rect.width() <= 0.0 || rect.height() <= 0.0) return nullptr;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 6);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    auto* vertices = geometry->vertexDataAsPoint2D();
    vertices[0].set(rect.left(), rect.top());
    vertices[1].set(rect.right(), rect.top());
    vertices[2].set(rect.left(), rect.bottom());
    vertices[3].set(rect.left(), rect.bottom());
    vertices[4].set(rect.right(), rect.top());
    vertices[5].set(rect.right(), rect.bottom());

    auto* material = new QSGFlatColorMaterial;
    material->setColor(color);
    auto* node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

void clearChildren(QSGNode* node) {
    while (QSGNode* child = node->firstChild()) {
        node->removeChildNode(child);
        delete child;
    }
}

} // namespace

WaveformView::WaveformView(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
}

void WaveformView::setController(WaveformController* controller) {
    if (m_controller == controller) return;
    if (m_controller) disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &WaveformController::renderNeeded, this, &WaveformView::update);
        connect(m_controller, &QObject::destroyed, this, [this] {
            m_controller = nullptr;
            update();
            emit controllerChanged();
        });
    }
    update();
    emit controllerChanged();
}

QSGNode* WaveformView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    clearChildren(root);
    if (!m_controller) return root;

    const QRectF plot = plotRect();
    if (plot.width() < 2.0 || plot.height() < 2.0) return root;
    const WaveformRenderSnapshot snapshot = m_controller->renderSnapshot(qMax(1, qRound(plot.width())));
    if (snapshot.endUs <= snapshot.startUs) return root;

    const auto xForTime = [&snapshot, &plot](qint64 timeUs) {
        const double ratio = static_cast<double>(timeUs - snapshot.startUs)
            / static_cast<double>(snapshot.endUs - snapshot.startUs);
        return plot.left() + qBound(0.0, ratio, 1.0) * plot.width();
    };

    for (const auto& span : snapshot.disabledSpans) {
        const QRectF band(
            xForTime(span.startUs),
            plot.top(),
            qMax(1.0, xForTime(span.endUs) - xForTime(span.startUs)),
            plot.height());
        if (auto* node = rectangleNode(band, QColor(QStringLiteral("#f0f1f3")))) root->appendChildNode(node);
    }

    QVector<QPointF> minorGridLines;
    QVector<QPointF> gridLines;
    const QVector<double> timeTicks = WaveformController::numericTicks(
        static_cast<double>(snapshot.startUs),
        static_cast<double>(snapshot.endUs),
        7);
    for (double tick : timeTicks) {
        const qreal x = xForTime(qRound64(tick));
        gridLines.append({ x, plot.top() });
        gridLines.append({ x, plot.bottom() });
    }
    for (int index = 1; index < timeTicks.size(); ++index) {
        const qreal x = xForTime(qRound64((timeTicks[index - 1] + timeTicks[index]) * 0.5));
        minorGridLines.append({ x, plot.top() });
        minorGridLines.append({ x, plot.bottom() });
    }

    const int groupCount = qMax(1, snapshot.groups.size());
    constexpr qreal groupGap = 8.0;
    const qreal groupHeight = (plot.height() - groupGap * (groupCount - 1)) / groupCount;
    for (int groupIndex = 0; groupIndex < snapshot.groups.size(); ++groupIndex) {
        const WaveformRenderGroup& group = snapshot.groups[groupIndex];
        const qreal top = plot.top() + groupIndex * (groupHeight + groupGap);
        const double valueSpan = qMax(1e-12, group.maximum - group.minimum);
        const QVector<double> valueTicks = WaveformController::numericTicks(group.minimum, group.maximum, 6);
        for (double tick : valueTicks) {
            const qreal y = top + (group.maximum - tick) / valueSpan * groupHeight;
            gridLines.append({ plot.left(), y });
            gridLines.append({ plot.right(), y });
        }
        for (int index = 1; index < valueTicks.size(); ++index) {
            const double tick = (valueTicks[index - 1] + valueTicks[index]) * 0.5;
            const qreal y = top + (group.maximum - tick) / valueSpan * groupHeight;
            minorGridLines.append({ plot.left(), y });
            minorGridLines.append({ plot.right(), y });
        }
    }
    if (auto* node = lineNode(minorGridLines, QColor(QStringLiteral("#f1f3f5")))) root->appendChildNode(node);
    if (auto* node = lineNode(gridLines, QColor(QStringLiteral("#e1e5ea")))) root->appendChildNode(node);

    for (int groupIndex = 0; groupIndex < snapshot.groups.size(); ++groupIndex) {
        const WaveformRenderGroup& group = snapshot.groups[groupIndex];
        const qreal top = plot.top() + groupIndex * (groupHeight + groupGap);
        const QRectF groupRect(plot.left(), top, plot.width(), groupHeight);
        const double valueSpan = qMax(1e-12, group.maximum - group.minimum);
        const auto yForValue = [&group, &groupRect, valueSpan](double value) {
            const double ratio = (value - group.minimum) / valueSpan;
            return groupRect.bottom() - qBound(0.0, ratio, 1.0) * groupRect.height();
        };

        for (const auto& series : group.series) {
            QVector<QPointF> segments;
            QPointF previous;
            bool havePrevious = false;
            segments.reserve(series.envelopes.size() * 4);
            for (const auto& envelope : series.envelopes) {
                const QPointF first(xForTime(envelope.firstTimeUs), yForValue(envelope.first));
                const QPointF last(xForTime(envelope.lastTimeUs), yForValue(envelope.last));
                if (havePrevious && !envelope.startsSegment) {
                    segments.append(previous);
                    segments.append(first);
                }
                const qreal bucketX = (first.x() + last.x()) * 0.5;
                const qreal minimumY = yForValue(envelope.minimum);
                const qreal maximumY = yForValue(envelope.maximum);
                if (qAbs(minimumY - maximumY) < 0.25) {
                    segments.append({ bucketX - 0.5, minimumY });
                    segments.append({ bucketX + 0.5, maximumY });
                } else {
                    segments.append({ bucketX, minimumY });
                    segments.append({ bucketX, maximumY });
                }
                previous = last;
                havePrevious = true;
            }
            if (auto* node = lineNode(segments, series.channel.color, 1.25f)) root->appendChildNode(node);
        }
    }

    for (const auto& event : snapshot.events) {
        const qint64 timeUs = snapshot.timeAxis == WaveformTimeAxis::Device
            ? event.deviceTimeUs
            : event.hostReceiveTimeUs;
        const qreal x = xForTime(timeUs);
        const QColor color = event.severity >= 2
            ? QColor(QStringLiteral("#cf222e"))
            : event.severity == 1 ? QColor(QStringLiteral("#bf8700")) : QColor(QStringLiteral("#57606a"));
        const QVector<QPointF> marker { { x, plot.top() }, { x, plot.bottom() } };
        if (auto* node = lineNode(marker, color, 1.0f)) root->appendChildNode(node);
    }

    if (snapshot.cursorVisible) {
        const qreal x = xForTime(snapshot.cursorTimeUs);
        const QVector<QPointF> cursor { { x, plot.top() }, { x, plot.bottom() } };
        if (auto* node = lineNode(cursor, QColor(QStringLiteral("#24292f")), 1.0f)) {
            root->appendChildNode(node);
        }
        if (snapshot.cursorHasPoint) {
            for (int groupIndex = 0; groupIndex < snapshot.groups.size(); ++groupIndex) {
                const auto& group = snapshot.groups[groupIndex];
                if (group.key != snapshot.cursorGroupKey) continue;
                const qreal top = plot.top() + groupIndex * (groupHeight + groupGap);
                const double span = qMax(1e-12, group.maximum - group.minimum);
                const qreal y = top + (group.maximum - snapshot.cursorValue) / span * groupHeight;
                const QVector<QPointF> horizontal { { plot.left(), y }, { plot.right(), y } };
                if (auto* node = lineNode(horizontal, QColor(QStringLiteral("#57606a")), 1.0f))
                    root->appendChildNode(node);
                const QColor markerColor = snapshot.cursorSnapped
                    ? QColor(QStringLiteral("#cf222e")) : QColor(QStringLiteral("#24292f"));
                if (auto* node = rectangleNode(QRectF(x - 3.5, y - 3.5, 7.0, 7.0), markerColor))
                    root->appendChildNode(node);
                break;
            }
        }
    }
    return root;
}

void WaveformView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !m_controller) {
        event->ignore();
        return;
    }
    const QRectF plot = plotRect();
    const bool onYAxis = event->position().y() >= plot.top()
        && event->position().y() <= plot.bottom()
        && event->position().x() >= 0.0
        && event->position().x() < plot.left();
    if (onYAxis) {
        m_dragMode = DragMode::Vertical;
    } else if (plot.contains(event->position())) {
        m_dragMode = DragMode::Both;
    } else {
        event->ignore();
        return;
    }

    m_dragGroupKey = groupAtY(event->position().y(), nullptr, &m_dragGroupHeight);
    if (m_dragMode == DragMode::Vertical && m_dragGroupKey.isEmpty()) {
        m_dragMode = DragMode::None;
        event->ignore();
        return;
    }
    m_lastDragPosition = event->position();
    event->accept();
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragMode == DragMode::None || !m_controller) {
        event->ignore();
        return;
    }
    const QPointF position = event->position();
    const QPointF delta = position - m_lastDragPosition;
    m_lastDragPosition = position;
    if ((m_dragMode == DragMode::Vertical || m_dragMode == DragMode::Both)
        && !m_dragGroupKey.isEmpty() && !qFuzzyIsNull(delta.y())) {
        m_controller->panGroupBy(m_dragGroupKey, delta.y() / qMax(1.0, m_dragGroupHeight));
    }
    if (m_dragMode == DragMode::Both && !qFuzzyIsNull(delta.x())) {
        m_controller->panBy(-delta.x() / qMax(1.0, plotRect().width()));
    }
    event->accept();
}

void WaveformView::mouseReleaseEvent(QMouseEvent* event) {
    if (m_dragMode == DragMode::None || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragMode = DragMode::None;
    m_dragGroupKey.clear();
    m_dragGroupHeight = 1.0;
    event->accept();
}
void WaveformView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!m_controller) {
        event->ignore();
        return;
    }
    const QRectF plot = plotRect();
    if (event->position().y() >= plot.top() && event->position().y() <= plot.bottom()
        && event->position().x() >= 0.0 && event->position().x() < plot.left()) {
        double normalizedY = 0.5;
        const QString groupKey = groupAtY(event->position().y(), &normalizedY);
        if (groupKey.isEmpty()) {
            event->ignore();
            return;
        }
        m_controller->resetGroupRange(groupKey);
    } else if (plot.contains(event->position())) {
        m_controller->setFollow(true);
    } else {
        event->ignore();
        return;
    }
    event->accept();
}

void WaveformView::hoverMoveEvent(QHoverEvent* event) {
    if (!m_controller || m_dragMode != DragMode::None || !plotRect().contains(event->position())) {
        if (m_controller && m_dragMode == DragMode::None) m_controller->clearCursor();
        return;
    }
    double normalizedY = 0.5;
    qreal groupHeight = 1.0;
    const QString groupKey = groupAtY(event->position().y(), &normalizedY, &groupHeight);
    m_controller->setCursorAt(normalizedX(event->position().x()), groupKey, normalizedY,
                              plotRect().width(), groupHeight);
}

void WaveformView::hoverLeaveEvent(QHoverEvent*) {
    if (m_controller && m_dragMode == DragMode::None) m_controller->clearCursor();
}

void WaveformView::wheelEvent(QWheelEvent* event) {
    if (!m_controller) {
        event->ignore();
        return;
    }
    const QRectF plot = plotRect();
    const bool inPlot = plot.contains(event->position());
    const bool onYAxis = event->position().y() >= plot.top()
        && event->position().y() <= plot.bottom()
        && event->position().x() >= 0.0
        && event->position().x() < plot.left();
    if (!inPlot && !onYAxis) {
        event->ignore();
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (!qFuzzyIsNull(steps)) {
        const double factor = qPow(1.25, steps);
        const bool verticalZoom = onYAxis || event->modifiers().testFlag(Qt::ControlModifier);
        if (verticalZoom) {
            double normalizedY = 0.5;
            const QString groupKey = groupAtY(event->position().y(), &normalizedY);
            if (!groupKey.isEmpty()) m_controller->zoomGroupAt(groupKey, normalizedY, factor);
        } else {
            m_controller->zoomAt(normalizedX(event->position().x()), factor);
        }
    }
    event->accept();
}

QRectF WaveformView::plotRect() const {
    return boundingRect().adjusted(leftInset(), topInset(), -rightInset(), -bottomInset());
}

double WaveformView::normalizedX(qreal x) const {
    const QRectF plot = plotRect();
    return qBound(0.0, (x - plot.left()) / qMax(1.0, plot.width()), 1.0);
}

QString WaveformView::groupAtY(qreal y, double* normalizedPosition, qreal* height) const {
    if (!m_controller) return {};
    const QRectF plot = plotRect();
    if (!plot.contains(QPointF(plot.center().x(), y))) return {};

    const WaveformRenderSnapshot snapshot = m_controller->renderSnapshot(1);
    const int groupCount = snapshot.groups.size();
    if (groupCount == 0) return {};
    constexpr qreal groupGap = 8.0;
    const qreal groupHeight = (plot.height() - groupGap * (groupCount - 1)) / groupCount;
    if (groupHeight <= 0.0) return {};
    if (height) *height = groupHeight;
    for (int index = 0; index < groupCount; ++index) {
        const qreal top = plot.top() + index * (groupHeight + groupGap);
        if (y < top || y > top + groupHeight) continue;
        if (normalizedPosition) {
            *normalizedPosition = qBound(0.0, (y - top) / groupHeight, 1.0);
        }
        return snapshot.groups[index].key;
    }
    return {};
}

} // namespace akrion::gui
