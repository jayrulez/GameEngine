# Tools.Editor - the editor executable (assembly of editor.app + the
# per-subsystem editor plugin modules).
# The EMBEDDED fallback font: Roboto baked into the exe as a byte array, so a relocated
# editor always has a face even when every disk path fails (settings path, dev-tree path).
set(_embedded_font_src "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedEditorFont.cpp")
add_custom_command(
    OUTPUT "${_embedded_font_src}"
    COMMAND ${CMAKE_COMMAND}
        -DIN=${CMAKE_SOURCE_DIR}/Data/Engine/Fonts/roboto/Roboto-Regular.ttf
        -DOUT=${_embedded_font_src}
        -DSYMBOL=g_embeddedEditorFont
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    DEPENDS "${CMAKE_SOURCE_DIR}/Data/Engine/Fonts/roboto/Roboto-Regular.ttf"
            "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    COMMENT "Embedding the editor fallback font")

add_executable(Tools.Editor Main.cpp "${_embedded_font_src}")
# The pages are parked while the editor moves onto the GUI (Documentation/Specs/editor-gui.md, 4.1):
# each Editor:: page library comes back here with its port.
target_link_libraries(Tools.Editor PRIVATE
    Foundation::Core
    Foundation::Runtime Foundation::Runtime.Client Foundation::Runtime.Desktop Foundation::Shell.Desktop
    Foundation::Shell Foundation::Graphics Foundation::Graphics.Gpu
    Engine::Scene Engine::Render Engine::Animation Engine::Particles
    Engine::Composition # RegisterAllScriptFacades: the COMPLETE engine facade surface the cook
                          # AND live validation compile against (run/ui/physics/audio/... facades)
    Editor::Core Editor::App Editor::Navigation
    Editor::Scene # the scene page (its hierarchy and inspector come with their layers)
    Editor::Play # the Game tabs and the PIE tools
    Editor::Spline # the scene view's spline tool
    Editor::Terrain # the scene view's terrain brushes
    Editor::Vegetation # and the vegetation's
    Editor::Generic # the fallback asset page
    Editor::Image Editor::Heightfield Editor::Texture Editor::Fonts Editor::Geometry Editor::Physics Editor::Materials Editor::Render Editor::Audio Editor::Input Editor::Script Editor::Shaders Editor::GUI Editor::Particles Editor::Animation # the asset pages, as they port (X6)
    Pipeline::Core Pipeline::Cook
    Pipeline::Registration # the composition root: every builder/importer/type registration
    Foundation::Resource Foundation::Geometry.Resource Foundation::Materials.Resource Foundation::Texture.Resource
    Pipeline::Texture Pipeline::Fonts Pipeline::Image Pipeline::Geometry
    Pipeline::Animation Pipeline::Materials Pipeline::Shaders
    Pipeline::Particles Foundation::Input Foundation::Input.Resource Pipeline::Input Engine::Input Foundation::Physics Foundation::Physics.Resource Pipeline::Physics Engine::Physics Pipeline::Navigation Engine::Navigation Foundation::Navigation.Resource Foundation::Audio Foundation::Audio.Resource Pipeline::Audio Engine::Audio
    # Script backends + cooks (angelscript | luau) come transitively from Pipeline::Registration.
    Foundation::Script Foundation::Script.Resource Foundation::Script.Facades Pipeline::Script Engine::Script Pipeline::ModelImporter Foundation::Image.Resource
    Foundation::Animation.Resource)
# Stage the editor's runtime deps beside it + emit Tools.Editor.runtime-libs (the editor packaging
# script reads that list for its sidecars). SDL3 rides in as EXTRA (a no-op when unset, e.g. Linux).
# DXC (libdxcompiler.so / dxcompiler.dll) is dlopen'd at runtime by the shader COOK: the editor
# authors and recompiles shaders, so unlike a shipped game player it needs DXC present, and it is not
# a link dependency so it must be staged explicitly. On Linux, $ORIGIN on the RUNPATH lets a relocated
# dist find DXC beside the exe (Windows searches the exe dir for a bare DLL by default).
# wgpu-native is a dlopen dependency with the SAME shape as DXC: the --webgpu backend resolves
# the bare soname via $ORIGIN in a relocated dist, so it must be staged too - without it a dist
# launched with --webgpu renders nothing.
# The third-party notices ride along (cmake/ThirdPartyNotices.cmake).
util_copy_runtime_deps(Tools.Editor EXTRA ${BUILDSYSTEM_DXC_LIB} ${BUILDSYSTEM_WGPU_LIB}
                                    ${BUILDSYSTEM_NOTICES_FILE})
if(UNIX AND NOT APPLE)
    set_property(TARGET Tools.Editor APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    set_property(TARGET Tools.Editor PROPERTY INSTALL_RPATH "$ORIGIN")
endif()

