// Draconic::ScriptWrenEditor - the `draconic.script.wren.editor` module (tooling).
//
// The Wren cook service (scripting.md §5 + §7.5): compile-checks a Wren behavior in a
// cooker-owned Wren VM (resolved through the backend registry by language), harvests the
// `static properties` map via a Fiber probe, scans handlers, and supplies the New-Asset
// starter. ALL Wren-specific cook syntax lives HERE, not in the neutral draconic.script.editor.
//
// This is a plain module interface unit: it spins up the cook VM through the NEUTRAL
// IScriptContext surface (CreateScriptManagerForLanguage), so no Wren C header appears
// here at all - the GCC module-hygiene rule (backend headers out of interface units) is
// satisfied by construction.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.script.wren.editor;

import draconic.foundation;
import draconic.script; // registry + IScriptManager/IScriptContext + RegisterReflectedTypes
import draconic.script.resource; // ScriptClassSource + ScriptPropertyDesc + parse helpers
import draconic.script.facades;  // RegisterScriptFacadeReflection (the cook VM's "main" surface)
import draconic.script.editor;   // IScriptLanguageCook + registry + shared cook helpers
import draconic.script.wren;     // ensures the Wren backend is available to the registry

using namespace draconic::foundation;

export namespace draconic::script
{
    // The New Asset starter (the behavior convention pre-filled). Property values reach an
    // instance through plain Wren SETTERS ("speed" -> `speed=(v)`) - harvested names are
    // pushed via `Invoke("<name>=")` at instantiate, after construct new(entity).
    inline constexpr StringView kScriptBehaviorStarter =
        u8"// Behavior class - attach via a ScriptComponent behavior slot.\n"
        u8"// NOTE Wren is newline-sensitive: `{` must sit on the signature's line.\n"
        u8"// Reflected engine types live in the \"main\" module:\n"
        u8"//   import \"main\" for Float3\n"
        u8"class NewBehavior {\n"
        u8"    // name: [type, default, description?]  - types: float, int, bool, string,\n"
        u8"    // color, vec3, entity, asset:<TypeName>\n"
        u8"    static properties { {\n"
        u8"        \"speed\": [\"float\", 1.0, \"units per second\"],\n"
        u8"    } }\n"
        u8"\n"
        u8"    construct new(entity) {\n"
        u8"        _entity = entity\n"
        u8"        _speed = 1.0\n"
        u8"    }\n"
        u8"    // One setter per declared property (the engine pushes values through them).\n"
        u8"    speed=(v) { _speed = v }\n"
        u8"\n"
        u8"    // Facades use Wren method syntax (dot, lowercase): Log.info, Time.delta,\n"
        u8"    // Random.value; the entity handle likewise (_entity.setName, ...). The scene is\n"
        u8"    // reached THROUGH the entity: _entity.scene.find(\"name\"), _entity.scene.spawn(...).\n"
        u8"    onStart() {\n"
        u8"        Log.info(\"NewBehavior started on %(_entity.name())\")\n"
        u8"    }\n"
        u8"    onUpdate(dt) {\n"
        u8"        // Drift along +X at `speed` units/second.\n"
        u8"        var p = _entity.position()\n"
        u8"        _entity.setPosition(p.x + _speed * dt, p.y, p.z)\n"
        u8"    }\n"
        u8"    onDestroy() {}\n"
        u8"}\n";

    // The New Asset starter for a Level (the scene-scripting tier): one object per scene,
    // constructed with the scene's bound handle. All handlers optional (dispatch by presence).
    // onUpdate/onFixedUpdate run ONLY while the scene simulates.
    inline constexpr StringView kScriptLevelStarter =
        u8"// Level class - the per-scene script. Set it on the scene's Scene Script settings.\n"
        u8"// One instance per scene, constructed with the scene handle. NOTE Wren is\n"
        u8"// newline-sensitive: `{` must sit on the signature's line.\n"
        u8"class Level {\n"
        u8"    construct new(scene) {\n"
        u8"        _scene = scene\n"
        u8"    }\n"
        u8"\n"
        u8"    // Facades use Wren method syntax (dot, lowercase): Log.info, Time.delta. The scene\n"
        u8"    // is the bound handle: _scene.find(\"name\"), _scene.spawn(prefab, x, y, z).\n"
        u8"    onStart() {\n"
        u8"        Log.info(\"Level started\")\n"
        u8"    }\n"
        u8"    // Gameplay dt; runs only while the scene simulates.\n"
        u8"    onUpdate(dt) {}\n"
        u8"    // Fixed-step dt (physics lane); runs only while the scene simulates.\n"
        u8"    onFixedUpdate(dt) {}\n"
        u8"    onStop() {}\n"
        u8"}\n";

    // The New Asset starter for the game orchestrator: the MANDATORY class `Game`. One per
    // run; drives scene loading (through the SceneLoader facade) and the game-wide update.
    inline constexpr StringView kScriptGameStarter =
        u8"// Game class - the game orchestrator (mandatory name `Game`). One per run.\n"
        u8"// NOTE Wren is newline-sensitive: `{` must sit on the signature's line.\n"
        u8"class Game {\n"
        u8"    construct new() {}\n"
        u8"\n"
        u8"    // Runs once at start. Load the opening scene here (SceneLoader facade):\n"
        u8"    //   import \"main\" for SceneLoader\n"
        u8"    //   SceneLoader.loadScene(\"Main\")\n"
        u8"    launch() {\n"
        u8"        Log.info(\"Game launched\")\n"
        u8"    }\n"
        u8"    // Game-wide update (context dt). Per-scene logic belongs in a Level.\n"
        u8"    update(dt) {}\n"
        u8"    exit() {}\n"
        u8"}\n";

    /// Registers the Wren cook (and, idempotently, the Wren backend it needs) so the
    /// neutral ScriptClassAssetBuilder resolves it by language. Entry points call this
    /// (exactly like registering the backend). Idempotent.
    void RegisterWrenScriptCook();
}
