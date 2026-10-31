# Foundation::GUI.VFS - `foundation.gui.vfs`: a VFS-backed IResourceProvider for the resources the
# GUI's CSS names (background-image: url()). Kept separate from the core Foundation::GUI so the GUI
# stays free of a VFS dependency (the app links this and hands the provider to a StyleManager).
util_add_engine_library(GUI.VFS ALIAS Foundation::GUI.VFS)
target_link_libraries(GUI.VFS
    PUBLIC Foundation::Core Foundation::GUI Foundation::Image Foundation::Image.IO Foundation::VFS Foundation::Policy)

target_sources(GUI.VFS
    PUBLIC
        FILE_SET guivfs_modules TYPE CXX_MODULES FILES
            VfsResourceProvider.cppm)

