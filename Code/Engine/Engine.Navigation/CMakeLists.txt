# Engine::Navigation - `engine.navigation`: scene integration
# (zone and agent components, a per-scene Detour crowd, steering writeback).
util_add_engine_library(Engine.Navigation ALIAS Engine::Navigation)
target_link_libraries(Engine.Navigation
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene Foundation::Resource
           Foundation::Navigation Foundation::Navigation.Resource
           Foundation::Render Engine::Render
           Foundation::Script.Facades Foundation::Profiler Engine::Domain Foundation::Policy)

target_sources(Engine.Navigation
    PRIVATE
        NavigationSubsystemImpl.cpp   # REFLECT_VALUE bodies (kept out of interface units)
    PUBLIC
        FILE_SET navigation_engine_modules TYPE CXX_MODULES FILES
            NavigationModule.cppm
            NavigationComponents.cppm
            NavigationSubsystem.cppm)
