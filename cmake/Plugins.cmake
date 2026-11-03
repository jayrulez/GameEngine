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
