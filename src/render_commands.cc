#include "render_commands.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "art.h"
#include "color.h"
#include "display_scaler.h"
#include "diagnostics.h"
#include "kb.h"
#include "object.h"
#include "platform_compat.h"
#include "settings.h"
#include "window_manager.h"

namespace fallout {
namespace {

constexpr size_t kRenderCommandTileCapacity = 4096;
constexpr size_t kRenderViewportEventCapacity = 128;
constexpr int kRenderCommandsDumpHotkey = KEY_CTRL_F9;
constexpr uint32_t kRenderCommandSerializedMagic = 'RCMD';
constexpr uint16_t kRenderCommandSerializedVersion = 2;
constexpr int kReplayLightingStep = 512;
constexpr int kTileIntensityMapBaseOffset = kRenderCommandTileIntensityMapBaseOffset;

struct TileLightingRowEntry {
    int offset;
    int length;
};

struct TileLightingTriangle {
    uint8_t a;
    uint8_t b;
    uint8_t c;
};

constexpr std::array<int, kRenderCommandMaxLightingVertices> kTileLightingVertexOffsets = {
    16,
    48,
    960,
    992,
    1024,
    1936,
    1968,
    2000,
    2912,
    2944,
};

constexpr std::array<TileLightingTriangle, 5> kRightSideUpTriangles = {
    TileLightingTriangle { 2, 3, 0 },
    TileLightingTriangle { 3, 4, 1 },
    TileLightingTriangle { 5, 6, 3 },
    TileLightingTriangle { 6, 7, 4 },
    TileLightingTriangle { 8, 9, 6 },
};

constexpr std::array<TileLightingTriangle, 5> kUpsideDownTriangles = {
    TileLightingTriangle { 0, 3, 1 },
    TileLightingTriangle { 2, 5, 3 },
    TileLightingTriangle { 3, 6, 4 },
    TileLightingTriangle { 5, 8, 6 },
    TileLightingTriangle { 6, 9, 7 },
};

constexpr std::array<TileLightingRowEntry, 13> kRightSideUpRowTable = {
    TileLightingRowEntry { -1, 2 },
    TileLightingRowEntry { 78, 2 },
    TileLightingRowEntry { 76, 6 },
    TileLightingRowEntry { 73, 8 },
    TileLightingRowEntry { 71, 10 },
    TileLightingRowEntry { 68, 14 },
    TileLightingRowEntry { 65, 16 },
    TileLightingRowEntry { 63, 18 },
    TileLightingRowEntry { 61, 20 },
    TileLightingRowEntry { 58, 24 },
    TileLightingRowEntry { 55, 26 },
    TileLightingRowEntry { 53, 28 },
    TileLightingRowEntry { 50, 32 },
};

constexpr std::array<TileLightingRowEntry, 13> kUpsideDownRowTable = {
    TileLightingRowEntry { 0, 32 },
    TileLightingRowEntry { 48, 32 },
    TileLightingRowEntry { 49, 30 },
    TileLightingRowEntry { 52, 26 },
    TileLightingRowEntry { 55, 24 },
    TileLightingRowEntry { 57, 22 },
    TileLightingRowEntry { 60, 18 },
    TileLightingRowEntry { 63, 16 },
    TileLightingRowEntry { 65, 14 },
    TileLightingRowEntry { 67, 12 },
    TileLightingRowEntry { 70, 8 },
    TileLightingRowEntry { 73, 6 },
    TileLightingRowEntry { 75, 4 },
};

std::array<RenderCommandTileBlit, kRenderCommandTileCapacity> gTileCommands;
size_t gTileCommandCount = 0;
RenderCommandStats gRenderCommandStats = {};
uint32_t gRenderCommandSequence = 0;
uint16_t gRenderCommandFrameIndex = 0;
bool gRenderCommandsInitialized = false;
std::vector<RenderCommandTileBlit> gLastFrameCommands;
RenderCommandStats gLastFrameStats = {};
uint16_t gLastFrameIndex = 0;
uint32_t gRenderCommandDumpCounter = 0;
std::vector<uint8_t> gLastFrameSerialized;
std::vector<uint8_t> gLastFrameReferenceBuffer;
int gLastFrameReferenceWidth = 0;
int gLastFrameReferenceHeight = 0;
std::vector<uint8_t> gReplayScratchBuffer;
std::vector<uint8_t> gReplayCoverageMask;
std::vector<RenderCommandTileBlit> gDecodedCommands;
std::array<RenderViewportEvent, kRenderViewportEventCapacity> gViewportEvents;
size_t gViewportEventCount = 0;
constexpr size_t kRenderCommandFallbackReasonMax = 64;
bool gRenderCommandDirectBlitFallbackActive = false;
char gRenderCommandDirectBlitFallbackReason[kRenderCommandFallbackReasonMax] = "none";

struct RenderCommandReplayStats {
    bool ran = false;
    bool matched = false;
    uint32_t commandsReplayed = 0;
    uint32_t commandsFailed = 0;
    uint32_t comparedPixels = 0;
    uint32_t mismatchedPixels = 0;
    uint32_t ignoredPixels = 0;
};

RenderCommandReplayStats gReplayStats = {};

void renderCommandsEnsureInitialized()
{
    if (!gRenderCommandsInitialized) {
        renderCommandsInit();
    }
}

void renderCommandsLogStatsIfNeeded()
{
    if (!renderCommandCaptureEnabled()) {
        return;
    }

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "command_queue stats frame=%u queued=%u dropped=%u",
            gRenderCommandFrameIndex,
            gRenderCommandStats.queued,
            gRenderCommandStats.dropped);
    }
}

void renderCommandsResetFrameCaches();
void renderCommandResetFallbackState();
void renderCommandsCaptureReferenceFrame();
void renderCommandsSerializeLastFrame();
void renderCommandsRunReplaySelfTest();
bool renderCommandsDecodeSerializedFrame(const std::vector<uint8_t>& buffer, std::vector<RenderCommandTileBlit>& out);
bool renderCommandsReplayFrame(const std::vector<RenderCommandTileBlit>& commands);
bool renderCommandsReplayTileCommand(const RenderCommandTileBlit& command, unsigned char* dest, int destPitch);
void renderCommandsReplayMarkCoverage(const Rect& rect);
bool renderCommandsClampRectToReference(Rect* rect);
void renderCommandsReplayLogStats();
bool renderCommandsReplayTileCommandFlat(const RenderCommandTileBlit& command,
    const Rect& destRect,
    unsigned char* dest,
    int destPitch,
    unsigned char* frameData,
    int frameWidth,
    int frameHeight);
bool renderCommandsReplayTileCommandPerPixel(const RenderCommandTileBlit& command,
    const Rect& destRect,
    unsigned char* dest,
    int destPitch,
    unsigned char* frameData,
    int frameWidth,
    int frameHeight);
void renderCommandsFillRightSideUpTriangles(const std::array<int32_t, kRenderCommandMaxLightingVertices>& intensities,
    std::array<int, kRenderCommandTileIntensityMapSize>& mapBuffer);
void renderCommandsFillUpsideDownTriangles(const std::array<int32_t, kRenderCommandMaxLightingVertices>& intensities,
    std::array<int, kRenderCommandTileIntensityMapSize>& mapBuffer);
void renderCommandsLogFrameDigest();
void renderCommandsEvaluateAutoFallback();

void renderCommandsResetFrameCaches()
{
    gLastFrameSerialized.clear();
    gLastFrameReferenceBuffer.clear();
    gReplayScratchBuffer.clear();
    gReplayCoverageMask.clear();
    gDecodedCommands.clear();
    gLastFrameReferenceWidth = 0;
    gLastFrameReferenceHeight = 0;
    gReplayStats = {};
}

void renderCommandResetFallbackState()
{
    gRenderCommandDirectBlitFallbackActive = false;
    std::snprintf(gRenderCommandDirectBlitFallbackReason,
        sizeof(gRenderCommandDirectBlitFallbackReason),
        "%s",
        "none");
}

} // namespace

bool renderCommandBuildPerPixelIntensityMap(const RenderCommandTileBlitPayload& payload,
    std::array<int, kRenderCommandTileIntensityMapSize>& out)
{
    if (payload.perPixelLightingCount < kRenderCommandMaxLightingVertices) {
        return false;
    }

    out.fill(0);

    std::array<int32_t, kRenderCommandMaxLightingVertices> intensities {};
    for (size_t i = 0; i < kRenderCommandMaxLightingVertices; i++) {
        intensities[i] = payload.perPixelLighting[i];
    }

    renderCommandsFillRightSideUpTriangles(intensities, out);
    renderCommandsFillUpsideDownTriangles(intensities, out);
    return true;
}

void renderCommandsInit()
{
    gTileCommandCount = 0;
    gRenderCommandStats = {};
    gRenderCommandSequence = 0;
    gRenderCommandFrameIndex = 0;
    gRenderCommandsInitialized = true;
    gLastFrameCommands.clear();
    gLastFrameStats = {};
    gLastFrameIndex = 0;
    gRenderCommandDumpCounter = 0;
    gViewportEventCount = 0;
    renderCommandsResetFrameCaches();
    renderCommandResetFallbackState();
}

bool renderCommandCaptureEnabled()
{
    if (settings.system.render_command_trace) {
        return true;
    }

    if (renderCommandDirectBlitFallbackActive()) {
        return false;
    }

    return settings.system.render_display_orchestrator;
}

const RenderCommandStats& renderCommandGetStats()
{
    return gRenderCommandStats;
}

bool renderCommandsPeekTileCommands(RenderCommandBufferView& outView)
{
    renderCommandsEnsureInitialized();

    outView.commands = gTileCommands.data();
    outView.count = gTileCommandCount;
    outView.frameIndex = gRenderCommandFrameIndex;
    return true;
}

bool renderCommandsPeekViewportEvents(RenderViewportEventBufferView& outView)
{
    renderCommandsEnsureInitialized();

    outView.events = gViewportEvents.data();
    outView.count = gViewportEventCount;
    outView.frameIndex = gRenderCommandFrameIndex;
    return true;
}

void renderCommandsBeforePresent()
{
    renderCommandsEnsureInitialized();

    if (!renderCommandCaptureEnabled()) {
        if (!gLastFrameCommands.empty()) {
            gLastFrameCommands.clear();
            gLastFrameStats = {};
        }
        renderCommandsResetFrameCaches();
        return;
    }

    renderCommandsCaptureReferenceFrame();
    renderCommandsLogStatsIfNeeded();

    gLastFrameCommands.assign(gTileCommands.begin(), gTileCommands.begin() + gTileCommandCount);
    gLastFrameStats = gRenderCommandStats;
    gLastFrameIndex = gRenderCommandFrameIndex;

    renderCommandsSerializeLastFrame();
    renderCommandsRunReplaySelfTest();
    renderCommandsLogFrameDigest();
    renderCommandsEvaluateAutoFallback();

    gRenderCommandFrameIndex++;
    gTileCommandCount = 0;
    gRenderCommandStats = {};
    gViewportEventCount = 0;
}

void renderCommandEmitTileBlit(RenderCommandOp op, const RenderCommandTileBlitPayload& payload)
{
    renderCommandsEnsureInitialized();

    if (!renderCommandCaptureEnabled()) {
        return;
    }

    gRenderCommandStats.queued++;

    if (gTileCommandCount >= gTileCommands.size()) {
        gRenderCommandStats.dropped++;
        if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
            diagnosticsLog(DiagnosticsLevel::Trace,
                "SCALER",
                "command_queue overflow capacity=%zu",
                gTileCommands.size());
        }
        return;
    }

    RenderCommandTileBlit& command = gTileCommands[gTileCommandCount++];
    command.header.sequence = ++gRenderCommandSequence;
    command.header.frameIndex = gRenderCommandFrameIndex;
    command.header.payloadSize = sizeof(RenderCommandTileBlitPayload);
    command.op = op;
    command.payload = payload;

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        const Rect& rect = command.payload.screenRect;
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "command_emit op=%d fid=%u rect=(%d,%d %dx%d) flags=0x%X lighting=%d",
            static_cast<int>(op),
            command.payload.fid,
            rect.left,
            rect.top,
            rectGetWidth(&rect),
            rectGetHeight(&rect),
            command.payload.flags,
            command.payload.lighting);
    }
}

void renderCommandEmitViewportEvent(RenderViewportEventType type,
    const Rect& rect,
    int16_t param0,
    int16_t param1,
    uint32_t dirtySequence)
{
    renderCommandsEnsureInitialized();

    if (!renderCommandCaptureEnabled()) {
        return;
    }

    if (gViewportEventCount >= gViewportEvents.size()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
            diagnosticsLog(DiagnosticsLevel::Trace,
                "SCALER",
                "viewport_event_drop type=%d dirty_seq=%u",
                static_cast<int>(type),
                dirtySequence);
        }
        return;
    }

    RenderViewportEvent& event = gViewportEvents[gViewportEventCount++];
    event.header.sequence = ++gRenderCommandSequence;
    event.header.frameIndex = gRenderCommandFrameIndex;
    event.header.payloadSize = sizeof(RenderViewportEventPayload);
    event.payload.type = type;
    event.payload.rect = rect;
    event.payload.param0 = param0;
    event.payload.param1 = param1;
    event.payload.dirtySequence = dirtySequence;

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "viewport_event type=%d seq=%u rect=(%d,%d %dx%d) params=(%d,%d) dirty_seq=%u",
            static_cast<int>(type),
            event.header.sequence,
            rect.left,
            rect.top,
            rectGetWidth(&rect),
            rectGetHeight(&rect),
            param0,
            param1,
            dirtySequence);
    }
}

namespace {

void renderCommandsCaptureReferenceFrame()
{
    if (!windowIsVirtualScreenEnabled()) {
        gLastFrameReferenceBuffer.clear();
        gLastFrameReferenceWidth = 0;
        gLastFrameReferenceHeight = 0;
        return;
    }

    unsigned char* buffer = windowGetVirtualScreenBuffer();
    int pitch = windowGetVirtualScreenPitch();
    if (buffer == nullptr || pitch <= 0) {
        gLastFrameReferenceBuffer.clear();
        gLastFrameReferenceWidth = 0;
        gLastFrameReferenceHeight = 0;
        return;
    }

    const Rect& logicalBounds = displayScalerGetLogicalBounds();
    const int width = rectGetWidth(&logicalBounds);
    const int height = rectGetHeight(&logicalBounds);

    if (width <= 0 || height <= 0) {
        gLastFrameReferenceBuffer.clear();
        gLastFrameReferenceWidth = 0;
        gLastFrameReferenceHeight = 0;
        return;
    }

    gLastFrameReferenceBuffer.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        std::memcpy(&gLastFrameReferenceBuffer[static_cast<size_t>(y) * width], buffer + y * pitch, width);
    }

    gLastFrameReferenceWidth = width;
    gLastFrameReferenceHeight = height;
}

void renderCommandsSerializeLastFrame()
{
    gLastFrameSerialized.clear();

    if (gLastFrameCommands.empty()) {
        return;
    }

    const auto appendUint8 = [](std::vector<uint8_t>& buffer, uint8_t value) {
        buffer.push_back(value);
    };

    const auto appendUint16 = [&](uint16_t value) {
        appendUint8(gLastFrameSerialized, static_cast<uint8_t>(value & 0xFF));
        appendUint8(gLastFrameSerialized, static_cast<uint8_t>((value >> 8) & 0xFF));
    };

    const auto appendUint32 = [&](uint32_t value) {
        appendUint16(static_cast<uint16_t>(value & 0xFFFF));
        appendUint16(static_cast<uint16_t>((value >> 16) & 0xFFFF));
    };

    const auto appendInt32 = [&](int32_t value) {
        appendUint32(static_cast<uint32_t>(value));
    };

    const auto appendInt16 = [&](int16_t value) {
        appendUint16(static_cast<uint16_t>(value));
    };

    appendUint32(kRenderCommandSerializedMagic);
    appendUint16(kRenderCommandSerializedVersion);
    appendUint16(gLastFrameIndex);
    appendUint16(static_cast<uint16_t>(std::min<size_t>(gLastFrameCommands.size(), 0xFFFF)));
    appendUint32(gLastFrameStats.queued);
    appendUint32(gLastFrameStats.dropped);

    for (const RenderCommandTileBlit& cmd : gLastFrameCommands) {
        appendUint32(cmd.header.sequence);
        appendUint16(cmd.header.frameIndex);
        appendUint16(cmd.header.payloadSize);
        appendUint8(gLastFrameSerialized, static_cast<uint8_t>(cmd.op));
        appendUint8(gLastFrameSerialized, 0); // reserved for future flags

        const Rect& rect = cmd.payload.screenRect;
        appendInt32(rect.left);
        appendInt32(rect.top);
        appendInt32(rect.right);
        appendInt32(rect.bottom);

        appendUint32(cmd.payload.asset.fid);
        appendUint16(cmd.payload.asset.frame);
        appendUint8(gLastFrameSerialized, cmd.payload.asset.rotation);
        appendUint8(gLastFrameSerialized, cmd.payload.asset.variant);

        appendUint32(cmd.payload.fid);
        appendInt32(cmd.payload.tileIndex);
        appendUint8(gLastFrameSerialized, cmd.payload.depthBucket);
        appendUint8(gLastFrameSerialized, cmd.payload.elevation);
        appendUint16(cmd.payload.paletteId);
        appendUint16(cmd.payload.flags);
        appendInt16(cmd.payload.lighting);
        appendInt16(cmd.payload.isoTileX);
        appendInt16(cmd.payload.isoTileY);
        appendInt16(cmd.payload.sourceOffsetX);
        appendInt16(cmd.payload.sourceOffsetY);
        appendUint16(cmd.payload.sourceWidth);
        appendUint16(cmd.payload.sourceHeight);
        appendUint8(gLastFrameSerialized, cmd.payload.perPixelLightingCount);
        appendUint8(gLastFrameSerialized, 0);
        for (size_t i = 0; i < kRenderCommandMaxLightingVertices; i++) {
            appendInt32(cmd.payload.perPixelLighting[i]);
        }
    }
}

void renderCommandsRunReplaySelfTest()
{
    if (!settings.system.render_command_replay) {
        gReplayStats = {};
        return;
    }

    if (gLastFrameSerialized.empty() || gLastFrameReferenceBuffer.empty()) {
        gReplayStats = {};
        return;
    }

    if (!renderCommandsDecodeSerializedFrame(gLastFrameSerialized, gDecodedCommands)) {
        gReplayStats = {};
        return;
    }

    renderCommandsReplayFrame(gDecodedCommands);
}

bool renderCommandsDecodeSerializedFrame(const std::vector<uint8_t>& buffer, std::vector<RenderCommandTileBlit>& out)
{
    out.clear();

    if (buffer.size() < 14) {
        return false;
    }

    size_t offset = 0;

    const auto readUint8 = [&](uint8_t& value) {
        if (offset >= buffer.size()) {
            return false;
        }
        value = buffer[offset++];
        return true;
    };

    const auto readUint16 = [&](uint16_t& value) {
        uint8_t lo = 0;
        uint8_t hi = 0;
        if (!readUint8(lo) || !readUint8(hi)) {
            return false;
        }
        value = static_cast<uint16_t>(lo | (hi << 8));
        return true;
    };

    const auto readUint32 = [&](uint32_t& value) {
        uint16_t lo = 0;
        uint16_t hi = 0;
        if (!readUint16(lo) || !readUint16(hi)) {
            return false;
        }
        value = static_cast<uint32_t>(lo | (static_cast<uint32_t>(hi) << 16));
        return true;
    };

    const auto readInt32 = [&](int32_t& value) {
        uint32_t raw = 0;
        if (!readUint32(raw)) {
            return false;
        }
        value = static_cast<int32_t>(raw);
        return true;
    };

    const auto readInt16 = [&](int16_t& value) {
        uint16_t raw = 0;
        if (!readUint16(raw)) {
            return false;
        }
        value = static_cast<int16_t>(raw);
        return true;
    };

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t frameIndex = 0;
    uint16_t commandCount = 0;
    uint32_t queued = 0;
    uint32_t dropped = 0;

    if (!readUint32(magic) || magic != kRenderCommandSerializedMagic) {
        return false;
    }

    if (!readUint16(version)) {
        return false;
    }

    if (version == 0 || version > kRenderCommandSerializedVersion) {
        return false;
    }

    const bool hasPerPixelPayload = version >= 2;

    if (!readUint16(frameIndex) || !readUint16(commandCount) || !readUint32(queued) || !readUint32(dropped)) {
        return false;
    }

    (void)queued;
    (void)dropped;
    (void)frameIndex;

    out.reserve(commandCount);

    for (uint16_t i = 0; i < commandCount; i++) {
        RenderCommandTileBlit cmd;
        uint8_t opValue = 0;
        uint8_t reserved = 0;

        if (!readUint32(cmd.header.sequence) || !readUint16(cmd.header.frameIndex) || !readUint16(cmd.header.payloadSize)) {
            return false;
        }

        if (!readUint8(opValue) || !readUint8(reserved)) {
            return false;
        }

        cmd.op = static_cast<RenderCommandOp>(opValue);

        if (!readInt32(cmd.payload.screenRect.left) || !readInt32(cmd.payload.screenRect.top) || !readInt32(cmd.payload.screenRect.right) || !readInt32(cmd.payload.screenRect.bottom)) {
            return false;
        }

        if (!readUint32(cmd.payload.asset.fid) || !readUint16(cmd.payload.asset.frame)) {
            return false;
        }

        if (!readUint8(cmd.payload.asset.rotation) || !readUint8(cmd.payload.asset.variant)) {
            return false;
        }

        if (!readUint32(cmd.payload.fid) || !readInt32(cmd.payload.tileIndex)) {
            return false;
        }

        if (!readUint8(cmd.payload.depthBucket) || !readUint8(cmd.payload.elevation)) {
            return false;
        }

        if (!readUint16(cmd.payload.paletteId) || !readUint16(cmd.payload.flags) || !readInt16(cmd.payload.lighting) || !readInt16(cmd.payload.isoTileX) || !readInt16(cmd.payload.isoTileY) || !readInt16(cmd.payload.sourceOffsetX) || !readInt16(cmd.payload.sourceOffsetY)) {
            return false;
        }

        if (!readUint16(cmd.payload.sourceWidth) || !readUint16(cmd.payload.sourceHeight)) {
            return false;
        }

        if (hasPerPixelPayload) {
            uint8_t perPixelCount = 0;
            uint8_t perPixelReserved = 0;
            if (!readUint8(perPixelCount) || !readUint8(perPixelReserved)) {
                return false;
            }
            cmd.payload.perPixelLightingCount = perPixelCount;
            for (size_t vertex = 0; vertex < kRenderCommandMaxLightingVertices; vertex++) {
                if (!readInt32(cmd.payload.perPixelLighting[vertex])) {
                    return false;
                }
            }
        } else {
            cmd.payload.perPixelLightingCount = 0;
            cmd.payload.perPixelLighting.fill(0);
        }

        out.push_back(cmd);
    }

    return true;
}

bool renderCommandsReplayFrame(const std::vector<RenderCommandTileBlit>& commands)
{
    gReplayStats = {};
    gReplayStats.ran = true;

    if (gLastFrameReferenceBuffer.empty() || gLastFrameReferenceWidth <= 0 || gLastFrameReferenceHeight <= 0) {
        return false;
    }

    const size_t pixelCount = gLastFrameReferenceBuffer.size();
    gReplayScratchBuffer.assign(pixelCount, 0);
    gReplayCoverageMask.assign(pixelCount, 0);

    unsigned char* replayBuffer = gReplayScratchBuffer.data();
    const int replayPitch = gLastFrameReferenceWidth;

    for (const RenderCommandTileBlit& cmd : commands) {
        if (cmd.op != RenderCommandOp::TileBlit && cmd.op != RenderCommandOp::RoofBlit) {
            gReplayStats.commandsFailed++;
            continue;
        }

        if (!renderCommandsReplayTileCommand(cmd, replayBuffer, replayPitch)) {
            gReplayStats.commandsFailed++;
            continue;
        }

        gReplayStats.commandsReplayed++;
        renderCommandsReplayMarkCoverage(cmd.payload.screenRect);
    }

    for (size_t i = 0; i < pixelCount; i++) {
        if (gReplayCoverageMask[i] == 0) {
            gReplayStats.ignoredPixels++;
            continue;
        }

        gReplayStats.comparedPixels++;
        if (gReplayScratchBuffer[i] != gLastFrameReferenceBuffer[i]) {
            gReplayStats.mismatchedPixels++;
        }
    }

    gReplayStats.matched = gReplayStats.mismatchedPixels == 0;
    renderCommandsReplayLogStats();

    return gReplayStats.matched;
}

bool renderCommandsReplayTileCommand(const RenderCommandTileBlit& command, unsigned char* dest, int destPitch)
{
    if (command.payload.sourceWidth == 0 || command.payload.sourceHeight == 0) {
        return false;
    }

    Rect destRect = command.payload.screenRect;
    if (!renderCommandsClampRectToReference(&destRect)) {
        return false;
    }

    CacheEntry* cacheEntry = nullptr;
    Art* art = artLock(command.payload.fid, &cacheEntry);
    if (art == nullptr) {
        return false;
    }

    const uint16_t commandFrame = command.payload.asset.frame;
    const uint8_t commandRotation = command.payload.asset.rotation;
    const int frameWidth = artGetWidth(art, commandFrame, commandRotation);
    const int frameHeight = artGetHeight(art, commandFrame, commandRotation);
    unsigned char* frameData = artGetFrameData(art, commandFrame, commandRotation);
    if (frameData == nullptr || frameWidth <= 0 || frameHeight <= 0) {
        artUnlock(cacheEntry);
        return false;
    }


    bool success = false;
    if ((command.payload.flags & RenderCommandFlag_LightingPerPixel) != 0) {
        success = renderCommandsReplayTileCommandPerPixel(command, destRect, dest, destPitch, frameData, frameWidth, frameHeight);
    } else {
        success = renderCommandsReplayTileCommandFlat(command, destRect, dest, destPitch, frameData, frameWidth, frameHeight);
    }

    artUnlock(cacheEntry);
    return success;
}

bool renderCommandsReplayTileCommandFlat(const RenderCommandTileBlit& command,
    const Rect& destRect,
    unsigned char* dest,
    int destPitch,
    unsigned char* frameData,
    int frameWidth,
    int frameHeight)
{
    const int srcOffsetX = std::clamp(static_cast<int>(command.payload.sourceOffsetX), 0, frameWidth - 1);
    const int srcOffsetY = std::clamp(static_cast<int>(command.payload.sourceOffsetY), 0, frameHeight - 1);
    const int srcWidth = std::min(static_cast<int>(command.payload.sourceWidth), frameWidth - srcOffsetX);
    const int srcHeight = std::min(static_cast<int>(command.payload.sourceHeight), frameHeight - srcOffsetY);

    if (srcWidth <= 0 || srcHeight <= 0) {
        return false;
    }

    unsigned char* src = frameData + srcOffsetY * frameWidth + srcOffsetX;

    int lightingIndex = command.payload.lighting >= 0 ? command.payload.lighting : 0;
    lightingIndex = std::clamp(lightingIndex, 0, 255);

    const int lightingValue = lightingIndex * kReplayLightingStep;

    _dark_trans_buf_to_buf(src,
        srcWidth,
        srcHeight,
        frameWidth,
        dest,
        destRect.left,
        destRect.top,
        destPitch,
        lightingValue);

    return true;
}

bool renderCommandsReplayTileCommandPerPixel(const RenderCommandTileBlit& command,
    const Rect& destRect,
    unsigned char* dest,
    int destPitch,
    unsigned char* frameData,
    int frameWidth,
    int frameHeight)
{
    if (command.payload.perPixelLightingCount < kRenderCommandMaxLightingVertices) {
        return false;
    }

    const int srcOffsetX = std::clamp(static_cast<int>(command.payload.sourceOffsetX), 0, frameWidth - 1);
    const int srcOffsetY = std::clamp(static_cast<int>(command.payload.sourceOffsetY), 0, frameHeight - 1);
    const int srcWidth = std::min(static_cast<int>(command.payload.sourceWidth), frameWidth - srcOffsetX);
    const int srcHeight = std::min(static_cast<int>(command.payload.sourceHeight), frameHeight - srcOffsetY);

    if (srcWidth <= 0 || srcHeight <= 0) {
        return false;
    }

    const int destWidth = destRect.right - destRect.left + 1;
    const int destHeight = destRect.bottom - destRect.top + 1;
    const int blitWidth = std::min(destWidth, srcWidth);
    const int blitHeight = std::min(destHeight, srcHeight);
    if (blitWidth <= 0 || blitHeight <= 0) {
        return false;
    }
    if (destWidth <= 0 || destHeight <= 0) {
        return false;
    }

    std::array<int, kRenderCommandTileIntensityMapSize> intensityMap {};
    if (!renderCommandBuildPerPixelIntensityMap(command.payload, intensityMap)) {
        return false;
    }

    const int intensityStartIndex = kTileIntensityMapBaseOffset + kRenderCommandTileIntensityMapStride * srcOffsetY + srcOffsetX;
    const int intensityMaxIndex = intensityStartIndex + kRenderCommandTileIntensityMapStride * (srcHeight - 1) + (srcWidth - 1);
    if (intensityStartIndex < 0 || intensityMaxIndex >= kRenderCommandTileIntensityMapSize) {
        return false;
    }

    unsigned char* srcRow = frameData + srcOffsetY * frameWidth + srcOffsetX;
    unsigned char* destRow = dest + destPitch * destRect.top + destRect.left;
    int* intensityRow = intensityMap.data() + intensityStartIndex;

    const int srcRowAdvance = frameWidth - blitWidth;
    const int destRowAdvance = destPitch - blitWidth;
    const int intensityRowAdvance = kRenderCommandTileIntensityMapStride - blitWidth;

    for (int row = 0; row < blitHeight; row++) {
        for (int col = 0; col < blitWidth; col++) {
            unsigned char paletteIndex = *srcRow++;
            if (paletteIndex != 0) {
                int intensityIndex = std::clamp(*intensityRow >> 9, 0, 255);
                *destRow = intensityColorTable[paletteIndex][intensityIndex];
            }
            intensityRow++;
            destRow++;
        }

        srcRow += srcRowAdvance;
        destRow += destRowAdvance;
        intensityRow += intensityRowAdvance;
    }

    return true;
}

void renderCommandsFillRightSideUpTriangles(const std::array<int32_t, kRenderCommandMaxLightingVertices>& intensities,
    std::array<int, kRenderCommandTileIntensityMapSize>& mapBuffer)
{
    for (const auto& triangle : kRightSideUpTriangles) {
        int v32 = intensities[triangle.c];
        int v33 = kTileLightingVertexOffsets[triangle.c];
        int v34 = intensities[triangle.b] - intensities[triangle.a];
        int v35 = v34 / 32;
        int v36 = (intensities[triangle.a] - v32) / 13;
        int* cursor = mapBuffer.data() + v33;

        if (v35 != 0) {
            if (v36 != 0) {
                for (const auto& row : kRightSideUpRowTable) {
                    int current = v32;
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = current;
                        current += v35;
                    }
                    v32 += v36;
                }
            } else {
                for (const auto& row : kRightSideUpRowTable) {
                    int current = v32;
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = current;
                        current += v35;
                    }
                }
            }
        } else {
            if (v36 != 0) {
                for (const auto& row : kRightSideUpRowTable) {
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = v32;
                    }
                    v32 += v36;
                }
            } else {
                for (const auto& row : kRightSideUpRowTable) {
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = v32;
                    }
                }
            }
        }
    }
}

void renderCommandsFillUpsideDownTriangles(const std::array<int32_t, kRenderCommandMaxLightingVertices>& intensities,
    std::array<int, kRenderCommandTileIntensityMapSize>& mapBuffer)
{
    for (const auto& triangle : kUpsideDownTriangles) {
        int v50 = intensities[triangle.a];
        int v51 = kTileLightingVertexOffsets[triangle.a];
        int v52 = intensities[triangle.c] - v50;
        int v53 = v52 / 32;
        int v54 = (intensities[triangle.b] - v50) / 13;
        int* cursor = mapBuffer.data() + v51;

        if (v53 != 0) {
            if (v54 != 0) {
                for (const auto& row : kUpsideDownRowTable) {
                    int current = v50;
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = current;
                        current += v53;
                    }
                    v50 += v54;
                }
            } else {
                for (const auto& row : kUpsideDownRowTable) {
                    int current = v50;
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = current;
                        current += v53;
                    }
                }
            }
        } else {
            if (v54 != 0) {
                for (const auto& row : kUpsideDownRowTable) {
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = v50;
                    }
                    v50 += v54;
                }
            } else {
                for (const auto& row : kUpsideDownRowTable) {
                    cursor += row.offset;
                    for (int j = 0; j < row.length; j++) {
                        *cursor++ = v50;
                    }
                }
            }
        }
    }
}

void renderCommandsReplayMarkCoverage(const Rect& rect)
{
    Rect clipped = rect;
    if (!renderCommandsClampRectToReference(&clipped)) {
        return;
    }

    const int width = gLastFrameReferenceWidth;
    for (int y = clipped.top; y <= clipped.bottom; y++) {
        uint8_t* row = gReplayCoverageMask.data() + static_cast<size_t>(y) * width;
        std::fill_n(row + clipped.left, clipped.right - clipped.left + 1, 1);
    }
}

bool renderCommandsClampRectToReference(Rect* rect)
{
    if (rect == nullptr || gLastFrameReferenceWidth <= 0 || gLastFrameReferenceHeight <= 0) {
        return false;
    }

    Rect bounds;
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = gLastFrameReferenceWidth - 1;
    bounds.bottom = gLastFrameReferenceHeight - 1;

    return rectIntersection(rect, &bounds, rect) == 0;
}

void renderCommandsReplayLogStats()
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        if (!gReplayStats.matched && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "SCALER",
                "command_replay mismatch frame=%u compared=%u mismatched=%u failures=%u",
                gLastFrameIndex,
                gReplayStats.comparedPixels,
                gReplayStats.mismatchedPixels,
                gReplayStats.commandsFailed);
        }
        return;
    }

    const double coverage = gReplayStats.comparedPixels + gReplayStats.ignoredPixels > 0
        ? (static_cast<double>(gReplayStats.comparedPixels) / static_cast<double>(gReplayStats.comparedPixels + gReplayStats.ignoredPixels)) * 100.0
        : 0.0;

    diagnosticsLog(DiagnosticsLevel::Trace,
        "SCALER",
        "command_replay frame=%u commands=%u failed=%u compared=%u mismatched=%u coverage=%.2f%%",
        gLastFrameIndex,
        gReplayStats.commandsReplayed,
        gReplayStats.commandsFailed,
        gReplayStats.comparedPixels,
        gReplayStats.mismatchedPixels,
        coverage);
}

void renderCommandsLogFrameDigest()
{
    if (!renderCommandCaptureEnabled()) {
        return;
    }

    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        return;
    }

    const bool replayEnabled = settings.system.render_command_replay;
    const bool replayRan = gReplayStats.ran;
    const bool replayMatched = gReplayStats.matched;
    const double compared = static_cast<double>(gReplayStats.comparedPixels);
    const double totalCompared = gReplayStats.comparedPixels + gReplayStats.ignoredPixels;
    const double coverage = totalCompared > 0 ? (compared / totalCompared) * 100.0 : 0.0;

    diagnosticsLog(DiagnosticsLevel::Trace,
        "SCALER",
        "command_frame frame=%u commands=%zu queued=%u dropped=%u replay_enabled=%d replay_ran=%d replay_matched=%d replay_failed=%u replay_mismatched=%u compared_px=%u coverage=%.2f%% fallback_active=%d reason=%s",
        gRenderCommandFrameIndex,
        gTileCommandCount,
        gRenderCommandStats.queued,
        gRenderCommandStats.dropped,
        replayEnabled ? 1 : 0,
        replayRan ? 1 : 0,
        replayMatched ? 1 : 0,
        gReplayStats.commandsFailed,
        gReplayStats.mismatchedPixels,
        gReplayStats.comparedPixels,
        coverage,
        gRenderCommandDirectBlitFallbackActive ? 1 : 0,
        gRenderCommandDirectBlitFallbackActive ? gRenderCommandDirectBlitFallbackReason : "none");
}

void renderCommandsEvaluateAutoFallback()
{
    if (!settings.system.render_display_orchestrator) {
        return;
    }

    if (!settings.system.virtual_adapter || !settings.system.virtual_adapter_fullres) {
        return;
    }

    if (gRenderCommandDirectBlitFallbackActive) {
        return;
    }

    if (!settings.system.render_command_direct_blit_fallback) {
        return;
    }

    if (gLastFrameStats.dropped > 0) {
        renderCommandTriggerDirectBlitFallback("command_queue_overflow");
        return;
    }

    if (settings.system.render_command_replay && gReplayStats.ran && !gReplayStats.matched) {
        renderCommandTriggerDirectBlitFallback("command_replay_mismatch");
    }
}

} // namespace

bool renderCommandsDumpLastFrame(const char* reason)
{
    renderCommandsEnsureInitialized();

    if (!renderCommandCaptureEnabled()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump skipped (trace disabled)");
        }
        return false;
    }

    if (gLastFrameCommands.empty()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump skipped (no recorded frame)");
        }
        return false;
    }

    char filePath[COMPAT_MAX_PATH];
    std::snprintf(filePath, sizeof(filePath), "log/render_commands_frame_%06u.json", gRenderCommandDumpCounter++);

    FILE* stream = compat_fopen(filePath, "wb");
    if (stream == nullptr) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump unable to open %s", filePath);
        }
        return false;
    }

    std::fprintf(stream, "{\n");
    std::fprintf(stream, "  \"reason\": \"%s\",\n", reason != nullptr ? reason : "manual");
    std::fprintf(stream, "  \"frame_index\": %u,\n", gLastFrameIndex);
    std::fprintf(stream, "  \"commands\": [\n");

    for (size_t i = 0; i < gLastFrameCommands.size(); i++) {
        const RenderCommandTileBlit& cmd = gLastFrameCommands[i];
        const Rect& rect = cmd.payload.screenRect;
        std::fprintf(stream,
            "    {\"seq\":%u,\"op\":%d,\"fid\":%u,\"rect\":[%d,%d,%d,%d],\"flags\":%u,\"lighting\":%d,\"iso\":[%d,%d,%d],\"tile\":%d,\"palette\":%u,\"src\":[%d,%d,%u,%u],\"per_pixel\":%u}%s\n",
            cmd.header.sequence,
            static_cast<int>(cmd.op),
            cmd.payload.fid,
            rect.left,
            rect.top,
            rectGetWidth(&rect),
            rectGetHeight(&rect),
            cmd.payload.flags,
            cmd.payload.lighting,
            cmd.payload.isoTileX,
            cmd.payload.isoTileY,
            cmd.payload.elevation,
            cmd.payload.tileIndex,
            cmd.payload.paletteId,
            cmd.payload.sourceOffsetX,
            cmd.payload.sourceOffsetY,
            cmd.payload.sourceWidth,
            cmd.payload.sourceHeight,
            cmd.payload.perPixelLightingCount,
            i + 1 < gLastFrameCommands.size() ? "," : "");
    }

    std::fprintf(stream, "  ],\n");
    std::fprintf(stream, "  \"stats\": {\"queued\":%u,\"dropped\":%u}\n", gLastFrameStats.queued, gLastFrameStats.dropped);
    std::fprintf(stream, "}\n");
    std::fclose(stream);

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(DiagnosticsLevel::Info,
            "SCALER",
            "command_dump wrote %zu commands to %s",
            gLastFrameCommands.size(),
            filePath);
    }

    return true;
}

bool renderCommandsHandleHotkey(int keyCode)
{
    if (keyCode != kRenderCommandsDumpHotkey) {
        return false;
    }

    if (!windowIsVirtualScreenEnabled()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump hotkey ignored (virtual adapter disabled)");
        }
        return true;
    }

    if (!renderCommandsDumpLastFrame("hotkey")) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump failed (see prior logs)");
        }
    }

    return true;
}

bool renderCommandDirectBlitFallbackActive()
{
    return gRenderCommandDirectBlitFallbackActive;
}

const char* renderCommandDirectBlitFallbackReason()
{
    return gRenderCommandDirectBlitFallbackReason;
}

void renderCommandTriggerDirectBlitFallback(const char* reason)
{
    if (gRenderCommandDirectBlitFallbackActive) {
        return;
    }

    if (!settings.system.render_command_direct_blit_fallback) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
            diagnosticsLog(DiagnosticsLevel::Trace,
                "SCALER",
                "command_fallback suppressed (config disabled) reason=%s",
                reason != nullptr ? reason : "unknown");
        }
        return;
    }

    gRenderCommandDirectBlitFallbackActive = true;
    const char* message = (reason != nullptr && reason[0] != '\0') ? reason : "unknown";
    std::snprintf(gRenderCommandDirectBlitFallbackReason,
        sizeof(gRenderCommandDirectBlitFallbackReason),
        "%s",
        message);

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(DiagnosticsLevel::Info,
            "SCALER",
            "command_fallback activated reason=%s (direct blits only)",
            gRenderCommandDirectBlitFallbackReason);
    }
}

} // namespace fallout
