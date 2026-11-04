# NativeSample.Editor - the reference game's addition to the editor: its plugin package's Editor
# part, which only the editor loads.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART} PRIVATE Editor::Core Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART} PRIVATE NativeSampleEditor.cpp)
