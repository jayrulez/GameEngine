# Foundation::Content - `foundation.content` (content database: Groups and Instances of
# serializable objects, addressed by Guid/path, stored over a VFS mount).

util_add_engine_library(Content ALIAS Foundation::Content)
target_link_libraries(Content PUBLIC Foundation::Core Foundation::VFS Foundation::Policy)

target_sources(Content
    PRIVATE
        ContentModuleImpl.cpp   # out-of-line defs (see ContentModule.cppm)
    PUBLIC
        FILE_SET content_modules TYPE CXX_MODULES FILES
            ContentModule.cppm)

