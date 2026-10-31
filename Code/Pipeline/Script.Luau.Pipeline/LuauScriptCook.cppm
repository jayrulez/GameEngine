// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script.Luau - the `script.luau.pipeline` module (tooling).
//
// The Luau cook service: compile-checks a Luau behavior in a cooker-owned Luau VM (resolved
// through the backend registry by language), harvests editor properties by constructing the
// class and walking the instance's fields, compiles the source to a bytecode blob for the pack,
// and supplies the New-Asset starters. All Luau-specific cook syntax lives here, not in the
// neutral script.pipeline.
//
// The cook VM is created through the neutral IScriptContext surface
// (CreateScriptManagerForLanguage), so no Luau C header appears in this interface unit.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module script.luau.pipeline;

import foundation.core;
import foundation.script;         // registry, IScriptManager/IScriptContext, RegisterReflectedTypes
import foundation.script.resource; // ScriptClassSource and ScriptPropertyDesc
import foundation.script.facades;  // RegisterScriptFacadeReflection (the cook VM's global surface)
import script.pipeline;   // IScriptLanguageCook, registry, shared cook helpers
import foundation.script.luau;     // ensures the Luau backend is available to the registry

using namespace foundation::core;
using namespace foundation::script;

export namespace pipeline{
    // The New Asset starter (the behavior convention pre-filled). Luau OOP is a global class
    // table and metatable; the engine instantiates via `<Class>.new(entity)`. Editor properties
    // are plain fields set in the constructor, which the cook harvests by walking the built
    // instance. Reflected instance methods use a colon (self.entity:position()); static facades
    // use a dot (Log.info, Time.delta); Float3 is Luau's native vector (v.x, arithmetic).
    inline constexpr StringView kLuauBehaviorStarter =
        u8"-- Behavior class - attach via a ScriptComponent behavior slot.\n"
        u8"-- Luau OOP: a GLOBAL class table + metatable; the engine calls {ClassName}.new(entity).\n"
        u8"-- Reflected engine types are globals: Float3 (native vector), Log, Time, Random.\n"
        u8"{ClassName} = {}\n"
        u8"{ClassName}.__index = {ClassName}\n"
        u8"\n"
        u8"function {ClassName}.new(entity)\n"
        u8"    local self = setmetatable({}, {ClassName})\n"
        u8"    self.entity = entity\n"
        u8"    -- Editor properties are plain fields set here; the cook harvests them by walking\n"
        u8"    -- the constructed instance. Harvested kinds: number (float), boolean (bool),\n"
        u8"    -- string, Float3 (vec3). Keep the constructor pure field init (it runs at cook).\n"
        u8"    self.speed = 1.0\n"
        u8"    return self\n"
        u8"end\n"
        u8"\n"
        u8"-- Handlers dispatch by presence (onStart/onUpdate/on<Event>). Reflected instance\n"
        u8"-- methods use COLON (self.entity:name()); static facades use DOT (Log.info).\n"
        u8"function {ClassName}:onStart()\n"
        u8"    Log.info(\"{ClassName} started on \" .. self.entity:name())\n"
        u8"end\n"
        u8"\n"
        u8"function {ClassName}:onUpdate(dt)\n"
        u8"    -- Drift along +X at `speed` units/second.\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + self.speed * dt, p.y, p.z)\n"
        u8"end\n"
        u8"\n"
        u8"function {ClassName}:onDestroy()\n"
        u8"end\n";

    // The New Asset starter for a Level (the scene-scripting tier): one object per scene,
    // constructed with the scene's bound handle. All handlers optional (dispatch by presence).
    // onUpdate/onFixedUpdate run only while the scene simulates.
    inline constexpr StringView kLuauLevelStarter =
        u8"-- Level class - the per-scene script. Set it on the scene's Scene Script settings.\n"
        u8"-- One instance per scene, constructed with the scene handle (reserved name `Level`).\n"
        u8"Level = {}\n"
        u8"Level.__index = Level\n"
        u8"\n"
        u8"function Level.new(scene)\n"
        u8"    return setmetatable({ scene = scene }, Level)\n"
        u8"end\n"
        u8"\n"
        u8"-- Static facades use DOT (Log.info); the scene handle uses COLON: self.scene:find(\"name\").\n"
        u8"function Level:onStart()\n"
        u8"    Log.info(\"Level started\")\n"
        u8"end\n"
        u8"-- Gameplay dt; runs only while the scene simulates.\n"
        u8"function Level:onUpdate(dt)\n"
        u8"end\n"
        u8"-- Fixed-step dt (physics lane); runs only while the scene simulates.\n"
        u8"function Level:onFixedUpdate(dt)\n"
        u8"end\n"
        u8"function Level:onStop()\n"
        u8"end\n";

    // The New Asset starter for the game orchestrator: the mandatory class `Game`. One per
    // run; drives scene loading (through the run facade) and the game-wide update.
    inline constexpr StringView kLuauGameStarter =
        u8"-- Game class - the game orchestrator (mandatory name `Game`). One per run.\n"
        u8"Game = {}\n"
        u8"Game.__index = Game\n"
        u8"\n"
        u8"function Game.new()\n"
        u8"    return setmetatable({}, Game)\n"
        u8"end\n"
        u8"\n"
        u8"-- Runs once at start. Load the opening scene here via the run facade:\n"
        u8"--   run.loadScene(Guid.new(\"<the scene's id>\"))\n"
        u8"function Game:launch()\n"
        u8"    Log.info(\"Game launched\")\n"
        u8"end\n"
        u8"-- Game-wide update (context dt). Per-scene logic belongs in a Level.\n"
        u8"function Game:update(dt)\n"
        u8"end\n"
        u8"function Game:exit()\n"
        u8"end\n";

    /// Registers the Luau cook (and, idempotently, the Luau backend it needs) so the neutral
    /// ScriptClassAssetBuilder resolves it by language. Entry points call this alongside
    /// RegisterAngelScriptScriptCook(). Idempotent.
    void RegisterLuauScriptCook();
}
