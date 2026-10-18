# LED Memory Game

## Convert multiple pictures at once

Put your PNG files in `tools/image_converter/images`. Name each file with letters, digits, and underscores only, such as `arrow_up.png`.

Run these commands from the project root:

```bash
cd tools/image_converter
mkdir -p ../../src/display/images_bin
gcc convert.c -o convert -lm
./convert images/*.png
```

The converter creates these files:

- `src/display/images_bin/NAME.bin`: the 8x8 RGB data for each picture.
- `src/display/imagedata_bin.S`: embeds each `.bin` file in the firmware.
- `src/display/imagedata.h`: declares `NAME_bin` for each picture.

Do not edit the generated files by hand. Run the converter again after you add or change a picture.
