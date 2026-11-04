# NativeSample.Pipeline - the reference game's asset type, its cook and its creator: its plugin
# package's Pipeline part, which the editor and the CLI tools load.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::Content Pipeline::Core Pipeline::Importer Foundation::Runtime Foundation::Policy)
target_sources(${PLUGIN_PART}
    PRIVATE
        NativeSamplePipeline.cpp # the part's entry
        NoteTypes.cpp            # the types' definitions
    PUBLIC
        FILE_SET nativesample_pipeline_modules TYPE CXX_MODULES FILES
            Notes.cppm)
