idf_component_register(
    # ldacenc.c is a desktop command-line encoder. It depends on libsndfile
    # and libsamplerate, so it must not be compiled into the ESP32 decoder.
    SRCS "libldacdec.c" "bit_allocation.c" "huffCodes.c" "bit_reader.c"
         "utility.c" "imdct.c" "spectrum.c"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
