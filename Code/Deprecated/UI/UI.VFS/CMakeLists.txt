# Foundation::UI.VFS - `foundation.ui.vfs`: a VFS-backed IResourceProvider for the UI.
# Kept separate from the core Foundation::UI so the UI stays free of a VFS dependency
# (the app links this and wires the provider into StyleSheetLoader). Mirrors
# Sedulous.UI.IO.
util_add_engine_library(UI.VFS ALIAS Foundation::UI.VFS)
target_link_libraries(UI.VFS
    PUBLIC Foundation::Core Foundation::UI Foundation::Image Foundation::Image.IO Foundation::VFS Foundation::Policy)

target_sources(UI.VFS
    PUBLIC
        FILE_SET uivfs_modules TYPE CXX_MODULES FILES
            VfsResourceProvider.cppm)

