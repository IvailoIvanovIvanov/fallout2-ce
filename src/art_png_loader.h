#ifndef ART_PNG_LOADER_H
#define ART_PNG_LOADER_H

namespace fallout {

// Load a PNG override for fid and convert to 8-bit indexed using current palette.
// Applies @Nx downscaling to match logical size. Returns true on success.
// Caller must free(*outData) with internal_free.
bool artPngLoadIndexed(int fid, unsigned char** outData, int* outWidth, int* outHeight);

}

#endif // ART_PNG_LOADER_H
