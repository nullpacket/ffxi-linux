# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(ffxi-linux VERSION 0.1.0 DESCRIPTION "Native Linux client for FINAL FANTASY XI on a LandSandBoat server" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(XI_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
option(XI_BUILD_TESTS "Build unit tests (requires Catch2 v3)" ON)
option(XI_WERROR "Treat compiler warnings as errors" OFF)
# NVIDIA DLSS 4 (goal 15, docs/dlss.md): off by default. Needs the DLSS SDK (github.com/NVIDIA/DLSS; tools/fetch_dlss_sdk.sh)
# and is only used when the client runs with --dlss. Nothing of the SDK is kept in this repository.
option(XI_PRIVATE_FEATURES "Private-only features left out of the public release (/bugreport)" OFF)
option(XI_DLSS "Build the optional NVIDIA DLSS 4 path (needs XI_DLSS_SDK)" OFF)
set(XI_DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK checkout, for XI_DLSS")

include(GNUInstallDirs)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(XI_WERROR)
    add_compile_options(-Werror)
endif()
if(XI_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(OpenSSL REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(ZLIB REQUIRED)

add_library(xi_core STATIC
    src/xi/bytes.cpp
    src/xi/md5.cpp
    src/xi/blowfish.cpp
    src/xi/huffman.cpp
    src/xi/settings.cpp
    src/xi/install.cpp
    src/xi/game_data.cpp
    src/xi/ashita_dir.cpp
    src/net/socket.cpp
    src/net/tls.cpp
    src/lobby/lobby.cpp
    src/map/map_client.cpp
    src/dat/dat_index.cpp
    src/dat/chunk.cpp
    src/dat/texture.cpp
    src/gfx/zone_collision.cpp
    src/gfx/cloth.cpp
    src/dat/zone.cpp
    src/dat/collision.cpp
    src/dat/weather.cpp
    src/dat/character.cpp
    src/dat/motion.cpp
    src/dat/messages.cpp
    src/dat/emote_list.cpp
    src/dat/items.cpp
    src/dat/ui.cpp
    src/dat/event.cpp
    src/dat/event_runner.cpp
    src/dat/effect.cpp
    src/dat/dll_data.cpp
    src/dat/zone_map.cpp
    src/dat/spells.cpp
    src/dat/sound.cpp
    src/dat/macro_file.cpp
    src/dat/hd_textures.cpp
)
target_include_directories(xi_core PUBLIC src)
# The bundled Ashita addons (third_party/ashita-addons, #138): xi::ashitadir::bundledDir() looks beside the executable's
# install prefix, then at the configured one, then in this source tree (a build tree's programs).
set_property(SOURCE src/xi/ashita_dir.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    XI_INSTALL_ADDON_DIR="${CMAKE_INSTALL_FULL_DATADIR}/ffxi-linux/ashita-addons"
    XI_SOURCE_ADDON_DIR="${CMAKE_SOURCE_DIR}/third_party/ashita-addons")
target_link_libraries(xi_core PUBLIC OpenSSL::SSL OpenSSL::Crypto fmt::fmt spdlog::spdlog nlohmann_json::nlohmann_json ZLIB::ZLIB)

add_executable(xi-headless apps/xi-headless/main.cpp)
target_link_libraries(xi-headless PRIVATE xi_core)

add_executable(xi-setup apps/xi-setup/main.cpp)
target_link_libraries(xi-setup PRIVATE xi_core)

add_executable(xi-dat apps/xi-dat/main.cpp src/client/game_camera.cpp)
target_link_libraries(xi-dat PRIVATE xi_core)

# --- Graphics (Vulkan 1.3 + SDL3) ---
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_program(GLSLC glslc REQUIRED)

set(XI_SHADERS shaders/gallery.vert shaders/gallery.frag shaders/mesh.vert shaders/mesh.frag shaders/marker.vert shaders/marker.frag shaders/skinned.vert shaders/skinned.frag shaders/skinned_refract.frag shaders/sky.vert shaders/sky.frag shaders/ui.vert shaders/ui.frag shaders/fx.vert shaders/fx.frag shaders/gamma.vert shaders/gamma.frag shaders/silhouette.frag shaders/motion_camera.frag shaders/depth_upscale.frag shaders/motion_skinned.vert shaders/motion_skinned.frag shaders/rt_skin.comp shaders/rt_trace.comp shaders/rt_denoise.comp shaders/rt_composite.vert shaders/rt_composite.frag shaders/rt_reflect.frag shaders/rt_guides.comp shaders/post_volume.comp shaders/post_volume_rq.comp shaders/post_rays.comp shaders/post_prep.comp shaders/post_bloom_down.comp shaders/post_bloom_up.comp shaders/post_final.comp shaders/pt_trace.comp shaders/pt_denoise.comp shaders/pt_composite.frag shaders/pt_expose.comp)
set(XI_SPV)
foreach(src ${XI_SHADERS})
    get_filename_component(name ${src} NAME)
    set(out ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -o ${out} ${CMAKE_SOURCE_DIR}/${src}
        DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${CMAKE_SOURCE_DIR}/shaders/rt_common.glsl ${CMAKE_SOURCE_DIR}/shaders/post_common.glsl ${CMAKE_SOURCE_DIR}/shaders/post_volume.glsl ${CMAKE_SOURCE_DIR}/shaders/pt_common.glsl VERBATIM)
    list(APPEND XI_SPV ${out})
endforeach()
add_custom_target(xi_shaders DEPENDS ${XI_SPV})

add_library(xi_gfx STATIC
    src/gfx/context.cpp
    src/gfx/capabilities.cpp
    src/gfx/pipeline.cpp
    src/gfx/texture_set.cpp
    src/gfx/scene_target.cpp
    src/gfx/gamma_pass.cpp
    src/gfx/mesh_renderer.cpp
    src/gfx/zone_scene.cpp
    src/gfx/prop_layer.cpp
    src/gfx/marker_renderer.cpp
    src/gfx/effect_renderer.cpp
    src/gfx/skinned_renderer.cpp
    src/gfx/sky_renderer.cpp
    src/gfx/environment.cpp
    src/gfx/character_model.cpp
    src/gfx/character_anim.cpp
    src/gfx/look_at.cpp
    src/gfx/window.cpp
    src/gfx/dlss.cpp
    src/gfx/upscaler.cpp
    src/gfx/motion_vectors.cpp
    src/gfx/raytracing.cpp
    src/gfx/pathtrace.cpp
    src/gfx/post.cpp
)
target_include_directories(xi_gfx PUBLIC src)
target_link_libraries(xi_gfx PUBLIC xi_core Vulkan::Vulkan SDL3::SDL3)
# `VkFoo info{ VK_STRUCTURE_TYPE_FOO }` zero-fills the rest by design.
target_compile_options(xi_gfx PUBLIC -Wno-missing-field-initializers)
# vk_mem_alloc.h is a third-party single header; don't hold it to our warning flags.
set_source_files_properties(src/gfx/context.cpp PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-shadow;-Wno-pedantic;-Wno-unused-variable;-Wno-unused-parameter;-Wno-missing-field-initializers")
if(XI_DLSS)
    set(_ngx_lib ${XI_DLSS_SDK}/lib/Linux_x86_64/libnvsdk_ngx.a)
    if(NOT EXISTS ${XI_DLSS_SDK}/include/nvsdk_ngx_vk.h OR NOT EXISTS ${_ngx_lib})
        message(FATAL_ERROR "XI_DLSS needs XI_DLSS_SDK pointing at a DLSS SDK checkout (include/, lib/Linux_x86_64/); see docs/dlss.md")
    endif()
    file(GLOB _ngx_dlss_so ${XI_DLSS_SDK}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.*)
    set_property(SOURCE src/gfx/dlss.cpp APPEND PROPERTY COMPILE_DEFINITIONS XI_HAVE_DLSS=1 XI_DLSS_DEFAULT_LIB_DIR="${XI_DLSS_SDK}/lib/Linux_x86_64/rel")
    target_include_directories(xi_gfx SYSTEM PRIVATE ${XI_DLSS_SDK}/include)
    target_link_libraries(xi_gfx PRIVATE ${_ngx_lib} ${CMAKE_DL_LIBS})
    if(_ngx_dlss_so)
        install(FILES ${_ngx_dlss_so} DESTINATION ${CMAKE_INSTALL_BINDIR}) # NGX looks beside the executable
    endif()
    message(STATUS "DLSS: on (${XI_DLSS_SDK})")
endif()
include(cmake/fsr.cmake) # FSR 3.1 (issue #139): xi_ffx, src/gfx/fsr.cpp
# Where `cmake --install` puts the compiled shaders (gfx::shaderDir's last fallback).
set_property(SOURCE src/gfx/pipeline.cpp APPEND PROPERTY COMPILE_DEFINITIONS XI_INSTALL_SHADER_DIR="${CMAKE_INSTALL_FULL_DATADIR}/ffxi-linux/shaders")

# Dear ImGui (UI), pinned release, with the SDL3 + Vulkan backends.
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(xi_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(xi_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(xi_imgui PUBLIC Vulkan::Vulkan SDL3::SDL3)
target_compile_options(xi_imgui PRIVATE -w)

add_library(xi_client STATIC src/client/session.cpp src/client/entity_names.cpp src/client/combat.cpp src/client/emotes.cpp src/client/search.cpp src/client/effects.cpp src/client/effects_actor.cpp src/client/action_effects.cpp src/client/doors.cpp src/client/cutscene.cpp src/client/ships.cpp src/client/battle_colours.cpp src/client/mog_house.cpp src/client/name_icons.cpp src/client/chat_filter.cpp src/client/game_camera.cpp src/client/zone_banner.cpp src/client/guild_shop.cpp src/ashita/game_time.cpp)
target_include_directories(xi_client PUBLIC src)
target_link_libraries(xi_client PUBLIC xi_core)
target_link_libraries(xi_client PRIVATE ZLIB::ZLIB nlohmann_json::nlohmann_json)
if(XI_PRIVATE_FEATURES)
    # /bugreport's machinery (goal 48): the log ring, the bundle writer and the GitLab uploader (libcurl).
    find_package(CURL REQUIRED)
    target_sources(xi_client PRIVATE src/client/bugreport.cpp)
    target_link_libraries(xi_client PRIVATE CURL::libcurl)
endif()
find_package(glm REQUIRED)
target_link_libraries(xi_client PUBLIC glm::glm)

# The game's own interface (sprites, window layouts), drawn through ImGui draw lists.
add_library(xi_ui STATIC src/ui/button_label.cpp src/ui/ui_draw.cpp src/ui/layout_view.cpp src/ui/game_ui.cpp src/ui/game_ui_macros.cpp src/ui/game_ui_trade.cpp src/ui/game_ui_autotranslate.cpp src/ui/keymap.cpp src/ui/map_markers.cpp src/ui/currencies.cpp src/ui/full_log.cpp)
target_include_directories(xi_ui PUBLIC src)
target_link_libraries(xi_ui PUBLIC xi_gfx xi_imgui xi_client)

enable_language(C)
# Ashita v4 addon compatibility (LuaJIT; PLANS goal 31, docs/ashita-compat.md). The prelude is compiled in.
find_package(PkgConfig REQUIRED)
# LuaJIT built as Ashita builds its own: 2.1 with LUAJIT_ENABLE_LUA52COMPAT (table.pack, Lua 5.2's relaxed
# "ambiguous syntax" rule, xpcall arguments ...), static and position-independent, from a pinned commit.
FetchContent_Declare(luajit GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git GIT_TAG 95f931cb1ee7ab14f249d61fb3734803429025f8)
FetchContent_GetProperties(luajit)
if(NOT luajit_POPULATED)
    FetchContent_Populate(luajit)
endif()
set(XI_LUAJIT_A ${luajit_SOURCE_DIR}/src/libluajit.a)
add_custom_command(OUTPUT ${XI_LUAJIT_A}
    COMMAND make -C ${luajit_SOURCE_DIR}/src libluajit.a BUILDMODE=static "XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT -fPIC" CC=${CMAKE_C_COMPILER} > /dev/null
    WORKING_DIRECTORY ${luajit_SOURCE_DIR} VERBATIM)
add_custom_target(xi_luajit_build DEPENDS ${XI_LUAJIT_A})
add_library(xi_luajit STATIC IMPORTED GLOBAL)
set_target_properties(xi_luajit PROPERTIES IMPORTED_LOCATION ${XI_LUAJIT_A} INTERFACE_INCLUDE_DIRECTORIES ${luajit_SOURCE_DIR}/src
    INTERFACE_LINK_LIBRARIES "m;dl")
add_dependencies(xi_luajit xi_luajit_build)
set(XI_PRELUDE_CPP ${CMAKE_CURRENT_BINARY_DIR}/prelude_lua.cpp)
add_custom_command(OUTPUT ${XI_PRELUDE_CPP}
    COMMAND ${CMAKE_COMMAND} "-DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude.lua|${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude_memory.lua|${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude_scan.lua|${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude_d3d.lua" -DOUT=${XI_PRELUDE_CPP} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_prelude.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude.lua ${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude_memory.lua ${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude_scan.lua ${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/prelude_d3d.lua ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_prelude.cmake
    VERBATIM)
# stb_image (PNG / JPEG / BMP / TGA for D3DXCreateTextureFrom*), header-only, pinned.
FetchContent_Declare(stb URL https://github.com/nothings/stb/archive/a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d.tar.gz)
FetchContent_MakeAvailable(stb)
add_library(xi_ashita STATIC src/ashita/addon_manager.cpp src/ashita/input_map.cpp src/ashita/addon_info.cpp src/ashita/lua_native.cpp src/ashita/lua_state.cpp src/ashita/lua_imgui.cpp src/ashita/scan_registry.cpp src/ashita/d3d8_emu.cpp src/ashita/imgui_ini.cpp ${XI_PRELUDE_CPP})
target_include_directories(xi_ashita PUBLIC src)
target_include_directories(xi_ashita SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_include_directories(xi_core SYSTEM PRIVATE ${stb_SOURCE_DIR}) # stb_image for the HD texture cache (goal 41)
target_link_libraries(xi_ashita PUBLIC xi_client xi_imgui xi_luajit)

# LuaSocket and LuaSec, built against our LuaJIT for the addons that use sockets / HTTPS (mobtracker, for one):
# only their C modules (socket.core, mime.core, ssl.core / context / x509 / config), linked in and handed to
# `require` by package.preload; the Lua halves are the ones Ashita ships in addons/libs.
FetchContent_Declare(luasocket GIT_REPOSITORY https://github.com/lunarmodules/luasocket.git GIT_TAG v3.1.0 GIT_SHALLOW TRUE)
FetchContent_Declare(luasec GIT_REPOSITORY https://github.com/lunarmodules/luasec.git GIT_TAG v1.3.2 GIT_SHALLOW TRUE)
FetchContent_GetProperties(luasocket)
if(NOT luasocket_POPULATED)
    FetchContent_Populate(luasocket)
endif()
FetchContent_GetProperties(luasec)
if(NOT luasec_POPULATED)
    FetchContent_Populate(luasec)
endif()
set(XI_LS ${luasocket_SOURCE_DIR}/src)
add_library(xi_luasocket STATIC ${XI_LS}/luasocket.c ${XI_LS}/timeout.c ${XI_LS}/buffer.c ${XI_LS}/io.c ${XI_LS}/auxiliar.c
    ${XI_LS}/compat.c ${XI_LS}/options.c ${XI_LS}/inet.c ${XI_LS}/except.c ${XI_LS}/select.c ${XI_LS}/tcp.c ${XI_LS}/udp.c
    ${XI_LS}/usocket.c ${XI_LS}/mime.c)
target_link_libraries(xi_luasocket PRIVATE xi_luajit)
target_compile_options(xi_luasocket PRIVATE -w)
set_target_properties(xi_luasocket PROPERTIES POSITION_INDEPENDENT_CODE ON)
# LuaSec carries its own copy of part of LuaSocket (buffer, io, timeout, usocket); its symbols are renamed so both link.
set(XI_LSEC ${luasec_SOURCE_DIR}/src)
add_library(xi_luasec STATIC ${XI_LSEC}/ssl.c ${XI_LSEC}/context.c ${XI_LSEC}/x509.c ${XI_LSEC}/config.c ${XI_LSEC}/ec.c
    ${XI_LSEC}/options.c ${XI_LSEC}/luasocket/buffer.c ${XI_LSEC}/luasocket/io.c ${XI_LSEC}/luasocket/timeout.c
    ${XI_LSEC}/luasocket/usocket.c)
target_include_directories(xi_luasec PRIVATE ${XI_LSEC} ${XI_LSEC}/luasocket)
target_compile_options(xi_luasec PRIVATE -w -include ${CMAKE_CURRENT_SOURCE_DIR}/src/ashita/luasec_rename.h)
target_compile_definitions(xi_luasec PRIVATE WITH_LUASOCKET LUASOCKET_DEBUG)
target_link_libraries(xi_luasec PRIVATE xi_luajit OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(xi_ashita PRIVATE xi_luasocket xi_luasec OpenSSL::SSL OpenSSL::Crypto)

# Sound output (SDL3 audio).
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil) # ATRAC3 (format 3 waves and music)
add_library(xi_audio STATIC src/audio/mixer.cpp src/audio/atrac3.cpp src/audio/sound_system.cpp src/audio/footsteps.cpp)
target_include_directories(xi_audio PUBLIC src)
target_link_libraries(xi_audio PUBLIC xi_core xi_client SDL3::SDL3 PkgConfig::FFMPEG)

add_executable(xi-client apps/xi-client/main.cpp apps/xi-client/chat_commands.cpp apps/xi-client/characters.cpp apps/xi-client/actor_poses.cpp apps/xi-client/ground_shadows.cpp apps/xi-client/mog_house.cpp apps/xi-client/test_script.cpp apps/xi-client/client_app.cpp apps/xi-client/input.cpp apps/xi-client/world_frame.cpp apps/xi-client/test_hooks.cpp apps/xi-client/script_steps.cpp apps/xi-client/zone_frame.cpp apps/xi-client/movement.cpp apps/xi-client/ui_frame.cpp apps/xi-client/actors_draw.cpp apps/xi-client/target_flash.cpp apps/xi-client/effects_frame.cpp apps/xi-client/render.cpp apps/xi-client/addons.cpp apps/xi-client/addon_native.cpp apps/xi-client/fishing.cpp apps/xi-client/addons_page.cpp)
# /bugreport is private: without XI_PRIVATE_FEATURES the command says it is not built in.
if(XI_PRIVATE_FEATURES)
    target_sources(xi-client PRIVATE apps/xi-client/bugreport.cpp)
else()
    target_sources(xi-client PRIVATE apps/xi-client/bugreport_none.cpp)
endif()
target_link_libraries(xi-client PRIVATE xi_gfx xi_client xi_imgui xi_ui xi_audio xi_ashita nlohmann_json::nlohmann_json)
# Ashita plugins built natively (issue #34): the plugin host when the Ashita SDK is found, and the plugins to build.
include(cmake/ashita_plugins.cmake)
xi_ashita_plugin_exports(xi-client)
# /bugreport records the build (goal 48): the commit at configure time and whether DLSS is in.
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD OUTPUT_VARIABLE XI_GIT_COMMIT_VALUE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT XI_GIT_COMMIT_VALUE)
    set(XI_GIT_COMMIT_VALUE unknown)
endif()
if(XI_DLSS)
    set(_xi_has_dlss 1)
else()
    set(_xi_has_dlss 0)
endif()
set_property(SOURCE apps/xi-client/bugreport.cpp apps/xi-client/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS XI_GIT_COMMIT="${XI_GIT_COMMIT_VALUE}" XI_HAS_DLSS=${_xi_has_dlss})
add_dependencies(xi-client xi_shaders)

# xi-launcher (goal 44): server profiles, settings and presets, one-click Connect (SDL3's renderer + ImGui).
add_library(xi_launcher_core STATIC src/launcher/profile.cpp src/launcher/process.cpp src/launcher/first_run.cpp)
target_include_directories(xi_launcher_core PUBLIC src)
target_link_libraries(xi_launcher_core PUBLIC fmt::fmt xi_core)
add_executable(xi-launcher apps/xi-launcher/main.cpp)
target_link_libraries(xi-launcher PRIVATE xi_launcher_core xi_core xi_imgui nlohmann_json::nlohmann_json)

add_executable(xi-viewer apps/xi-viewer/main.cpp)
target_link_libraries(xi-viewer PRIVATE xi_client xi_gfx)
add_dependencies(xi-viewer xi_shaders)

if(XI_BUILD_TESTS)
    find_package(Catch2 3 QUIET)
    if(Catch2_FOUND)
        enable_testing()
        add_executable(xi_tests tests/test_blowfish.cpp tests/test_search.cpp tests/test_mog_house.cpp tests/test_effects.cpp tests/test_huffman.cpp tests/test_chunk.cpp tests/test_zone.cpp tests/test_combat.cpp tests/test_settings.cpp tests/test_install.cpp
                                tests/test_keymap.cpp tests/test_sound.cpp tests/test_collision.cpp tests/test_move.cpp tests/test_effects_actor.cpp tests/test_effects_7b.cpp tests/test_skills.cpp tests/test_name_icons.cpp tests/test_chat_filter.cpp tests/test_game_camera.cpp tests/test_ashita.cpp tests/test_ashita_scan.cpp tests/test_game_time.cpp tests/test_macro_file.cpp tests/test_hd_textures.cpp tests/test_addon_info.cpp tests/test_launcher.cpp tests/test_first_run.cpp tests/test_ashita_dir.cpp tests/test_entity_names.cpp tests/test_pacing.cpp tests/test_session_end.cpp tests/test_status_pulse.cpp tests/test_zone_banner.cpp tests/test_button_label.cpp tests/test_map_markers.cpp tests/test_currencies.cpp tests/test_guild_shop.cpp tests/test_actor_ground.cpp tests/test_full_log.cpp tests/test_item_count.cpp tests/test_equip_rules.cpp tests/test_imgui_ini.cpp tests/test_ws_motion.cpp tests/test_loot_text.cpp tests/test_target_lock.cpp tests/test_item_sort_keys.cpp tests/test_status_select.cpp tests/test_compass.cpp tests/test_list_cursor.cpp tests/test_yes_no.cpp tests/test_log_object.cpp src/gfx/character_anim.cpp src/ui/currencies.cpp src/ui/full_log.cpp src/ui/button_label.cpp src/ui/keymap.cpp src/ui/map_markers.cpp src/gfx/zone_collision.cpp)
        target_link_libraries(xi_tests PRIVATE xi_core xi_client xi_audio xi_ashita xi_launcher_core SDL3::SDL3 Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
        xi_ashita_plugin_exports(xi_tests)
        if(XI_PRIVATE_FEATURES)
            target_sources(xi_tests PRIVATE tests/test_bugreport.cpp)
        endif()
        include(Catch)
        catch_discover_tests(xi_tests)
        # The Ashita layer's memory and resource interfaces (headless, against a hand-built world).
        add_executable(xi_ashita_memory_tests tests/test_ashita_memory.cpp)
        target_link_libraries(xi_ashita_memory_tests PRIVATE xi_ashita xi_core Catch2::Catch2WithMain)
        catch_discover_tests(xi_ashita_memory_tests)
        # Direct3D 8 / D3DX / gdifonts stand-ins (headless).
        add_executable(xi_ashita_d3d_tests tests/test_ashita_d3d.cpp)
        target_link_libraries(xi_ashita_d3d_tests PRIVATE xi_ashita xi_core Catch2::Catch2WithMain)
        catch_discover_tests(xi_ashita_d3d_tests)
        # Natively built Ashita plugins (issue #34): the host against a test plugin built from tests/ (SDK builds only).
        if(XI_HAVE_ASHITA_PLUGINS)
            xi_ashita_plugin_module(xi_test_plugin_xitest xitest ${CMAKE_BINARY_DIR}/test-plugins tests/ashita_plugin_xitest.cpp)
            add_executable(xi_ashita_plugin_tests tests/test_ashita_plugins.cpp)
            target_link_libraries(xi_ashita_plugin_tests PRIVATE xi_ashita xi_core Catch2::Catch2WithMain)
            target_compile_definitions(xi_ashita_plugin_tests PRIVATE "XI_TEST_PLUGIN=\"${CMAKE_BINARY_DIR}/test-plugins/xitest.so\"")
            add_dependencies(xi_ashita_plugin_tests xi_test_plugin_xitest)
            xi_ashita_plugin_exports(xi_ashita_plugin_tests)
            catch_discover_tests(xi_ashita_plugin_tests)
        endif()
        # DLSS helpers (goal 15): jitter, modes, the jittered projection; when the swapchain is stale (#78); the upscaler
        # choice, FSR modes and the GPU-pretend limits (#139) (no GPU).
        add_executable(xi_gfx_tests tests/test_dlss.cpp tests/test_swapchain.cpp tests/test_upscaler.cpp)
        target_link_libraries(xi_gfx_tests PRIVATE xi_gfx Catch2::Catch2WithMain)
        catch_discover_tests(xi_gfx_tests)
        # FSR 3.1 on the GPU (#139): context, shared resources, dispatches, validation clean; skips without a GPU.
        add_executable(xi_fsr_gpu_test tests/test_fsr_gpu.cpp)
        target_link_libraries(xi_fsr_gpu_test PRIVATE xi_gfx)
        add_test(NAME fsr_gpu COMMAND xi_fsr_gpu_test)
        set_tests_properties(fsr_gpu PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEO_DRIVER=x11")
        # The bundled Ashita addons (#138) match their manifest: nothing added, removed or edited by hand.
        find_program(XI_PYTHON3 python3)
        if(XI_PYTHON3)
            add_test(NAME ashita_bundle COMMAND ${XI_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/vendor_ashita_addons.py check)
            set_tests_properties(ashita_bundle PROPERTIES ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
        endif()
    else()
        message(STATUS "Catch2 v3 not found; skipping tests")
    endif()
endif()

# Install: the programs in bin/, the launcher as `ffxi-linux`, the shaders in share/ffxi-linux/shaders, the bundled
# Ashita addons in share/ffxi-linux/ashita-addons, a desktop entry.
install(TARGETS xi-client xi-launcher xi-setup xi-headless xi-dat xi-viewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS scripts/xi-client.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME ffxi-linux)
install(FILES ${XI_SPV} DESTINATION ${CMAKE_INSTALL_DATADIR}/ffxi-linux/shaders)
install(FILES packaging/ffxi-linux.desktop packaging/ffxi-linux-launcher.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY third_party/ashita-addons/ DESTINATION ${CMAKE_INSTALL_DATADIR}/ffxi-linux/ashita-addons)
install(FILES README.md LICENSE THIRD-PARTY.md config/client.ini.example scripts/local.env.example DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Packages (local only): `cpack --config build/CPackConfig.cmake` makes a .tar.gz everywhere, a .deb when built on
# Debian / Ubuntu and an .rpm when built on Fedora. A package's dependencies come from the libraries the binaries link
# (dpkg-shlibdeps / rpmbuild's automatic requires), so build it on the distribution it is for.
set(CPACK_PACKAGE_NAME ffxi-linux)
set(CPACK_PACKAGE_VENDOR "ffxi-linux")
set(CPACK_PACKAGE_CONTACT "ffxi-linux <nobody@localhost>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY ffxi-linux)
set(CPACK_STRIP_FILES ON)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_GENERATOR TGZ)
if(EXISTS /etc/debian_version)
    list(APPEND CPACK_GENERATOR DEB)
endif()
if(EXISTS /etc/fedora-release)
    list(APPEND CPACK_GENERATOR RPM)
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION games)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later") # LICENSE; THIRD-PARTY.md lists the dependencies (#132)
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
include(CPack)
