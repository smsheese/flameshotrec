// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include "annotationpolicy.h"
#include "tools/capturetool.h"
#include "utils/confighandler.h"

namespace Recording {

// Adapter that stores the annotation policy in the application configuration.
//
// Every read and write goes through ConfigHandler's generic value()/setValue(),
// so its value validation, its "file changed" bookkeeping and - most
// importantly - its unrecognised-setting guard stay in force. A key that has
// not been registered in ConfigHandler's option table is reported as
// unavailable and never written behind its back; see the handoff note
// `tool-config-hooks.md` for the registration these keys need.
class ConfigHandlerAnnotationStore final : public AnnotationStore
{
public:
    bool isAvailable(const QString& key) const override
    {
        return ConfigHandler::recognizedGeneralOptions().contains(key);
    }

    QVariant read(const QString& key) const override
    {
        return ConfigHandler::getInstance()->value(key);
    }

    bool write(const QString& key, const QVariant& value) override
    {
        if (!isAvailable(key)) {
            return false;
        }
        ConfigHandler::getInstance()->setValue(key, value);
        return true;
    }

    void remove(const QString& key) override
    {
        if (isAvailable(key)) {
            ConfigHandler::getInstance()->remove(key);
        }
    }
};

// The capture tool behind a policy tool. Used to look up a tool's existing
// drawing default (size, colour, ...) through ConfigHandler::toolSize().
inline CaptureTool::Type captureToolType(AnnotationTool tool)
{
    switch (tool) {
        case AnnotationTool::Pen:
            return CaptureTool::TYPE_PENCIL;
        case AnnotationTool::Line:
            return CaptureTool::TYPE_DRAWER;
        case AnnotationTool::Arrow:
            return CaptureTool::TYPE_ARROW;
        case AnnotationTool::Box:
            return CaptureTool::TYPE_RECTANGLE;
        case AnnotationTool::Circle:
            return CaptureTool::TYPE_CIRCLE;
        case AnnotationTool::Marker:
            return CaptureTool::TYPE_MARKER;
        case AnnotationTool::Text:
            return CaptureTool::TYPE_TEXT;
        case AnnotationTool::Selection:
            return CaptureTool::TYPE_SELECTION;
        case AnnotationTool::Counter:
            return CaptureTool::TYPE_CIRCLECOUNT;
        case AnnotationTool::Invert:
            return CaptureTool::TYPE_INVERT;
        case AnnotationTool::Pixelate:
            return CaptureTool::TYPE_PIXELATE;
    }
    return CaptureTool::NONE;
}

// Size a tool falls back to while its own size is left on "Default": the value
// the drawing tools already use for that tool (drawThickness, drawFontSize,
// drawMarkerSize, drawPixelateSize or drawCircleCounterSize).
inline int drawingDefaultSize(AnnotationTool tool)
{
    if (AnnotationPolicy::sizeKind(tool) == AnnotationSizeKind::None) {
        return 0;
    }
    return ConfigHandler::getInstance()->toolSize(captureToolType(tool));
}

// Snapshot of the policy as the overlay should apply it right now: the global
// recording hold/fade are the values every unset per-tool entry inherits.
inline AnnotationPolicy loadAnnotationPolicyFromConfig()
{
    ConfigHandlerAnnotationStore store;
    ConfigHandler* config = ConfigHandler::getInstance();
    return AnnotationPolicy::load(store,
                                  config->recordingHoldDuration(),
                                  config->recordingFadeDuration());
}

// Size the overlay must draw `tool` with: the per-tool value, or the existing
// drawing default when the tool is left on "Default".
inline int resolvedDrawingSize(const AnnotationPolicy& policy,
                               AnnotationTool tool)
{
    return AnnotationPolicy::resolveSize(policy.configuredSize(tool),
                                         drawingDefaultSize(tool));
}

} // namespace Recording
