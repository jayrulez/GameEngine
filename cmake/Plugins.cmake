# Plugin packages.
#
# A package is a folder `<Name>/` holding `Plugin.xml` and one subfolder per part,
# `<Name>.<Kind>` (Engine, Pipeline, Tooling, Editor). The manifest is the one place the
# package's id, parts and dependencies are written; util_add_plugin reads it and declares
# every part, so a part's target name, its link to the part before it and its links to the
# parts of the plugins it depends on are never written in a CMakeLists by hand.
#
# CMake has no XML reader, so util_read_plugin_manifest reads the shape the engine's XML
# writer emits: one element per line. That is the only shape it accepts, and it is pinned to
# the C++ reader (foundation.runtime, ReadPluginManifest) by Runtime.Tests, which reads every
# manifest the configure saw with the C++ reader and compares the two.

# Every manifest this configure read, one line each (manifest|id|parts|depends, lists joined
# with commas), for Runtime.Tests' pin. Rewritten on each configure.
set(UTIL_PLUGIN_MANIFESTS_READ "${CMAKE_BINARY_DIR}/plugin-manifests-read.txt")
file(WRITE "${UTIL_PLUGIN_MANIFESTS_READ}" "")

# The part kinds in load order: a part links the one before it that the package has.
set(UTIL_PLUGIN_PART_KINDS Engine Pipeline Tooling Editor)

# The parts each host role loads. The C++ copy is RoleLoads (foundation.runtime);
# Runtime.Tests compares the two through the file written here.
set(UTIL_PLUGIN_ROLES player editor cook export mcp)
set(UTIL_PLUGIN_ROLE_player_KINDS Engine)
set(UTIL_PLUGIN_ROLE_editor_KINDS Engine Pipeline Tooling Editor)
set(UTIL_PLUGIN_ROLE_cook_KINDS Engine Pipeline)
set(UTIL_PLUGIN_ROLE_export_KINDS Engine Pipeline Tooling)
set(UTIL_PLUGIN_ROLE_mcp_KINDS Engine Pipeline Tooling)
set(UTIL_PLUGIN_ROLES_FILE "${CMAKE_BINARY_DIR}/plugin-roles.txt")
set(_util_roles "")
foreach(_role IN LISTS UTIL_PLUGIN_ROLES)
    list(JOIN UTIL_PLUGIN_ROLE_${_role}_KINDS "," _kinds)
    string(APPEND _util_roles "${_role}|${_kinds}\n")
endforeach()
file(WRITE "${UTIL_PLUGIN_ROLES_FILE}" "${_util_roles}")

# util_read_plugin_manifest(<file> <prefix>)
#
# Sets <prefix>_ID (the plugin id), <prefix>_PARTS (the part kinds, as written) and
# <prefix>_DEPENDS (the ids it depends on) in the caller's scope.
function(util_read_plugin_manifest file prefix)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "util_read_plugin_manifest: ${file} does not exist")
    endif()
    file(STRINGS "${file}" _lines)
    set(_stack "")
    set(_id "")
    set(_parts "")
    set(_depends "")
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line STREQUAL "<root>" OR _line STREQUAL "</root>")
            continue()
        elseif(_line MATCHES "^<array name=\"([^\"]+)\"[^>]*/>$")
            continue() # an empty array
        elseif(_line MATCHES "^<array name=\"([^\"]+)\"[^>]*>$")
            list(APPEND _stack "${CMAKE_MATCH_1}")
        elseif(_line STREQUAL "<object>")
            list(APPEND _stack "object")
        elseif(_line STREQUAL "</array>" OR _line STREQUAL "</object>")
            list(POP_BACK _stack)
        elseif(_line MATCHES "^<string name=\"([^\"]+)\">([^<]*)</string>$")
            set(_key "${CMAKE_MATCH_1}")
            set(_value "${CMAKE_MATCH_2}")
            list(JOIN _stack "/" _path)
            if(_path STREQUAL "" AND _key STREQUAL "id")
                set(_id "${_value}")
            elseif(_path STREQUAL "parts/object" AND _key STREQUAL "kind")
                list(APPEND _parts "${_value}")
            elseif(_path STREQUAL "dependsOn/object" AND _key STREQUAL "id")
                list(APPEND _depends "${_value}")
            endif()
        endif()
        # Every other line (scalars, empty strings, the data versions' numbers, deploy paths) is
        # not the build's business.
    endforeach()
    if(_id STREQUAL "")
        message(FATAL_ERROR "util_read_plugin_manifest: ${file} has no top-level id")
    endif()
    foreach(_kind IN LISTS _parts)
        if(NOT _kind IN_LIST UTIL_PLUGIN_PART_KINDS)
            message(FATAL_ERROR "util_read_plugin_manifest: ${file}: unknown part kind '${_kind}' (Engine, Pipeline, Tooling or Editor)")
        endif()
    endforeach()
    set(${prefix}_ID "${_id}" PARENT_SCOPE)
    set(${prefix}_PARTS "${_parts}" PARENT_SCOPE)
    set(${prefix}_DEPENDS "${_depends}" PARENT_SCOPE)
endfunction()

# util_record_plugin_manifest(<file> <id> <parts> <depends>)
#
# Adds a manifest to the list Runtime.Tests pins against the C++ reader.
function(util_record_plugin_manifest file id parts depends)
    list(JOIN parts "," _parts)
    list(JOIN depends "," _depends)
    file(APPEND "${UTIL_PLUGIN_MANIFESTS_READ}" "${file}|${id}|${_parts}|${_depends}\n")
endfunction()

# util_add_plugin(<package dir>)
#
# Reads <dir>/Plugin.xml and adds each part's folder in load order. A plugin it depends on must
# have been added first. Each part's CMakeLists starts with util_add_plugin_part().
function(util_add_plugin dir)
    get_filename_component(_dir "${dir}" ABSOLUTE)
    get_filename_component(_name "${_dir}" NAME)
    set(_manifest "${_dir}/Plugin.xml")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest}")
    util_read_plugin_manifest("${_manifest}" _plugin)
    util_record_plugin_manifest("${_manifest}" "${_plugin_ID}" "${_plugin_PARTS}" "${_plugin_DEPENDS}")
    get_property(_known GLOBAL PROPERTY UTIL_PLUGIN_${_plugin_ID}_NAME SET)
    if(_known)
        message(FATAL_ERROR "util_add_plugin: plugin id '${_plugin_ID}' (${_dir}) is already added")
    endif()
    foreach(_dependency IN LISTS _plugin_DEPENDS)
        get_property(_added GLOBAL PROPERTY UTIL_PLUGIN_${_dependency}_NAME SET)
        if(NOT _added)
            message(FATAL_ERROR "util_add_plugin: '${_plugin_ID}' depends on '${_dependency}', which is not added before it")
        endif()
    endforeach()
    set_property(GLOBAL PROPERTY UTIL_PLUGIN_${_plugin_ID}_NAME "${_name}")
    set_property(GLOBAL PROPERTY UTIL_PLUGIN_${_plugin_ID}_DEPENDS "${_plugin_DEPENDS}")
    set_property(GLOBAL PROPERTY UTIL_PLUGIN_${_plugin_ID}_MANIFEST "${_manifest}")

    # What util_add_plugin_part reads in each part's folder.
    set(PLUGIN_ID "${_plugin_ID}")
    set(PLUGIN_NAME "${_name}")
    set(PLUGIN_DEPENDS "${_plugin_DEPENDS}")
    set(PLUGIN_PREVIOUS_PART "")
    foreach(_kind IN LISTS UTIL_PLUGIN_PART_KINDS)
        if(_kind IN_LIST _plugin_PARTS)
            set(PLUGIN_PART_KIND "${_kind}")
            set(PLUGIN_PART "${_name}.${_kind}")
            if(NOT EXISTS "${_dir}/${PLUGIN_PART}/CMakeLists.txt")
                message(FATAL_ERROR "util_add_plugin: '${_plugin_ID}' lists part ${_kind}, but ${_dir}/${PLUGIN_PART}/CMakeLists.txt does not exist")
            endif()
            add_subdirectory("${_dir}/${PLUGIN_PART}")
            set_property(GLOBAL PROPERTY UTIL_PLUGIN_${_plugin_ID}_${_kind} "${PLUGIN_PART}")
            set(PLUGIN_PREVIOUS_PART "${PLUGIN_PART}")
        endif()
    endforeach()
endfunction()

# util_add_plugin_part()
#
# Declares the part whose folder util_add_plugin is adding: the target `${PLUGIN_PART}`
# (`<Name>.<Kind>`, alias `Plugins::<Name>.<Kind>`), linked PUBLIC to the package's previous
# part and to the same kind of part of each plugin it depends on, where that plugin has one.
# The part's CMakeLists then adds its sources and its engine links to `${PLUGIN_PART}`.
macro(util_add_plugin_part)
    if(NOT PLUGIN_PART)
        message(FATAL_ERROR "util_add_plugin_part: call it from a part folder that util_add_plugin adds")
    endif()
    util_add_engine_library(${PLUGIN_PART} ALIAS Plugins::${PLUGIN_PART})
    # The names PLUGIN_PART (Runtime/PluginEntry.h) builds the entry from; private, so a
    # consumer of the part never inherits them.
    target_compile_definitions(${PLUGIN_PART} PRIVATE
        PLUGIN_PART_ENTRY_NAME=PluginPart_${PLUGIN_ID}_${PLUGIN_PART_KIND}
        PLUGIN_PART_ID=u8\"${PLUGIN_ID}\"
        PLUGIN_PART_KIND=${PLUGIN_PART_KIND})
    if(PLUGIN_PREVIOUS_PART)
        target_link_libraries(${PLUGIN_PART} PUBLIC ${PLUGIN_PREVIOUS_PART})
    endif()
    foreach(_plugin_dependency IN LISTS PLUGIN_DEPENDS)
        get_property(_plugin_dependency_part GLOBAL PROPERTY UTIL_PLUGIN_${_plugin_dependency}_${PLUGIN_PART_KIND})
        if(_plugin_dependency_part)
            target_link_libraries(${PLUGIN_PART} PUBLIC ${_plugin_dependency_part})
        endif()
    endforeach()
endmacro()

# util_link_plugins(<target> ROLE <role> PLUGINS <id>...)
#
# Links into <target> the parts a host of <role> loads (UTIL_PLUGIN_ROLE_<role>_KINDS) of each
# named plugin and of every plugin they depend on, and generates the binary's registry: the
# LinkedPlugins function foundation.runtime declares, listing each plugin's manifest text and its
# linked parts' entry points. Referencing the entries is what pulls each part's archive into a
# static link, with no whole-archive or forced references. Linked is not enabled: the plugin host
# loads only what the project enables.
function(util_link_plugins target)
    cmake_parse_arguments(ULP "" "ROLE" "PLUGINS" ${ARGN})
    if(NOT ULP_ROLE OR NOT ULP_ROLE IN_LIST UTIL_PLUGIN_ROLES)
        message(FATAL_ERROR "util_link_plugins(${target}): ROLE must be one of ${UTIL_PLUGIN_ROLES}")
    endif()
    # The dependency closure, dependencies first.
    set(_ordered "")
    set(_pending ${ULP_PLUGINS})
    while(_pending)
        list(POP_FRONT _pending _id)
        if(_id IN_LIST _ordered)
            continue()
        endif()
        get_property(_known GLOBAL PROPERTY UTIL_PLUGIN_${_id}_NAME SET)
        if(NOT _known)
            message(FATAL_ERROR "util_link_plugins(${target}): plugin '${_id}' is not added (util_add_plugin)")
        endif()
        get_property(_depends GLOBAL PROPERTY UTIL_PLUGIN_${_id}_DEPENDS)
        set(_missing "")
        foreach(_dependency IN LISTS _depends)
            if(NOT _dependency IN_LIST _ordered)
                list(APPEND _missing ${_dependency})
            endif()
        endforeach()
        if(_missing)
            list(PREPEND _pending ${_missing} ${_id})
        else()
            list(APPEND _ordered ${_id})
        endif()
    endwhile()

    set(_declarations "")
    set(_tables "")
    set(_rows "")
    set(_count 0)
    foreach(_id IN LISTS _ordered)
        get_property(_manifest GLOBAL PROPERTY UTIL_PLUGIN_${_id}_MANIFEST)
        file(READ "${_manifest}" _xml)
        set(_entries "")
        set(_partCount 0)
        foreach(_kind IN LISTS UTIL_PLUGIN_ROLE_${ULP_ROLE}_KINDS)
            get_property(_part GLOBAL PROPERTY UTIL_PLUGIN_${_id}_${_kind})
            if(_part)
                target_link_libraries(${target} PRIVATE ${_part})
                string(APPEND _declarations "extern \"C\" const foundation::runtime::PluginPart* PluginPart_${_id}_${_kind}();\n")
                string(APPEND _entries "&PluginPart_${_id}_${_kind}, ")
                math(EXPR _partCount "${_partCount} + 1")
            endif()
        endforeach()
        if(_partCount EQUAL 0)
            set(_entries "nullptr")
        endif()
        string(APPEND _tables "    constexpr char8_t kManifest_${_id}[] = u8R\"PLUGINXML(${_xml})PLUGINXML\";\n")
        string(APPEND _tables "    const foundation::runtime::PluginPartEntryFn kParts_${_id}[] = {${_entries}};\n")
        string(APPEND _rows "        {kManifest_${_id}, kParts_${_id}, ${_partCount}},\n")
        math(EXPR _count "${_count} + 1")
    endforeach()
    if(_count EQUAL 0)
        set(_list "    const foundation::runtime::LinkedPlugin* const kLinked = nullptr;\n")
    else()
        set(_list "    const foundation::runtime::LinkedPlugin kLinked[] = {\n${_rows}    };\n")
    endif()
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/${target}_PluginRegistry.cpp")
    file(CONFIGURE OUTPUT "${_source}" CONTENT "// Generated by util_link_plugins (cmake/Plugins.cmake) for ${target}, role ${ULP_ROLE}. Do not edit.

#include \"Core/Prelude.h\"

import foundation.core;
import foundation.runtime;

${_declarations}
namespace
{
${_tables}${_list}}

extern \"C\" const foundation::runtime::LinkedPlugin* LinkedPlugins(foundation::core::usize* count)
{
    *count = ${_count};
    return kLinked;
}
" @ONLY)
    target_sources(${target} PRIVATE "${_source}")
endfunction()
