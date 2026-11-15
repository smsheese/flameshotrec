// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QTimer>
#include <QWidget>

#include <QVector>

namespace Recording {

// Transparent drawing surface intended to be positioned over a recording
// region. Callers own screen selection and click-through/interaction policy.
class RecordingOverlay final : public QWidget
{
    Q_OBJECT
public:
    enum class Tool { Pencil, Line, Rectangle, Ellipse, Arrow, Marker };
    Q_ENUM(Tool)

    explicit RecordingOverlay(QWidget* parent = nullptr);

    void setRecordingRegion(const QRect& globalRegion);
    void setTool(Tool tool);
    void setPenColor(const QColor& color);
    void setPenWidth(qreal width);
    void setHoldDuration(int milliseconds);
    void setFadeDuration(int milliseconds);
    void setPaused(bool paused);
    void clearAnnotations();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    void refreshAnnotations();

private:
    struct Mark {
        Tool tool;
        QVector<QPointF> points;
        QColor color;
        qreal width;
        qint64 completedAtMs;
    };

    static void drawMark(QPainter& painter, const Mark& mark);
    int lifetimeMs() const;
    QPointF localPosition(const QMouseEvent* event) const;

    QVector<Mark> m_marks;
    Mark m_current;
    Tool m_tool{ Tool::Pencil };
    QColor m_color{ Qt::red };
    qreal m_width{ 3.0 };
    int m_holdDurationMs{ 3000 };
    int m_fadeDurationMs{ 300 };
    QElapsedTimer m_clock;
    qint64 m_pausedAtMs{ -1 };
    qint64 m_totalPausedMs{ 0 };
    QTimer m_refreshTimer;
    bool m_drawing{ false };
    bool m_paused{ false };
};

} // namespace Recording
