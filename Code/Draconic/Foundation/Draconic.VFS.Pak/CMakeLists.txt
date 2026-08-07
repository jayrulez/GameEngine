# Draconic::VFS.Pak - `draconic.vfs.pak` (packed read-only archive backend + builder).
# Separate from the core VFS so future compression codecs stay out of it.

add_library(Draconic.VFS.Pak STATIC)
add_library(Draconic::VFS.Pak ALIAS Draconic.VFS.Pak)

target_link_libraries(Draconic.VFS.Pak PUBLIC Draconic::Foundation Draconic::VFS Draconic::Policy)

target_sources(Draconic.VFS.Pak
    PUBLIC
        FILE_SET vfs_pak_modules TYPE CXX_MODULES FILES
            PakModule.cppm)

