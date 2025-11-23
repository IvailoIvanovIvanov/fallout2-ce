#include "render_trace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "diagnostics.h"
#include "display_scaler.h"
#include "draw.h"
#include "kb.h"
#include "window_manager.h"

namespace fallout {
namespace {

constexpr size_t kRenderTraceCapacity = 4096;
constexpr int kReplayWindowWidth = 320;
constexpr int kReplayWindowHeight = 240;
constexpr int kReplayWindowMargin = 12;
constexpr int kRenderTraceHotkey = KEY_CTRL_F8;

struct FrameData {
    std::vector<RenderTraceOp> ops;
    bool overflow = false;
};

FrameData gCurrentFrame;
FrameData gLastFrame;
int gReplayWindowId = -1;
bool gReplayWindowVisible = false;

void ensureFrameStorage(FrameData& frame)
{
    if (frame.ops.capacity() < kRenderTraceCapacity) {
        frame.ops.reserve(kRenderTraceCapacity);
    }
}

const char* layerToString(RenderTraceLayer layer)
{
    switch (layer) {
    case RenderTraceLayer::Ui:
        return "ui";
    case RenderTraceLayer::TileFloor:
        return "floor";
    case RenderTraceLayer::TileRoof:
        return "roof";
    case RenderTraceLayer::ObjectPreRoof:
        return "object_pre";
    case RenderTraceLayer::ObjectPostRoof:
        return "object_post";
    default:
        return "unknown";
    }
}

unsigned char layerColor(RenderTraceLayer layer)
{
    switch (layer) {
    case RenderTraceLayer::Ui:
        return 31;
    case RenderTraceLayer::TileFloor:
        return 67;
    case RenderTraceLayer::TileRoof:
        return 196;
    case RenderTraceLayer::ObjectPreRoof:
        return 137;
    case RenderTraceLayer::ObjectPostRoof:
        return 215;
    default:
        return 0;
    }
}

void destroyReplayWindow()
{
    if (gReplayWindowId != -1 && gWindowSystemInitialized) {
        windowDestroy(gReplayWindowId);
    }

    gReplayWindowId = -1;
    gReplayWindowVisible = false;
}

Rect scaleRectToReplaySpace(const Rect& rect)
{
    Rect logical = displayScalerGetLogicalBounds();
    const double logicalWidth = std::max(1, rectGetWidth(&logical));
    const double logicalHeight = std::max(1, rectGetHeight(&logical));

    const double scaleX = static_cast<double>(kReplayWindowWidth) / logicalWidth;
    const double scaleY = static_cast<double>(kReplayWindowHeight) / logicalHeight;

    Rect scaled;
    scaled.left = static_cast<int>(std::lround((rect.left - logical.left) * scaleX));
    scaled.top = static_cast<int>(std::lround((rect.top - logical.top) * scaleY));
    scaled.right = scaled.left + static_cast<int>(std::lround(rectGetWidth(&rect) * scaleX)) - 1;
    scaled.bottom = scaled.top + static_cast<int>(std::lround(rectGetHeight(&rect) * scaleY)) - 1;
    return scaled;
}

void redrawReplayWindow()
{
    if (!gReplayWindowVisible || gReplayWindowId == -1) {
        return;
    }

    Window* window = windowGetWindow(gReplayWindowId);
    if (window == nullptr || window->buffer == nullptr) {
        destroyReplayWindow();
        return;
    }

    unsigned char* buffer = window->buffer;
    const int width = window->width;
    const int height = window->height;
    bufferFill(buffer, width, height, width, 0);

    Rect bufferBounds = { 0, 0, width - 1, height - 1 };
    for (size_t index = 0; index < gLastFrame.ops.size(); index++) {
        const RenderTraceOp& op = gLastFrame.ops[index];
        Rect scaled = scaleRectToReplaySpace(op.rect);
        if (rectIntersection(&scaled, &bufferBounds, &scaled) == -1) {
            continue;
        }

        bufferFill(
            buffer + width * scaled.top + scaled.left,
            rectGetWidth(&scaled),
            rectGetHeight(&scaled),
            width,
            layerColor(op.layer));
    }

    char summary[128];
    std::snprintf(
        summary,
        sizeof(summary),
        "ops=%zu%s",
        gLastFrame.ops.size(),
        gLastFrame.overflow ? " (overflow)" : "");
    windowDrawText(gReplayWindowId, summary, 0, 4, 4, 0x2000000 | 5);

    windowRefresh(gReplayWindowId);
}

void ensureReplayWindow()
{
    if (gReplayWindowVisible) {
        return;
    }

    if (!gWindowSystemInitialized) {
        return;
    }

    const Rect& logical = displayScalerGetLogicalBounds();
    const int usableWidth = std::min(rectGetWidth(&logical), kReplayWindowWidth);
    const int usableHeight = std::min(rectGetHeight(&logical), kReplayWindowHeight);

    int x = logical.left + kReplayWindowMargin;
    int y = logical.top + kReplayWindowMargin;
    if (x + usableWidth > logical.right + 1) {
        x = logical.right + 1 - usableWidth;
    }
    if (y + usableHeight > logical.bottom + 1) {
        y = logical.bottom + 1 - usableHeight;
    }

    gReplayWindowId = windowCreate(x, y, usableWidth, usableHeight, 256, WINDOW_MOVE_ON_TOP);
    if (gReplayWindowId != -1) {
        gReplayWindowVisible = true;
        windowShow(gReplayWindowId);
        redrawReplayWindow();
    }
}

} // namespace

void renderTraceRecord(RenderTraceLayer layer, int fid, int frame, int rotation, const Rect& rect, int elevation, int depthKey)
{
    if (!windowIsVirtualScreenEnabled()) {
        return;
    }

    ensureFrameStorage(gCurrentFrame);

    if (gCurrentFrame.ops.size() >= kRenderTraceCapacity) {
        gCurrentFrame.overflow = true;
        return;
    }

    RenderTraceOp op;
    op.layer = layer;
    op.fid = fid;
    op.frame = frame;
    op.rotation = rotation;
    op.rect = rect;
    op.elevation = elevation;
    op.depthKey = depthKey;
    gCurrentFrame.ops.push_back(op);

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(
            DiagnosticsLevel::Trace,
            "RENDERTRACE",
            "%s fid=%08X frame=%d rot=%d rect=(%d,%d %dx%d) elev=%d depth=%d",
            layerToString(layer),
            fid,
            frame,
            rotation,
            rect.left,
            rect.top,
            rectGetWidth(&rect),
            rectGetHeight(&rect),
            elevation,
            depthKey);
    }
}

void renderTraceCommitFrame()
{
    if (!windowIsVirtualScreenEnabled()) {
        if (!gCurrentFrame.ops.empty()) {
            gCurrentFrame.ops.clear();
        }
        gCurrentFrame.overflow = false;
        return;
    }

    ensureFrameStorage(gCurrentFrame);
    ensureFrameStorage(gLastFrame);

    gLastFrame.ops = gCurrentFrame.ops;
    gLastFrame.overflow = gCurrentFrame.overflow;
    gCurrentFrame.ops.clear();
    gCurrentFrame.overflow = false;

    if (gReplayWindowVisible) {
        redrawReplayWindow();
    }
}

bool renderTraceHandleHotkey(int keyCode)
{
    if (keyCode != kRenderTraceHotkey) {
        return false;
    }

    if (!windowIsVirtualScreenEnabled()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "RENDERTRACE", "hotkey ignored (virtual adapter disabled)");
        }
        return true;
    }

    if (gReplayWindowVisible) {
        destroyReplayWindow();
    } else {
        ensureReplayWindow();
    }

    return true;
}

void renderTraceReset()
{
    destroyReplayWindow();
    gCurrentFrame.ops.clear();
    gCurrentFrame.overflow = false;
    gLastFrame.ops.clear();
    gLastFrame.overflow = false;
}

} // namespace fallout
