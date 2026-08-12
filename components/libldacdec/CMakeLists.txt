idf_component_register(
    SRCS "libldacdec.c" "bit_allocation.c" "huffCodes.c" "bit_reader.c"
         "utility.c" "imdct.c" "spectrum.c"
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
