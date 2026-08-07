# Draconic::Content - `draconic.content` (content database: Groups + Instances of
# serializable objects, addressed by Guid/path, stored over a VFS mount).

add_library(Draconic.Content STATIC)
add_library(Draconic::Content ALIAS Draconic.Content)

target_link_libraries(Draconic.Content PUBLIC Draconic::Foundation Draconic::VFS Draconic::Policy)

target_sources(Draconic.Content
    PRIVATE
        ContentModuleImpl.cpp   # out-of-line defs (see ContentModule.cppm)
    PUBLIC
        FILE_SET content_modules TYPE CXX_MODULES FILES
            ContentModule.cppm)

