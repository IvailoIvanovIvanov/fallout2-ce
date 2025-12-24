#version 430
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D InputTexture;
layout(binding = 1, rgba16f) uniform writeonly image2D OutputTexture;

layout(std140, binding = 0) uniform Constants {
    ivec2 inputResolution;
    ivec2 outputResolution;
    vec2 offset;
    vec2 scale;
};

void main() {
    ivec2 dispatchThreadId = ivec2(gl_GlobalInvocationID.xy);
    if (dispatchThreadId.x >= outputResolution.x || dispatchThreadId.y >= outputResolution.y) return;

    vec2 screenCoord = vec2(dispatchThreadId);
    vec2 inputCoord = (screenCoord - offset) / scale;

    vec4 color = vec4(0, 0, 0, 1);

    if (inputCoord.x >= 0.0 && inputCoord.x < float(inputResolution.x) &&
        inputCoord.y >= 0.0 && inputCoord.y < float(inputResolution.y)) {
        
        // Nearest Neighbor
        color = texelFetch(InputTexture, ivec2(inputCoord), 0);
    }

    imageStore(OutputTexture, dispatchThreadId, color);
}
