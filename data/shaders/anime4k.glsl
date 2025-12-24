#version 430
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D InputTexture;
layout(binding = 0, rgba8) uniform image2D OutputTexture;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    vec4 color = texelFetch(InputTexture, pos, 0);
    // Simple pass-through for now
    imageStore(OutputTexture, pos, color);
}
