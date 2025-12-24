#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0, rgba8) uniform readonly image2D inputImage;
layout(binding = 1, rgba8) uniform writeonly image2D outputImage;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 color = imageLoad(inputImage, pos);
    float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    
    if(brightness > 0.7) // Threshold
        imageStore(outputImage, pos, color);
    else
        imageStore(outputImage, pos, vec4(0.0, 0.0, 0.0, 1.0));
}
