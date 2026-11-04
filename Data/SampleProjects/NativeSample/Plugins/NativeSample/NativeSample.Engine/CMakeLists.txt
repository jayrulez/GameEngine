# NativeSample.Engine - the reference game's runtime code: its plugin package's Engine part, which
# every host loads. util_add_plugin_part declares it from Plugin.xml; it builds like any engine
# library, a shared one for the editor (Project > Build Project Plugins) and a static one the
# exported player links.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::Scene Foundation::Runtime Foundation::Policy
    PRIVATE Engine::Composition) # EnginePluginServices (the scene manager it adds)
target_sources(${PLUGIN_PART}
    PRIVATE
        NativeSampleEngine.cpp # the part's entry
    PUBLIC
        FILE_SET nativesample_engine_modules TYPE CXX_MODULES FILES
            NativeSample.cppm)
