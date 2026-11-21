# SPDX-License-Identifier: GPL-3.0-or-later
# AMD FidelityFX Super Resolution 3.1 (the upscaler only), the vendor-neutral upscaler (issue #139, docs/fsr.md).
#
# The FidelityFX SDK (MIT) is not kept in this repository: XI_FSR_SDK names a checkout (tools/fetch_fsr_sdk.sh), and
# without one the pinned release is fetched into the build tree at configure time, as ImGui is. The SDK's Windows-only
# shader compiler is replaced by tools/fsr_shaders.py (glslangValidator + a small SPIR-V reader), which writes the
# permutation headers its code includes; src/gfx/fsr/ffx_linux_compat.h fills in the few MSVC library calls.
# Without the SDK (offline, no glslangValidator) the client builds without FSR and --fsr says so.
option(XI_FSR "Build the FSR 3.1 upscaler (AMD FidelityFX SDK, MIT; fetched when XI_FSR_SDK is empty)" ON)
set(XI_FSR_SDK "" CACHE PATH "AMD FidelityFX SDK checkout (tools/fetch_fsr_sdk.sh), for XI_FSR")
set(XI_FSR_SDK_TAG v1.1.4)
set(XI_FSR_SDK_COMMIT c6efa6bf7f2027b3ec94f28578bb5965eabb9e55) # the tag's commit, checked after a fetch

set(XI_HAVE_FSR OFF)
if(XI_FSR)
    set(_fsr_sdk "${XI_FSR_SDK}")
    if(NOT _fsr_sdk)
        if(DEFINED ENV{XDG_DATA_HOME} AND EXISTS "$ENV{XDG_DATA_HOME}/ffxi-linux/fidelityfx-sdk/LICENSE.txt")
            set(_fsr_sdk "$ENV{XDG_DATA_HOME}/ffxi-linux/fidelityfx-sdk")
        elseif(EXISTS "$ENV{HOME}/.local/share/ffxi-linux/fidelityfx-sdk/LICENSE.txt")
            set(_fsr_sdk "$ENV{HOME}/.local/share/ffxi-linux/fidelityfx-sdk")
        else()
            set(_fsr_sdk "${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk")
            if(NOT EXISTS "${_fsr_sdk}/LICENSE.txt")
                message(STATUS "FSR: fetching FidelityFX SDK ${XI_FSR_SDK_TAG} into ${_fsr_sdk}")
                file(REMOVE_RECURSE "${_fsr_sdk}")
                execute_process(COMMAND sh ${CMAKE_SOURCE_DIR}/tools/fetch_fsr_sdk.sh ${_fsr_sdk} ${XI_FSR_SDK_TAG}
                                RESULT_VARIABLE _fsr_rc OUTPUT_QUIET ERROR_VARIABLE _fsr_err)
                if(NOT _fsr_rc EQUAL 0)
                    message(WARNING "FSR: could not fetch the FidelityFX SDK (${_fsr_err}); building without FSR")
                    file(REMOVE_RECURSE "${_fsr_sdk}")
                endif()
            endif()
            if(EXISTS "${_fsr_sdk}/.git")
                execute_process(COMMAND git -C ${_fsr_sdk} rev-parse HEAD OUTPUT_VARIABLE _fsr_head OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(NOT _fsr_head STREQUAL XI_FSR_SDK_COMMIT)
                    message(FATAL_ERROR "FSR: ${_fsr_sdk} is at ${_fsr_head}, not ${XI_FSR_SDK_TAG} (${XI_FSR_SDK_COMMIT}); delete it to fetch again")
                endif()
            endif()
        endif()
    endif()
    find_program(XI_GLSLANG glslangValidator)
    find_package(Python3 COMPONENTS Interpreter)
    set(_fsr_root "${_fsr_sdk}/sdk")
    if(EXISTS "${_fsr_root}/include/FidelityFX/host/ffx_fsr3upscaler.h" AND XI_GLSLANG AND Python3_Interpreter_FOUND)
        set(XI_HAVE_FSR ON)
    elseif(EXISTS "${_fsr_root}")
        message(WARNING "FSR: needs glslangValidator and python3 to build the SDK's shaders; building without FSR")
    endif()
endif()

if(XI_HAVE_FSR)
    message(STATUS "FSR: on (FidelityFX SDK ${_fsr_sdk})")
    set(_fsr_passes autogen_reactive accumulate luma_pyramid prepare_reactivity prepare_inputs shading_change rcas
                    shading_change_pyramid luma_instability debug_view)
    set(_fsr_gen ${CMAKE_BINARY_DIR}/fsr-shaders)
    set(_fsr_headers ${_fsr_gen}/ffx_fsr3upscaler_permutation_info.h)
    set(_fsr_glsl)
    foreach(p IN LISTS _fsr_passes)
        list(APPEND _fsr_glsl ${_fsr_root}/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_${p}_pass.glsl)
        foreach(v "" _wave64 _16bit _wave64_16bit)
            list(APPEND _fsr_headers ${_fsr_gen}/ffx_fsr3upscaler_${p}_pass${v}_permutations.h)
        endforeach()
    endforeach()
    file(GLOB _fsr_gpu_headers ${_fsr_root}/include/FidelityFX/gpu/*.h ${_fsr_root}/include/FidelityFX/gpu/fsr3upscaler/*.h
                               ${_fsr_root}/include/FidelityFX/gpu/spd/*.h)
    add_custom_command(OUTPUT ${_fsr_headers}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fsr_shaders.py ${_fsr_root} ${_fsr_gen} --glslang ${XI_GLSLANG}
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/fsr_shaders.py ${_fsr_glsl} ${_fsr_gpu_headers}
        COMMENT "FSR: compiling the FSR 3.1 upscaler's shader permutations" VERBATIM)
    add_custom_target(xi_fsr_shaders DEPENDS ${_fsr_headers})

    # The SDK's own sources, unchanged: the FSR 3 upscaler, the Vulkan back end, the shared helpers.
    add_library(xi_ffx STATIC
        ${_fsr_root}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
        ${CMAKE_SOURCE_DIR}/src/gfx/fsr/ffx_vk_unaligned.cpp # includes the SDK's ffx_vk.cpp (see there)
        ${_fsr_root}/src/backends/shared/ffx_shader_blobs.cpp
        ${_fsr_root}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
        ${_fsr_root}/src/shared/ffx_assert.cpp
        ${_fsr_root}/src/shared/ffx_message.cpp
        ${_fsr_root}/src/shared/ffx_object_management.cpp
        ${_fsr_root}/src/shared/ffx_breadcrumbs_list.cpp
        ${CMAKE_SOURCE_DIR}/src/gfx/fsr/ffx_fg_stub.cpp) # frame generation's one symbol the back end references
    add_dependencies(xi_ffx xi_fsr_shaders)
    target_include_directories(xi_ffx SYSTEM PUBLIC ${_fsr_root}/include)
    target_include_directories(xi_ffx SYSTEM PRIVATE ${_fsr_root}/src/shared ${_fsr_root}/src/backends/shared
                               ${_fsr_root}/src/backends/shared/blob_accessors ${_fsr_root}/src/components
                               ${_fsr_root}/src/components/fsr3upscaler ${_fsr_gen})
    # FFX_GCC: the SDK's non-MSVC build; only the FSR 3 upscaler's shader blobs are compiled in.
    target_compile_definitions(xi_ffx PUBLIC FFX_GCC PRIVATE FFX_FSR3UPSCALER FFX_VK)
    target_compile_options(xi_ffx PRIVATE -w -include ${CMAKE_SOURCE_DIR}/src/gfx/fsr/ffx_linux_compat.h)
    target_include_directories(xi_ffx PRIVATE ${CMAKE_SOURCE_DIR}/src) # ffx_fg_stub.cpp's "gfx/fsr/..."
    set_property(SOURCE ${CMAKE_SOURCE_DIR}/src/gfx/fsr/ffx_vk_unaligned.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 XI_FFX_VK_CPP="${_fsr_root}/src/backends/vk/ffx_vk.cpp")
    set_property(SOURCE ${CMAKE_SOURCE_DIR}/src/gfx/fsr/ffx_vk_unaligned.cpp APPEND PROPERTY OBJECT_DEPENDS
                 ${_fsr_root}/src/backends/vk/ffx_vk.cpp)
    target_link_libraries(xi_ffx PUBLIC Vulkan::Vulkan)
    set_target_properties(xi_ffx PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_sources(xi_gfx PRIVATE src/gfx/fsr.cpp src/gfx/fsr/fsr_device.cpp)
    set_property(SOURCE src/gfx/fsr.cpp APPEND PROPERTY COMPILE_DEFINITIONS XI_HAVE_FSR=1)
    target_link_libraries(xi_gfx PRIVATE xi_ffx)
    # MIT: the SDK's notice goes with the client.
    install(FILES ${_fsr_sdk}/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ffxi-linux RENAME FidelityFX-SDK-LICENSE.txt)
else()
    target_sources(xi_gfx PRIVATE src/gfx/fsr.cpp src/gfx/fsr/fsr_device.cpp) # the stub: fsrBuilt() is false, the class reports why
endif()
