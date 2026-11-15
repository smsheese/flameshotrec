// SPDX-License-Identifier: GPL-3.0-or-later
#include "x11input.h"

#include <QVector>
#include <QtMath>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

namespace Recording {
namespace {

// Shape state kept across calls. The overlay recomputes its input region
// whenever a control appears or moves, and opening a display plus combining
// rectangles for each update is far too expensive for that path, so the
// connection stays open and an unchanged region is not re-applied.
struct ShapeState
{
    // One long-lived connection for the whole process.
    Display* display = XOpenDisplay(nullptr);
    quintptr lastWindow = 0;
    QRegion lastRegion;
    qreal lastScale = -1.0;
    bool shaped = false;
};

ShapeState& state()
{
    static ShapeState instance;
    return instance;
}

bool shapeAvailable(Display* connection)
{
    int event = 0;
    int error = 0;
    return XShapeQueryExtension(connection, &event, &error) != 0;
}

// Edges are scaled and then subtracted, so neighboring rectangles stay
// contiguous under fractional scale factors instead of leaving one pixel gaps.
QVector<XRectangle> toPhysical(const QRegion& region, qreal scale)
{
    QVector<XRectangle> rectangles;
    rectangles.reserve(region.rectCount());
    for (const QRect& logical : region) {
        XRectangle physical{};
        physical.x = static_cast<short>(qRound(logical.x() * scale));
        physical.y = static_cast<short>(qRound(logical.y() * scale));
        physical.width = static_cast<short>(
          qRound((logical.x() + logical.width()) * scale) - physical.x);
        physical.height = static_cast<short>(
          qRound((logical.y() + logical.height()) * scale) - physical.y);
        rectangles.append(physical);
    }
    return rectangles;
}

} // namespace

bool setX11InputRegion(quintptr window, const QRegion& region, qreal scale)
{
    if (window == 0) {
        return false;
    }
    ShapeState& shape = state();
    if (shape.display == nullptr || !shapeAvailable(shape.display)) {
        return false;
    }
    if (shape.lastWindow == window && shape.lastRegion == region &&
        qFuzzyCompare(scale, shape.lastScale)) {
        return true;
    }
    QVector<XRectangle> rectangles = toPhysical(region, scale);
    XShapeCombineRectangles(shape.display,
                            static_cast<Window>(window),
                            ShapeInput,
                            0,
                            0,
                            rectangles.data(),
                            static_cast<int>(rectangles.size()),
                            ShapeSet,
                            Unsorted);
    XSync(shape.display, False);
    shape.lastWindow = window;
    shape.lastRegion = region;
    shape.lastScale = scale;
    shape.shaped = true;
    return true;
}

bool resetX11InputRegion(quintptr window)
{
    if (window == 0) {
        return false;
    }
    ShapeState& shape = state();
    if (shape.display == nullptr || !shapeAvailable(shape.display)) {
        return false;
    }
    // An empty mask restores the default: the whole window takes input again.
    XShapeCombineMask(
      shape.display, static_cast<Window>(window), ShapeInput, 0, 0, None, ShapeSet);
    XSync(shape.display, False);
    shape.lastWindow = 0;
    shape.lastRegion = QRegion();
    shape.lastScale = -1.0;
    shape.shaped = false;
    return true;
}

} // namespace Recording
