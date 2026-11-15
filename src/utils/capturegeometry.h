// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QList>
#include <QRect>
#include <QRegion>
#include <QtGlobal>

// Pure geometry and timing rules shared by the capture overlay and the
// recording session. Nothing here touches widget or window-system state, so the
// desktop spanning, click-through and annotation lifetime rules can be tested
// without a display server.
namespace CaptureGeometry {

// Union of the screen rectangles in logical coordinates. Negative origins (a
// monitor placed left of or above the primary one) are preserved.
inline QRect unitedDesktop(const QList<QRect>& screens)
{
    QRect desktop;
    for (const QRect& screen : screens) { // NOLINT(altera-unroll-loops)
        desktop = desktop.united(screen);
    }
    return desktop;
}

// True when the desktop covers more than one monitor, which means the overlay
// cannot use a single screen's fullscreen geometry.
inline bool spansMultipleScreens(const QList<QRect>& screens)
{
    if (screens.size() < 2) {
        return false;
    }
    const QRect desktop = unitedDesktop(screens);
    for (const QRect& screen : screens) { // NOLINT(altera-unroll-loops,readability-use-anyofallof,boost-use-ranges,modernize-use-ranges)
        if (screen != desktop) {
            return true;
        }
    }
    return false;
}

// The strip drawn immediately outside a recording region. It never overlaps the
// region, so it cannot appear in the encoded video crop.
inline QRegion boundaryRing(const QRect& region, int width)
{
    if (region.isEmpty() || width <= 0) {
        return {};
    }
    const QRect inner = region.normalized();
    const QRect outer = inner.adjusted(-width, -width, width, width);
    return QRegion(outer).subtracted(QRegion(inner));
}

// Region of the overlay window that accepts pointer input. `interactiveRegion`
// is the area that must stay clickable while a drawing tool is selected;
// `overlays` are the child widgets (toolbar, panels) that always stay usable.
// Everything else is click-through so the desktop, including the other
// monitors, keeps working during a recording.
inline QRegion overlayInputRegion(const QRect& widgetRect,
                                  const QRect& interactiveRegion,
                                  const QList<QRect>& overlays,
                                  bool captureInteractive)
{
    QRegion input;
    if (captureInteractive && !interactiveRegion.isEmpty()) {
        input += QRegion(interactiveRegion.intersected(widgetRect));
    }
    for (const QRect& overlay : overlays) { // NOLINT(altera-unroll-loops)
        const QRect clipped = overlay.intersected(widgetRect);
        if (!clipped.isEmpty()) {
            input += QRegion(clipped);
        }
    }
    return input;
}

// Annotation opacity on the recording timeline. The full strength lasts for the
// whole hold time measured from the release of the stroke, then fades out.
inline qreal annotationOpacity(qint64 ageMs, int holdMs, int fadeMs) // NOLINT(bugprone-easily-swappable-parameters)
{
    const qint64 hold = holdMs < 0 ? 0 : holdMs;
    const qint64 fade = fadeMs < 0 ? 0 : fadeMs;
    if (ageMs <= hold) {
        return 1.0;
    }
    if (fade == 0) {
        return 0.0;
    }
    return qBound(0.0, 1.0 - (qreal(ageMs - hold) / qreal(fade)), 1.0);
}

// True once the annotation finished fading and must be removed from the
// overlay and from the undo stack.
inline bool annotationExpired(qint64 ageMs, int holdMs, int fadeMs) // NOLINT(bugprone-easily-swappable-parameters)
{
    const qint64 hold = holdMs < 0 ? 0 : holdMs;
    const qint64 fade = fadeMs < 0 ? 0 : fadeMs;
    return ageMs >= hold + fade;
}

// Physical pixel rectangle handed to the encoder. FFmpeg's x11grab offsets are
// physical pixels while the overlay works in logical coordinates.
inline QRect physicalRect(const QRect& logical, qreal scale)
{
    if (scale <= 0.0 || qFuzzyCompare(scale, 1.0)) {
        return logical;
    }
    const int left = qRound(logical.x() * scale);
    const int top = qRound(logical.y() * scale);
    const int right = qRound((logical.x() + logical.width()) * scale);
    const int bottom = qRound((logical.y() + logical.height()) * scale);
    return { left, top, right - left, bottom - top };
}

} // namespace CaptureGeometry
