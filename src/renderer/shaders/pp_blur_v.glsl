#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0, rgba8) uniform readonly image2D inputImage;
layout(binding = 1, rgba8) uniform writeonly image2D outputImage;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 color = vec4(0.0);
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    
    color += imageLoad(inputImage, pos) * weights[0];
    for(int i = 1; i < 5; ++i) {
        color += imageLoad(inputImage, clamp(pos + ivec2(0, i), ivec2(0), size - 1)) * weights[i];
        color += imageLoad(inputImage, clamp(pos - ivec2(0, i), ivec2(0), size - 1)) * weights[i];
    }
    imageStore(outputImage, pos, color);
}
