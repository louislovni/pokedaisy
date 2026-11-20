# ROM patches (pk_patch.c: IPS, UPS, BPS, xdelta with xdelta3's decoder and XZ Embedded for its LZMA): one static
# library, for the app's JNI (PATCH A ROM) and the PC host. Include this file, then link `pkpatch`.
set(PKPATCH_DIR "${CMAKE_CURRENT_LIST_DIR}")
add_library(pkpatch STATIC
    "${PKPATCH_DIR}/pk_patch.c"
    "${PKPATCH_DIR}/xd3_capsule.c"
    "${PKPATCH_DIR}/xz-shim/lzma_shim.c"
    "${PKPATCH_DIR}/xz-embedded/xz_crc32.c"
    "${PKPATCH_DIR}/xz-embedded/xz_dec_lzma2.c"
    "${PKPATCH_DIR}/xz-embedded/xz_dec_stream.c")
target_include_directories(pkpatch PUBLIC "${PKPATCH_DIR}" PRIVATE "${PKPATCH_DIR}/xz-shim" "${PKPATCH_DIR}/xz-embedded")
set_target_properties(pkpatch PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(pkpatch PRIVATE -w)
endif()
