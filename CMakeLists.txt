cmake_minimum_required(VERSION 3.2)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(KOTOR2 C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "KOTOR II")
set(VITA_TITLEID  "KOTR00002")
set(VITA_VERSION  "01.00")
# ATTRIBUTE2=12 grants the extended-memory partition (~+109 MB + phycont).
# Without it the app only has the base user partition and a large newlib heap
# fails to allocate at CRT init -- which crashes in _sbrk_r before main().
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# NOTE: This SDK (vitaGL/vitashark/mathneon) is built hardfp ("VFP registers"),
# so the loader is compiled hardfp to link. The Android armeabi-v7a ABI is
# softfp, so the RUNTIME port will need a softfp toolchain (rebuild vitaGL &
# deps with -mfloat-abi=softfp) to match float arguments across the .so
# boundary. That is a runtime-phase concern; this skeleton only needs to link.
# -z max-page-size: vita-elf-create appends ~4.5 KB of SCE data into the slack
# between the RX and RW segments, and with the default 4 KB alignment that slack
# is one page -- so a build fails with "Cannot allocate N bytes for SCE data at
# end of segment 0" as soon as .text lands in the wrong place. It did, over 32
# bytes. At 64 KB the slack is a page of 64 KB rather than 4 KB, which is 14x
# what the SCE data needs and is a floor rather than a coincidence.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -D_GNU_SOURCE -Wl,-q -Wl,-z,max-page-size=0x10000 -Wall -O2 -fno-optimize-sibling-calls")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++11 -Wno-write-strings")

option(KOTOR_BINK_SHADER_TEST "Initialize the embedded Bink shaders while continuing to skip movies" OFF)
option(KOTOR_BINK_LEGAL_VIDEO_TEST "Play silent legal.bik once while skipping all audio-bearing movies" OFF)
option(KOTOR_BINK_OPENSL_TEST "Play short 01c.bik once through the existing Vita mixer" OFF)

add_executable(KOTOR2
  loader/main.c
  loader/so_util.c
  loader/dynlib.c
  loader/obb_index.c
  loader/loadscreen.c
  loader/obbzip.c
  loader/tga.c
  loader/hints.c
  loader/font.c
  loader/jni_patch.c
  loader/audio_patch.c
  loader/audio_mp3.c
  loader/audio_ring.c
  loader/bink_audio_queue.c
  loader/opensl_patch.c
  loader/bink_patch.c
  loader/fs_patch.c
  loader/sdl_patch.c
  loader/input_patch.c
  loader/ime_patch.c
  loader/gl_patch.c
  loader/gl_worker.c
  loader/glw_gen.c
  loader/glw_prof.c
  loader/occlusion_cull.c
  loader/ai_list_cache.c
  loader/cansee_cache.c
  loader/bloom_ctl.c
  loader/ramfile.c
  loader/obb_cache.c
  loader/gl_state_filter.c
  loader/ai_budget.c
  loader/autotest.c
  loader/glsl_prep.c
  loader/crash.c
  loader/heap.c
  loader/bigalloc.c
  loader/log.c
)

add_custom_command(TARGET KOTOR2 POST_BUILD
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/check_vitagl_build.sh
          $<TARGET_FILE:KOTOR2> ${CMAKE_NM}
  COMMENT "Checking KOTOR II vitaGL build flags"
)

# KOTOR II embeds its Bink decoder, renderer, and OpenSL adapter in libkotor2.so.
# Route the observed OpenSL contract into the existing Vita mixer; config.h
# selects normal movie playback by default.
target_compile_definitions(KOTOR2 PRIVATE KOTOR2_BUILD=1 KOTOR_USE_BINK_OPENSL=1)


if((KOTOR_BINK_SHADER_TEST AND KOTOR_BINK_LEGAL_VIDEO_TEST) OR
   (KOTOR_BINK_SHADER_TEST AND KOTOR_BINK_OPENSL_TEST) OR
   (KOTOR_BINK_LEGAL_VIDEO_TEST AND KOTOR_BINK_OPENSL_TEST))
  message(FATAL_ERROR "Select only one Bink test mode")
endif()
if(KOTOR_BINK_SHADER_TEST)
  target_compile_definitions(KOTOR2 PRIVATE BINK_MODE=1)
elseif(KOTOR_BINK_LEGAL_VIDEO_TEST)
  target_compile_definitions(KOTOR2 PRIVATE BINK_MODE=2)
elseif(KOTOR_BINK_OPENSL_TEST)
  target_compile_definitions(KOTOR2 PRIVATE BINK_MODE=3)
endif()

target_link_libraries(KOTOR2
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  # real-code archives first, then the libs they depend on (stdc++, m) so the
  # linker resolves left-to-right without undefined std:: references
  SDL2
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  # FreeType (KOTOR's font/text engine) + its decode deps, before stdc++/m
  freetype
  png16
  z
  bz2
  stdc++
  m
  taihen_stub
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudiodec_stub   # hardware MP3 decode for the FMOD backend (audio_mp3.c)
  SceAudioIn_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceVshBridge_stub
  # additional Sce stubs pulled in by Vita SDL2 (input/motion/ime/fios)
  SceHid_stub
  SceMotion_stub
  SceIme_stub
  SceFios2_stub
)

# Bundle our edited GLSL shader chunks into the VPK at app0:shaders/. The loader's
# SDL_RWFromFile hook serves these in preference to ux0:data/kotor2/ so the shaders
# (which we edit for GXM's TEXCOORD budget) always travel with the loader and never
# depend on a manual card copy. Globbed from the private sibling runtime/assets
# input; if that directory is absent, the loader falls back to the card.
file(GLOB SHADER_SRCS
  "${CMAKE_SOURCE_DIR}/../runtime/assets/*.vert"
  "${CMAKE_SOURCE_DIR}/../runtime/assets/*.frag"
  "${CMAKE_SOURCE_DIR}/../runtime/assets/*.glsl")
set(SHADER_VPK_FILES "")
foreach(f ${SHADER_SRCS})
  get_filename_component(fn ${f} NAME)
  list(APPEND SHADER_VPK_FILES ${f} "shaders/${fn}")
endforeach()

# Bundle font metrics (.txi) into the VPK at app0:fonts/. Aspyr shipped the loose
# high-res font TGAs in override/ WITHOUT their companion .txi glyph metrics (the
# real metrics are embedded in the swpc_tex_gui.erf TPCs). Without them the game
# frees fontInfo -> null -> no GUI text. We extract the TXI text from those TPCs and
# ship them here; the AurResGet .txi-fallback hook + SDL_RWFromFile hook serve them.
#
# iosdialog.otf rides along in the same directory. The compatibility behavior
# here was inherited from VitaKotor (KOTOR I); it is not KOTOR II validation.
# The FT_New_Face shim in dynlib.c tries app0:fonts/ before the card.
file(GLOB FONT_TXI_SRCS
  "${CMAKE_SOURCE_DIR}/../runtime/assets/*.txi"
  "${CMAKE_SOURCE_DIR}/../runtime/assets/*.otf"
  "${CMAKE_SOURCE_DIR}/../runtime/assets/*.ttf")
set(FONT_VPK_FILES "")
foreach(f ${FONT_TXI_SRCS})
  get_filename_component(fn ${f} NAME)
  list(APPEND FONT_VPK_FILES ${f} "fonts/${fn}")
endforeach()

vita_create_self(eboot.bin KOTOR2 UNSAFE)
vita_create_vpk(KOTOR2.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
       ${SHADER_VPK_FILES}
       ${FONT_VPK_FILES}
)

add_custom_target(check-livearea
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/check_livearea.sh ${CMAKE_SOURCE_DIR}
  COMMENT "Validating Vita LiveArea assets"
)
add_dependencies(KOTOR2.vpk-vpk check-livearea)

# Keep the last few builds (vpk + matching source tarball) under backups/ so a
# bad change can be undone without a rebuild. Never fails the build.
add_custom_command(TARGET KOTOR2.vpk-vpk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E env KEEP_BUILDS=4
          bash ${CMAKE_SOURCE_DIR}/tools/snapshot.sh || ${CMAKE_COMMAND} -E true
  COMMENT "Snapshotting build to backups/"
)

# GL worker (loader/gl_worker.c): every vitaGL function the loader uses is
# wrapped at link time, so all GL calls go through the worker's command stream.
include(${CMAKE_SOURCE_DIR}/cmake/glw_wraps.cmake)
target_link_libraries(KOTOR2 ${GLW_WRAP_FLAGS})

option(KOTOR_AUTOTEST "Drive menus and gameplay with scripted controller input" OFF)
set(KOTOR_AUTOTEST_PLAY_SECONDS 120 CACHE STRING "Autotest gameplay duration")
if(KOTOR_AUTOTEST)
  target_compile_definitions(KOTOR2 PRIVATE AUTOTEST_ENABLE=1 AUTOTEST_PLAY_S=${KOTOR_AUTOTEST_PLAY_SECONDS})
  target_link_libraries(KOTOR2 "-Wl,--wrap=sceCtrlPeekBufferPositive2")
endif()
