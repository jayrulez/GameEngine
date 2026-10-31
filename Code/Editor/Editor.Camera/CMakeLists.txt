# Editor::Camera - `editor.camera`: the shared free-fly viewport camera (EditorCamera), in a
# small module over the foundation only, so every editor page lib can import it without pulling
# in the render, scene and viewport graph. It stays apart from editor.preview because a combined
# interface triggers a GCC module-merge ICE. Tooling only.

util_add_engine_library(Editor.Camera ALIAS Editor::Camera)
target_link_libraries(Editor.Camera
    PUBLIC Foundation::Core Foundation::Shell Foundation::Policy)

target_sources(Editor.Camera
    PUBLIC
        FILE_SET editor_camera_modules TYPE CXX_MODULES FILES
            EditorCamera.cppm)
