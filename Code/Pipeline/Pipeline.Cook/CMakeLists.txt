# Pipeline::Cook - `pipeline.cook` (the incremental cook driver and pipeline DB).
# Tooling only; headless (no shell/GPU) so the CLI cooker can link it.

util_add_engine_library(Pipeline.Cook ALIAS Pipeline::Cook)
target_link_libraries(Pipeline.Cook PUBLIC
    Foundation::Core Foundation::Content Foundation::VFS Pipeline::Core Foundation::Policy)

target_sources(Pipeline.Cook
    PRIVATE
        CookDriverImpl.cpp
    PUBLIC
        FILE_SET editorcook_modules TYPE CXX_MODULES FILES
            CookDriver.cppm)

