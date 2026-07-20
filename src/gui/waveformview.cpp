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

    QVector<QPointF> gridLines;
    for (int index = 0; index <= 6; ++index) {
        const qreal x = plot.left() + plot.width() * index / 6.0;
        gridLines.append({ x, plot.top() });
        gridLines.append({ x, plot.bottom() });
    }

    const int groupCount = qMax(1, snapshot.groups.size());
    constexpr qreal groupGap = 8.0;
    const qreal groupHeight = (plot.height() - groupGap * (groupCount - 1)) / groupCount;
    for (int groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        const qreal top = plot.top() + groupIndex * (groupHeight + groupGap);
        for (int line = 0; line <= 4; ++line) {
            const qreal y = top + groupHeight * line / 4.0;
            gridLines.append({ plot.left(), y });
            gridLines.append({ plot.right(), y });
        }
    }
    if (auto* node = lineNode(gridLines, QColor(QStringLiteral("#e6e9ed")))) root->appendChildNode(node);

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
    }
    return root;
}

void WaveformView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !plotRect().contains(event->position())) {
        event->ignore();
        return;
    }
    m_dragging = true;
    m_lastDragPosition = event->position();
    event->accept();
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging || !m_controller) {
        event->ignore();
        return;
    }
    const qreal delta = event->position().x() - m_lastDragPosition.x();
    m_lastDragPosition = event->position();
    if (!qFuzzyIsNull(delta)) m_controller->panBy(-delta / qMax(1.0, plotRect().width()));
    event->accept();
}

void WaveformView::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_dragging || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging = false;
    event->accept();
}

void WaveformView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!m_controller || !plotRect().contains(event->position())) {
        event->ignore();
        return;
    }
    m_controller->setFollow(true);
    event->accept();
}

void WaveformView::hoverMoveEvent(QHoverEvent* event) {
    if (!m_controller || m_dragging || !plotRect().contains(event->position())) {
        if (m_controller && !m_dragging) m_controller->clearCursor();
        return;
    }
    m_controller->setCursorPosition(normalizedX(event->position().x()));
}

void WaveformView::hoverLeaveEvent(QHoverEvent*) {
    if (m_controller && !m_dragging) m_controller->clearCursor();
}

void WaveformView::wheelEvent(QWheelEvent* event) {
    if (!m_controller || !plotRect().contains(event->position())) {
        event->ignore();
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (!qFuzzyIsNull(steps)) {
        m_controller->zoomAt(normalizedX(event->position().x()), qPow(1.25, steps));
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

} // namespace akrion::gui
