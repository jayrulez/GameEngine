// Draconic::ScriptAngelScriptEditor - the `draconic.script.angelscript.editor` module.
//
// The AngelScript cook service (scripting.md §5 + §7.5): compile-check in a cooker-owned
// AngelScript VM (resolved through the backend registry by language) + the shared
// on<Upper>(...) handler scan + PROPERTY HARVEST + an AngelScript starter template.
//
// AngelScript's editor-property surface is typed member FIELDS annotated with the
// language's own `[metadata]`: `[default, "description"]` before a field declares that
// field an inspector property (a field with no metadata is not one). The cook builds the
// behavior through the vendored CScriptBuilder add-on (which pre-processes `[metadata]`)
// and walks the class's fields, mapping each metadata'd field's declared type + default +
// description into the SAME ScriptPropertyDesc metadata the Wren cook produces. All the
// AngelScript / scriptbuilder contact lives in the implementation unit.
//
// Plain module interface unit: no AngelScript SDK header appears here (GCC module hygiene
// by construction); the implementation unit reaches the cook VM's engine through the
// backend's AngelScriptEngineHandle seam.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.script.angelscript.editor;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::script
{
    // The New Asset starter for AngelScript: a behavior class whose constructor takes the
    // entity handle, with the lifecycle handlers stubbed. No properties (harvest deferred).
    inline constexpr StringView kAngelScriptBehaviorStarter =
        u8"// Behavior class - attach via a ScriptComponent behavior slot.\n"
        u8"// Reflected engine facades are visible globally (no import). In AngelScript the\n"
        u8"// static facades are NAMESPACED and lowercase - Log::info, Time::delta,\n"
        u8"// Random::value - and the entity handle uses '.' (self.setName, ...). The scene is\n"
        u8"// reached THROUGH the entity: self.scene.find(\"name\"), self.scene.spawn(...).\n"
        u8"class NewBehavior\n"
        u8"{\n"
        u8"    private Entity@ self;\n"
        u8"\n"
        u8"    NewBehavior(Entity@ entity) { @self = entity; }\n"
        u8"\n"
        u8"    void onStart()\n"
        u8"    {\n"
        u8"        Log::info(\"NewBehavior started on '\" + self.name() + \"'\");\n"
        u8"    }\n"
        u8"\n"
        u8"    void onUpdate(double dt)\n"
        u8"    {\n"
        u8"        // Drift along +X at 1 unit/second. Reflected types are handles in\n"
        u8"        // AngelScript, so self.position() (a Float3) binds to a Float3@.\n"
        u8"        Float3@ p = self.position();\n"
        u8"        self.setPosition(p.x + float(dt), p.y, p.z);\n"
        u8"        // Also available: Time::delta(), Random::value(), self.scene.find(\"name\").\n"
        u8"    }\n"
        u8"\n"
        u8"    void onDestroy() {}\n"
        u8"}\n";

    // The New Asset starter for a Level (the scene-scripting tier): one object per scene,
    // constructed with the scene handle. All handlers optional. onUpdate/onFixedUpdate run
    // ONLY while the scene simulates.
    inline constexpr StringView kAngelScriptLevelStarter =
        u8"// Level class - the per-scene script. Set it on the scene's Scene Script settings.\n"
        u8"// One instance per scene, constructed with the scene handle. In AngelScript the\n"
        u8"// static facades are NAMESPACED and lowercase - Log::info, Time::delta - and the\n"
        u8"// scene handle uses '.': scene.find(\"name\"), scene.spawn(...).\n"
        u8"class Level\n"
        u8"{\n"
        u8"    private Scene@ scene;\n"
        u8"\n"
        u8"    Level(Scene@ s) { @scene = s; }\n"
        u8"\n"
        u8"    void onStart() { Log::info(\"Level started\"); }\n"
        u8"    // Gameplay dt; runs only while the scene simulates.\n"
        u8"    void onUpdate(double dt) {}\n"
        u8"    // Fixed-step dt (physics lane); runs only while the scene simulates.\n"
        u8"    void onFixedUpdate(double dt) {}\n"
        u8"    void onStop() {}\n"
        u8"}\n";

    // The New Asset starter for the game orchestrator: the MANDATORY class `Game`. One per
    // run; drives scene loading (through the SceneLoader facade) and the game-wide update.
    inline constexpr StringView kAngelScriptGameStarter =
        u8"// Game class - the game orchestrator (mandatory name `Game`). One per run.\n"
        u8"class Game\n"
        u8"{\n"
        u8"    Game() {}\n"
        u8"\n"
        u8"    // Runs once at start. Load the opening scene here via the SceneLoader facade:\n"
        u8"    //   SceneLoader::loadScene(\"Main\");\n"
        u8"    void launch() { Log::info(\"Game launched\"); }\n"
        u8"    // Game-wide update (context dt). Per-scene logic belongs in a Level.\n"
        u8"    void update(double dt) {}\n"
        u8"    void exit() {}\n"
        u8"}\n";

    /// Registers the AngelScript cook (and, idempotently, the AngelScript backend it
    /// needs) so the neutral ScriptClassAssetBuilder resolves it by language. Entry points
    /// call this alongside RegisterWrenScriptCook(). Idempotent.
    void RegisterAngelScriptScriptCook();
}
