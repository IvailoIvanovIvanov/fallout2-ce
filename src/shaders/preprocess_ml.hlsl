// Preprocess shader: Convert RGBA8 to Float32 Planar for ML inference
// Input: Texture2D<float4> (RGBA8 normalized to [0,1])
// Output: RWTexture2D<float> (3 separate planes: R, G, B)

Texture2D<float4> inputTexture : register(t0);
RWTexture2D<float> outputR : register(u0);
RWTexture2D<float> outputG : register(u1);
RWTexture2D<float> outputB : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pos = dispatchThreadID.xy;
    
    // Sample input pixel
    float4 color = inputTexture[pos];
    
    // Write to separate planes
    outputR[pos] = color.r;
    outputG[pos] = color.g;
    outputB[pos] = color.b;
}
