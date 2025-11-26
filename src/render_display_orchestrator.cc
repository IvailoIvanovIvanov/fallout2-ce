#include "render_display_orchestrator.h"

#include <algorithm>
#include <array>
#include <cmath>
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

struct HdSamplingContext {
    const uint32_t* base = nullptr;
    int stride = 0;
    int textureWidth = 0;
    int textureHeight = 0;
    double texelOriginX = 0.0;
    double texelOriginY = 0.0;
    double texelsPerLogicalX = 1.0;
    double texelsPerLogicalY = 1.0;
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

static inline double computeSampleCoordinate(double blockSpan, int spanIndex, int spanLength)
{
    if (blockSpan <= 0.0 || spanLength <= 0) {
        return 0.0;
    }

    const double block = blockSpan;
    const double length = static_cast<double>(std::max(spanLength, 1));
    return (static_cast<double>(spanIndex) + 0.5) * (block / length) - 0.5;
}

uint32_t sampleHdPixelBilinear(const HdSamplingContext& context,
    int logicalRow,
    int logicalColumn,
    int spanRow,
    int rowSpanLength,
    int spanColumn,
    int columnSpanLength)
{
    if (context.base == nullptr || context.stride <= 0 || context.textureWidth <= 0 || context.textureHeight <= 0) {
        return 0;
    }

    const double localX = context.texelOriginX
        + static_cast<double>(logicalColumn) * context.texelsPerLogicalX
        + computeSampleCoordinate(context.texelsPerLogicalX, spanColumn, columnSpanLength);
    const double localY = context.texelOriginY
        + static_cast<double>(logicalRow) * context.texelsPerLogicalY
        + computeSampleCoordinate(context.texelsPerLogicalY, spanRow, rowSpanLength);

    const double maxX = static_cast<double>(std::max(context.textureWidth - 1, 0));
    const double maxY = static_cast<double>(std::max(context.textureHeight - 1, 0));
    const double clampedX = std::clamp(localX, 0.0, maxX);
    const double clampedY = std::clamp(localY, 0.0, maxY);

    const int x0 = static_cast<int>(std::floor(clampedX));
    const int y0 = static_cast<int>(std::floor(clampedY));
    const int x1 = std::min(x0 + 1, context.textureWidth - 1);
    const int y1 = std::min(y0 + 1, context.textureHeight - 1);

    const double fx = clampedX - static_cast<double>(x0);
    const double fy = clampedY - static_cast<double>(y0);

    const uint32_t c00 = context.base[y0 * context.stride + x0];
    const uint32_t c10 = context.base[y0 * context.stride + x1];
    const uint32_t c01 = context.base[y1 * context.stride + x0];
    const uint32_t c11 = context.base[y1 * context.stride + x1];

    const auto lerp = [](double a, double b, double t) {
        return a + (b - a) * t;
    };

    const auto component = [](uint32_t color, int shift) {
        return static_cast<double>((color >> shift) & 0xFF);
    };

    const double aTop = lerp(component(c00, 24), component(c10, 24), fx);
    const double aBottom = lerp(component(c01, 24), component(c11, 24), fx);
    const double rTop = lerp(component(c00, 16), component(c10, 16), fx);
    const double rBottom = lerp(component(c01, 16), component(c11, 16), fx);
    const double gTop = lerp(component(c00, 8), component(c10, 8), fx);
    const double gBottom = lerp(component(c01, 8), component(c11, 8), fx);
    const double bTop = lerp(component(c00, 0), component(c10, 0), fx);
    const double bBottom = lerp(component(c01, 0), component(c11, 0), fx);

    const double a = lerp(aTop, aBottom, fy);
    const double r = lerp(rTop, rBottom, fy);
    const double g = lerp(gTop, gBottom, fy);
    const double b = lerp(bTop, bBottom, fy);

    const auto clampComponent = [](double value) -> uint32_t {
        long component = static_cast<long>(std::lround(value));
        if (component < 0) {
            component = 0;
        } else if (component > 255) {
            component = 255;
        }
        return static_cast<uint32_t>(component);
    };

    return (clampComponent(a) << 24)
        | (clampComponent(r) << 16)
        | (clampComponent(g) << 8)
        | clampComponent(b);
}

bool blitToPhysicalOverlay(const TileOverlayTarget& target,
    const HdTrueColorFrameView& view,
    const Rect& logicalRect,
    int sourceOffsetX,
    int sourceOffsetY,
    int width,
    int height,
    const TileLightingSampler& lighting,
    const HdSamplingContext& samplingContext)
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
                    const uint32_t hdPixel = sampleHdPixelBilinear(samplingContext,
                        logicalRow,
                        logicalColumn,
                        spanRow,
                        rowSpanHeight,
                        spanColumn,
                        columnSpanWidth);
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
    double& maxHdScale)
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

    const int frameWidth = view.logicalWidth;
    const int frameHeight = view.logicalHeight;

    const double fallbackScaleX = frameWidth > 0 ? static_cast<double>(view.width) / static_cast<double>(frameWidth) : 1.0;
    const double fallbackScaleY = frameHeight > 0 ? static_cast<double>(view.height) / static_cast<double>(frameHeight) : 1.0;
    const double texelsPerLogicalX = view.texelsPerLogicalX > 0.0 ? view.texelsPerLogicalX : fallbackScaleX;
    const double texelsPerLogicalY = view.texelsPerLogicalY > 0.0 ? view.texelsPerLogicalY : fallbackScaleY;
    const double effectiveScale = std::max(texelsPerLogicalX, texelsPerLogicalY);

    maxHdScale = std::max(maxHdScale, effectiveScale);
    if (viewportScale > 0.0 && effectiveScale > viewportScale + 0.01) {
        detailClamped = true;
    }

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

    HdSamplingContext samplingContext;
    samplingContext.base = view.pixels;
    samplingContext.stride = view.width;
    samplingContext.textureWidth = view.width;
    samplingContext.textureHeight = view.height;
    samplingContext.texelsPerLogicalX = texelsPerLogicalX;
    samplingContext.texelsPerLogicalY = texelsPerLogicalY;
    samplingContext.texelOriginX = view.texelOriginX + texelsPerLogicalX * static_cast<double>(sampledOffsetX);
    samplingContext.texelOriginY = view.texelOriginY + texelsPerLogicalY * static_cast<double>(sampledOffsetY);

    if (samplingContext.base == nullptr || samplingContext.stride <= 0 || samplingContext.textureWidth <= 0 || samplingContext.textureHeight <= 0) {
        return false;
    }

    if (!blitToPhysicalOverlay(target, view, clippedRect, sampledOffsetX, sampledOffsetY, copyWidth, copyHeight, lighting, samplingContext)) {
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
    double maxHdScale = 1.0;

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
            "orchestrator frame=%u commands=%zu hd=%u fallback=%u logical_px=%llu physical_scale=%.2f detail_clamped=%d hd_scale_max=%.2f",
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
