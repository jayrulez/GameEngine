# Foundation::VFS.Pak - `foundation.vfs.pak` (packed read-only archive backend and builder).
# Separate from the core VFS so compression codecs stay out of it.

util_add_engine_library(VFS.Pak ALIAS Foundation::VFS.Pak)
target_link_libraries(VFS.Pak PUBLIC Foundation::Core Foundation::VFS Foundation::Policy)

target_sources(VFS.Pak
    PUBLIC
        FILE_SET vfs_pak_modules TYPE CXX_MODULES FILES
            PakModule.cppm)

