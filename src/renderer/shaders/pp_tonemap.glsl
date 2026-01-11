#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0, rgba8) uniform readonly image2D inputImage;
layout(binding = 1, rgba8) uniform writeonly image2D outputImage;

// Configuration for Color Grading
// Adjust these values to tune the look of the game
const float InputBlackPoint = 0.06; // Crush blacks (0.0 - 1.0). Higher = darker blacks. Fixes "grayish" blacks.
const float InputWhitePoint = 0.95; // Clip whites (0.0 - 1.0). Lower = brighter whites.
const float Gamma = 1.1;            // Gamma correction. > 1.0 brightens midtones, < 1.0 darkens.
const float Saturation = 1.15;      // Color saturation. 1.0 = normal, 0.0 = grayscale, > 1.0 = vibrant.
const float Exposure = 1.0;         // Overall exposure.

vec3 adjustLevels(vec3 color, float inBlack, float inWhite, float gamma) {
    return pow(max(color - inBlack, 0.0) / (inWhite - inBlack), vec3(1.0 / gamma));
}

vec3 adjustSaturation(vec3 color, float saturation) {
    float grey = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(grey), color, saturation);
}

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 color = imageLoad(inputImage, pos);
    vec3 rgb = color.rgb;

    // 1. Levels Adjustment (Fixes grayish blacks and contrast)
    rgb = adjustLevels(rgb, InputBlackPoint, InputWhitePoint, Gamma);

    // 2. Saturation Adjustment (Fixes "strange" or washed out colors)
    rgb = adjustSaturation(rgb, Saturation);

    // 3. Exposure / Tone Mapping (Reinhard-ish soft clip)
    rgb = vec3(1.0) - exp(-rgb * Exposure);

    imageStore(outputImage, pos, vec4(rgb, color.a));
}
