#include "art_png_loader.h"

#include "art_texture.h"
#include "memory.h"
#include "svga.h"
#include "db.h"
#include "color.h"
#include "debug.h"

#ifdef HAVE_SDL2_IMAGE
#include <SDL.h>
#include <SDL_image.h>
#endif

#include <string>
#include <unordered_map>

namespace fallout {

#ifdef HAVE_SDL2_IMAGE
// Simple cache for indexed PNG conversions keyed by fid. Lifetime: until artPngIndexedCacheClear.
struct IndexedCacheEntry {
    unsigned char* data;
    int w;
    int h;
};
static std::unordered_map<int, IndexedCacheEntry> g_indexedCache;
static void getCurrentPaletteRGB(unsigned char* rgbOut768)
{
    // Use the currently loaded game palette (_cmap). Values are 0-63; convert to 8-bit.
    unsigned char* pal6 = _cmap;
    for (int i = 0; i < 256; ++i) {
        rgbOut768[i * 3 + 0] = pal6[i * 3 + 0] << 2;
        rgbOut768[i * 3 + 1] = pal6[i * 3 + 1] << 2;
        rgbOut768[i * 3 + 2] = pal6[i * 3 + 2] << 2;
    }
}

static inline int colorDistanceSq(unsigned char r1, unsigned char g1, unsigned char b1,
                                  unsigned char r2, unsigned char g2, unsigned char b2)
{
    int dr = int(r1) - int(r2);
    int dg = int(g1) - int(g2);
    int db = int(b1) - int(b2);
    return dr * dr + dg * dg + db * db;
}

static unsigned char mapRgbaToPalette(Uint8 r, Uint8 g, Uint8 b, Uint8 a, const unsigned char* pal)
{
    if (a == 0) return 0; // transparent -> index 0 assumed transparent
    int bestIdx = 0;
    int bestDist = 0x7FFFFFFF;
    for (int i = 1; i < 256; ++i) { // skip 0 so transparent stays reserved
        int d = colorDistanceSq(r, g, b, pal[i * 3 + 0], pal[i * 3 + 1], pal[i * 3 + 2]);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
            if (d == 0) break;
        }
    }
    return (unsigned char)bestIdx;
}

static SDL_Surface* downscaleNearest(SDL_Surface* src, int dstW, int dstH)
{
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, dstW, dstH, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) return nullptr;

    int srcW = src->w;
    int srcH = src->h;
    Uint32* sp = (Uint32*)src->pixels;
    Uint32* dp = (Uint32*)dst->pixels;
    int spitch = src->pitch / 4;
    int dpitch = dst->pitch / 4;

    for (int y = 0; y < dstH; ++y) {
        int sy = (y * srcH) / dstH;
        const Uint32* srow = sp + sy * spitch;
        Uint32* drow = dp + y * dpitch;
        for (int x = 0; x < dstW; ++x) {
            int sx = (x * srcW) / dstW;
            drow[x] = srow[sx];
        }
    }

    return dst;
}

bool artPngLoadIndexed(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    *outData = nullptr;
    *outWidth = 0;
    *outHeight = 0;

    const ArtTextureMeta* meta = artTextureGetMeta(fid);
    if (!meta || !meta->exists) return false;

    const char* frmPath = artBuildFilePath(fid);
    if (!frmPath || frmPath[0] == '\0') return false;
    std::string pngPath = std::string(frmPath);
    size_t dot = pngPath.find_last_of('.');
    if (dot == std::string::npos) pngPath += ".png"; else pngPath.replace(dot, std::string::npos, ".png");

    // Load PNG bytes via game VFS so files under patch dir (e.g. data\...) are found.
    File* f = fileOpen(pngPath.c_str(), "rb");
    if (!f) {
        debugPrint("PNG: not found for fid=%d path=\"%s\" (falling back to FRM)\n", fid, pngPath.c_str());
        return false;
    }
    int fsize = fileGetSize(f);
    if (fsize <= 0) {
        fileClose(f);
        return false;
    }
    void* bytes = internal_malloc(fsize);
    if (!bytes) {
        fileClose(f);
        return false;
    }
    size_t readCount = fileRead(bytes, 1, fsize, f);
    fileClose(f);
    if ((int)readCount != fsize) {
        internal_free(bytes);
        return false;
    }

    SDL_RWops* rw = SDL_RWFromMem(bytes, fsize);
    if (!rw) {
        internal_free(bytes);
        return false;
    }
    SDL_Surface* surface = IMG_Load_RW(rw, 1 /*freesrc*/);
    // IMG_Load_RW copies the data into a surface; we can free our buffer now.
    internal_free(bytes);
    if (!surface) {
        debugPrint("PNG: failed to decode for fid=%d path=\"%s\"\n", fid, pngPath.c_str());
        return false;
    }

    // Normalize to RGBA32
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return false;

    SDL_Surface* logical = rgba;
    if (rgba->w != meta->logicalWidth || rgba->h != meta->logicalHeight) {
        SDL_Surface* scaled = downscaleNearest(rgba, meta->logicalWidth, meta->logicalHeight);
        SDL_FreeSurface(rgba);
        logical = scaled;
        if (!logical) return false;
    }

    // Palette mapping
    unsigned char pal[768];
    getCurrentPaletteRGB(pal);

    int w = logical->w;
    int h = logical->h;
    unsigned char* dst = (unsigned char*)internal_malloc(w * h);
    if (!dst) {
        SDL_FreeSurface(logical);
        return false;
    }

    Uint8 r, g, b, a;
    for (int y = 0; y < h; ++y) {
        Uint32* row = (Uint32*)((Uint8*)logical->pixels + y * logical->pitch);
        for (int x = 0; x < w; ++x) {
            SDL_GetRGBA(row[x], logical->format, &r, &g, &b, &a);
            dst[y * w + x] = mapRgbaToPalette(r, g, b, a, pal);
        }
    }

    SDL_FreeSurface(logical);

    *outData = dst;
    *outWidth = w;
    *outHeight = h;
    debugPrint("PNG: loaded indexed fid=%d %dx%d from \"%s\"\n", fid, w, h, pngPath.c_str());
    return true;
}

SDL_Surface* artPngLoadSurface(int fid)
{
    const ArtTextureMeta* meta = artTextureGetMeta(fid);
    if (!meta || !meta->exists) return nullptr;

    const char* frmPath = artBuildFilePath(fid);
    if (!frmPath || frmPath[0] == '\0') return nullptr;
    std::string pngPath = std::string(frmPath);
    size_t dot = pngPath.find_last_of('.');
    if (dot == std::string::npos) pngPath += ".png"; else pngPath.replace(dot, std::string::npos, ".png");

    File* f = fileOpen(pngPath.c_str(), "rb");
    if (!f) {
        debugPrint("PNG: not found (surface) for fid=%d path=\"%s\"\n", fid, pngPath.c_str());
        return nullptr;
    }
    int fsize = fileGetSize(f);
    if (fsize <= 0) {
        fileClose(f);
        return nullptr;
    }
    void* bytes = internal_malloc(fsize);
    if (!bytes) {
        fileClose(f);
        return nullptr;
    }
    size_t readCount = fileRead(bytes, 1, fsize, f);
    fileClose(f);
    if ((int)readCount != fsize) {
        internal_free(bytes);
        return nullptr;
    }
    SDL_RWops* rw = SDL_RWFromMem(bytes, fsize);
    if (!rw) {
        internal_free(bytes);
        return nullptr;
    }
    SDL_Surface* surface = IMG_Load_RW(rw, 1 /*freesrc*/);
    internal_free(bytes);
    if (!surface) {
        debugPrint("PNG: failed to decode (surface) for fid=%d path=\"%s\"\n", fid, pngPath.c_str());
        return nullptr;
    }
    // Convert to a standard truecolor format for predictable blits.
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    if (!rgba) {
        debugPrint("PNG: failed to convert to RGBA (surface) for fid=%d path=\"%s\"\n", fid, pngPath.c_str());
    } else {
        debugPrint("PNG: loaded surface fid=%d %dx%d from \"%s\"\n", fid, rgba->w, rgba->h, pngPath.c_str());
    }
    return rgba; // may be nullptr if conversion failed
}

bool artPngGetIndexedCached(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    *outData = nullptr;
    *outWidth = 0;
    *outHeight = 0;

    auto it = g_indexedCache.find(fid);
    if (it != g_indexedCache.end()) {
        *outData = it->second.data;
        *outWidth = it->second.w;
        *outHeight = it->second.h;
        return it->second.data != nullptr;
    }

    unsigned char* data = nullptr;
    int w = 0, h = 0;
    if (!artPngLoadIndexed(fid, &data, &w, &h)) {
        // Cache negative result to avoid repeated attempts.
        g_indexedCache.emplace(fid, IndexedCacheEntry{ nullptr, 0, 0 });
        return false;
    }

    g_indexedCache.emplace(fid, IndexedCacheEntry{ data, w, h });
    *outData = data;
    *outWidth = w;
    *outHeight = h;
    return true;
}

void artPngIndexedCacheClear()
{
    for (auto& kv : g_indexedCache) {
        if (kv.second.data) {
            internal_free(kv.second.data);
        }
    }
    g_indexedCache.clear();
}

#else

#include <vector>
#include <unordered_map>

// Fallback helpers (duplicated from SDL2_image path)
static void getCurrentPaletteRGB(unsigned char* rgbOut768)
{
    unsigned char* pal6 = _cmap;
    for (int i = 0; i < 256; ++i) {
        rgbOut768[i * 3 + 0] = pal6[i * 3 + 0] << 2;
        rgbOut768[i * 3 + 1] = pal6[i * 3 + 1] << 2;
        rgbOut768[i * 3 + 2] = pal6[i * 3 + 2] << 2;
    }
}

static inline int colorDistanceSq(unsigned char r1, unsigned char g1, unsigned char b1,
                                  unsigned char r2, unsigned char g2, unsigned char b2)
{
    int dr = int(r1) - int(r2);
    int dg = int(g1) - int(g2);
    int db = int(b1) - int(b2);
    return dr * dr + dg * dg + db * db;
}

static unsigned char mapRgbaToPalette(Uint8 r, Uint8 g, Uint8 b, Uint8 a, const unsigned char* pal)
{
    if (a == 0) return 0;
    int bestIdx = 0;
    int bestDist = 0x7FFFFFFF;
    for (int i = 1; i < 256; ++i) {
        int d = colorDistanceSq(r, g, b, pal[i * 3 + 0], pal[i * 3 + 1], pal[i * 3 + 2]);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
            if (d == 0) break;
        }
    }
    return (unsigned char)bestIdx;
}

static SDL_Surface* downscaleNearest(SDL_Surface* src, int dstW, int dstH)
{
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, dstW, dstH, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) return nullptr;
    int srcW = src->w;
    int srcH = src->h;
    Uint32* sp = (Uint32*)src->pixels;
    Uint32* dp = (Uint32*)dst->pixels;
    int spitch = src->pitch / 4;
    int dpitch = dst->pitch / 4;
    for (int y = 0; y < dstH; ++y) {
        int sy = (y * srcH) / dstH;
        const Uint32* srow = sp + sy * spitch;
        Uint32* drow = dp + y * dpitch;
        for (int x = 0; x < dstW; ++x) {
            int sx = (x * srcW) / dstW;
            drow[x] = srow[sx];
        }
    }
    return dst;
}

struct IndexedCacheEntry {
    unsigned char* data;
    int w;
    int h;
};

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

static bool gdiPlusInitialized = false;
static ULONG_PTR gdiPlusToken = 0;

static void ensureGdiPlus()
{
    if (!gdiPlusInitialized) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiPlusToken, &input, nullptr) == Gdiplus::Ok) {
            gdiPlusInitialized = true;
        }
    }
}

static bool decodePngGdiPlus(const void* data, int size, std::vector<unsigned char>& outRgba, int& w, int& h)
{
    ensureGdiPlus();
    if (!gdiPlusInitialized) return false;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return false;
    void* pMem = GlobalLock(hMem);
    if (!pMem) { GlobalFree(hMem); return false; }
    memcpy(pMem, data, size);
    GlobalUnlock(hMem);

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE /*fDeleteOnRelease*/, &pStream) != S_OK) {
        GlobalFree(hMem);
        return false;
    }

    Gdiplus::Bitmap bmp(pStream);
    pStream->Release();

    if (bmp.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }

    w = bmp.GetWidth();
    h = bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;

    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData locked = {};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &locked) != Gdiplus::Ok) {
        return false;
    }

    outRgba.resize((size_t)w * (size_t)h * 4);
    // Convert from GDI+ ARGB (premultiplied, memory order BGRA on little endian) to RGBA.
    const unsigned char* src = static_cast<const unsigned char*>(locked.Scan0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* srow = src + y * locked.Stride;
        unsigned char* drow = outRgba.data() + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; ++x) {
            unsigned char b = srow[x * 4 + 0];
            unsigned char g = srow[x * 4 + 1];
            unsigned char r = srow[x * 4 + 2];
            unsigned char a = srow[x * 4 + 3];
            drow[x * 4 + 0] = r;
            drow[x * 4 + 1] = g;
            drow[x * 4 + 2] = b;
            drow[x * 4 + 3] = a;
        }
    }

    bmp.UnlockBits(&locked);
    return true;
}

#endif // _WIN32

bool artPngLoadIndexed(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    *outData = nullptr;
    *outWidth = 0;
    *outHeight = 0;

    const ArtTextureMeta* meta = artTextureGetMeta(fid);
    if (!meta || !meta->exists) return false;

    const char* frmPath = artBuildFilePath(fid);
    if (!frmPath || frmPath[0] == '\0') return false;
    std::string pngPath = std::string(frmPath);
    size_t dot = pngPath.find_last_of('.');
    if (dot == std::string::npos) pngPath += ".png"; else pngPath.replace(dot, std::string::npos, ".png");

    File* f = fileOpen(pngPath.c_str(), "rb");
    if (!f) {
        debugPrint("PNG: not found for fid=%d path=\"%s\" (falling back to FRM)\n", fid, pngPath.c_str());
        return false;
    }
    int fsize = fileGetSize(f);
    if (fsize <= 0) { fileClose(f); return false; }
    void* bytes = internal_malloc(fsize);
    if (!bytes) { fileClose(f); return false; }
    size_t readCount = fileRead(bytes, 1, fsize, f);
    fileClose(f);
    if ((int)readCount != fsize) { internal_free(bytes); return false; }

#if defined(_WIN32)
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!decodePngGdiPlus(bytes, fsize, rgba, w, h)) {
        internal_free(bytes);
        debugPrint("PNG: failed to decode for fid=%d path=\"%s\"\n", fid, pngPath.c_str());
        return false;
    }
    internal_free(bytes);

    // If size doesn't match logical, downscale with nearest.
    SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormatFrom(rgba.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!tmp) return false;
    SDL_Surface* logical = tmp;
    if (w != meta->logicalWidth || h != meta->logicalHeight) {
        SDL_Surface* scaled = downscaleNearest(tmp, meta->logicalWidth, meta->logicalHeight);
        SDL_FreeSurface(tmp);
        logical = scaled;
        if (!logical) return false;
        w = logical->w;
        h = logical->h;
    }

    unsigned char pal[768];
    getCurrentPaletteRGB(pal);
    unsigned char* dst = (unsigned char*)internal_malloc((size_t)w * (size_t)h);
    if (!dst) { SDL_FreeSurface(logical); return false; }
    Uint8 r, g, b, a;
    for (int y = 0; y < h; ++y) {
        Uint32* row = (Uint32*)((Uint8*)logical->pixels + y * logical->pitch);
        for (int x = 0; x < w; ++x) {
            SDL_GetRGBA(row[x], logical->format, &r, &g, &b, &a);
            dst[y * w + x] = mapRgbaToPalette(r, g, b, a, pal);
        }
    }
    SDL_FreeSurface(logical);

    *outData = dst;
    *outWidth = w;
    *outHeight = h;
    debugPrint("PNG: loaded indexed fid=%d %dx%d from \"%s\" (GDI+)\n", fid, w, h, pngPath.c_str());
    return true;
#else
    (void)bytes; // silence unused warning on non-Windows
    debugPrint("PNG: no decoder available on this platform (no SDL2_image)\n");
    internal_free(bytes);
    return false;
#endif
}

SDL_Surface* artPngLoadSurface(int fid)
{
    const ArtTextureMeta* meta = artTextureGetMeta(fid);
    if (!meta || !meta->exists) return nullptr;

    const char* frmPath = artBuildFilePath(fid);
    if (!frmPath || frmPath[0] == '\0') return nullptr;
    std::string pngPath = std::string(frmPath);
    size_t dot = pngPath.find_last_of('.');
    if (dot == std::string::npos) pngPath += ".png"; else pngPath.replace(dot, std::string::npos, ".png");

    File* f = fileOpen(pngPath.c_str(), "rb");
    if (!f) { debugPrint("PNG: not found (surface) for fid=%d path=\"%s\"\n", fid, pngPath.c_str()); return nullptr; }
    int fsize = fileGetSize(f);
    if (fsize <= 0) { fileClose(f); return nullptr; }
    void* bytes = internal_malloc(fsize);
    if (!bytes) { fileClose(f); return nullptr; }
    size_t readCount = fileRead(bytes, 1, fsize, f);
    fileClose(f);
    if ((int)readCount != fsize) { internal_free(bytes); return nullptr; }

#if defined(_WIN32)
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!decodePngGdiPlus(bytes, fsize, rgba, w, h)) {
        internal_free(bytes);
        debugPrint("PNG: failed to decode (surface) for fid=%d path=\"%s\"\n", fid, pngPath.c_str());
        return nullptr;
    }
    internal_free(bytes);

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;
    for (int y = 0; y < h; ++y) {
        memcpy((Uint8*)surf->pixels + y * surf->pitch, rgba.data() + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }
    debugPrint("PNG: loaded surface fid=%d %dx%d from \"%s\" (GDI+)\n", fid, w, h, pngPath.c_str());
    return surf;
#else
    internal_free(bytes);
    debugPrint("PNG: no decoder available on this platform (no SDL2_image)\n");
    return nullptr;
#endif
}

bool artPngGetIndexedCached(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    static std::unordered_map<int, IndexedCacheEntry> g_fallbackCache; // local cache for fallback path

    *outData = nullptr;
    *outWidth = 0;
    *outHeight = 0;

    auto it = g_fallbackCache.find(fid);
    if (it != g_fallbackCache.end()) {
        *outData = it->second.data;
        *outWidth = it->second.w;
        *outHeight = it->second.h;
        return it->second.data != nullptr;
    }

    unsigned char* data = nullptr;
    int w = 0, h = 0;
    if (!artPngLoadIndexed(fid, &data, &w, &h)) {
        g_fallbackCache.emplace(fid, IndexedCacheEntry{ nullptr, 0, 0 });
        return false;
    }

    g_fallbackCache.emplace(fid, IndexedCacheEntry{ data, w, h });
    *outData = data;
    *outWidth = w;
    *outHeight = h;
    return true;
}

void artPngIndexedCacheClear()
{
    // No global cache in this branch; local caches are cleared on process restart.
}

#endif

} // namespace fallout
