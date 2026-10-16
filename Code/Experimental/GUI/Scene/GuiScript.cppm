// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :gui_script partition
//
// The lab's script surface (G1 of Specs/vg-and-gui-lab.md). There is no separate UI-script layer:
// the widgets and their properties are reflected through our RTTI (GUIReflectionImpl.cpp), so the
// engine's reflection-driven binding reaches the node tree from AngelScript and Luau alike, and
// markup creates the same reflected element types by tag (ElementTypes). What lives here is the way in: the
// `gui` facade (`gui.root()`, the root the host installed for the calling script), the binding a
// host installs per script context, and the registry of element types creatable by name.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:gui_script;

import foundation.core;
import foundation.script;
import :node;
import :scene_root;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    inline constexpr core::StringView kGuiScriptService = u8"gui.runtime";

    /// What a host gives a script context: the root its scripts build under.
    struct GuiScriptBinding
    {
        SceneRoot* root = nullptr;
    };

    inline void InstallGuiScriptService(foundation::script::IScriptContext& context, GuiScriptBinding& binding)
    {
        context.SetService(kGuiScriptService, &binding);
    }
    inline void ClearGuiScriptService(foundation::script::IScriptContext& context)
    {
        context.SetService(kGuiScriptService, nullptr);
    }

    /// The `gui` facade: statics only (script name `gui`).
    class Gui : public Object
    {
        RTTI_OBJECT(Gui, Object)
    public:
        /// The calling script's root (a Node handle), or null when its host installed none.
        [[nodiscard]] static Variant Root();
        /// The GUI's data value `name` as a number, text or boolean (converted; 0, empty or false
        /// when unset or unconvertible). A read a binding follows.
        [[nodiscard]] static f64 GetNumber(core::StringView name);
        [[nodiscard]] static core::String GetText(core::StringView name);
        [[nodiscard]] static bool GetBool(core::StringView name);
        /// Sets the GUI's data value `name` (whatever is bound to it follows).
        static void Set(core::StringView name, const Variant& value);
        /// Commands (G7): define one (with its label), switch it on or off, set its checked state,
        /// run it along the focus path (true when a handler ran).
        static void DefineCommand(core::StringView id, core::StringView label);
        static void SetCommandEnabled(core::StringView id, bool enabled);
        static void SetCommandChecked(core::StringView id, bool checked);
        [[nodiscard]] static bool IsCommandChecked(core::StringView id);
        static bool RunCommand(core::StringView id);
        /// Binds `keys` ("ctrl-s", "pad-east", a chord) to `command` where `context` holds (empty:
        /// everywhere); false when they do not parse.
        static bool BindKey(core::StringView keys, core::StringView command, core::StringView context);
    };

    /// What a script handler (`node.on(name, fn)`) is given: the event's data, positions in the
    /// GUI's own units (`x`, `y` from the root, `localX`, `localY` from the node the handler is
    /// on). A handler returning true takes the event: it goes no further on its route.
    struct GuiEvent
    {
        core::String type;  ///< the name it was listened for: "click", "mousedown", "keydown", ...
        core::String phase; ///< "capture", "target" or "bubble"
        core::String pointer; ///< "mouse" or "touch", for pointer events
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 localX = 0.0f;
        f32 localY = 0.0f;
        i32 button = 0; ///< 0 left, 1 right, 2 middle, 3 and 4 the extra buttons
        i32 clickCount = 0;
        i32 key = 0; ///< a KeyCode value for keys
        i32 modifiers = 0; ///< shift 1, ctrl 2, alt 4, super 8
        core::String text; ///< typed text
        f32 deltaX = 0.0f; ///< the wheel
        f32 deltaY = 0.0f;
    };

    /// Registers the lab's own element types (ElementTypes::Register: Label, Button, the layouts,
    /// ...), once. A host calls it at start-up, as a library adding widgets of its own registers
    /// those.
    void RegisterGuiElements();

    /// Registers the lab's element types and the `gui` facade for scripts, once (before a script
    /// manager binds the reflection registry).
    void RegisterGuiScriptSurface();
}
