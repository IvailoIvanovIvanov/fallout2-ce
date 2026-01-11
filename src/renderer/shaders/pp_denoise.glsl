#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0, rgba8) uniform readonly image2D inputImage;
layout(binding = 1, rgba8) uniform writeonly image2D outputImage;

// Bilateral Filter Configuration
// Helps remove "grain" and dithering artifacts while preserving edges.
const int Radius = 2;               // Kernel radius (2 = 5x5 area)
const float SigmaSpatial = 3.0;     // Spatial weight (how far to look)
const float SigmaColor = 0.15;      // Color weight (sensitivity to edges). Lower = preserves more edges.

float gaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma));
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 centerColor = imageLoad(inputImage, pos);
    vec4 sum = vec4(0.0);
    float weightSum = 0.0;

    for(int x = -Radius; x <= Radius; ++x) {
        for(int y = -Radius; y <= Radius; ++y) {
            ivec2 offset = ivec2(x, y);
            ivec2 samplePos = clamp(pos + offset, ivec2(0), size - 1);
            
            vec4 sampleColor = imageLoad(inputImage, samplePos);
            
            float spatialDist = length(vec2(offset));
            float colorDist = distance(centerColor.rgb, sampleColor.rgb);
            
            float weight = gaussian(spatialDist, SigmaSpatial) * gaussian(colorDist, SigmaColor);
            
            sum += sampleColor * weight;
            weightSum += weight;
        }
    }

    imageStore(outputImage, pos, sum / weightSum);
}
