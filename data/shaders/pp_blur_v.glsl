#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform sampler2D inputTexture;
layout(binding = 1, rgba16f) uniform writeonly image2D outputTexture;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTexture);
    
    if (pos.x >= size.x || pos.y >= size.y) return;

    // Improved: 9-tap Gaussian Kernel (Sigma ~2.0)
    float weights[5] = float[](0.227027, 0.194595, 0.121622, 0.054054, 0.016216);

    vec4 sum = texelFetch(inputTexture, pos, 0) * weights[0];

    for(int i = 1; i < 5; i++) {
        sum += texelFetch(inputTexture, clamp(pos + ivec2(0, i), ivec2(0), size - 1), 0) * weights[i];
        sum += texelFetch(inputTexture, clamp(pos - ivec2(0, i), ivec2(0), size - 1), 0) * weights[i];
    }

    // Strength control: 0.0 = no blur, 1.0 = full blur
    float strength = 1.0; 
    vec4 original = texelFetch(inputTexture, pos, 0);
    imageStore(outputTexture, pos, mix(original, sum, strength));
}
