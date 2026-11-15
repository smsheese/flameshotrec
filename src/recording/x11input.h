// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QRegion>

namespace Recording {

// Applies `region` (in logical overlay coordinates) as the input shape of the
// X11 window `window`. Pixels outside the shape are click-through, so the
// desktop and the other monitors keep receiving pointer events while the
// recording overlay is visible. Returns false when the X11 server or the Shape
// extension is unavailable.
bool setX11InputRegion(quintptr window, const QRegion& region, qreal scale);

// Restores the default input shape (the whole window) for `window`.
bool resetX11InputRegion(quintptr window);

} // namespace Recording
