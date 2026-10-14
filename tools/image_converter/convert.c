#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

// Packs red, green and blue into one number
static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r) << 8) | ((uint32_t)(g) << 16) | (uint32_t)(b);
}

int main(int argc, char *argv[]){
// Get user input of the actual png name and the name we want to give
if (argc < 3){
printf("Usage : convert file.png array_name\n");
return 1;
}
const char *file_name = argv[1];
const char *array_name = argv[2];

// Convert png to array using stbi extension
int width, height, channels;
unsigned char *pixels = stbi_load(file_name, &width, &height, &channels, 3);
if (pixels == NULL){
    printf("load failed: %s\n", stbi_failure_reason());
    return 1;
}
printf("loaded %d x %d\n", width, height);

// To adjust the size to display the image on the LED
unsigned char small[8*8*3];
unsigned char *result = stbir_resize(
    pixels, width, height, 0,
    small, 8, 8, 0,
    STBIR_RGB,
    STBIR_TYPE_UINT8,
    STBIR_EDGE_CLAMP,
    STBIR_FILTER_POINT_SAMPLE
);

if (result == NULL){
    printf("resize failed\n");
    return 1;
}
stbi_image_free(pixels);
printf("first pixel: %d %d %d\n", small[0], small[1], small[2]);

// To adjust the brightness
uint32_t frame[64];
int lit = 0;
for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
        int pos = (y * 8 + x) * 3;
        int red = small[pos];
        int green = small[pos + 1];
        int blue = small[pos + 2];

        // Scale to 20 percent brightness
        red = red * 20 / 100;
        green = green * 20 / 100;
        blue = blue * 20 / 100;

        // reverse odd rows so panel LED lights up correctly
        int index = y * 8 + x;
        frame[index] = urgb_u32(red, green, blue);
        
        if (frame[index] != 0) {
            lit++;
        }
    }
}
printf("lit LEDs: %d\n", lit);

// To display convert image as array
printf("const uint32_t %s[64] = {\n", array_name);
for (int i = 0; i < 64; i++){
    if (i % 8 == 0){
        printf("    ");
    }
    printf("0x%08X", frame[i]);
    if (i < 63){
        printf(",");
    }
    if (i % 8 == 7){
        printf("\n");
    }
    else{
        printf(" ");
    }
}
printf("};\n");

// Preview the picture in the terminal, # is a lit LED and . is a dark LED
printf("\nImage Preview:\n");
for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
        int pos = (y * 8 + x) * 3;
        // Uses the 8x8 RGB values before brightness dimming so it's clearly visible in console
        printf("\033[48;2;%d;%d;%dm  \033[0m", small[pos], small[pos + 1], small[pos + 2]);
    }
    printf("\n");
}

return 0;
}