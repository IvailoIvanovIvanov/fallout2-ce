#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0, rgba8) uniform readonly image2D inputImage;
layout(binding = 1, rgba8) uniform writeonly image2D outputImage;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 center = imageLoad(inputImage, pos);
    vec4 up = imageLoad(inputImage, clamp(pos + ivec2(0, 1), ivec2(0), size - 1));
    vec4 down = imageLoad(inputImage, clamp(pos - ivec2(0, 1), ivec2(0), size - 1));
    vec4 left = imageLoad(inputImage, clamp(pos - ivec2(1, 0), ivec2(0), size - 1));
    vec4 right = imageLoad(inputImage, clamp(pos + ivec2(1, 0), ivec2(0), size - 1));
    
    // Simple Laplacian sharpen
    vec4 result = center * 5.0 - (up + down + left + right);
    imageStore(outputImage, pos, clamp(result, 0.0, 1.0));
}
