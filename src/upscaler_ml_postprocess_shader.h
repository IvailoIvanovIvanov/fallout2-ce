#pragma once

namespace fallout {

// Shader to convert Planar Float32 RGB to Interleaved RGBA8
// Input: Buffer<float> (R plane, G plane, B plane)
// Output: RWStructuredBuffer<uint> (RGBA8 interleaved)

const char* UPSCALER_ML_POSTPROCESS_SHADER = R"(
cbuffer Params : register(b0) {
    uint2 outputSize;
    uint2 mlSize;
}

StructuredBuffer<float> InputBuffer : register(t0);
RWBuffer<uint> OutputBuffer : register(u0);

uint packColor(float r, float g, float b) {
    uint ur = (uint)(saturate(r) * 255.0f);
    uint ug = (uint)(saturate(g) * 255.0f);
    uint ub = (uint)(saturate(b) * 255.0f);
    uint ua = 255;
    // ARGB8888 (0xAARRGGBB) maps to BGRA in memory on Little Endian
    return (ua << 24) | (ur << 16) | (ug << 8) | ub;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= outputSize.x || DTid.y >= outputSize.y) return;

    // Nearest Neighbor Sampling
    // (Can be improved to Bilinear later)
    
    uint mlX = (DTid.x * mlSize.x) / outputSize.x;
    uint mlY = (DTid.y * mlSize.y) / outputSize.y;
    
    // Clamp to be safe
    mlX = min(mlX, mlSize.x - 1);
    mlY = min(mlY, mlSize.y - 1);
    
    uint pixelCount = mlSize.x * mlSize.y;
    uint rIndex = mlY * mlSize.x + mlX;
    uint gIndex = rIndex + pixelCount;
    uint bIndex = gIndex + pixelCount;
    
    float r = InputBuffer[rIndex];
    float g = InputBuffer[gIndex];
    float b = InputBuffer[bIndex];
    
    uint outIndex = DTid.y * outputSize.x + DTid.x;
    OutputBuffer[outIndex] = packColor(r, g, b);
}
)";

} // namespace fallout
