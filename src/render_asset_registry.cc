#include "render_asset_registry.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "color.h"
#include "diagnostics.h"

namespace fallout {
namespace {

struct RenderAssetKey {
    uint32_t fid;
    uint16_t frame;
    uint8_t rotation;
    uint8_t variant;
};

struct RenderAssetKeyHasher {
    size_t operator()(const RenderAssetKey& key) const noexcept
    {
        uint64_t value = static_cast<uint64_t>(key.fid)
            | (static_cast<uint64_t>(key.frame) << 32)
            | (static_cast<uint64_t>(key.rotation) << 48)
            | (static_cast<uint64_t>(key.variant) << 56);
        return std::hash<uint64_t>()(value);
    }
};

struct RenderAssetKeyEqual {
    bool operator()(const RenderAssetKey& lhs, const RenderAssetKey& rhs) const noexcept
    {
        return lhs.fid == rhs.fid && lhs.frame == rhs.frame && lhs.rotation == rhs.rotation && lhs.variant == rhs.variant;
    }
};

struct RenderAssetMetadata {
    RenderAssetHandle handle;
    RenderAssetKey key {};
    uint64_t id = 0;
    const unsigned char* indexed = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;
    HdTrueColorFrameView hdView {};
    bool hdViewValid = false;
    bool hdIsFallback = false;
    std::unique_ptr<uint32_t[]> fallbackStorage;
};

using AssetMap = std::unordered_map<RenderAssetKey, RenderAssetMetadata, RenderAssetKeyHasher, RenderAssetKeyEqual>;
using PointerMap = std::unordered_map<const unsigned char*, RenderAssetKey>;
using OwnerMap = std::unordered_map<const void*, std::unordered_set<const unsigned char*>>;

AssetMap gAssets;
PointerMap gPointerToAsset;
OwnerMap gOwnerToPointers;

RenderAssetKey makeKey(const RenderAssetHandle& handle)
{
    return RenderAssetKey { handle.fid, handle.frame, handle.rotation, handle.variant };
}

uint64_t computeAssetId(const RenderAssetHandle& handle)
{
    return static_cast<uint64_t>(handle.fid)
        | (static_cast<uint64_t>(handle.frame) << 32)
        | (static_cast<uint64_t>(handle.rotation) << 48)
        | (static_cast<uint64_t>(handle.variant) << 56);
}

RenderAssetMetadata* findMetadata(const RenderAssetHandle& handle)
{
    auto it = gAssets.find(makeKey(handle));
    if (it == gAssets.end()) {
        return nullptr;
    }
    return &(it->second);
}

RenderAssetMetadata* findMetadataByPointer(const unsigned char* indexed)
{
    if (indexed == nullptr) {
        return nullptr;
    }

    auto pointerIt = gPointerToAsset.find(indexed);
    if (pointerIt == gPointerToAsset.end()) {
        return nullptr;
    }

    auto assetIt = gAssets.find(pointerIt->second);
    if (assetIt == gAssets.end()) {
        return nullptr;
    }

    return &(assetIt->second);
}

static bool conformHdView(RenderAssetMetadata& metadata);

void ensureFallback(RenderAssetMetadata& metadata, const unsigned char* indexed)
{
    if (metadata.hdViewValid && !metadata.hdIsFallback) {
        return;
    }

    if (indexed == nullptr) {
        indexed = metadata.indexed;
    }

    if (indexed == nullptr || metadata.width == 0 || metadata.height == 0) {
        return;
    }

    const size_t pixelCount = static_cast<size_t>(metadata.width) * metadata.height;
    if (pixelCount == 0) {
        return;
    }

    metadata.fallbackStorage = std::make_unique<uint32_t[]>(pixelCount);
    uint32_t* dest = metadata.fallbackStorage.get();
    const unsigned char* src = indexed;
    for (size_t i = 0; i < pixelCount; i++) {
        const unsigned char index = *src++;
        dest[i] = index == 0 ? 0 : paletteIndexToArgb(index);
    }

    metadata.hdView.pixels = metadata.fallbackStorage.get();
    metadata.hdView.width = metadata.width;
    metadata.hdView.height = metadata.height;
    metadata.hdView.logicalWidth = metadata.width;
    metadata.hdView.logicalHeight = metadata.height;
    metadata.hdView.scaleX = 1;
    metadata.hdView.scaleY = 1;
    metadata.hdView.texelOriginX = 0.0;
    metadata.hdView.texelOriginY = 0.0;
    metadata.hdView.texelsPerLogicalX = 1.0;
    metadata.hdView.texelsPerLogicalY = 1.0;
    metadata.hdView.alphaMode = HdAlphaMode::Straight;
    metadata.hdViewValid = metadata.hdView.pixels != nullptr;
    metadata.hdIsFallback = true;

    if (metadata.hdViewValid) {
        conformHdView(metadata);
    }

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "asset_registry fallback fid=%u frame=%u rot=%u variant=%u",
            metadata.handle.fid,
            metadata.handle.frame,
            metadata.handle.rotation,
            metadata.handle.variant);
    }
}

static bool conformHdView(RenderAssetMetadata& metadata)
{
    if (!metadata.hdViewValid) {
        return false;
    }

    if (!artConformTrueColorFrame(static_cast<int>(metadata.handle.fid), metadata.width, metadata.height, metadata.hdView)) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "SCALER",
                "asset_registry hd_view_rejected fid=%u frame=%u rot=%u variant=%u",
                metadata.handle.fid,
                metadata.handle.frame,
                metadata.handle.rotation,
                metadata.handle.variant);
        }
        metadata.hdView = {};
        metadata.hdViewValid = false;
        metadata.hdIsFallback = false;
        return false;
    }

    return true;
}

} // namespace

void renderAssetRegistryReset()
{
    gAssets.clear();
    gPointerToAsset.clear();
    gOwnerToPointers.clear();
}

void renderAssetRegistryTrackFrame(const RenderAssetHandle& handle,
    const void* owner,
    const unsigned char* indexed,
    uint16_t width,
    uint16_t height)
{
    if (indexed == nullptr || width == 0 || height == 0) {
        return;
    }

    RenderAssetKey key = makeKey(handle);
    RenderAssetMetadata& metadata = gAssets[key];
    metadata.handle = handle;
    metadata.key = key;
    metadata.id = computeAssetId(handle);
    metadata.width = width;
    metadata.height = height;
    metadata.indexed = indexed;

    gPointerToAsset[indexed] = key;

    if (owner != nullptr) {
        gOwnerToPointers[owner].insert(indexed);
    }

    if (!metadata.hdViewValid) {
        ensureFallback(metadata, indexed);
    }
}

void renderAssetRegistryReleaseFramesForOwner(const void* owner)
{
    if (owner == nullptr) {
        return;
    }

    auto ownerIt = gOwnerToPointers.find(owner);
    if (ownerIt == gOwnerToPointers.end()) {
        return;
    }

    for (const unsigned char* indexed : ownerIt->second) {
        auto pointerIt = gPointerToAsset.find(indexed);
        if (pointerIt == gPointerToAsset.end()) {
            continue;
        }

        auto assetIt = gAssets.find(pointerIt->second);
        if (assetIt != gAssets.end() && assetIt->second.indexed == indexed) {
            assetIt->second.indexed = nullptr;
            if (!assetIt->second.hdIsFallback) {
                assetIt->second.hdView = {};
                assetIt->second.hdViewValid = false;
            }
        }

        gPointerToAsset.erase(pointerIt);
    }

    gOwnerToPointers.erase(ownerIt);
}

bool renderAssetRegistryAttachHdView(const unsigned char* indexed,
    const HdTrueColorFrameView& view,
    bool isFallback)
{
    RenderAssetMetadata* metadata = findMetadataByPointer(indexed);
    if (metadata == nullptr) {
        return false;
    }

    metadata->hdView = view;
    metadata->hdViewValid = view.pixels != nullptr && view.width > 0 && view.height > 0;
    metadata->hdIsFallback = isFallback;
    if (!isFallback) {
        metadata->fallbackStorage.reset();
    }

    if (!metadata->hdViewValid) {
        return false;
    }

    if (!conformHdView(*metadata)) {
        ensureFallback(*metadata, indexed);
    }

    return metadata->hdViewValid;
}

void renderAssetRegistryDetachHdView(const unsigned char* indexed)
{
    RenderAssetMetadata* metadata = findMetadataByPointer(indexed);
    if (metadata == nullptr || metadata->hdIsFallback) {
        return;
    }

    metadata->hdView = {};
    metadata->hdViewValid = false;
}

bool renderAssetRegistryGetHdView(const RenderAssetHandle& handle,
    HdTrueColorFrameView& outView,
    bool* outIsFallback)
{
    RenderAssetMetadata* metadata = findMetadata(handle);
    if (metadata == nullptr || !metadata->hdViewValid) {
        if (outIsFallback != nullptr) {
            *outIsFallback = false;
        }
        return false;
    }

    outView = metadata->hdView;
    if (outIsFallback != nullptr) {
        *outIsFallback = metadata->hdIsFallback;
    }

    return true;
}

bool renderAssetRegistryGetState(const RenderAssetHandle& handle, RenderAssetState& outState)
{
    RenderAssetMetadata* metadata = findMetadata(handle);
    if (metadata == nullptr) {
        return false;
    }

    outState.handle = metadata->handle;
    outState.assetId = metadata->id;
    outState.width = metadata->width;
    outState.height = metadata->height;
    outState.hdAvailable = metadata->hdViewValid;
    outState.hdIsFallback = metadata->hdViewValid && metadata->hdIsFallback;
    return true;
}

} // namespace fallout
