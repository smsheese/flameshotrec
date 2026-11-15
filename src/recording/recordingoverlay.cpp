// SPDX-License-Identifier: GPL-3.0-or-later
#include "recordingoverlay.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygonF>
#include <QLineF>

#include <algorithm>
#include <cmath>

namespace Recording {

RecordingOverlay::RecordingOverlay(QWidget* parent)
  : QWidget(parent)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    m_clock.start();
    m_refreshTimer.setInterval(16);
    connect(&m_refreshTimer, &QTimer::timeout,
            this, &RecordingOverlay::refreshAnnotations);
    m_refreshTimer.start();
}

void RecordingOverlay::setRecordingRegion(const QRect& globalRegion)
{
    setGeometry(globalRegion);
    show();
    raise();
}

void RecordingOverlay::setTool(Tool tool) { m_tool = tool; }

void RecordingOverlay::setPenColor(const QColor& color)
{
    if (color.isValid()) m_color = color;
}

void RecordingOverlay::setPenWidth(qreal width)
{
    if (std::isfinite(width) && width > 0.0) m_width = width;
}

void RecordingOverlay::setHoldDuration(int milliseconds)
{
    m_holdDurationMs = std::max(0, milliseconds);
}

void RecordingOverlay::setFadeDuration(int milliseconds)
{
    m_fadeDurationMs = std::max(0, milliseconds);
}

void RecordingOverlay::clearAnnotations()
{
    m_marks.clear();
    m_drawing = false;
    update();
}

int RecordingOverlay::lifetimeMs() const
{
    return m_holdDurationMs + m_fadeDurationMs;
}

void RecordingOverlay::setPaused(bool paused)
{
    m_paused = paused;
    if (paused) m_drawing = false;
    const qint64 now = m_clock.elapsed();
    if (paused && m_pausedAtMs < 0) {
        m_pausedAtMs = now;
    } else if (!paused && m_pausedAtMs >= 0) {
        m_totalPausedMs += now - m_pausedAtMs;
        m_pausedAtMs = -1;
    }
    update();
}

QPointF RecordingOverlay::localPosition(const QMouseEvent* event) const
{
    return event->position();
}

void RecordingOverlay::mousePressEvent(QMouseEvent* event)
{
    if (m_paused) { event->ignore(); return; }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_current = { m_tool, { localPosition(event) }, m_color, m_width, -1 };
    m_drawing = true;
    event->accept();
    update();
}

void RecordingOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_drawing || !(event->buttons() & Qt::LeftButton)) return;
    const QPointF point = localPosition(event);
    if (m_current.tool == Tool::Pencil) m_current.points.push_back(point);
    else if (m_current.points.size() == 1) m_current.points.push_back(point);
    else m_current.points[1] = point;
    event->accept();
    update();
}

void RecordingOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_drawing || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const QPointF point = localPosition(event);
    if (m_current.tool == Tool::Pencil) m_current.points.push_back(point);
    else if (m_current.points.size() == 1) m_current.points.push_back(point);
    else m_current.points[1] = point;
    m_current.completedAtMs = m_clock.elapsed() - m_totalPausedMs;
    m_marks.push_back(m_current);
    m_drawing = false;
    event->accept();
    update();
}

void RecordingOverlay::drawMark(QPainter& painter, const Mark& mark)
{
    if (mark.points.isEmpty()) return;
    QColor color = mark.color;
    qreal width = mark.width;
    if (mark.tool == Tool::Marker) {
        color.setAlphaF(0.45);
        width = std::max<qreal>(8.0, width * 3.0);
    }
    QPen pen(color, width, Qt::SolidLine, Qt::RoundCap,
             Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (mark.tool == Tool::Pencil) {
        if (mark.points.size() == 1) painter.drawPoint(mark.points.first());
        else painter.drawPolyline(QPolygonF(mark.points));
        return;
    }
    if (mark.points.size() < 2) {
        painter.drawPoint(mark.points.first());
        return;
    }
    const QPointF start = mark.points[0];
    const QPointF end = mark.points[1];
    const QRectF rect(start, end);
    switch (mark.tool) {
        case Tool::Pencil: break;
        case Tool::Line: painter.drawLine(start, end); break;
        case Tool::Rectangle: painter.drawRect(rect.normalized()); break;
        case Tool::Ellipse: painter.drawEllipse(rect.normalized()); break;
        case Tool::Arrow: {
            painter.drawLine(start, end);
            const QLineF shaft(start, end);
            if (shaft.length() < 1.0) break;
            const qreal angle = std::atan2(shaft.dy(), shaft.dx());
            const qreal head = std::max<qreal>(8.0, mark.width * 4.0);
            const QPointF left = end - QPointF(std::cos(angle - 0.55) * head,
                                               std::sin(angle - 0.55) * head);
            const QPointF right = end - QPointF(std::cos(angle + 0.55) * head,
                                                std::sin(angle + 0.55) * head);
            painter.drawLine(end, left);
            painter.drawLine(end, right);
            break;
        }
        case Tool::Marker:
            painter.drawLine(start, end);
            break;
    }
}

void RecordingOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qint64 now = (m_pausedAtMs >= 0 ? m_pausedAtMs : m_clock.elapsed()) - m_totalPausedMs;
    for (const Mark& mark : m_marks) {
        qreal opacity = 1.0;
        const qint64 age = now - mark.completedAtMs;
        if (age > m_holdDurationMs) {
            opacity = m_fadeDurationMs == 0
                        ? 0.0
                        : 1.0 - qreal(age - m_holdDurationMs) / m_fadeDurationMs;
        }
        if (opacity <= 0.0) continue;
        painter.save();
        painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
        drawMark(painter, mark);
        painter.restore();
    }
    if (m_drawing) drawMark(painter, m_current);
}

void RecordingOverlay::refreshAnnotations()
{
    const qint64 now = (m_pausedAtMs >= 0 ? m_pausedAtMs : m_clock.elapsed()) - m_totalPausedMs;
    const auto oldSize = m_marks.size();
    m_marks.erase(std::remove_if(m_marks.begin(), m_marks.end(),
                                 [this, now](const Mark& mark) {
                                     return now - mark.completedAtMs >= lifetimeMs();
                                 }),
                  m_marks.end());
    if (oldSize != m_marks.size() || !m_marks.isEmpty() || m_drawing) update();
}

} // namespace Recording
