# Foundation::Navigation - `foundation.navigation`: the Recast/Detour-backed navmesh core.
# Recast/Detour are private: rc*/dt* types never cross the module surface, which keeps the API
# clean and spares GCC's module serializer the third-party headers in an interface unit.
# The bake is a pure function (triangle soup in, serialized navmesh out) with no scene,
# geometry, or device dependency; it runs headless. Scene integration (components, zones,
# per-scene crowds, debug draw) lives in engine.navigation.
util_add_engine_library(Navigation ALIAS Foundation::Navigation)
target_link_libraries(Navigation
    PUBLIC Foundation::Core Foundation::Policy
    PRIVATE ThirdParty::Recast Foundation::Profiler)

target_sources(Navigation
    PRIVATE
        NavigationImpl.cpp   # module implementation unit: all Recast/Detour contact
    PUBLIC
        FILE_SET navigation_modules TYPE CXX_MODULES FILES
            NavigationModule.cppm
            NavigationBuilder.cppm
            NavigationMesh.cppm)
