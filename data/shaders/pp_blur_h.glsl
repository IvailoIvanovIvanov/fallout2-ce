#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform sampler2D inputTexture;
layout(binding = 1, rgba16f) uniform writeonly image2D outputTexture;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTexture);
    
    if (pos.x >= size.x || pos.y >= size.y) return;

    // Gaussian Kernel 5x1 (Sigma=1.0)
    // 0.06136, 0.24477, 0.38774, 0.24477, 0.06136
    
    vec4 original = texelFetch(inputTexture, pos, 0);
    vec4 sum = vec4(0.0);
    sum += texelFetch(inputTexture, pos + ivec2(-2, 0), 0) * 0.06136;
    sum += texelFetch(inputTexture, pos + ivec2(-1, 0), 0) * 0.24477;
    sum += original * 0.38774;
    sum += texelFetch(inputTexture, pos + ivec2(1, 0), 0) * 0.24477;
    sum += texelFetch(inputTexture, pos + ivec2(2, 0), 0) * 0.06136;

    // Strength control: 1.0 = full blur, 0.0 = no blur
    float strength = 0.5;
    imageStore(outputTexture, pos, mix(original, sum, strength));
}
