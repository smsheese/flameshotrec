// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QtGlobal>

// Live desktop pixels from *below* the capture overlay.
//
// An X11 client cannot read the pixels under a patch it paints itself: the
// screen composite contains the patch, so inverting it again is a no-op,
// pixelating it blurs the effect instead of the content, and hiding the patch
// for the read would flash an unprotected area on screen and into the
// recording. The only source of the real content is the window stack, which a
// composited server keeps in per-window offscreen storage. Walking the stack
// below the overlay's own window rebuilds what the compositor shows there,
// without the overlay and without a screen-wide read.
namespace LiveDesktop {

// One sampled area.
//
// `failure` is the whole point of this type being more than a QImage: an effect
// that cannot be rebuilt faithfully must *abstain* with a reason, never hand
// back a plausible-looking patch (a frozen frame, the overlay's own pixels, the
// capture-time screenshot, black where content exists). The caller shows the
// reason and clears the effect.
struct Sample
{
    QImage image;         // null unless the walk succeeded
    QRect rect;           // requested area, root coordinates, physical pixels
    QRect covered;        // area really rebuilt, root coordinates
    bool complete{false}; // every pixel of `covered` came from real content
    // True when some pixels came from the desktop's advertised wallpaper
    // instead of a window. Never a guess: when this is false and `complete` is
    // true, every pixel was taken from a real (opaque) window.
    bool backgroundPainted{false};
    QString failure;      // empty exactly when `image` is usable

    bool usable() const { return !image.isNull() && failure.isEmpty(); }
};

// True when the X11 server offers the composite extension and a compositing
// manager owns `_NET_WM_CM_S<screen>`. Without a compositing manager the
// windows below the overlay are not redirected, obscured pixels are undefined
// and nothing here is safe to use.
bool compositingActive();

// Synchronous, bounded sample of `physicalRect` (root coordinates) taken from
// the window stack below `overlayWindow`, on the connection owned by the calling
// thread. The overlay window is excluded together with everything above it.
//
// Bounded on purpose: at most `kMaxWindows` windows, `kMaxPixels` pixels and
// `kSampleBudgetMs` of wall clock per sample. Exceeding a bound abstains instead
// of stalling the caller (the overlay runs this from a paint, and a GUI that
// stops responding is not a fix for a frozen effect).
Sample capture(quintptr overlayWindow, const QRect& physicalRect);

// Shape-preserving convenience wrapper: the image, plus the area really covered
// and the reason when nothing usable came back.
QImage below(quintptr overlayWindow,
             const QRect& globalRect,
             bool* complete = nullptr,
             QRect* covered = nullptr,
             QString* failure = nullptr);

// Off-thread sampler.
//
// The expensive part of a sample is the X round-trip per window, which must not
// happen on the GUI thread while the user is drawing. `Sampler` owns a worker
// thread with its own X11 connection: it opens the connection *inside* that
// thread and never touches the connection the GUI thread uses (Xlib's error
// handler is process-global, so a separate connection is what keeps this module
// from swallowing or misattributing another thread's X errors).
//
// Requests are coalesced: at most one is in flight per id, the newest replaces
// a queued one, and a result whose request was superseded is dropped. The id is
// chosen by the caller (an annotation pointer, a counter, anything stable).
class Sampler : public QObject
{
    Q_OBJECT
public:
    explicit Sampler(QObject* parent = nullptr);
    ~Sampler() override;

    // Queues a sample of `physicalRect` (root coordinates) below
    // `overlayWindow`. Returns false when the sampler could not be started.
    bool request(quint64 id, quintptr overlayWindow, const QRect& physicalRect);

    // Drops any queued or in-flight request for `id`; its result is discarded.
    void forget(quint64 id);

    // Stops the worker thread. Called by the destructor as well.
    void stop();

signals:
    void sampled(quint64 id, const LiveDesktop::Sample& sample);

private:
    class WorkerThread;
    friend class WorkerThread;

    // Called on the GUI thread.
    void deliver(quint64 id, quint64 generation, const Sample& sample);

    struct Pending
    {
        quintptr overlay{0};
        QRect rect;
        quint64 generation{0};
        bool inFlight{false};
    };

    WorkerThread* m_worker{nullptr};
    QHash<quint64, Pending> m_pending;
};

} // namespace LiveDesktop

Q_DECLARE_METATYPE(LiveDesktop::Sample)
