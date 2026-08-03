# Draconic::VFS - `draconic.vfs` (virtual filesystem: byte access by logical path,
# decoupled from the backing store). Depends only on Core (stream primitives).

add_library(Draconic.VFS STATIC)
add_library(Draconic::VFS ALIAS Draconic.VFS)

target_link_libraries(Draconic.VFS PUBLIC Draconic::Core Draconic::Policy)

target_sources(Draconic.VFS
    PRIVATE
        VFSReflectionImpl.cpp
    PUBLIC
        FILE_SET vfs_modules TYPE CXX_MODULES FILES
            VfsModule.cppm
            IFileSystem.cppm
            SourcePath.cppm
            NativeFileSystem.cppm
            VirtualFileSystem.cppm)

