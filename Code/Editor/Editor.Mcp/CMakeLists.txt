# Editor::Mcp - `editor.mcp`: the project MCP tools (project_create/open/info) over foundation.mcp,
# built on the headless EditorProject. A module's own tool contribution; headless (no UI).

util_add_engine_library(Editor.Mcp ALIAS Editor::Mcp)
target_link_libraries(Editor.Mcp PUBLIC
    Editor::Project Foundation::Content Foundation::VFS
    Pipeline::Core Pipeline::Importer Pipeline::Cook Pipeline::Audio # SoundCueAsset (health audit)
    Foundation::Scene Foundation::Scene.Resource Foundation::Xml.Serialization
    Foundation::Resource # factory-less ResourceManager = the scene-Ref collector (asset_uses)
    Pipeline::Script Foundation::Script.Resource # script_validate: the language-cook registry
    Foundation::Script # script_create: backend registry (language -> file extension)
    Pipeline::Shaders Foundation::Shaders # shader_validate: the shader cook's check
    Pipeline::GUI Foundation::GUI # gui_validate: the GUI cooks' check
    Pipeline::Materials Foundation::Materials.Resource # material_set_shader: a material on a shader
    Engine::Composition # the FULL manager set for scene_validate's scratch (full payload validation)
                         # and the scene format reference's composed example (every manager + settings block)
    Engine::Script # the reference example's script behaviour (one override per property kind)
    Engine::Project Engine::Render # project_settings_set: the reflected settings, the MSAA levels
    Foundation::Mcp.Reflection Foundation::Mcp.Script # the engine surface root registers their tools too
    Foundation::Mcp Foundation::Json Foundation::Core Foundation::Policy)

target_sources(Editor.Mcp
    PUBLIC
        FILE_SET editor_mcp_modules TYPE CXX_MODULES FILES
            McpSession.cppm
            ProjectOperations.cppm
            AssetCreate.cppm
            AssetData.cppm
            ProjectSettingsTools.cppm
            ReflectedFields.cppm
            ExportPresetTools.cppm
            AssetDelete.cppm
            SceneTools.cppm
            AssetUses.cppm
            ProjectHealth.cppm
            LogTools.cppm
            McpResources.cppm
            ScriptValidate.cppm
            ShaderValidate.cppm
            GuiValidate.cppm
            MaterialShader.cppm
            ScriptCreate.cppm
            ProjectExport.cppm
            SceneReference.cppm
            ProjectTools.cppm
    PRIVATE
        SceneReferenceImpl.cpp)
