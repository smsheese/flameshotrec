// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

class QVariant;

namespace Recording {

// Tools that draw on the live recording overlay. The identifiers are stored in
// the configuration file, so they are part of the on-disk format: never rename
// or reorder an entry, append new tools at the end.
enum class AnnotationTool
{
    Pen = 0,
    Line,
    Arrow,
    Box,
    Circle,
    Marker,
    Text,
    Selection,
    Counter,
    Invert,
    Pixelate,
};

// How the bubbles of one circle-counter walk-through age. The counter is the
// only tool where several strokes belong to one visual group.
enum class AnnotationCounterMode
{
    // Default. The bubbles keep full strength for as long as the counter tool
    // is the one in use, so a numbered walk-through stays readable, and start
    // fading when the user switches to another tool.
    HoldUntilToolChange = 0,
    // The whole group ages from its first bubble: hold, then fade, no matter
    // which tool is selected.
    GroupFromFirstBubble,
};

// The size axis a tool really has. Tools without one carry timing only, which
// keeps the settings section free of controls that would do nothing.
enum class AnnotationSizeKind
{
    None = 0,
    StrokeWidth, // level, the same scale as ConfigHandler's drawThickness
    FontSize,    // level, drawFontSize
    MarkerSize,  // level, drawMarkerSize
    BlockSize,   // pixelate block, drawPixelateSize
    BadgeSize,   // circle counter badge, drawCircleCounterSize
};

// The option of a tool that can be configured individually.
enum class AnnotationOption
{
    HoldMs = 0,
    FadeMs,
    Size,
};

// Milliseconds.
struct AnnotationTiming
{
    int holdMs = 0;
    int fadeMs = 0;
};

// `kInherit` as a stored value means "use the existing global setting": the
// recording-wide hold/fade duration for timing, and the drawing tool's own
// default size for sizes. Keeping inheritance as a value instead of copying the
// global default into every tool is what preserves the settings of users who
// never open this section.
inline constexpr int kInherit = -1;
// Mirror of the `recordingHoldDuration` / `recordingFadeDuration` defaults.
inline constexpr int kDefaultHoldMs = 3000;
inline constexpr int kDefaultFadeMs = 300;
inline constexpr int kMaxHoldMs = 30000;
inline constexpr int kMaxFadeMs = 10000;
// Size levels: 1 is the thinnest setting the overlay accepts and 50 is
// `maxToolSize`, the upper bound of the on-overlay size slider.
inline constexpr int kMinSize = 1;
inline constexpr int kMaxSize = 50;

// One tool's entry in a policy snapshot.
struct AnnotationToolSettings
{
    // As stored: `kInherit` means the global value applies.
    AnnotationTiming configured;
    // As used: the global value is already substituted, never `kInherit`.
    AnnotationTiming resolved;
    // As stored: `kInherit` means the drawing tool's default size applies.
    int configuredSize = kInherit;
};

// Storage behind the policy. The inline adapter for the application's options
// lives in `annotationpolicy_confighandler.h`; tests use a plain map, so this
// translation unit never depends on the widget or configuration layer.
class AnnotationStore
{
public:
    virtual ~AnnotationStore() = default;
    // False when the configuration does not know the key. Writing such a key
    // behind ConfigHandler's back would trip its unrecognised-setting guard, so
    // the policy refuses instead (see tool-config-hooks.md in the handoff
    // notes for the registration that makes these keys available).
    virtual bool isAvailable(const QString& key) const = 0;
    virtual QVariant read(const QString& key) const = 0;
    virtual bool write(const QString& key, const QVariant& value) = 0;
    virtual void remove(const QString& key) = 0;
};

// Immutable snapshot of the per-tool annotation settings plus the write path
// that produced it. Reading is pure: a snapshot never consults the store again,
// so the overlay can hold one for the length of a recording.
class AnnotationPolicy
{
public:
    // Display order of the settings section and of the supported tools.
    static QList<AnnotationTool> supportedTools();

    // Stable identifiers: lowercase for messages, the capitalised form is used
    // inside the configuration keys ("annotationPenHoldMs").
    static QString toolIdentifier(AnnotationTool tool);
    static QString displayName(AnnotationTool tool);
    static AnnotationSizeKind sizeKind(AnnotationTool tool);
    // Translated label for the size control of this tool, empty when the tool
    // has no size axis.
    static QString sizeLabel(AnnotationTool tool);

    // Configuration keys. Callers never spell a key themselves.
    static QString key(AnnotationTool tool, AnnotationOption option);
    static QString counterModeKey();
    // Every key the policy reads, in a stable order (configuration
    // registration uses this list).
    static QList<QString> configuredKeys();

    static QString counterModeIdentifier(AnnotationCounterMode mode);
    static bool parseCounterMode(const QString& identifier,
                                 AnnotationCounterMode* mode);
    static AnnotationCounterMode defaultCounterMode();

    static bool isValidHoldMs(int holdMs);
    static bool isValidFadeMs(int fadeMs);
    static bool isValidSize(int size);

    // Inherit resolution. Exposed so the consumer resolves a size exactly the
    // way the settings UI describes it.
    static int resolveHoldMs(int configured, int globalHoldMs);
    static int resolveFadeMs(int configured, int globalFadeMs);
    static int resolveSize(int configured, int drawingDefault);

    // A snapshot with every tool inheriting the given global values.
    static AnnotationPolicy defaults(int globalHoldMs = kDefaultHoldMs,
                                     int globalFadeMs = kDefaultFadeMs);
    // Reads and validates every key. `globalHoldMs`/`globalFadeMs` are the
    // current `recordingHoldDuration`/`recordingFadeDuration` values.
    static AnnotationPolicy load(const AnnotationStore& store,
                                 int globalHoldMs = kDefaultHoldMs,
                                 int globalFadeMs = kDefaultFadeMs);

    const AnnotationToolSettings& settings(AnnotationTool tool) const;
    // Resolved timing, except for the counter in HoldUntilToolChange mode,
    // which is what `effectiveTiming()` is for.
    AnnotationTiming timing(AnnotationTool tool) const;
    // Timing the overlay must apply for stamps of this tool. The counter in
    // HoldUntilToolChange mode gets a zero hold because its full-strength time
    // is the time the tool stays selected; the fade still runs, so the hold and
    // fade the user configured are both respected.
    AnnotationTiming effectiveTiming(AnnotationTool tool) const;
    int configuredSize(AnnotationTool tool) const;
    AnnotationCounterMode counterMode() const;

    // Keys the store reported as unavailable, in `configuredKeys()` order. The
    // recording uses the inherited defaults for them, and the settings UI says
    // so instead of silently storing values nobody reads.
    QList<QString> unavailableKeys() const;
    bool isFullyAvailable() const;

    // Writes. Each returns false and leaves the store untouched when the value
    // is out of range or the key is not available. `setTiming` writes both
    // durations; a per-tool value of `kInherit` is written as such, so the tool
    // follows the global value again.
    static bool setHoldMs(AnnotationStore& store,
                          AnnotationTool tool,
                          int holdMs);
    static bool setFadeMs(AnnotationStore& store,
                          AnnotationTool tool,
                          int fadeMs);
    static bool setTiming(AnnotationStore& store,
                          AnnotationTool tool,
                          const AnnotationTiming& configured);
    static bool setSize(AnnotationStore& store,
                        AnnotationTool tool,
                        int size);
    static bool setCounterMode(AnnotationStore& store,
                               AnnotationCounterMode mode);
    // Drops the per-tool entry so the global value applies again.
    static bool resetTool(AnnotationStore& store, AnnotationTool tool);
    static bool resetCounterMode(AnnotationStore& store);

    // Counter group rule. `ownStampMs` is the release stamp of one bubble,
    // `counterGroupStampMs` the release stamp of the first bubble of the group
    // (negative when there is no group yet). In GroupFromFirstBubble mode every
    // bubble ages from the first one, which makes the group fade as one.
    static qint64 stampFor(AnnotationTool tool,
                           AnnotationCounterMode mode,
                           qint64 ownStampMs,
                           qint64 counterGroupStampMs);

private:
    void refreshResolved(int globalHoldMs, int globalFadeMs);

    QList<AnnotationToolSettings> m_settings; // indexed like supportedTools()
    AnnotationCounterMode m_counterMode = defaultCounterMode();
    qint64 m_globalHoldMs = kDefaultHoldMs;
    qint64 m_globalFadeMs = kDefaultFadeMs;
    QList<QString> m_unavailableKeys;
};

} // namespace Recording
