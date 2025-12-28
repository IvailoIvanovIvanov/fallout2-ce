#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform sampler2D inputTexture;
layout(binding = 1, rgba16f) uniform writeonly image2D outputTexture;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTexture);
    
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 center = texelFetch(inputTexture, pos, 0);
    vec4 up = texelFetch(inputTexture, pos + ivec2(0, -1), 0);
    vec4 down = texelFetch(inputTexture, pos + ivec2(0, 1), 0);
    vec4 left = texelFetch(inputTexture, pos + ivec2(-1, 0), 0);
    vec4 right = texelFetch(inputTexture, pos + ivec2(1, 0), 0);

    vec4 sum = up + down + left + right;
    vec4 result = center * 5.0 - sum;

    imageStore(outputTexture, pos, result);
}
