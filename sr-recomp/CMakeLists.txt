# SAINTS ROW: ReInstated: the two recompiled games and their app, built inside gta4-ps5's tree
# (glue/rexglue-sdk-main/sr-recomp, see tools/local/build.sh) as two static PS5 programs, the
# way gta4-recomp builds GTA IV: sr1 (Saints Row, eboot.bin: the menu opens on it) and sr2
# (Saints Row 2, sr2.bin, which the menu starts). Both are made from the same app sources, told
# apart by SR_GAME.

foreach(game 1 2)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/p${game}/generated/sources.cmake")
        message(FATAL_ERROR "sr-recomp/p${game} generated sources are missing: run rexglue codegen p${game}/sr${game}_manifest.toml")
    endif()
endforeach()

function(sr_game game)
    set(target sr${game})
    include(p${game}/generated/sources.cmake)
    set(app_sources src/main.cpp src/sr_app.cpp)
    if(PS5)
        # The menu (its fonts: gta4-ps5's, SIL Open Font License) and gta4-ps5's
        # own sound mixer for its song.
        set(GTA4_PS5_SRC "${REXSDK_DIR}/gta4-recomp/src/ps5")
        set(SR_PS5_FONTS "${REXSDK_DIR}/../../ps5/assets/fonts")
        list(APPEND app_sources src/ps5/sr_menu.cpp src/ps5/sr_install.cpp src/ps5/sr_trophies.cpp
             src/ps5/sr_trophies_game.cpp src/ps5/sr_overlay.cpp src/ps5/sr_dualsense.cpp src/ps5/sr_prompts.cpp
             src/ps5/sr_lua.cpp src/ps5/sr_quick_menu.cpp src/ps5/sr_intro.cpp
             src/ps5/sr_textures_install.cpp
             "${GTA4_PS5_SRC}/game/sound.cpp" "${GTA4_PS5_SRC}/hub/hub_sfx.cpp")
        # The PlayStation button prompts (tools/prompts/make_prompts.py, made from the user's game
        # files: SR_PROMPTS_DIR, default /root/srgame/prompts); without them, an empty blob.
        set(SR_PROMPTS_DIR "/root/srgame/prompts" CACHE PATH "make_prompts.py's prompts<N>.bin")
        set(sr_prompts_bin "${SR_PROMPTS_DIR}/prompts${game}.bin")
        if(NOT EXISTS "${sr_prompts_bin}")
            set(sr_prompts_bin "${CMAKE_CURRENT_BINARY_DIR}/prompts${game}-none.bin")
            file(WRITE "${sr_prompts_bin}" "")
        endif()
        set_property(SOURCE src/ps5/sr_prompts.cpp APPEND PROPERTY OBJECT_DEPENDS "${sr_prompts_bin}")
        add_executable(${target} ${app_sources} ${GENERATED_SOURCES})
        target_compile_definitions(${target} PRIVATE SR_GAME=${game} SR_GAME_HOOKS=0 "SR_INIT_H=\"sr${game}_init.h\""
            "SRPS5_FONTS_DIR=\"${SR_PS5_FONTS}\"" "SRPS5_PROMPTS_BIN=\"${sr_prompts_bin}\"")
        target_include_directories(${target} PRIVATE "${GTA4_PS5_SRC}")
        # The recompiled game's own files, as gta4-recomp marks GTA IV's.
        set(guest_sources ${GENERATED_SOURCES})
        list(FILTER guest_sources INCLUDE REGEX "sr${game}_recomp\.[0-9]+\.cpp$")
        set_property(SOURCE ${guest_sources} APPEND PROPERTY COMPILE_DEFINITIONS REX_PS5_GUEST_CODE=1)
        set_property(SOURCE p${game}/generated/sr${game}_register.cpp APPEND PROPERTY COMPILE_OPTIONS
            "$<$<CONFIG:Release,RelWithDebInfo>:-O0>")
        target_compile_definitions(${target} PRIVATE REX_FUSED_VMADDFP=1)
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/p${game}/generated
            ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto
            ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
            ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/inja/third_party/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/stb
        )
        target_compile_options(${target} PRIVATE "SHELL:-include ${REXSDK_DIR}/include/rex/ps5_compat.h")
        target_link_libraries(${target} PRIVATE
            rex::runtime
            rexgpu-xenos
            xxHash::xxhash
            SDL3::SDL3
            SceUserService ScePad SceAudioOut SceSystemService SceVideoOut
        )
        rexglue_configure_target(${target})
    else()
        add_executable(${target} ${app_sources} ${GENERATED_SOURCES})
        target_compile_definitions(${target} PRIVATE SR_GAME=${game} "SR_INIT_H=\"sr${game}_init.h\"")
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/p${game}/generated
            ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
        )
        target_link_libraries(${target} PRIVATE rex::runtime)
        rexglue_configure_target(${target} GPU_PLUGINS xenos)
    endif()
endfunction()

sr_game(1)
sr_game(2)
