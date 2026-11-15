// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "x11livedesktop.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRegion>
#include <QThread>
#include <QWaitCondition>
#include <QtDebug>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <cstring>
#include <mutex>

namespace LiveDesktop {
namespace {

// Depth of the window tree that is walked. A composited desktop is at most a
// frame window, the client window and a handful of child controls deep.
constexpr int kMaxDepth = 6;
// Work bounds. A sample can run from a paint or from the sampler thread, so it
// has to finish in bounded time on *any* window stack; when a bound is reached
// the sample abstains with a reason instead of stalling the caller.
constexpr int kMaxWindows = 256;
constexpr qint64 kMaxPixels = 4000000;
constexpr qint64 kSampleBudgetMs = 25;

void ensureXlibThreads()
{
    // Xlib has process-global state (the error and IO handlers), so it may only
    // be used from several threads after the library was told so, *before* its
    // first call. Qt's X11 platform plugin talks xcb, not Xlib, so this is the
    // earliest Xlib use in the process.
    static std::once_flag once;
    std::call_once(once, []() {
        XInitThreads();
        XSetIOErrorHandler([](Display*) -> int {
            // The default handler terminates the process, which would throw away
            // a recording the user already made. The sampler notices the dead
            // connection through the failing calls that follow and abstains.
            qCritical("live desktop: the X11 connection was lost");
            return 0;
        });
    });
}

// Xlib reports errors asynchronously and exits the process by default. Windows
// can disappear between the stack query and the read (a menu closing, an
// application exiting), which is expected here, so they are counted instead.
//
// The handler is process-global, so it is installed for exactly one connection
// and one caller at a time (guarded by a mutex) and *chains*: an error from
// another connection -- another Xlib user in this process, on another thread --
// goes to the handler that was installed before instead of being swallowed or
// counted against this sample.
QMutex g_errorMutex;
XErrorHandler g_previousHandler = nullptr;
Display* g_guardedDisplay = nullptr;
int g_guardedErrors = 0;

int guardedErrorHandler(Display* display, XErrorEvent* event)
{
    if (display == g_guardedDisplay) {
        ++g_guardedErrors;
        return 0;
    }
    if (g_previousHandler != nullptr) {
        return g_previousHandler(display, event);
    }
    return 0;
}

class ErrorGuard
{
public:
    explicit ErrorGuard(Display* display)
      : m_lock(&g_errorMutex)
    {
        g_guardedDisplay = display;
        g_guardedErrors = 0;
        g_previousHandler = XSetErrorHandler(guardedErrorHandler);
    }
    ~ErrorGuard()
    {
        XSetErrorHandler(g_previousHandler);
        g_previousHandler = nullptr;
        g_guardedDisplay = nullptr;
    }
    ErrorGuard(const ErrorGuard&) = delete;
    ErrorGuard& operator=(const ErrorGuard&) = delete;

    bool clean() const { return g_guardedErrors == 0; }

private:
    QMutexLocker<QMutex> m_lock;
};

// One connection per thread. The GUI thread and the sampler thread each have
// their own, which is what makes the chained error handler above meaningful.
Display* threadDisplay()
{
    struct Connection
    {
        Display* display{nullptr};
        ~Connection()
        {
            if (display != nullptr) {
                XCloseDisplay(display);
            }
        }
    };
    thread_local Connection connection;
    if (connection.display == nullptr) {
        ensureXlibThreads();
        connection.display = XOpenDisplay(nullptr);
    }
    return connection.display;
}

bool hasCompositeExtension(Display* dpy)
{
    int opcode = 0;
    int event = 0;
    int error = 0;
    // Only the extension presence is needed: a composited server keeps every
    // window in its own offscreen storage, so plain XGetImage already returns
    // it and no libXcomposite call is made.
    return XQueryExtension(dpy, "Composite", &opcode, &event, &error) != 0;
}

bool compositingManagerOwnsScreen(Display* dpy)
{
    if (!hasCompositeExtension(dpy)) {
        return false;
    }
    const QByteArray selection = QByteArrayLiteral("_NET_WM_CM_S") +
                                 QByteArray::number(DefaultScreen(dpy));
    const Atom atom = XInternAtom(dpy, selection.constData(), False);
    return atom != None && XGetSelectionOwner(dpy, atom) != None;
}

// Pixel count of a region, for the diagnostic in a refusal. Never a silent
// substitute: it only makes the message say *how much* could not be rebuilt.
int qRegionArea(const QRegion& region)
{
    int area = 0;
    for (const QRect& rectangle : region) {
        area += rectangle.width() * rectangle.height();
    }
    return area;
}

// One TrueColor channel of a packed pixel, scaled to 8 bits.
int channelValue(quint32 pixel, quint32 mask)
{
    if (mask == 0) {
        return 0;
    }
    int shift = 0;
    quint32 scaled = mask;
    while ((scaled & 1U) == 0U) {
        scaled >>= 1;
        ++shift;
    }
    const quint32 value = (pixel & mask) >> shift;
    return int((value * 255U) / scaled);
}

// How a window's own pixels have to be interpreted, or why they cannot be.
enum class VisualSupport
{
    Opaque,
    ArgbPremultiplied,
    Unsupported
};

VisualSupport visualSupport(const XWindowAttributes& attributes)
{
    if (attributes.visual == nullptr) {
        return VisualSupport::Unsupported;
    }
    const Visual* visual = attributes.visual;
    if (visual->red_mask == 0 || visual->green_mask == 0 ||
        visual->blue_mask == 0) {
        return VisualSupport::Unsupported;
    }
    if (attributes.depth == 32) {
        // A 32-bit visual is only ARGB when its masks say so; the alpha channel
        // is what makes translucency possible at all. Any other 32-bit visual
        // has no conversion this module can prove correct.
        if (visual->red_mask == 0x00ff0000U &&
            visual->green_mask == 0x0000ff00U &&
            visual->blue_mask == 0x000000ffU) {
            return VisualSupport::ArgbPremultiplied;
        }
        return VisualSupport::Unsupported;
    }
    if (attributes.depth == 24 || attributes.depth == 16 ||
        attributes.depth == 15 || attributes.depth == 8) {
        return VisualSupport::Opaque;
    }
    // Deep-colour (30 bit) and odd visuals have no proven conversion here.
    return VisualSupport::Unsupported;
}

// One window's own drawing as a QImage the painter can blend.
QImage windowImage(Display* dpy,
                   Drawable drawable,
                   int x,
                   int y,
                   int width,
                   int height,
                   bool premultiplied)
{
    if (width <= 0 || height <= 0) {
        return {};
    }
    XImage* ximage =
      XGetImage(dpy, drawable, x, y, width, height, AllPlanes, ZPixmap);
    if (ximage == nullptr) {
        return {};
    }
    QImage image(width,
                 height,
                 premultiplied ? QImage::Format_ARGB32_Premultiplied
                               : QImage::Format_RGB32);
    image.setDevicePixelRatio(1.0);
    const quint32 redMask = quint32(ximage->red_mask);
    const quint32 greenMask = quint32(ximage->green_mask);
    const quint32 blueMask = quint32(ximage->blue_mask);
    const bool direct = ximage->bits_per_pixel == 32 &&
                        ximage->byte_order == LSBFirst &&
                        redMask == 0x00ff0000U && greenMask == 0x0000ff00U &&
                        blueMask == 0x000000ffU;
    if (direct) {
        // Byte-for-byte the layout QImage wants on this platform. XGetPixel per
        // pixel is the most expensive call in the whole walk (one function call
        // per pixel), so the direct case must not use it.
        const size_t rowBytes = size_t(width) * 4U;
        for (int row = 0; row < height; ++row) {
            std::memcpy(image.scanLine(row),
                        ximage->data + row * ximage->bytes_per_line,
                        rowBytes);
            if (!premultiplied) {
                // An opaque window's 32-bit words carry no alpha, and a zero
                // there survives into a premultiplied layer as "invisible".
                // Setting it here keeps the fast row copy and is far cheaper
                // than the XGetPixel-per-pixel path below.
                auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(row));
                for (int column = 0; column < width; ++column) {
                    pixels[column] |= 0xff000000U;
                }
            }
        }
    } else if (premultiplied) {
        // A 32-bit image that is not in the direct layout: its premultiplied
        // channels cannot be unpacked with the generic opaque conversion.
        XDestroyImage(ximage);
        return {};
    } else {
        for (int row = 0; row < height; ++row) {
            auto* out = reinterpret_cast<QRgb*>(image.scanLine(row));
            for (int column = 0; column < width; ++column) {
                const quint32 pixel = quint32(XGetPixel(ximage, column, row));
                out[column] = qRgba(channelValue(pixel, redMask),
                                    channelValue(pixel, greenMask),
                                    channelValue(pixel, blueMask),
                                    0xff);
            }
        }
    }
    XDestroyImage(ximage);
    return image;
}

// Blocks of a painted ARGB image that are fully opaque. A 32-bit window is not
// automatically translucent: what it covers depends on the alpha of its own
// pixels, so the coverage has to be measured instead of assumed from the visual.
// The scan is conservative (a block counts only when every pixel in it is
// opaque) and bounded, so an unverifiable window makes the sample abstain rather
// than claim coverage it does not have.
constexpr int kCoverageBlock = 8;
constexpr int kMaxCoverageBlocks = 4096;

bool opaqueBlocks(const QImage& content, const QRect& target, QRegion* region,
                  QString* failure)
{
    const int columns = (target.width() + kCoverageBlock - 1) / kCoverageBlock;
    const int rows = (target.height() + kCoverageBlock - 1) / kCoverageBlock;
    if (qint64(columns) * rows > kMaxCoverageBlocks) {
        if (failure != nullptr) {
            *failure = QStringLiteral("a translucent window is too large to "
                                      "verify (more than %1 coverage blocks)")
                         .arg(kMaxCoverageBlocks);
        }
        return false;
    }
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const int x0 = column * kCoverageBlock;
            const int y0 = row * kCoverageBlock;
            const int x1 = qMin(x0 + kCoverageBlock, content.width());
            const int y1 = qMin(y0 + kCoverageBlock, content.height());
            bool opaque = true;
            for (int y = y0; y < y1 && opaque; ++y) {
                const auto* line =
                  reinterpret_cast<const QRgb*>(content.constScanLine(y));
                for (int x = x0; x < x1; ++x) {
                    if (qAlpha(line[x]) != 255) {
                        opaque = false;
                        break;
                    }
                }
            }
            if (opaque) {
                *region += QRect(target.topLeft() + QPoint(x0, y0),
                                 QSize(x1 - x0, y1 - y0));
            }
        }
    }
    return true;
}

// State of one sampling walk: the work already done, the bounds it must stay
// inside, and the reason to abstain when a bound is hit or a reconstruction is
// not provable.
struct Walk
{
    Display* dpy{nullptr};
    QRect area; // requested, clamped to the root window, root coordinates
    QPainter* painter{nullptr};
    QRegion opaque; // parts of `area` covered by a fully opaque window
    QElapsedTimer clock;
    int windows{0};
    qint64 pixels{0};
    QString failure;
    Atom opacityAtom{None};
    bool shapeAvailable{false};

    // True when the walk must stop: a bound was reached, or an earlier step
    // already decided the sample cannot be faithful.
    bool stop()
    {
        if (!failure.isEmpty()) {
            return true;
        }
        if (windows > kMaxWindows) {
            failure =
              QStringLiteral("more than %1 windows are stacked below the effect")
                .arg(kMaxWindows);
            return true;
        }
        if (clock.elapsed() > kSampleBudgetMs) {
            failure = QStringLiteral("sampling the windows below the effect took "
                                     "longer than %1 ms")
                        .arg(kSampleBudgetMs);
            return true;
        }
        return false;
    }

    bool takePixels(qint64 count)
    {
        if (count <= 0) {
            return false;
        }
        pixels += count;
        if (pixels > kMaxPixels) {
            failure = QStringLiteral(
                        "the effect area needs more than %1 pixels to rebuild")
                        .arg(kMaxPixels);
            return false;
        }
        return true;
    }
};

// Fills `canvas` (in area coordinates) with the wallpaper where the window stack
// did not paint an opaque window. Returns false when a pixel of the area has no
// determinable source, because a guessed background is exactly the kind of
// silent wrongness this module must never produce.
bool paintBackground(Walk& walk, const Window root, QImage& canvas)
{
    Q_UNUSED(root)
    canvas = QImage(walk.area.size(), QImage::Format_RGB32);
    canvas.fill(Qt::black);
    const QRegion unknown = QRegion(walk.area) - walk.opaque;
    if (unknown.isEmpty()) {
        // Every pixel is a window's own opaque drawing; there is no background
        // to reconstruct.
        return true;
    }
    // A fullscreen overlay cannot read what is below it, so the desktop
    // background is only reconstructible from a pixmap the desktop advertises,
    // tiled correctly, with a known pattern. Rather than pretend, this abstains:
    // the caller shows the reason and drops the effect for that frame instead of
    // painting black, a stale patch or the capture-time screenshot into the
    // recording. On desktops with a fullscreen desktop window (muffin, metacity,
    // cinnamon, KWin with a desktop window) this branch is not reached at all,
    // because that window covers the area opaquely.
    const QRect uncovered = unknown.boundingRect();
    walk.failure =
      QStringLiteral(
        "part of the effect area shows only the desktop background (%1 px in "
        "%2 region(s) starting at %3,%4); a fullscreen overlay cannot read it")
        .arg(qRegionArea(unknown))
        .arg(unknown.rectCount())
        .arg(uncovered.x())
        .arg(uncovered.y());
    return false;
}

double windowOpacity(Walk& walk, Window window)
{
    if (walk.opacityAtom == None) {
        return 1.0;
    }
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(walk.dpy,
                           window,
                           walk.opacityAtom,
                           0,
                           1,
                           False,
                           XA_CARDINAL,
                           &type,
                           &format,
                           &count,
                           &remaining,
                           &data) != Success ||
        data == nullptr) {
        return 1.0;
    }
    double opacity = 1.0;
    if (count > 0 && format == 32) {
        const unsigned long raw = *reinterpret_cast<unsigned long*>(data);
        opacity = double(raw) / double(0xffffffffUL);
    }
    XFree(data);
    return qBound(0.0, opacity, 1.0);
}

// Applies the window's bounding shape, which is what X clips the window (and its
// whole subtree) to. `origin` is the window's interior origin in root
// coordinates, because shape rectangles are in window coordinates. Returns false
// when the shape is unknown, because then the pixels are unknown too.
bool applyWindowShape(Walk& walk,
                      Window window,
                      const QPoint& origin,
                      QRegion* region)
{
    if (!walk.shapeAvailable) {
        return true;
    }
    Bool boundingShaped = 0;
    int shapeX = 0;
    int shapeY = 0;
    unsigned int shapeWidth = 0;
    unsigned int shapeHeight = 0;
    Bool clipShaped = 0;
    int clipX = 0;
    int clipY = 0;
    unsigned int clipWidth = 0;
    unsigned int clipHeight = 0;
    if (XShapeQueryExtents(walk.dpy,
                          window,
                          &boundingShaped,
                          &shapeX,
                          &shapeY,
                          &shapeWidth,
                          &shapeHeight,
                          &clipShaped,
                          &clipX,
                          &clipY,
                          &clipWidth,
                          &clipHeight) == 0) {
        walk.failure = QStringLiteral("the shape of a window could not be read");
        return false;
    }
    if (boundingShaped == 0) {
        // A plain rectangle: the window's own geometry already is its shape.
        return true;
    }
    int count = 0;
    int ordering = 0;
    XRectangle* rectangles =
      XShapeGetRectangles(walk.dpy, window, ShapeBounding, &count, &ordering);
    if (rectangles == nullptr && count != 0) {
        walk.failure = QStringLiteral("the shape of a window could not be read");
        return false;
    }
    QRegion shape;
    for (int index = 0; index < count; ++index) {
        shape += QRect(rectangles[index].x + origin.x(),
                       rectangles[index].y + origin.y(),
                       int(rectangles[index].width),
                       int(rectangles[index].height));
    }
    if (rectangles != nullptr) {
        XFree(rectangles);
    }
    if (shape.isEmpty()) {
        // Shaped away completely: the window and its subtree are invisible.
        *region = QRegion();
        return true;
    }
    *region &= shape;
    return true;
}

// Paints `window` and, in stacking order, its children.
//
// `rect` is the window's frame in root coordinates (interior plus border),
// `clip` what the ancestors plus the request area still allow it to show, and
// `opacity` the accumulated compositor opacity of the ancestors. X clips a child
// to its ancestors, so passing `clip` down is exactly the clipping X performs.
void compositeBelow(Walk& walk,
                    Window window,
                    const QRect& rect,
                    const QRegion& clip,
                    double opacity,
                    int depth)
{
    if (depth > kMaxDepth || walk.stop()) {
        return;
    }
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(walk.dpy, window, &attributes) == 0) {
        return;
    }
    // An InputOnly window has depth 0; it takes events and draws nothing.
    if (attributes.map_state != IsViewable || attributes.depth == 0) {
        return;
    }
    ++walk.windows;
    if (walk.stop()) {
        return;
    }
    const VisualSupport support = visualSupport(attributes);
    if (support == VisualSupport::Unsupported) {
        walk.failure = QStringLiteral("a window uses a pixel format this "
                                      "sampler cannot reproduce (depth %1)")
                         .arg(attributes.depth);
        return;
    }

    const QRect interior(rect.topLeft() + QPoint(attributes.border_width,
                                                 attributes.border_width),
                         QSize(attributes.width, attributes.height));
    QRegion visible = clip.intersected(QRegion(rect));
    if (visible.isEmpty()) {
        // Nothing of this window is visible, and X clips its subtree the same
        // way, so there is nothing to walk either.
        return;
    }
    if (!applyWindowShape(walk, window, interior.topLeft(), &visible)) {
        return;
    }
    if (visible.isEmpty()) {
        return;
    }

    const double ownOpacity = opacity * windowOpacity(walk, window);
    // The window's own drawing, in window coordinates: XGetImage on a window
    // drawable addresses its interior, so the border is not part of it.
    const QRegion painted = visible.intersected(QRegion(interior));
    if (!painted.isEmpty()) {
        const QRect target = painted.boundingRect();
        if (!walk.takePixels(qint64(target.width()) * target.height())) {
            return;
        }
        const QImage content =
          windowImage(walk.dpy,
                      window,
                      target.x() - interior.x(),
                      target.y() - interior.y(),
                      target.width(),
                      target.height(),
                      support == VisualSupport::ArgbPremultiplied);
        if (content.isNull()) {
            walk.failure =
              QStringLiteral("the pixels of a window could not be read");
            return;
        }
        if (ownOpacity <= 0.0) {
            // Fully transparent: it hides nothing below it, and it certainly
            // does not make the background visible.
        } else {
            if (qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_SAMPLER")) {
                qInfo("sampler: window %lu rect %d,%d %dx%d depth %d target "
                      "%d,%d %dx%d content %s pixel 0x%08x opacity %.2f",
                      static_cast<unsigned long>(window),
                      rect.x(),
                      rect.y(),
                      rect.width(),
                      rect.height(),
                      attributes.depth,
                      target.x(),
                      target.y(),
                      target.width(),
                      target.height(),
                      content.isNull() ? "null" : "ok",
                      content.isNull() ? 0U : uint(content.pixel(0, 0)),
                      ownOpacity);
            }
            walk.painter->save();
            walk.painter->setOpacity(ownOpacity);
            // The clip and the draw position are in root coordinates; the layer
            // the painter writes into starts at the requested area.
            walk.painter->setClipRegion(painted.translated(-walk.area.topLeft()));
            walk.painter->drawImage(target.topLeft() - walk.area.topLeft(),
                                    content);
            walk.painter->restore();
            QRegion coverage;
            if (ownOpacity >= 1.0) {
                if (support == VisualSupport::Opaque) {
                    coverage = painted;
                } else if (!opaqueBlocks(content, target, &coverage, &walk.failure)) {
                    return;
                }
            }
            // Coverage is a union, never a subtraction: a translucent window
            // above an opaque one blends with content that is already fully
            // determined, so the background is still not visible there. Only a
            // provably opaque part of a window makes it impossible for the
            // background to show.
            walk.opaque += coverage;
        }
    }

    Window queryRoot = 0;
    Window parent = 0;
    Window* children = nullptr;
    unsigned int childCount = 0;
    if (XQueryTree(walk.dpy, window, &queryRoot, &parent, &children, &childCount) ==
        0) {
        return;
    }
    for (unsigned int index = 0; index < childCount; ++index) {
        if (walk.stop()) {
            break;
        }
        XWindowAttributes childAttributes{};
        if (XGetWindowAttributes(walk.dpy, children[index], &childAttributes) == 0) {
            continue;
        }
        // A child's frame includes its own border, which extends outward from
        // the position the server reports.
        const QRect childRect(interior.topLeft() +
                                QPoint(childAttributes.x -
                                         childAttributes.border_width,
                                       childAttributes.y -
                                         childAttributes.border_width),
                              QSize(childAttributes.width +
                                      2 * childAttributes.border_width,
                                    childAttributes.height +
                                      2 * childAttributes.border_width));
        if (!childRect.intersects(walk.area)) {
            continue;
        }
        if (visible.intersected(QRegion(childRect)).isEmpty()) {
            continue;
        }
        compositeBelow(
          walk, children[index], childRect, visible, ownOpacity, depth + 1);
    }
    if (children != nullptr) {
        XFree(children);
    }
}

// The ancestor of `window` that is a direct child of the root window. A window
// manager reparents every client into a frame, so the overlay is usually a
// grandchild of the root and never appears in the root's own stack.
Window rootAncestorOf(Display* dpy, Window window)
{
    const Window root = DefaultRootWindow(dpy);
    Window current = window;
    for (int step = 0; step < kMaxDepth && current != root && current != 0;
         ++step) {
        Window queryRoot = 0;
        Window parent = 0;
        Window* children = nullptr;
        unsigned int count = 0;
        if (XQueryTree(dpy, current, &queryRoot, &parent, &children, &count) ==
            0) {
            break;
        }
        if (children != nullptr) {
            XFree(children);
        }
        if (parent == root) {
            return current;
        }
        current = parent;
    }
    return 0;
}

Sample captureOn(Display* dpy, quintptr overlayWindow, const QRect& physicalRect)
{
    Sample sample;
    sample.rect = physicalRect;
    if (physicalRect.isEmpty()) {
        sample.failure = QStringLiteral("the effect area is empty");
        return sample;
    }
    if (dpy == nullptr) {
        sample.failure = QStringLiteral("no X11 connection is available");
        return sample;
    }
    // The guard covers the whole walk, including the compositor query, so a
    // display that goes away mid-sample can never be reported as clean.
    ErrorGuard guard(dpy);
    if (!compositingManagerOwnsScreen(dpy)) {
        sample.failure = QStringLiteral(
          "no compositing manager owns the screen, so the windows below the "
          "overlay cannot be read");
        return sample;
    }
    const Window root = DefaultRootWindow(dpy);
    const Window overlayAncestor =
      rootAncestorOf(dpy, static_cast<Window>(overlayWindow));
    if (overlayAncestor == 0) {
        sample.failure = QStringLiteral(
          "the overlay is not part of the window stack being sampled");
        return sample;
    }
    XWindowAttributes rootAttributes{};
    if (XGetWindowAttributes(dpy, root, &rootAttributes) == 0) {
        sample.failure = QStringLiteral("the root window could not be inspected");
        return sample;
    }
    Walk walk;
    walk.dpy = dpy;
    walk.area =
      physicalRect.intersected(QRect(0,
                                     0,
                                     rootAttributes.width,
                                     rootAttributes.height));
    walk.clock.start();
    int shapeEvent = 0;
    int shapeError = 0;
    walk.shapeAvailable = XShapeQueryExtension(dpy, &shapeEvent, &shapeError) != 0;
    walk.opacityAtom = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", True);
    if (walk.area.isEmpty()) {
        sample.failure = QStringLiteral(
          "the effect area is outside the X11 root window");
        return sample;
    }

    // The windows are painted into their own layer first: what the background
    // has to provide is then exactly the part no opaque window covers, and a
    // background that cannot be determined only matters when something is
    // actually visible through the stack.
    QImage windows(walk.area.size(), QImage::Format_ARGB32_Premultiplied);
    windows.fill(Qt::transparent);
    QPainter painter(&windows);
    walk.painter = &painter;
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    Window queryRoot = 0;
    Window parent = 0;
    Window* children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(dpy, root, &queryRoot, &parent, &children, &count) == 0) {
        painter.end();
        sample.failure = QStringLiteral("the window stack could not be queried");
        return sample;
    }
    // Bottom to top, stopping at the overlay: everything above it (the overlay
    // itself and anything stacked on it) must stay out of the sample, otherwise
    // the effect would read its own pixels back.
    for (unsigned int index = 0; index < count; ++index) {
        if (walk.stop()) {
            break;
        }
        const Window child = children[index];
        if (child == overlayAncestor) {
            break;
        }
        XWindowAttributes attributes{};
        if (XGetWindowAttributes(dpy, child, &attributes) == 0) {
            continue;
        }
        const QRect childRect(attributes.x - attributes.border_width,
                              attributes.y - attributes.border_width,
                              attributes.width + 2 * attributes.border_width,
                              attributes.height + 2 * attributes.border_width);
        if (!childRect.intersects(walk.area)) {
            continue;
        }
        compositeBelow(walk, child, childRect, QRegion(walk.area), 1.0, 0);
    }
    if (children != nullptr) {
        XFree(children);
    }
    painter.end();
    if (!walk.failure.isEmpty()) {
        sample.failure = walk.failure;
        return sample;
    }
    if (!guard.clean()) {
        // A window vanished mid-walk: the sample mixes two states, which is
        // worse than no sample for an effect that must match the screen.
        sample.failure =
          QStringLiteral("the window stack changed while it was being sampled");
        return sample;
    }
    if (walk.area != physicalRect) {
        // Only the part on the root window could be read. Mixing it with
        // anything else would be a guess, so this abstains instead.
        sample.failure = QStringLiteral(
          "the effect area is not completely inside the X11 root window");
        return sample;
    }

    QImage canvas;
    if (!paintBackground(walk, root, canvas)) {
        sample.failure = walk.failure;
        return sample;
    }
    if (qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_SAMPLER")) {
        const QPoint probe = windows.rect().center();
        qInfo("sampler: layer 0x%08x (raw 0x%08x) format %d at centre of %dx%d, "
              "canvas %dx%d format %d",
              uint(windows.pixel(probe)),
              uint(reinterpret_cast<const QRgb*>(
                     windows.constScanLine(probe.y()))[probe.x()]),
              int(windows.format()),
              windows.width(),
              windows.height(),
              canvas.width(),
              canvas.height(),
              int(canvas.format()));
    }
    QPainter composite(&canvas);
    composite.drawImage(QPoint(0, 0), windows);
    composite.end();
    if (qEnvironmentVariableIsSet("FLAMESHOT_DEBUG_SAMPLER")) {
        qInfo("sampler: composed 0x%08x",
              uint(canvas.pixel(canvas.rect().center())));
    }
    sample.image = canvas;
    sample.covered = walk.area;
    sample.complete = true;
    return sample;
}

} // namespace

bool compositingActive()
{
    Display* dpy = threadDisplay();
    if (dpy == nullptr) {
        return false;
    }
    ErrorGuard guard(dpy);
    return compositingManagerOwnsScreen(dpy);
}

Sample capture(quintptr overlayWindow, const QRect& physicalRect)
{
    if (overlayWindow == 0) {
        Sample sample;
        sample.rect = physicalRect;
        sample.failure =
          QStringLiteral("there is no overlay window to sample below");
        return sample;
    }
    return captureOn(threadDisplay(), overlayWindow, physicalRect);
}

QImage below(quintptr overlayWindow,
             const QRect& globalRect,
             bool* complete,
             QRect* covered,
             QString* failure)
{
    if (complete != nullptr) {
        *complete = false;
    }
    if (covered != nullptr) {
        *covered = QRect();
    }
    const Sample sample = capture(overlayWindow, globalRect);
    if (failure != nullptr) {
        *failure = sample.failure;
    }
    if (covered != nullptr) {
        *covered = sample.covered;
    }
    if (complete != nullptr) {
        *complete = sample.complete;
    }
    return sample.image;
}

// ---------------------------------------------------------------------------
// Sampler

class Sampler::WorkerThread : public QThread
{
public:
    explicit WorkerThread(Sampler* owner)
      : m_owner(owner)
    {}

    ~WorkerThread() override { stop(); }

    void stop()
    {
        {
            QMutexLocker<QMutex> lock(&m_mutex);
            m_quit = true;
            m_pending.clear();
            m_order.clear();
        }
        m_wake.wakeAll();
        wait();
    }

    void post(quint64 id, quint64 generation, quintptr overlay, const QRect& rect)
    {
        {
            QMutexLocker<QMutex> lock(&m_mutex);
            // Coalesce per request id: a newer request for the *same* effect
            // replaces the older one that has not been picked up yet, but two
            // effects never replace each other. A single shared slot did exactly
            // that: the refresh loop asks for every live effect in one pass, so
            // the request posted last (the invert) overwrote the one posted first
            // (the pixelate) before the worker looked at it. Its id had already
            // been marked in flight, so every later request for it coalesced and
            // it never sampled again -- the pixelate patch stayed frozen on the
            // last image it had.
            Request& queued = m_pending[id];
            if (!queued.valid) {
                m_order.append(id);
            }
            queued.valid = true;
            queued.id = id;
            queued.generation = generation;
            queued.overlay = overlay;
            queued.rect = rect;
        }
        m_wake.wakeAll();
    }

    void run() override
    {
        // The connection is created *inside* this thread: Xlib connections are
        // thread-affine, and the GUI thread has its own.
        ensureXlibThreads();
        Display* dpy = XOpenDisplay(nullptr);
        while (true) {
            Request request;
            {
                QMutexLocker<QMutex> lock(&m_mutex);
                while (!m_quit && m_order.isEmpty()) {
                    m_wake.wait(&m_mutex);
                }
                if (m_quit) {
                    break;
                }
                // One queued request per id, oldest id first: every effect gets
                // its turn and the newest rectangle for that effect is used.
                const quint64 key = m_order.takeFirst();
                auto queued = m_pending.find(key);
                if (queued == m_pending.end()) {
                    continue;
                }
                request = *queued;
                m_pending.erase(queued);
                request.valid = false;
            }
            Sample sample;
            if (dpy == nullptr) {
                sample.rect = request.rect;
                sample.failure = QStringLiteral(
                  "the sampler could not open an X11 connection");
            } else {
                sample = captureOn(dpy, request.overlay, request.rect);
            }
            Sampler* owner = m_owner;
            const quint64 id = request.id;
            const quint64 generation = request.generation;
            // Hand the result back to the owner's thread without needing a
            // second object with its own meta-object: the queued lambda runs in
            // the owner's thread.
            QMetaObject::invokeMethod(
              owner,
              [owner, id, generation, sample]() {
                  owner->deliver(id, generation, sample);
              },
              Qt::QueuedConnection);
        }
        if (dpy != nullptr) {
            XCloseDisplay(dpy);
        }
    }

private:
    struct Request
    {
        quint64 id{0};
        quint64 generation{0};
        quintptr overlay{0};
        QRect rect;
        bool valid{false};
    };

    Sampler* m_owner;
    QMutex m_mutex;
    QWaitCondition m_wake;
    // Queued requests, one entry per id, plus the order they were first posted
    // in so no effect can starve another.
    QHash<quint64, Request> m_pending;
    QList<quint64> m_order;
    bool m_quit{false};
};

Sampler::Sampler(QObject* parent)
  : QObject(parent)
{
    static const bool registered = []() {
        qRegisterMetaType<LiveDesktop::Sample>("LiveDesktop::Sample");
        return true;
    }();
    Q_UNUSED(registered)
    m_worker = new WorkerThread(this);
    m_worker->start(QThread::LowPriority);
}

Sampler::~Sampler()
{
    stop();
}

void Sampler::stop()
{
    if (m_worker != nullptr) {
        m_worker->stop();
        delete m_worker;
        m_worker = nullptr;
    }
}

bool Sampler::request(quint64 id,
                      quintptr overlayWindow,
                      const QRect& physicalRect)
{
    if (m_worker == nullptr || overlayWindow == 0 || physicalRect.isEmpty()) {
        return false;
    }
    Pending& pending = m_pending[id];
    pending.overlay = overlayWindow;
    pending.rect = physicalRect;
    ++pending.generation;
    if (pending.inFlight) {
        // A request for this id is already running; its result will be dropped
        // and this rectangle picked up right after it.
        return true;
    }
    pending.inFlight = true;
    m_worker->post(id, pending.generation, pending.overlay, pending.rect);
    return true;
}

void Sampler::forget(quint64 id)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return;
    }
    if (it->inFlight) {
        // Bumping the generation is enough: the result in flight is discarded
        // and nothing new is queued.
        ++it->generation;
        it->overlay = 0;
        it->rect = QRect();
        return;
    }
    m_pending.erase(it);
}

void Sampler::deliver(quint64 id, quint64 generation, const Sample& sample)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return;
    }
    it->inFlight = false;
    if (it->overlay == 0) {
        // Forgotten: nobody is waiting for this rectangle any more.
        return;
    }
    if (it->generation != generation) {
        // A newer rectangle is already queued. Produce it next, but still report
        // this one: on a slow server a constant stream of newer requests would
        // otherwise discard every result and the caller would never update --
        // that is a frozen effect, which is exactly what must not happen. The
        // caller decides whether the rectangle it got is the one it needs.
        it->inFlight = true;
        m_worker->post(id, it->generation, it->overlay, it->rect);
    }
    emit sampled(id, sample);
}

} // namespace LiveDesktop
