# h264_dec (Hiroki Kawakami, MIT) - the H.264 decoder of the video service
# (services/tab5_video). Third-party code, with the licence alongside; the exact
# origin is in ORIGEM.txt and what we changed in it, in MUDANCAS.md.
#
# Merges the original project's two components (h264_dec and vdec_common): the
# common kernels (vdec_kernels_*) only serve this decoder here.
idf_component_register(
    SRCS
        "src/bits.c"
        "src/cabac.c"
        "src/cabac_tables.c"
        "src/deblock.c"
        "src/decoder.c"
        "src/inter.c"
        "src/intra.c"
        "src/kernels_c.c"
        "src/macroblock.c"
        "src/params.c"
        "src/rows.c"
        "src/slice.c"
        "src/tables.c"
        "src/transform.c"
        "src/vdec_kernels_c.c"
    INCLUDE_DIRS "include")

# On the P4, the hot routines in PIE extension assembly (128-bit SIMD).
if(IDF_TARGET STREQUAL "esp32p4")
    target_sources(${COMPONENT_LIB} PRIVATE "src/kernels_p4.S" "src/vdec_kernels_p4.S")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE H264_DEC_PIE VDEC_PIE)
endif()

# -O2 as in the original (the -Os of the rest of the system costs speed here).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# Per-stage measurement (the 'video h264' benchmark prints it): idf.py ... -DH264_DEC_PROFILE=1
if(H264_DEC_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC H264_DEC_PROFILE)
endif()
