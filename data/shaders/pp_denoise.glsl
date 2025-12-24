#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform sampler2D inputTexture;
layout(binding = 1, rgba16f) uniform writeonly image2D outputTexture;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTexture);
    
    if (pos.x >= size.x || pos.y >= size.y) return;

    // Simple box blur as denoise placeholder
    vec4 sum = vec4(0.0);
    for(int y=-1; y<=1; ++y) {
        for(int x=-1; x<=1; ++x) {
            sum += texelFetch(inputTexture, pos + ivec2(x, y), 0);
        }
    }
    
    imageStore(outputTexture, pos, sum / 9.0);
}
