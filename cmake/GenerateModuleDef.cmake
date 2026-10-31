# Generate a .def exporting every External function symbol of a target's objects,
# including C++20 module-attached ones.
#
# Why this exists: CMake's WINDOWS_EXPORT_ALL_SYMBOLS silently drops module-attached
# symbols. MSVC decorates them with a module suffix:
#
#     ?IsLoaded@DynamicLibrary@core@foundation@@QEBA_NXZ::<!foundation.core>
#
# and CMake's def generator does not emit those names, so a shared Core would export CRT/STL
# symbols and none of its own module functions. link.exe itself accepts the decorated names
# in a .def, so this is a CMake gap, not a toolchain limit.
#
# Functions only. Exported data needs __declspec(dllimport) at the point of use to be read
# correctly through an import table, which the "one BMI for producer and consumer" model
# cannot express. That loses nothing: every piece of process-wide state sits behind a
# non-inline accessor function, and the sharedlib tripwire keeps it that way.
#
# Invoked at PRE_LINK with:
#   DUMPBIN      - path to dumpbin.exe
#   OBJECTS_FILE - file listing the object paths, one per line (not a -D list: a ;-list
#                  argument needs COMMAND_EXPAND_LISTS, which splits it into separate argv
#                  entries and loses every object after the first)
#   DEF_FILE     - output path

if(NOT DUMPBIN OR NOT DEF_FILE OR NOT OBJECTS_FILE)
    message(FATAL_ERROR "GenerateModuleDef: DUMPBIN, OBJECTS_FILE and DEF_FILE are required")
endif()

file(STRINGS "${OBJECTS_FILE}" OBJECTS)

set(_symbols "")
foreach(_obj IN LISTS OBJECTS)
    if(NOT EXISTS "${_obj}")
        continue()
    endif()
    execute_process(COMMAND "${DUMPBIN}" /symbols "${_obj}"
                    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "GenerateModuleDef: dumpbin failed on ${_obj}: ${_err}")
    endif()
    string(REPLACE "\n" ";" _lines "${_out}")
    foreach(_line IN LISTS _lines)
        # A defined, externally visible function looks like:
        #   036 00000230 SECT4  notype ()    External     | <name> (demangled)
        # Undefined references say UNDEF and are skipped by the SECT requirement.
        if(NOT _line MATCHES "notype \\(\\)")
            continue()
        endif()
        if(NOT _line MATCHES "External")
            continue()
        endif()
        if(_line MATCHES "UNDEF")
            continue()
        endif()
        if(NOT _line MATCHES "\\|[ \t]*([^ \t]+)")
            continue()
        endif()
        set(_name "${CMAKE_MATCH_1}")
        # Compiler/CRT scaffolding that must not be re-exported.
        if(_name MATCHES "^__?(real|xmm|imp_)" OR _name MATCHES "^\\$" OR _name STREQUAL "")
            continue()
        endif()
        list(APPEND _symbols "${_name}")
    endforeach()
endforeach()

list(REMOVE_DUPLICATES _symbols)
list(SORT _symbols)

set(_body "EXPORTS\n")
foreach(_sym IN LISTS _symbols)
    string(APPEND _body "    ${_sym}\n")
endforeach()

# Only rewrite when the content changes, so an unchanged export surface does not force a
# relink of every consumer.
set(_old "")
if(EXISTS "${DEF_FILE}")
    file(READ "${DEF_FILE}" _old)
endif()
if(NOT _old STREQUAL _body)
    file(WRITE "${DEF_FILE}" "${_body}")
endif()

list(LENGTH _symbols _count)
message(STATUS "GenerateModuleDef: ${_count} function symbols -> ${DEF_FILE}")
