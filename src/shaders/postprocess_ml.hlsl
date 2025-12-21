// Postprocess shader: Convert Float32 Planar back to RGBA8
// Input: Texture2D<float> (3 separate planes: R, G, B)
// Output: RWTexture2D<float4> (RGBA8)

Texture2D<float> inputR : register(t0);
Texture2D<float> inputG : register(t1);
Texture2D<float> inputB : register(t2);
RWTexture2D<float4> output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pos = dispatchThreadID.xy;
    
    // Read from separate planes
    float r = inputR[pos];
    float g = inputG[pos];
    float b = inputB[pos];
    
    // Clamp to [0, 1]
    r = saturate(r);
    g = saturate(g);
    b = saturate(b);
    
    // Write to output (Alpha = 1.0)
    output[pos] = float4(r, g, b, 1.0);
}
