idf_component_register(
    SRCS "freeaptx.c"
    INCLUDE_DIRS "include"
)

# freeaptX is a compute-heavy subband decoder.  The project is normally built
# with debug optimization, which is not fast enough for a real-time A2DP stream
# on the original ESP32.  Keep the rest of the application debuggable and only
# optimize this codec component.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
