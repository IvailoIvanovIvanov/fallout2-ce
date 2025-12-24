#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0) uniform sampler2D inputTexture;
layout(binding = 1, rgba16f) uniform writeonly image2D outputTexture;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputTexture);
    
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 color = texelFetch(inputTexture, pos, 0);
    
    // Simple Reinhard Tonemap
    // color.rgb = color.rgb / (color.rgb + vec3(1.0));
    
    // For now, just pass through as we don't have HDR input yet
    imageStore(outputTexture, pos, color);
}
