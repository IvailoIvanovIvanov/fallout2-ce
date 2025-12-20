// Anime4K v3.2 Upscale Original x2 (Ported to HLSL)
// Ported from: https://github.com/bloc97/Anime4K/blob/master/glsl/Upscale/Anime4K_Upscale_Original_x2.glsl

#define REFINE_STRENGTH 0.5
#define REFINE_BIAS 0.0

// Polynomial coefficients
#define P5 ( 11.68129591)
#define P4 (-42.46906057)
#define P3 ( 60.28286266)
#define P2 (-41.84451327)
#define P1 ( 14.05517353)
#define P0 (-1.081521930)

cbuffer UpscaleParams : register(b0)
{
    uint2 inputSize;    // Source resolution (640, 480)
    uint2 outputSize;   // Target resolution (2560, 1440)
    uint2 effectiveSize; // Scaled resolution (e.g. 1920, 1440)
    uint2 offset;        // Letterbox offset (e.g. 320, 0)
    float2 rcpInput;    // 1.0 / inputSize
    float2 rcpEffectiveOutput; // 1.0 / effectiveSize
    float strength;     // Enhancement strength (0.0-1.0)
    float3 padding;
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

float get_luma(float4 c)
{
    return dot(c.rgb, float3(0.299, 0.587, 0.114));
}

float power_function(float x)
{
    float x2 = x * x;
    float x3 = x2 * x;
    float x4 = x2 * x2;
    float x5 = x2 * x3;
    return P5 * x5 + P4 * x4 + P3 * x3 + P2 * x2 + P1 * x + P0;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= outputSize.x || DTid.y >= outputSize.y)
        return;

    // Letterboxing check
    if (DTid.x < offset.x || DTid.x >= offset.x + effectiveSize.x ||
        DTid.y < offset.y || DTid.y >= offset.y + effectiveSize.y)
    {
        OutputTexture[DTid.xy] = float4(0, 0, 0, 1); // Black bars
        return;
    }

    // Map to UV space of the effective area
    float2 pixelPos = float2(DTid.xy) - float2(offset);
    float2 uv = (pixelPos + 0.5f) * rcpEffectiveOutput;
    
    float2 d = rcpEffectiveOutput; // Use effective pixel size for sampling offsets

    // Sample center pixel (bilinear interpolation from input)
    float4 cc = InputTexture.SampleLevel(LinearSampler, uv, 0);

    // Calculate Luma of neighbors
    // We sample at offsets corresponding to the OUTPUT pixel size
    float t = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, -d.y), 0));
    float b = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y), 0));
    float l = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, 0), 0));
    float r = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, 0), 0));
    
    float tl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, -d.y), 0));
    float tr = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, -d.y), 0));
    float bl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, d.y), 0));
    float br = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, d.y), 0));
    
    // Sobel Gradients
    float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);

    // Gradient Magnitude (Normalized by 4.0 to keep in 0-1 range for power function)
    float sobel_norm = sqrt(gx * gx + gy * gy) / 4.0;
    
    // Refinement Strength
    float dval = power_function(saturate(sobel_norm));
    dval = saturate(dval * REFINE_STRENGTH + REFINE_BIAS);

    // Determine edge direction
    float xpos = (gx > 0.0) ? 1.0 : -1.0;
    float ypos = (gy > 0.0) ? 1.0 : -1.0;
    
    // Sample pixels along the gradient direction
    float4 xval = InputTexture.SampleLevel(LinearSampler, uv + float2(d.x * xpos, 0), 0);
    float4 yval = InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y * ypos), 0);
    
    // Interpolate between xval and yval based on gradient ratio
    float abs_gx = abs(gx);
    float abs_gy = abs(gy);
    float xy_ratio = abs_gx / (abs_gx + abs_gy + 0.0001);
    
    float4 avg = xval * xy_ratio + yval * (1.0 - xy_ratio);
    
    // Blend original and refined
    float4 result = avg * dval + cc * (1.0 - dval);
    
    OutputTexture[DTid.xy] = result;
}


