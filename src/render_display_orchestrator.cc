#include "render_display_orchestrator.h"

#include <algorithm>
#include <array>
#include <limits>

#include "color.h"
#include "diagnostics.h"
#include "display_scaler.h"
#include "render_asset_registry.h"
#include "render_commands.h"
#include "settings.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {
namespace {

struct TileOverlayTarget {
    int windowId = -1;
    Rect windowRect {};
    WindowPhysicalTrueColorBuffer physical {};
};

class TileLightingSampler {
public:
    bool initialize(const RenderCommandTileBlit& command,
        int sourceOffsetX,
        int sourceOffsetY,
        int width,
        int height)
    {
        width_ = width;
        height_ = height;
        stride_ = kRenderCommandTileIntensityMapStride;
        flatIntensity_ = static_cast<uint8_t>(std::clamp(static_cast<int>(command.payload.lighting), 0, 255));

        if ((command.payload.flags & RenderCommandFlag_LightingPerPixel) == 0) {
            perPixel_ = false;
            return true;
        }

        if (width <= 0 || height <= 0) {
            perPixel_ = false;
            return false;
        }

        perPixel_ = true;
        intensityMap_.fill(0);
        if (!renderCommandBuildPerPixelIntensityMap(command.payload, intensityMap_)) {
            perPixel_ = false;
            return false;
        }

        startIndex_ = kRenderCommandTileIntensityMapBaseOffset + kRenderCommandTileIntensityMapStride * sourceOffsetY + sourceOffsetX;
        maxIndex_ = startIndex_ + kRenderCommandTileIntensityMapStride * (height - 1) + (width - 1);
        if (startIndex_ < 0 || maxIndex_ >= static_cast<int>(intensityMap_.size())) {
            perPixel_ = false;
            return false;
        }

        return true;
    }

    int sample(int row, int column) const
    {
        if (!perPixel_) {
            return flatIntensity_;
        }

        const int index = startIndex_ + row * stride_ + column;
        if (index < startIndex_ || index > maxIndex_ || index < 0 || index >= static_cast<int>(intensityMap_.size())) {
            return flatIntensity_;
        }

        return std::clamp(intensityMap_[index] >> 9, 0, 255);
    }

private:
    bool perPixel_ = false;
    uint8_t flatIntensity_ = 0;
    int startIndex_ = 0;
    int maxIndex_ = -1;
    int stride_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::array<int, kRenderCommandTileIntensityMapSize> intensityMap_ {};
};

uint16_t sLastTileFrameIndex = std::numeric_limits<uint16_t>::max();
uint16_t sLastViewportEventFrameIndex = std::numeric_limits<uint16_t>::max();
size_t sProcessedViewportEventCount = 0;
bool sPendingFullClear = true;
bool sForceRedraw = true;
bool sBlackoutActive = false;
bool sOrchestratorActive = false;

void processViewportEvents();
void handleViewportEvent(const RenderViewportEvent& event);

static inline int selectSampleIndex(int position, int spanLength, int sampleCount)
{
    if (sampleCount <= 1 || spanLength <= 0) {
        return 0;
    }

    if (spanLength == 1) {
        return 0;
    }

    const int numerator = (2 * position + 1) * sampleCount;
    const int denominator = 2 * spanLength;
    int index = numerator / denominator;
    return std::clamp(index, 0, sampleCount - 1);
}

bool acquireTileOverlayTarget(TileOverlayTarget& outTarget)
{
    outTarget = {};

    int windowId = tileGetWindowId();
    if (windowId == -1) {
        return false;
    }

    if (windowGetRect(windowId, &outTarget.windowRect) == -1) {
        return false;
    }

    if (!windowGetPhysicalTrueColorOverlay(windowId, &outTarget.physical)) {
        return false;
    }

    outTarget.windowId = windowId;
    return true;
}

bool blitToPhysicalOverlay(const TileOverlayTarget& target,
    const HdTrueColorFrameView& view,
    const Rect& logicalRect,
    int sourceOffsetX,
    int sourceOffsetY,
    int width,
    int height,
    const TileLightingSampler& lighting)
{
    if (target.physical.pixels == nullptr || target.physical.mask == nullptr || target.physical.pitch <= 0 || width <= 0 || height <= 0) {
        return false;
    }

    const DisplayScalerScaleTable& scaleTable = displayScalerGetScaleTable();
    if (!scaleTable.valid) {
        return false;
    }

    const int horizontalLimit = static_cast<int>(scaleTable.horizontal.starts.size());
    const int verticalLimit = static_cast<int>(scaleTable.vertical.starts.size());
    if (horizontalLimit <= 0 || verticalLimit <= 0) {
        return false;
    }

    const int hdScaleX = std::max(1, view.scaleX);
    const int hdScaleY = std::max(1, view.scaleY);
    const int hdStride = view.width;
    if (hdStride <= 0 || view.pixels == nullptr) {
        return false;
    }

    const uint32_t* hdBase = view.pixels + (sourceOffsetY * hdScaleY) * hdStride + sourceOffsetX * hdScaleX;

    bool wrotePixels = false;

    for (int logicalRow = 0; logicalRow < height; logicalRow++) {
        const int logicalY = logicalRect.top + logicalRow;
        if (logicalY < 0 || logicalY >= verticalLimit) {
            continue;
        }

        int physicalRowStart = scaleTable.vertical.starts[logicalY] - target.physical.viewport.top;
        int physicalRowEnd = scaleTable.vertical.ends[logicalY] - target.physical.viewport.top;
        if (physicalRowEnd < physicalRowStart) {
            continue;
        }

        physicalRowStart = std::max(physicalRowStart, 0);
        physicalRowEnd = std::min(physicalRowEnd, target.physical.height - 1);
        if (physicalRowStart > physicalRowEnd) {
            continue;
        }

        const int rowSpanHeight = physicalRowEnd - physicalRowStart + 1;
        for (int spanRow = 0; spanRow < rowSpanHeight; spanRow++) {
            const int physicalRow = physicalRowStart + spanRow;
            const int hdRowOffset = selectSampleIndex(spanRow, rowSpanHeight, hdScaleY);
            const uint32_t* hdRow = hdBase + (logicalRow * hdScaleY + hdRowOffset) * hdStride;
            uint32_t* destRow = target.physical.pixels + physicalRow * target.physical.pitch;
            unsigned char* maskRow = target.physical.mask + physicalRow * target.physical.pitch;

            for (int logicalColumn = 0; logicalColumn < width; logicalColumn++) {
                const int logicalX = logicalRect.left + logicalColumn;
                if (logicalX < 0 || logicalX >= horizontalLimit) {
                    continue;
                }

                int physicalColumnStart = scaleTable.horizontal.starts[logicalX] - target.physical.viewport.left;
                int physicalColumnEnd = scaleTable.horizontal.ends[logicalX] - target.physical.viewport.left;
                if (physicalColumnEnd < physicalColumnStart) {
                    continue;
                }

                physicalColumnStart = std::max(physicalColumnStart, 0);
                physicalColumnEnd = std::min(physicalColumnEnd, target.physical.width - 1);
                if (physicalColumnStart > physicalColumnEnd) {
                    continue;
                }

                const int columnSpanWidth = physicalColumnEnd - physicalColumnStart + 1;
                const int intensityIndex = lighting.sample(logicalRow, logicalColumn);

                for (int spanColumn = 0; spanColumn < columnSpanWidth; spanColumn++) {
                    const int hdColumnOffset = selectSampleIndex(spanColumn, columnSpanWidth, hdScaleX);
                    const uint32_t hdPixel = hdRow[logicalColumn * hdScaleX + hdColumnOffset];
                    const uint8_t alpha = static_cast<uint8_t>(hdPixel >> 24);
                    uint32_t* destPixel = destRow + physicalColumnStart + spanColumn;
                    unsigned char* maskPixel = maskRow + physicalColumnStart + spanColumn;

                    if (alpha == 0) {
                        *destPixel = 0;
                        *maskPixel = 0;
                        continue;
                    }

                    *destPixel = colorApplyLightingToArgb(hdPixel, intensityIndex);
                    *maskPixel = 1;
                    wrotePixels = true;
                }
            }
        }
    }

    return wrotePixels;
}

bool processTileCommand(const RenderCommandTileBlit& command,
    const TileOverlayTarget& target,
    uint64_t& logicalPixelsDrawn,
    bool& usedFallback,
    double viewportScale,
    bool& detailClamped,
    int& maxHdScale)
{
    if (command.op != RenderCommandOp::TileBlit && command.op != RenderCommandOp::RoofBlit) {
        return false;
    }

    Rect screenRect;
    rectCopy(&screenRect, &command.payload.screenRect);
    Rect clippedRect;
    rectCopy(&clippedRect, &screenRect);
    if (rectIntersection(&clippedRect, &target.windowRect, &clippedRect) == -1) {
        return false;
    }

    const int blitWidth = rectGetWidth(&clippedRect);
    const int blitHeight = rectGetHeight(&clippedRect);
    if (blitWidth <= 0 || blitHeight <= 0) {
        return false;
    }

    HdTrueColorFrameView view;
    bool hdIsFallback = false;
    if (!renderAssetRegistryGetHdView(command.payload.asset, view, &hdIsFallback)) {
        return false;
    }

    if (view.pixels == nullptr || view.logicalWidth <= 0 || view.logicalHeight <= 0) {
        return false;
    }

    maxHdScale = std::max(maxHdScale, view.scaleX);
    if (viewportScale > 0.0 && static_cast<double>(view.scaleX) > viewportScale + 0.01) {
        detailClamped = true;
    }

    const int frameWidth = view.logicalWidth;
    const int frameHeight = view.logicalHeight;

    int baseOffsetX = std::clamp(static_cast<int>(command.payload.sourceOffsetX), 0, std::max(frameWidth - 1, 0));
    int baseOffsetY = std::clamp(static_cast<int>(command.payload.sourceOffsetY), 0, std::max(frameHeight - 1, 0));
    int sourceWidth = std::min(static_cast<int>(command.payload.sourceWidth), frameWidth - baseOffsetX);
    int sourceHeight = std::min(static_cast<int>(command.payload.sourceHeight), frameHeight - baseOffsetY);
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        return false;
    }

    const int clipOffsetX = clippedRect.left - screenRect.left;
    const int clipOffsetY = clippedRect.top - screenRect.top;
    if (clipOffsetX >= sourceWidth || clipOffsetY >= sourceHeight) {
        return false;
    }

    const int sampledOffsetX = baseOffsetX + clipOffsetX;
    const int sampledOffsetY = baseOffsetY + clipOffsetY;
    int copyWidth = std::min(blitWidth, sourceWidth - clipOffsetX);
    int copyHeight = std::min(blitHeight, sourceHeight - clipOffsetY);
    if (copyWidth <= 0 || copyHeight <= 0) {
        return false;
    }

    TileLightingSampler lighting;
    lighting.initialize(command, sampledOffsetX, sampledOffsetY, copyWidth, copyHeight);

    if (!blitToPhysicalOverlay(target, view, clippedRect, sampledOffsetX, sampledOffsetY, copyWidth, copyHeight, lighting)) {
        return false;
    }

    logicalPixelsDrawn += static_cast<uint64_t>(copyWidth) * static_cast<uint64_t>(copyHeight);
    usedFallback = hdIsFallback;
    return true;
}

void processViewportEvents()
{
    RenderViewportEventBufferView eventView;
    if (!renderCommandsPeekViewportEvents(eventView)) {
        return;
    }

    if (eventView.frameIndex != sLastViewportEventFrameIndex) {
        sLastViewportEventFrameIndex = eventView.frameIndex;
        sProcessedViewportEventCount = 0;
    }

    if (eventView.count <= sProcessedViewportEventCount) {
        return;
    }

    for (size_t index = sProcessedViewportEventCount; index < eventView.count; index++) {
        handleViewportEvent(eventView.events[index]);
    }

    sProcessedViewportEventCount = eventView.count;
}

void handleViewportEvent(const RenderViewportEvent& event)
{
    switch (event.payload.type) {
    case RenderViewportEventType::Scroll:
    case RenderViewportEventType::Resize:
        sPendingFullClear = true;
        sForceRedraw = true;
        break;
    case RenderViewportEventType::FadeIn:
        sBlackoutActive = false;
        sPendingFullClear = true;
        sForceRedraw = true;
        break;
    case RenderViewportEventType::FadeOut:
    case RenderViewportEventType::Blackout:
        sBlackoutActive = true;
        sPendingFullClear = true;
        sForceRedraw = true;
        break;
    default:
        break;
    }

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "orchestrator_event type=%d dirty_seq=%u frame=%u",
            static_cast<int>(event.payload.type),
            event.payload.dirtySequence,
            event.header.frameIndex);
    }
}

} // namespace

bool renderDisplayOrchestratorEnabled()
{
    if (renderCommandDirectBlitFallbackActive()) {
        return false;
    }

    return settings.system.render_display_orchestrator && settings.system.virtual_adapter && settings.system.virtual_adapter_fullres;
}

bool renderDisplayOrchestratorConsumesTileOverlays()
{
    return renderDisplayOrchestratorEnabled();
}

void renderDisplayOrchestratorProcess()
{
    if (!renderDisplayOrchestratorEnabled()) {
        sOrchestratorActive = false;
        return;
    }

    if (!sOrchestratorActive) {
        sOrchestratorActive = true;
        sPendingFullClear = true;
        sForceRedraw = true;
        sBlackoutActive = false;
        sLastTileFrameIndex = std::numeric_limits<uint16_t>::max();
        sLastViewportEventFrameIndex = std::numeric_limits<uint16_t>::max();
        sProcessedViewportEventCount = 0;
    }

    processViewportEvents();

    RenderCommandBufferView bufferView;
    if (!renderCommandsPeekTileCommands(bufferView)) {
        return;
    }

    if (bufferView.count == 0 && !sPendingFullClear && !sBlackoutActive) {
        return;
    }

    TileOverlayTarget target;
    if (!acquireTileOverlayTarget(target)) {
        return;
    }

    const int windowWidth = windowGetWidth(target.windowId);
    const int windowHeight = windowGetHeight(target.windowId);
    if (windowWidth <= 0 || windowHeight <= 0) {
        return;
    }

    const DisplayScalerScaleTable& scaleTable = displayScalerGetScaleTable();
    const double viewportScale = scaleTable.valid ? scaleTable.scale : displayScalerGetScale();

    if (sPendingFullClear || sBlackoutActive) {
        windowClearTrueColorRegion(target.windowId, 0, 0, windowWidth, windowHeight);
        sPendingFullClear = false;
    }

    if (bufferView.count == 0) {
        return;
    }

    if (bufferView.frameIndex == sLastTileFrameIndex && !sForceRedraw) {
        return;
    }
    sLastTileFrameIndex = bufferView.frameIndex;
    sForceRedraw = false;

    if (sBlackoutActive) {
        return;
    }

    uint64_t logicalPixelsDrawn = 0;
    uint32_t hdCommands = 0;
    uint32_t fallbackCommands = 0;
    bool detailClamped = false;
    int maxHdScale = 1;

    for (size_t index = 0; index < bufferView.count; index++) {
        const RenderCommandTileBlit& command = bufferView.commands[index];
        bool usedFallback = false;
        if (processTileCommand(command, target, logicalPixelsDrawn, usedFallback, viewportScale, detailClamped, maxHdScale)) {
            if (usedFallback) {
                fallbackCommands++;
            } else {
                hdCommands++;
            }
        }
    }

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "orchestrator frame=%u commands=%zu hd=%u fallback=%u logical_px=%llu physical_scale=%.2f detail_clamped=%d hd_scale_max=%d",
            bufferView.frameIndex,
            bufferView.count,
            hdCommands,
            fallbackCommands,
            static_cast<unsigned long long>(logicalPixelsDrawn),
            viewportScale,
            detailClamped ? 1 : 0,
            maxHdScale);
    }
}

} // namespace fallout
