# NativeSample - the reference native game module.
#
# A game's native code is an engine-style module: util_add_engine_library builds it as
# the dev shared library in shared-engine builds (the editor/player load it via the
# manifest's nativeModule) and as a static library in shipping builds (the exporter links it
# into Engine.GamePlayer), from one source.
util_add_engine_library(NativeSample ALIAS Game::NativeSample)
target_sources(NativeSample PRIVATE NativeSamplePlugin.cpp)
target_link_libraries(NativeSample PRIVATE Foundation::Runtime Foundation::Core Foundation::Policy)
# The dev module lands here, where the manifest's nativeModule points, so the
# editor and player load what was just built. Shipping (static) archives are unaffected.
# Both properties are required: RUNTIME_OUTPUT_DIRECTORY governs the .dll on Windows,
# LIBRARY_OUTPUT_DIRECTORY the .so elsewhere.
set_target_properties(NativeSample PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
