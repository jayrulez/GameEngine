# Tools.ModelViewer - the standalone model viewer (Documentation/Specs/model-viewer.md): model files
# opened without a project in a lit 3D view, on the engine's default application with a GUI of its
# own. Its headless half is the ModelViewer library; its window's markup and sheet are under
# Data/ModelViewer, its look the editor's sheet (Data/Editor).

add_executable(Tools.ModelViewer Main.cpp)
target_link_libraries(Tools.ModelViewer PRIVATE
    Foundation::Core Foundation::Runtime Foundation::Runtime.Client Foundation::Runtime.Desktop
    Foundation::Shell Foundation::Shell.Desktop Foundation::Graphics Foundation::Graphics.Gpu
    Foundation::RHI Foundation::VFS Foundation::Image
    Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.HarfBuzz Foundation::Fonts.Bidi
    Foundation::GUI Foundation::GUI.Hosting Foundation::GUI.Toolkit Foundation::GUI.VFS
    Foundation::Scene Foundation::Geometry Foundation::Render Engine::DefaultApp Engine::Render Engine::Model
    Editor::Preview.Viewport Tools::ModelViewer)
util_copy_runtime_deps(Tools.ModelViewer EXTRA ${BUILDSYSTEM_DXC_LIB} ${BUILDSYSTEM_WGPU_LIB})
if(UNIX AND NOT APPLE)
    set_property(TARGET Tools.ModelViewer APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    set_property(TARGET Tools.ModelViewer PROPERTY INSTALL_RPATH "$ORIGIN")
endif()
