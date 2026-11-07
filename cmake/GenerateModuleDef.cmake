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
# A library exports only the functions attached to the modules it DECLARES, plus functions
# with no module attachment (classic translation units). Template instantiations and inline
# functions belonging to other modules land in this library's objects too - Editor.App carried
# 8559 symbols attached to foundation.core, things like Array<byte>'s constructors and
# Move<ErrorCode> - and exporting them is pointless, since any consumer that needs such a
# function instantiates its own copy.
#
# It is also actively harmful at scale. Exporting everything gave Editor.App 17984 exports,
# whose import library link.exe then could not read back:
#
#     Editor.App.lib : fatal error LNK1127: library is corrupt
#
# That archive had a 8.66 MB symbol table (past the 8 MB that every other library stayed under)
# plus a 3.88 MB /CXXMODWEAKSYM/ member, the C++ modules weak-symbol table link.exe adds.
# Neither link.exe nor lib.exe would read it; lib.exe building the same export list on its own
# produced a readable archive, so the pair is what breaks. Own-module-only drops 56% of the
# exports, which puts the library well inside what the rest of the tree already proves works.
#
# Only cl decorates module-attached symbols this way. clang in GNU mode at the MSVC ABI emits
# no `::<!module>` suffix at all, so under the clang presets every symbol reads as unattached
# and this filter keeps everything, as it did before.
#
# Unattached symbols are kept wholesale, which is conservative. Two groups could go later, once
# there is a measurement to justify it: the deleting destructors (`??_G`, `??_E`), which lib
# already warns about ("export of deleting destructor; image may not run correctly", LNK4102),
# and the STL/CRT COMDATs, which a consumer re-instantiates exactly as it does core's templates.
#
# Invoked at PRE_LINK with:
#   DUMPBIN      - path to dumpbin.exe
#   OBJECTS_FILE - file listing the object paths, one per line (not a -D list: a ;-list
#                  argument needs COMMAND_EXPAND_LISTS, which splits it into separate argv
#                  entries and loses every object after the first)
#   SOURCE_DIR   - the target's source dir, scanned for the modules this library declares
#   DEF_FILE     - output path

# Run as a script (cmake -P), which starts with every policy unset: the project's minimum, so this
# reads as the build does (IN_LIST below is an operator only under CMP0057).
cmake_minimum_required(VERSION 3.28)

if(NOT DUMPBIN OR NOT DEF_FILE OR NOT OBJECTS_FILE)
    message(FATAL_ERROR "GenerateModuleDef: DUMPBIN, OBJECTS_FILE and DEF_FILE are required")
endif()

file(STRINGS "${OBJECTS_FILE}" OBJECTS)

# The modules this library declares, by primary name: `export module foundation.gui:button;`
# and `module foundation.gui;` both own `foundation.gui`. A symbol attached to anything else
# came from a template instantiated here and belongs to its own module's library.
#
# Read from the sources under SOURCE_DIR rather than the target's SOURCES property, because the
# module interfaces live in a `FILE_SET CXX_MODULES` and file-set files are not in SOURCES -
# $<TARGET_PROPERTY:SOURCES> came back empty for every module library, which left the filter
# exporting everything for some and dropping needed symbols for others. One library per
# directory tree is this repo's layout, so a recursive scan matches the target. If a directory
# ever holds two targets the set is a superset, and the filter errs toward exporting too much,
# which costs size and never breaks a link.
set(_own_modules "")
file(GLOB_RECURSE _module_sources "${SOURCE_DIR}/*.cppm" "${SOURCE_DIR}/*.cpp")
foreach(_src IN LISTS _module_sources)
    file(STRINGS "${_src}" _decls REGEX "^[ \t]*(export[ \t]+)?module[ \t]+[A-Za-z_]")
    foreach(_decl IN LISTS _decls)
        if(_decl MATCHES "module[ \t]+([A-Za-z0-9_.]+)")
            list(APPEND _own_modules "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()
if(_own_modules)
    list(REMOVE_DUPLICATES _own_modules)
endif()

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
        # Module-attached names carry a `::<!module.name>` suffix. Keep the symbol when it has
        # none (a classic TU) or when the module's root is one this library declares; a
        # partition suffix (`foundation.gui:button`) belongs to its primary module's root.
        if(_name MATCHES "::<!([A-Za-z0-9_.:]+)>$")
            set(_mod "${CMAKE_MATCH_1}")
            string(REGEX REPLACE ":.*$" "" _mod_root "${_mod}")
            if(NOT _mod_root IN_LIST _own_modules)
                continue()
            endif()
        endif()
        list(APPEND _symbols "${_name}")
    endforeach()
endforeach()

list(REMOVE_DUPLICATES _symbols)
list(SORT _symbols)

set(_body "EXPORTS\n")
set(_name_bytes 0)
foreach(_sym IN LISTS _symbols)
    string(APPEND _body "    ${_sym}\n")
    string(LENGTH "${_sym}" _len)
    math(EXPR _name_bytes "${_name_bytes} + ${_len} + 1")
endforeach()

# Fail while the message can still be read. The import library link.exe writes for a module
# library carries a /CXXMODWEAKSYM/ member (its C++ modules weak-symbol table) alongside the
# usual symbol table, and past roughly an 8.6 MB symbol table that archive stops being
# readable - by link.exe itself as much as by lib.exe:
#
#     Editor.App.lib : fatal error LNK1127: library is corrupt
#
# Size alone is not the trigger: lib.exe /def building the same 8.66 MB export list produced an
# archive that read back fine, so it is the pairing with /CXXMODWEAKSYM/ that breaks. Either
# way the symptom names the consuming library, never the oversized one, which makes it slow to
# diagnose. So complain here instead.
#
# The symbol table runs about twice the export names (each export appears plain and __imp_
# prefixed), so the budget is expressed in names and doubled. 3 MB of names is ~6 MB of table:
# above the 4.27 MB that the largest healthy library in the tree carries, well under the
# 8.66 MB that failed.
set(_name_budget 3145728) # 3 MB of export names, ~6 MB of symbol table
if(_name_bytes GREATER _name_budget)
    math(EXPR _name_mb "${_name_bytes} / 1048576")
    message(FATAL_ERROR
        "GenerateModuleDef: ${DEF_FILE} exports ${_name_mb} MB of symbol names, over the "
        "${_name_budget}-byte budget. The import library link.exe writes past roughly this "
        "size cannot be read back (LNK1127 'library is corrupt', reported against whichever "
        "library links this one). Export less from this target: split it, or narrow what its "
        "modules make public.")
endif()

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
list(LENGTH _own_modules _own_count)
if(_own_count EQUAL 0)
    # No module declarations found: everything is exported, as before. Worth saying, because a
    # library that does declare modules and reports 0 here has a scan that missed them.
    message(STATUS "GenerateModuleDef: ${_count} function symbols (no module declarations "
                   "found; exporting all) -> ${DEF_FILE}")
else()
    message(STATUS "GenerateModuleDef: ${_count} function symbols from ${_own_count} own "
                   "module(s) -> ${DEF_FILE}")
endif()
