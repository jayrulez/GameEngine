# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

# THIRD-PARTY-NOTICES.txt: the licence of every third-party component a shipped binary can carry,
# assembled at configure time from the licence files kept beside each component in ThirdParty/.
# Most of these licences (MIT, BSD, Apache, the LLVM release licence) require their notice to go
# with every copy, a binary one included. util_copy_runtime_deps stages the file beside each
# executable that passes it as EXTRA, which also lists it in "<target>.runtime-libs"; an export
# template reads that list for its sidecars, so every exported game, editor dist and template
# carries it with no step of its own.
#
# Left out: what never ships in a binary. doctest (tests only), Naga and Tint (cook-time tools the
# web cook runs from ThirdParty/), and GPUI (adapted shader code, credited in its own source:
# Data/Engine/Shaders/vg_box.ps.hlsl, with its licence in ThirdParty/gpui/).
#
# A component added to ThirdParty/ that ships in a binary joins the list below, with its licence
# file kept beside it.

set(_tpn_root "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty")
cmake_path(NORMAL_PATH _tpn_root)

# name | where it comes from | the licence it is used under | its licence file(s), relative to
# ThirdParty/, joined by + (a ; would split the entry)
set(_tpn_components
    "SDL|https://libsdl.org|Zlib|SDL3/LICENSE.txt"
    "Jolt Physics|https://github.com/jrouwe/JoltPhysics|MIT|JoltPhysics/LICENSE"
    "AngelScript|https://www.angelcode.com/angelscript|Zlib|angelscript/LICENSE.md"
    "Luau|https://github.com/luau-lang/luau|MIT|luau/LICENSE.txt"
    "HarfBuzz|https://github.com/harfbuzz/harfbuzz|Old MIT (HarfBuzz's own name for it)|harfbuzz/COPYING"
    "SheenBidi|https://github.com/Tehreer/SheenBidi|Apache-2.0|sheenbidi/LICENSE"
    "meshoptimizer|https://github.com/zeux/meshoptimizer|MIT|meshoptimizer/LICENSE.md"
    "Recast & Detour|https://github.com/recastnavigation/recastnavigation|Zlib|recastnavigation/License.txt"
    "Mbed TLS|https://github.com/Mbed-TLS/mbedtls|Apache-2.0 (chosen of Apache-2.0 OR GPL-2.0-or-later)|mbedtls/LICENSE"
    "Dear ImGui|https://github.com/ocornut/imgui|MIT|imgui/LICENSE.txt"
    "astc-encoder|https://github.com/ARM-software/astc-encoder|Apache-2.0|astcenc/LICENSE.txt"
    "msdfgen|https://github.com/Chlumsky/msdfgen|MIT|msdfgen/LICENSE.txt"
    "bc7enc_rdo|https://github.com/richgel999/bc7enc_rdo|MIT (chosen of MIT OR Unlicense)|bc7enc/LICENSE"
    "bcdec|https://github.com/iOrange/bcdec|MIT (chosen of MIT OR Unlicense)|bcdec/LICENSE"
    "cgltf (with jsmn)|https://github.com/jkuhlmann/cgltf|MIT|cgltf/LICENSE"
    "ufbx|https://github.com/ufbx/ufbx|MIT (chosen of MIT OR Unlicense)|ufbx/LICENSE"
    "stb (stb_image, stb_image_write, stb_truetype, stb_vorbis)|https://github.com/nothings/stb|MIT (chosen of MIT OR Unlicense)|stb/LICENSE"
    "miniaudio|https://github.com/mackron/miniaudio|MIT-0 (chosen of MIT-0 OR Unlicense)|miniaudio/LICENSE"
    "wgpu-native|https://github.com/gfx-rs/wgpu-native|MIT (chosen of MIT OR Apache-2.0)|WgpuNative/LICENSE.MIT"
    "DirectX Shader Compiler (editor and cook tools only)|https://github.com/microsoft/DirectXShaderCompiler|NCSA, with the notices of what it includes|DXC/LICENSE.TXT+DXC/ThirdPartyNotices.txt"
)

set(_tpn_text "Third-party notices\n\nThis software includes the third-party components below. Each is listed with where it comes\nfrom and the licence it is used under, followed by that licence's text.\n")
foreach(_tpn_entry IN LISTS _tpn_components)
    string(REPLACE "|" ";" _tpn_fields "${_tpn_entry}")
    string(REPLACE "+" ";" _tpn_fields "${_tpn_fields}")
    list(POP_FRONT _tpn_fields _tpn_name _tpn_url _tpn_spdx)
    string(APPEND _tpn_text "\n================================================================================\n"
                            "${_tpn_name}\n${_tpn_url}\nLicence: ${_tpn_spdx}\n"
                            "================================================================================\n\n")
    foreach(_tpn_rel IN LISTS _tpn_fields)
        set(_tpn_file "${_tpn_root}/${_tpn_rel}")
        if(NOT EXISTS "${_tpn_file}")
            message(FATAL_ERROR "ThirdPartyNotices: '${_tpn_name}' names ${_tpn_rel}, which is missing")
        endif()
        file(READ "${_tpn_file}" _tpn_body)
        string(APPEND _tpn_text "${_tpn_body}\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_tpn_file}")
    endforeach()
endforeach()

# file(WRITE) only when the text changed, so a reconfigure leaves the staged copies alone.
set(BUILDSYSTEM_NOTICES_FILE "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt")
set(_tpn_old "")
if(EXISTS "${BUILDSYSTEM_NOTICES_FILE}")
    file(READ "${BUILDSYSTEM_NOTICES_FILE}" _tpn_old)
endif()
if(NOT _tpn_old STREQUAL _tpn_text)
    file(WRITE "${BUILDSYSTEM_NOTICES_FILE}" "${_tpn_text}")
endif()
