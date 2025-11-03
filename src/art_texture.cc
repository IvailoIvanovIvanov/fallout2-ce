#include "art_texture.h"

#include <algorithm>
#include <string>
#include <cstring>
#include <unordered_map>
#include <cstdlib>

#include "db.h"
#include "svga.h"

#ifdef HAVE_SDL2_IMAGE
#include <SDL_image.h>
#endif

namespace fallout {

static std::unordered_map<int, ArtTextureMeta> g_metaCache;
static std::unordered_map<int, ArtTexture> g_texCache;

// Minimal PNG header reader: validates signature and extracts IHDR width/height.
static bool readPngSize(File* f, int* outW, int* outH)
{
    unsigned char sig[8];
    if (fileRead(sig, 1, 8, f) != 8) return false;
    const unsigned char pngSig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (memcmp(sig, pngSig, 8) != 0) return false;

    // Read first chunk length and type
    unsigned char lenType[8];
    if (fileRead(lenType, 1, 8, f) != 8) return false;
    // Type should be "IHDR"
    if (lenType[4] != 'I' || lenType[5] != 'H' || lenType[6] != 'D' || lenType[7] != 'R') return false;

    // IHDR length should be 13
    unsigned int len = (lenType[0] << 24) | (lenType[1] << 16) | (lenType[2] << 8) | lenType[3];
    if (len != 13) return false;

    unsigned char ihdr[13];
    if (fileRead(ihdr, 1, 13, f) != 13) return false;

    // width and height are big-endian 32-bit
    unsigned int w = (ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3];
    unsigned int h = (ihdr[4] << 24) | (ihdr[5] << 16) | (ihdr[6] << 8) | ihdr[7];
    if (w == 0 || h == 0) return false;

    *outW = static_cast<int>(w);
    *outH = static_cast<int>(h);
    return true;
}

static bool openIfExists(const char* path, File** out)
{
    File* f = fileOpen(path, "rb");
    if (f == nullptr) return false;
    *out = f;
    return true;
}

// Replace file extension with .png (case-insensitive, tolerant if no ext).
static std::string replaceExtWithPng(const char* path)
{
    std::string s(path);
    size_t dot = s.find_last_of('.');
    if (dot == std::string::npos) {
        s += ".png";
    } else {
        s.replace(dot, std::string::npos, ".png");
    }
    return s;
}

static int detectScaleFromName(const std::string& pngPath)
{
    // Look for suffix like "@2x", "@3x", "@4x" before extension.
    size_t dot = pngPath.find_last_of('.');
    size_t at = pngPath.find_last_of('@');
    if (at != std::string::npos && dot != std::string::npos && at < dot) {
        std::string tag = pngPath.substr(at + 1, dot - at - 1); // e.g., "2x"
        if (tag.size() >= 2 && tag.back() == 'x') {
            int n = atoi(tag.substr(0, tag.size() - 1).c_str());
            if (n >= 1 && n <= 8) return n;
        }
    }
    return 0;
}

bool artTextureProbePng(int fid, ArtTextureMeta* outMeta)
{
    if (outMeta == nullptr) return false;
    outMeta->exists = false;
    outMeta->pngWidth = 0;
    outMeta->pngHeight = 0;
    outMeta->logicalWidth = 0;
    outMeta->logicalHeight = 0;
    outMeta->sourceScale = 1;

    // Build FRM path and candidate PNG path next to it.
    const char* frmPath = artBuildFilePath(fid);
    if (frmPath == nullptr || frmPath[0] == '\0') {
        return false;
    }

    std::string pngPath = replaceExtWithPng(frmPath);

    File* f = nullptr;
    if (!openIfExists(pngPath.c_str(), &f)) {
        // No PNG override found.
        return false;
    }

    int pngW = 0, pngH = 0;
    bool ok = readPngSize(f, &pngW, &pngH);
    fileClose(f);
    if (!ok) {
        return false;
    }

    // Get FRM logical size via helper.
    FrmImage frm;
    if (!frm.lock(fid)) {
        // Without FRM size we still can assume 1x.
        outMeta->exists = true;
        outMeta->pngWidth = pngW;
        outMeta->pngHeight = pngH;
        outMeta->logicalWidth = pngW;
        outMeta->logicalHeight = pngH;
        outMeta->sourceScale = 1;
        return true;
    }
    int frmW = frm.getWidth();
    int frmH = frm.getHeight();
    frm.unlock();

    // Detect scale factor. Prefer explicit @Nx suffix, else infer from size ratio.
    int explicitScale = detectScaleFromName(pngPath);
    int scale;
    if (explicitScale > 0) {
        scale = explicitScale;
    } else {
        auto nearestInt = [](float v) { return static_cast<int>(v + 0.5f); };
        int scaleX = frmW > 0 ? std::max(1, nearestInt(static_cast<float>(pngW) / frmW)) : 1;
        int scaleY = frmH > 0 ? std::max(1, nearestInt(static_cast<float>(pngH) / frmH)) : 1;
        scale = std::min(scaleX, scaleY);
    }
    if (scale < 1) scale = 1;

    // Ensure logical size is at least 1x.
    int logicalW = pngW / scale;
    int logicalH = pngH / scale;
    if (logicalW <= 0 || logicalH <= 0) {
        scale = 1;
        logicalW = pngW;
        logicalH = pngH;
    }

    outMeta->exists = true;
    outMeta->pngWidth = pngW;
    outMeta->pngHeight = pngH;
    outMeta->logicalWidth = logicalW;
    outMeta->logicalHeight = logicalH;
    outMeta->sourceScale = scale;
    return true;
}

bool artTextureExists(int fid)
{
    ArtTextureMeta meta;
    return artTextureProbePng(fid, &meta);
}

const ArtTextureMeta* artTextureGetMeta(int fid)
{
    auto it = g_metaCache.find(fid);
    if (it != g_metaCache.end()) {
        return it->second.exists ? &it->second : nullptr;
    }

    ArtTextureMeta meta;
    if (artTextureProbePng(fid, &meta)) {
        g_metaCache[fid] = meta;
        return &g_metaCache[fid];
    }

    // Cache negative result to avoid repeated probes.
    meta.exists = false;
    g_metaCache[fid] = meta;
    return nullptr;
}

void artTextureCacheClear()
{
    g_metaCache.clear();
}

const ArtTexture* artTextureGet(int fid)
{
#ifndef HAVE_SDL2_IMAGE
    (void)fid;
    return nullptr;
#else
    // Check texture cache first.
    auto ti = g_texCache.find(fid);
    if (ti != g_texCache.end()) {
        return &ti->second;
    }

    // Need metadata (and existence) first.
    const ArtTextureMeta* meta = artTextureGetMeta(fid);
    if (meta == nullptr || !meta->exists) {
        return nullptr;
    }

    const char* frmPath = artBuildFilePath(fid);
    if (frmPath == nullptr || frmPath[0] == '\0') {
        return nullptr;
    }
    std::string pngPath = replaceExtWithPng(frmPath);

    // Load using SDL_image
    SDL_Surface* surface = IMG_Load(pngPath.c_str());
    if (!surface) {
        return nullptr;
    }

    // Convert surface to a texture in the shared renderer.
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gSdlRenderer, surface);
    SDL_FreeSurface(surface);
    if (!tex) {
        return nullptr;
    }

    ArtTexture at;
    at.texture = tex;
    at.logicalWidth = meta->logicalWidth;
    at.logicalHeight = meta->logicalHeight;
    at.sourceScale = meta->sourceScale;

    auto res = g_texCache.emplace(fid, at);
    return &res.first->second;
#endif
}

void artTextureClear()
{
#ifdef HAVE_SDL2_IMAGE
    for (auto& kv : g_texCache) {
        if (kv.second.texture) {
            SDL_DestroyTexture(kv.second.texture);
        }
    }
#endif
    g_texCache.clear();
}

} // namespace fallout
