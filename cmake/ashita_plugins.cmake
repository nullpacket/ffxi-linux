# SPDX-License-Identifier: GPL-3.0-or-later
# Ashita plugins built natively for Linux (issue #34, docs/ashita-compat.md "Plugins").
#
# Needs, at configure time, Ashita's plugins/sdk headers (third-party, never committed: from the user's Ashita install,
# found in the game folder's plugins/sdk unless XI_ASHITA_SDK names one) and Wine's Win32 headers (their types and
# macros only). With both, the client gets its plugin host (src/ashita/plugin_host.cpp) and every plugin source folder
# in XI_ASHITA_PLUGIN_SOURCES is built into <build>/plugins/<name>.so; without them the client says why on /load.
set(XI_ASHITA_SDK "" CACHE PATH "Ashita plugins/sdk folder (default: <game folder>/plugins/sdk)")
set(XI_ASHITA_PLUGIN_SOURCES "" CACHE STRING "Ashita plugin source folders to build natively (;-separated)")
set(XI_WINE_INCLUDE "/usr/include/wine/windows" CACHE PATH "Wine's Win32 headers")

set(_xi_sdk "${XI_ASHITA_SDK}")
if(NOT _xi_sdk)
    if(DEFINED ENV{XDG_DATA_HOME} AND NOT "$ENV{XDG_DATA_HOME}" STREQUAL "")
        set(_xi_sdk "$ENV{XDG_DATA_HOME}/ffxi-linux/game/plugins/sdk")
    else()
        set(_xi_sdk "$ENV{HOME}/.local/share/ffxi-linux/game/plugins/sdk")
    endif()
endif()

set(XI_HAVE_ASHITA_PLUGINS OFF)
if(EXISTS "${_xi_sdk}/Ashita.h" AND EXISTS "${XI_WINE_INCLUDE}/windows.h")
    set(XI_HAVE_ASHITA_PLUGINS ON)
endif()

set(XI_ASHITA_PLUGIN_DIR ${CMAKE_BINARY_DIR}/plugins)

if(XI_HAVE_ASHITA_PLUGINS)
    message(STATUS "Ashita plugins: SDK ${_xi_sdk}")
    set(XI_ASHITA_SDK_PORT ${CMAKE_BINARY_DIR}/ashita-sdk)
    execute_process(COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/ashita_sdk_port.py sdk ${_xi_sdk} ${XI_ASHITA_SDK_PORT}
        RESULT_VARIABLE _xi_rc)
    if(NOT _xi_rc EQUAL 0)
        message(FATAL_ERROR "tools/ashita_sdk_port.py could not prepare the Ashita SDK from ${_xi_sdk}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/ashita_sdk_port.py)
    # The include folders every translation unit that sees the SDK needs: our Windows-name forwarders, Wine's headers,
    # the prepared SDK (system headers: their warnings are not ours).
    set(XI_ASHITA_SDK_INCLUDES ${CMAKE_SOURCE_DIR}/src/ashita/plugins/include)
    set(XI_ASHITA_SDK_SYSTEM_INCLUDES ${XI_WINE_INCLUDE} ${XI_ASHITA_SDK_PORT})

    target_sources(xi_ashita PRIVATE src/ashita/plugin_host.cpp src/ashita/plugins/win32.cpp)
    # Only these two see the SDK (its imgui.h must not shadow the client's own for the rest of xi_ashita).
    set(_xi_sys)
    foreach(_d IN LISTS XI_ASHITA_SDK_SYSTEM_INCLUDES)
        list(APPEND _xi_sys -isystem ${_d})
    endforeach()
    set_source_files_properties(src/ashita/plugin_host.cpp src/ashita/plugins/win32.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${XI_ASHITA_SDK_INCLUDES}" COMPILE_OPTIONS "${_xi_sys}")
    target_link_libraries(xi_ashita PUBLIC dl)

    # A plugin as a MODULE (<dir>/<file>.so) with compat.h force-included, against the prepared SDK.
    function(xi_ashita_plugin_module target file dir)
        add_library(${target} MODULE ${ARGN} ${CMAKE_SOURCE_DIR}/src/ashita/plugins/alloc.cpp)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME ${file} PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${dir}
            CXX_VISIBILITY_PRESET hidden)
        target_compile_options(${target} PRIVATE -include ${CMAKE_SOURCE_DIR}/src/ashita/plugins/compat.h)
        # A plugin's calls to its own functions (and to alloc.cpp's operator new) stay inside it.
        target_link_options(${target} PRIVATE "LINKER:-Bsymbolic")
        target_include_directories(${target} PRIVATE ${XI_ASHITA_SDK_INCLUDES})
        target_include_directories(${target} SYSTEM PRIVATE ${XI_ASHITA_SDK_SYSTEM_INCLUDES})
    endfunction()

    # Natively built plugins: each source folder is copied into the build (Windows paths fixed), then compiled.
    foreach(_src IN LISTS XI_ASHITA_PLUGIN_SOURCES)
        get_filename_component(_name ${_src} NAME)
        string(TOLOWER ${_name} _lname)
        set(_port ${CMAKE_BINARY_DIR}/ashita-plugin-src/${_name}) # its own copy of any sibling common/ folder
        execute_process(COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/ashita_sdk_port.py plugin ${_src} ${_port} RESULT_VARIABLE _xi_rc)
        if(NOT _xi_rc EQUAL 0)
            message(FATAL_ERROR "tools/ashita_sdk_port.py could not prepare the plugin in ${_src}")
        endif()
        file(GLOB _cpp ${_port}/${_name}/*.cpp)
        xi_ashita_plugin_module(xi_plugin_${_lname} ${_lname} ${XI_ASHITA_PLUGIN_DIR} ${_cpp})
        # Their own warnings are not ours.
        target_compile_options(xi_plugin_${_lname} PRIVATE -w)
        # MSVC finds "thirdparty/x.h" from a header in common/ through the including file's folder; GCC needs the
        # plugin folder on the quote path for that.
        target_compile_options(xi_plugin_${_lname} PRIVATE -iquote ${_port}/${_name})
        target_include_directories(xi_plugin_${_lname} PRIVATE ${_port}/${_name})
        add_dependencies(xi-client xi_plugin_${_lname})
        message(STATUS "Ashita plugins: building ${_name} -> plugins/${_lname}.so")
    endforeach()
else()
    message(STATUS "Ashita plugins: off (no Ashita SDK at ${_xi_sdk} or no Wine headers at ${XI_WINE_INCLUDE})")
    target_sources(xi_ashita PRIVATE src/ashita/plugin_host_none.cpp)
endif()

# The Win32 stand-ins plugins call are exported from the executable that hosts them.
function(xi_ashita_plugin_exports target)
    if(XI_HAVE_ASHITA_PLUGINS)
        target_link_options(${target} PRIVATE "LINKER:--dynamic-list=${CMAKE_SOURCE_DIR}/src/ashita/plugins/exports.list")
    endif()
endfunction()
