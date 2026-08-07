#include <doctest/doctest.h>

#include <cstring>

#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;

using namespace draconic::foundation;

// Web has no dynamic linking (dlopen'd sidecars are a DESKTOP concept - web ships
// everything linked in), so the plugin-loading cases sit out; the missing-file
// error path still runs everywhere.
#if !DRACONIC_PLATFORM_WEB

// The plugin path is injected as a narrow build-system literal; the library API
// is now UTF-8, so wrap in a StringView directly.
static StringView PluginPath()
{
    return StringView{reinterpret_cast<const utf8char*>(DRACONIC_TEST_PLUGIN_PATH)};
}

// --- Library ---------------------------------------------------------------

TEST_CASE("library: load a real plugin, resolve and call symbols, unload")
{
    DynamicLibrary lib;
    CHECK_FALSE(lib.IsLoaded());

    REQUIRE(lib.Load(PluginPath()).IsOk());
    CHECK(lib.IsLoaded());

    using AddFn = int (*)(int, int);
    AddFn add = lib.GetSymbol<AddFn>(u8"DraconicTestAdd");
    REQUIRE(add != nullptr);
    CHECK(add(2, 3) == 5);

    using AnswerFn = int (*)();
    AnswerFn answer = lib.GetSymbol<AnswerFn>(u8"DraconicTestAnswer");
    REQUIRE(answer != nullptr);
    CHECK(answer() == 42);

    CHECK(lib.GetSymbol<AddFn>(u8"NoSuchSymbol") == nullptr);

    lib.Unload();
    CHECK_FALSE(lib.IsLoaded());
}

#endif // !DRACONIC_PLATFORM_WEB

TEST_CASE("library: loading a missing file fails cleanly")
{
    DynamicLibrary lib;
    Status status = lib.Load(u8"draconic_definitely_not_a_library.so");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::NotFound);
    CHECK_FALSE(lib.IsLoaded());
}

#if !DRACONIC_PLATFORM_WEB // loads the real plugin
TEST_CASE("library: move transfers ownership")
{
    DynamicLibrary a;
    REQUIRE(a.Load(PluginPath()).IsOk());

    DynamicLibrary b = Move(a);
    CHECK_FALSE(a.IsLoaded());
    CHECK(b.IsLoaded());

    using AnswerFn = int (*)();
    AnswerFn answer = b.GetSymbol<AnswerFn>(u8"DraconicTestAnswer");
    REQUIRE(answer != nullptr);
    CHECK(answer() == 42);
}
#endif // !DRACONIC_PLATFORM_WEB
