# Draconic::UI.VFS - `draconic.ui.vfs`: a VFS-backed IResourceProvider for the UI.
# Kept separate from the core Draconic::UI so the UI stays free of a VFS dependency
# (the app links this and wires the provider into StyleSheetLoader). Mirrors
# Sedulous.UI.IO.
add_library(Draconic.UI.VFS STATIC)
add_library(Draconic::UI.VFS ALIAS Draconic.UI.VFS)

target_link_libraries(Draconic.UI.VFS
    PUBLIC Draconic::Foundation Draconic::UI Draconic::Image Draconic::Image.IO Draconic::VFS Draconic::Policy)

target_sources(Draconic.UI.VFS
    PUBLIC
        FILE_SET uivfs_modules TYPE CXX_MODULES FILES
            VfsResourceProvider.cppm)

