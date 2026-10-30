include(ExternalProject)

set(CONVERTER_SRC ${CMAKE_SOURCE_DIR}/tools/image_converter)
set(CONVERTER_BIN ${CMAKE_BINARY_DIR}/image_converter)
set(IMAGE_OUT     ${CMAKE_BINARY_DIR}/generated_images)

ExternalProject_Add(image_converter_host
    SOURCE_DIR       ${CONVERTER_SRC}
    BINARY_DIR       ${CONVERTER_BIN}
    CMAKE_ARGS       -DCMAKE_BUILD_TYPE=Release
    INSTALL_COMMAND  ""
)
if(WIN32)
    set(CONVERTER_EXE ${CONVERTER_BIN}/convert.exe)
else()
    set(CONVERTER_EXE ${CONVERTER_BIN}/convert)
endif()

file(GLOB IMAGE_PNGS CONFIGURE_DEPENDS ${CONVERTER_SRC}/images/*.png)
list(SORT IMAGE_PNGS)
file(MAKE_DIRECTORY ${IMAGE_OUT}/images_bin)

add_custom_command(
    OUTPUT  ${IMAGE_OUT}/imagedata.h ${IMAGE_OUT}/imagedata_bin.S
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${IMAGE_OUT}/images_bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IMAGE_OUT}/images_bin
    COMMAND ${CONVERTER_EXE} ${IMAGE_OUT} ${IMAGE_PNGS}
    DEPENDS image_converter_host ${IMAGE_PNGS}
    COMMENT "Converting PNG images to 8x8 RGB data"
)
add_custom_target(generate_images
    DEPENDS ${IMAGE_OUT}/imagedata.h ${IMAGE_OUT}/imagedata_bin.S)