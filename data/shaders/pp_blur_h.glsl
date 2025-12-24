#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform sampler2D inputTexture;
layout(binding = 1, rgba16f) uniform writeonly image2D outputTexture;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTexture);
    
    if (pos.x >= size.x || pos.y >= size.y) return;

    // 1D Bilateral Filter (Horizontal)
    // Smooths patterns while preserving vertical edges
    vec4 center = texelFetch(inputTexture, pos, 0);
    
    const int radius = 4;
    const float sigmaSpatial = 4.0;
    const float sigmaColor = 0.2;

    vec4 sum = vec4(0.0);
    float totalWeight = 0.0;

    for(int i = -radius; i <= radius; ++i) {
        ivec2 samplePos = clamp(pos + ivec2(i, 0), ivec2(0), size - 1);
        vec4 sampleColor = texelFetch(inputTexture, samplePos, 0);

        float dist = float(i);
        float wSpatial = exp(-(dist * dist) / (2.0 * sigmaSpatial * sigmaSpatial));

        vec3 diff = sampleColor.rgb - center.rgb;
        float wColor = exp(-dot(diff, diff) / (2.0 * sigmaColor * sigmaColor));

        float weight = wSpatial * wColor;

        sum += sampleColor * weight;
        totalWeight += weight;
    }

    // Strength control: 0.0 = no blur, 1.0 = full blur
    float strength = 0.2; 
    imageStore(outputTexture, pos, mix(center, sum / totalWeight, strength));
}
