// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Luau C API includes for the foundation.script.luau backend. Kept in a header so the
// module unit's global module fragment stays one line (heavy third-party headers stay out
// of module units).
// LUA_USE_LONGJMP=1 arrives PUBLIC from the ThirdParty::Luau target: the engine builds
// -fno-exceptions, so Luau errors must unwind by longjmp, never a C++ throw.
#pragma once

#include <lua.h>
#include <lualib.h>
#include <luacode.h>

#include <Luau/Parser.h> // IScriptManager::Check: syntax errors with their columns
#include <Luau/Bytecode.h> // LBC_VERSION_TARGET, the vendored bytecode version (cook fingerprint)

#include <cstdlib> // free(): luau_compile returns a malloc'd buffer
#include <cstring> // strlen for chunk names
