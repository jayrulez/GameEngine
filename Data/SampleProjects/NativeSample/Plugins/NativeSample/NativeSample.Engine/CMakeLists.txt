# NativeSample.Engine - the reference game's native code: its plugin package's Engine part, which
# every host loads. util_add_plugin_part declares it from Plugin.xml; it builds like any engine
# library, a shared one for the editor (Project > Build Project Plugins) and a static one the
# exported player links.
util_add_plugin_part()
target_sources(${PLUGIN_PART} PRIVATE NativeSampleEngine.cpp)
target_link_libraries(${PLUGIN_PART} PRIVATE Foundation::Runtime Foundation::Core Foundation::Policy)
