// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "annotationpolicy.h"

#include <QCoreApplication>
#include <QStringList>
#include <QVariant>

namespace Recording {
namespace {

struct ToolDefinition
{
    AnnotationTool tool;
    const char* identifier; // capitalised, used inside the configuration keys
    AnnotationSizeKind sizeKind;
};

// `supportedTools()` order is the order of the settings section. The
// identifiers are on-disk data: append, never rename.
constexpr ToolDefinition kTools[] = {
    { AnnotationTool::Pen, "Pen", AnnotationSizeKind::StrokeWidth },
    { AnnotationTool::Line, "Line", AnnotationSizeKind::StrokeWidth },
    { AnnotationTool::Arrow, "Arrow", AnnotationSizeKind::StrokeWidth },
    { AnnotationTool::Box, "Box", AnnotationSizeKind::StrokeWidth },
    { AnnotationTool::Circle, "Circle", AnnotationSizeKind::StrokeWidth },
    { AnnotationTool::Marker, "Marker", AnnotationSizeKind::MarkerSize },
    { AnnotationTool::Text, "Text", AnnotationSizeKind::FontSize },
    { AnnotationTool::Selection, "Selection", AnnotationSizeKind::StrokeWidth },
    { AnnotationTool::Counter, "Counter", AnnotationSizeKind::BadgeSize },
    { AnnotationTool::Invert, "Invert", AnnotationSizeKind::None },
    { AnnotationTool::Pixelate, "Pixelate", AnnotationSizeKind::BlockSize },
};

constexpr int kToolCount = int(sizeof(kTools) / sizeof(kTools[0]));

constexpr const char* kKeyPrefix = "annotation";
constexpr const char* kHolderKey = "HoldMs";
constexpr const char* kFadeKey = "FadeMs";
constexpr const char* kSizeKey = "Size";
constexpr const char* kCounterModeKey = "annotationCounterMode";
constexpr const char* kCounterModeHold = "hold-until-tool-change";
constexpr const char* kCounterModeGroup = "group-from-first-bubble";

int toolIndex(AnnotationTool tool)
{
    // The enum is declared in the same order as kTools, but the mapping is
    // looked up rather than assumed so a future reordering cannot silently
    // write the wrong key.
    for (int i = 0; i < kToolCount; ++i) {
        if (kTools[i].tool == tool) {
            return i;
        }
    }
    return -1;
}

const ToolDefinition* definition(AnnotationTool tool)
{
    const int index = toolIndex(tool);
    return index < 0 ? nullptr : &kTools[index];
}

} // namespace

QList<AnnotationTool> AnnotationPolicy::supportedTools()
{
    QList<AnnotationTool> tools;
    tools.reserve(kToolCount);
    for (const auto& entry : kTools) { // NOLINT(altera-unroll-loops)
        tools.append(entry.tool);
    }
    return tools;
}

QString AnnotationPolicy::toolIdentifier(AnnotationTool tool)
{
    const ToolDefinition* entry = definition(tool);
    return entry == nullptr ? QString() : QString::fromLatin1(entry->identifier).toLower();
}

QString AnnotationPolicy::displayName(AnnotationTool tool)
{
    switch (tool) {
        case AnnotationTool::Pen:
            return QCoreApplication::translate("AnnotationPolicy", "Pencil");
        case AnnotationTool::Line:
            return QCoreApplication::translate("AnnotationPolicy", "Line");
        case AnnotationTool::Arrow:
            return QCoreApplication::translate("AnnotationPolicy", "Arrow");
        case AnnotationTool::Box:
            return QCoreApplication::translate("AnnotationPolicy", "Box");
        case AnnotationTool::Circle:
            return QCoreApplication::translate("AnnotationPolicy", "Circle");
        case AnnotationTool::Marker:
            return QCoreApplication::translate("AnnotationPolicy", "Marker");
        case AnnotationTool::Text:
            return QCoreApplication::translate("AnnotationPolicy", "Text");
        case AnnotationTool::Selection:
            return QCoreApplication::translate("AnnotationPolicy", "Rectangular selection");
        case AnnotationTool::Counter:
            return QCoreApplication::translate("AnnotationPolicy", "Circle counter");
        case AnnotationTool::Invert:
            return QCoreApplication::translate("AnnotationPolicy", "Invert");
        case AnnotationTool::Pixelate:
            return QCoreApplication::translate("AnnotationPolicy", "Pixelate");
    }
    return {};
}

AnnotationSizeKind AnnotationPolicy::sizeKind(AnnotationTool tool)
{
    const ToolDefinition* entry = definition(tool);
    return entry == nullptr ? AnnotationSizeKind::None : entry->sizeKind;
}

QString AnnotationPolicy::sizeLabel(AnnotationTool tool)
{
    switch (sizeKind(tool)) {
        case AnnotationSizeKind::StrokeWidth:
            return QCoreApplication::translate("AnnotationPolicy", "Stroke width");
        case AnnotationSizeKind::FontSize:
            return QCoreApplication::translate("AnnotationPolicy", "Font size");
        case AnnotationSizeKind::MarkerSize:
            return QCoreApplication::translate("AnnotationPolicy", "Marker size");
        case AnnotationSizeKind::BlockSize:
            return QCoreApplication::translate("AnnotationPolicy", "Pixel block size");
        case AnnotationSizeKind::BadgeSize:
            return QCoreApplication::translate("AnnotationPolicy", "Badge size");
        case AnnotationSizeKind::None:
            break;
    }
    return {};
}

QString AnnotationPolicy::key(AnnotationTool tool, AnnotationOption option)
{
    const ToolDefinition* entry = definition(tool);
    if (entry == nullptr) {
        return {};
    }
    QString suffix;
    switch (option) {
        case AnnotationOption::HoldMs:
            suffix = QString::fromLatin1(kHolderKey);
            break;
        case AnnotationOption::FadeMs:
            suffix = QString::fromLatin1(kFadeKey);
            break;
        case AnnotationOption::Size:
            if (entry->sizeKind == AnnotationSizeKind::None) {
                return {};
            }
            suffix = QString::fromLatin1(kSizeKey);
            break;
    }
    return QString::fromLatin1(kKeyPrefix) +
           QString::fromLatin1(entry->identifier) + suffix;
}

QString AnnotationPolicy::counterModeKey()
{
    return QString::fromLatin1(kCounterModeKey);
}

QList<QString> AnnotationPolicy::configuredKeys()
{
    QList<QString> keys;
    keys.reserve(kToolCount * 3 + 1);
    for (const auto& entry : kTools) { // NOLINT(altera-unroll-loops)
        keys.append(key(entry.tool, AnnotationOption::HoldMs));
        keys.append(key(entry.tool, AnnotationOption::FadeMs));
        if (entry.sizeKind != AnnotationSizeKind::None) {
            keys.append(key(entry.tool, AnnotationOption::Size));
        }
    }
    keys.append(counterModeKey());
    return keys;
}

QString AnnotationPolicy::counterModeIdentifier(AnnotationCounterMode mode)
{
    return QString::fromLatin1(mode == AnnotationCounterMode::GroupFromFirstBubble
                                 ? kCounterModeGroup
                                 : kCounterModeHold);
}

bool AnnotationPolicy::parseCounterMode(const QString& identifier,
                                        AnnotationCounterMode* mode)
{
    if (identifier == QLatin1String(kCounterModeHold)) {
        if (mode != nullptr) {
            *mode = AnnotationCounterMode::HoldUntilToolChange;
        }
        return true;
    }
    if (identifier == QLatin1String(kCounterModeGroup)) {
        if (mode != nullptr) {
            *mode = AnnotationCounterMode::GroupFromFirstBubble;
        }
        return true;
    }
    return false;
}

AnnotationCounterMode AnnotationPolicy::defaultCounterMode()
{
    return AnnotationCounterMode::HoldUntilToolChange;
}

bool AnnotationPolicy::isValidHoldMs(int holdMs)
{
    return holdMs == kInherit || (holdMs >= 0 && holdMs <= kMaxHoldMs);
}

bool AnnotationPolicy::isValidFadeMs(int fadeMs)
{
    return fadeMs == kInherit || (fadeMs >= 0 && fadeMs <= kMaxFadeMs);
}

bool AnnotationPolicy::isValidSize(int size)
{
    return size == kInherit || (size >= kMinSize && size <= kMaxSize);
}

int AnnotationPolicy::resolveHoldMs(int configured, int globalHoldMs)
{
    return isValidHoldMs(configured) && configured != kInherit ? configured
                                                               : globalHoldMs;
}

int AnnotationPolicy::resolveFadeMs(int configured, int globalFadeMs)
{
    return isValidFadeMs(configured) && configured != kInherit ? configured
                                                               : globalFadeMs;
}

int AnnotationPolicy::resolveSize(int configured, int drawingDefault)
{
    return isValidSize(configured) && configured != kInherit ? configured
                                                             : drawingDefault;
}

AnnotationPolicy AnnotationPolicy::defaults(int globalHoldMs, int globalFadeMs)
{
    AnnotationPolicy policy;
    policy.m_settings.clear();
    policy.m_settings.reserve(kToolCount);
    for (int i = 0; i < kToolCount; ++i) {
        AnnotationToolSettings settings;
        settings.configured = { kInherit, kInherit };
        settings.configuredSize = kInherit;
        policy.m_settings.append(settings);
    }
    policy.m_counterMode = defaultCounterMode();
    policy.refreshResolved(globalHoldMs, globalFadeMs);
    return policy;
}

AnnotationPolicy AnnotationPolicy::load(const AnnotationStore& store,
                                       int globalHoldMs,
                                       int globalFadeMs)
{
    AnnotationPolicy policy = defaults(globalHoldMs, globalFadeMs);

    const auto readInt = [&store, &policy](const QString& optionKey) -> int {
        if (optionKey.isEmpty() || !store.isAvailable(optionKey)) {
            if (!optionKey.isEmpty()) {
                policy.m_unavailableKeys.append(optionKey);
            }
            return kInherit;
        }
        bool ok = false;
        const int value = store.read(optionKey).toInt(&ok);
        // A registered option is range-checked by the configuration layer, but
        // a hand-edited file must not reach the overlay as a nonsense duration.
        return ok ? value : kInherit;
    };

    for (int index = 0; index < policy.m_settings.size(); ++index) {
        const AnnotationTool tool = kTools[index].tool;
        AnnotationToolSettings& settings = policy.m_settings[index];
        const int hold = readInt(key(tool, AnnotationOption::HoldMs));
        const int fade = readInt(key(tool, AnnotationOption::FadeMs));
        settings.configured.holdMs = isValidHoldMs(hold) ? hold : kInherit;
        settings.configured.fadeMs = isValidFadeMs(fade) ? fade : kInherit;
        if (kTools[index].sizeKind != AnnotationSizeKind::None) {
            const int size = readInt(key(tool, AnnotationOption::Size));
            settings.configuredSize = isValidSize(size) ? size : kInherit;
        }
    }

    const QString modeKey = counterModeKey();
    if (store.isAvailable(modeKey)) {
        AnnotationCounterMode mode = defaultCounterMode();
        if (parseCounterMode(store.read(modeKey).toString(), &mode)) {
            policy.m_counterMode = mode;
        }
    } else {
        policy.m_unavailableKeys.append(modeKey);
    }

    policy.refreshResolved(globalHoldMs, globalFadeMs);
    return policy;
}

void AnnotationPolicy::refreshResolved(int globalHoldMs, int globalFadeMs)
{
    m_globalHoldMs = resolveHoldMs(globalHoldMs, kDefaultHoldMs);
    m_globalFadeMs = resolveFadeMs(globalFadeMs, kDefaultFadeMs);
    for (auto& settings : m_settings) { // NOLINT(altera-unroll-loops)
        settings.resolved.holdMs =
          resolveHoldMs(settings.configured.holdMs, int(m_globalHoldMs));
        settings.resolved.fadeMs =
          resolveFadeMs(settings.configured.fadeMs, int(m_globalFadeMs));
    }
}

const AnnotationToolSettings& AnnotationPolicy::settings(
  AnnotationTool tool) const
{
    const int index = toolIndex(tool);
    static const AnnotationToolSettings unset;
    if (index < 0 || index >= m_settings.size()) {
        return unset;
    }
    return m_settings[index];
}

AnnotationTiming AnnotationPolicy::timing(AnnotationTool tool) const
{
    return settings(tool).resolved;
}

AnnotationTiming AnnotationPolicy::effectiveTiming(AnnotationTool tool) const
{
    const AnnotationTiming configured = timing(tool);
    if (tool == AnnotationTool::Counter &&
        m_counterMode == AnnotationCounterMode::HoldUntilToolChange) {
        // The bubbles already had their hold: they stayed at full strength for
        // as long as the counter tool was selected. Only the fade is left.
        return { 0, configured.fadeMs };
    }
    return configured;
}

int AnnotationPolicy::configuredSize(AnnotationTool tool) const
{
    return settings(tool).configuredSize;
}

AnnotationCounterMode AnnotationPolicy::counterMode() const
{
    return m_counterMode;
}

QList<QString> AnnotationPolicy::unavailableKeys() const
{
    return m_unavailableKeys;
}

bool AnnotationPolicy::isFullyAvailable() const
{
    return m_unavailableKeys.isEmpty();
}

bool AnnotationPolicy::setHoldMs(AnnotationStore& store,
                                 AnnotationTool tool,
                                 int holdMs)
{
    if (!isValidHoldMs(holdMs)) {
        return false;
    }
    const QString optionKey = key(tool, AnnotationOption::HoldMs);
    if (optionKey.isEmpty() || !store.isAvailable(optionKey)) {
        return false;
    }
    store.write(optionKey, holdMs);
    return true;
}

bool AnnotationPolicy::setFadeMs(AnnotationStore& store,
                                 AnnotationTool tool,
                                 int fadeMs)
{
    if (!isValidFadeMs(fadeMs)) {
        return false;
    }
    const QString optionKey = key(tool, AnnotationOption::FadeMs);
    if (optionKey.isEmpty() || !store.isAvailable(optionKey)) {
        return false;
    }
    store.write(optionKey, fadeMs);
    return true;
}

bool AnnotationPolicy::setTiming(AnnotationStore& store,
                                 AnnotationTool tool,
                                 const AnnotationTiming& configured)
{
    if (!isValidHoldMs(configured.holdMs) ||
        !isValidFadeMs(configured.fadeMs)) {
        return false;
    }
    if (!store.isAvailable(key(tool, AnnotationOption::HoldMs)) ||
        !store.isAvailable(key(tool, AnnotationOption::FadeMs))) {
        return false;
    }
    const bool holdWritten = setHoldMs(store, tool, configured.holdMs);
    const bool fadeWritten = setFadeMs(store, tool, configured.fadeMs);
    return holdWritten && fadeWritten;
}

bool AnnotationPolicy::setSize(AnnotationStore& store,
                               AnnotationTool tool,
                               int size)
{
    if (!isValidSize(size) || sizeKind(tool) == AnnotationSizeKind::None) {
        return false;
    }
    const QString optionKey = key(tool, AnnotationOption::Size);
    if (optionKey.isEmpty() || !store.isAvailable(optionKey)) {
        return false;
    }
    store.write(optionKey, size);
    return true;
}

bool AnnotationPolicy::setCounterMode(AnnotationStore& store,
                                      AnnotationCounterMode mode)
{
    const QString modeKey = counterModeKey();
    if (!store.isAvailable(modeKey)) {
        return false;
    }
    store.write(modeKey, counterModeIdentifier(mode));
    return true;
}

bool AnnotationPolicy::resetTool(AnnotationStore& store, AnnotationTool tool)
{
    bool removedAny = false;
    for (const auto option :
         { AnnotationOption::HoldMs, AnnotationOption::FadeMs,
           AnnotationOption::Size }) {
        const QString optionKey = key(tool, option);
        if (optionKey.isEmpty() || !store.isAvailable(optionKey)) {
            continue;
        }
        store.remove(optionKey);
        removedAny = true;
    }
    return removedAny;
}

bool AnnotationPolicy::resetCounterMode(AnnotationStore& store)
{
    const QString modeKey = counterModeKey();
    if (!store.isAvailable(modeKey)) {
        return false;
    }
    store.remove(modeKey);
    return true;
}

qint64 AnnotationPolicy::stampFor(AnnotationTool tool,
                                  AnnotationCounterMode mode,
                                  qint64 ownStampMs,
                                  qint64 counterGroupStampMs)
{
    if (tool == AnnotationTool::Counter &&
        mode == AnnotationCounterMode::GroupFromFirstBubble &&
        counterGroupStampMs >= 0) {
        return counterGroupStampMs;
    }
    return ownStampMs;
}

} // namespace Recording
