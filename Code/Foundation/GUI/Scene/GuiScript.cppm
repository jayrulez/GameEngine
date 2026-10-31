// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :gui_script partition
//
// The GUI's script surface. There is no separate UI-script layer: the widgets and their
// properties are reflected through our RTTI (GUIReflectionImpl.cpp), so the engine's
// reflection-driven binding reaches the node tree from AngelScript and Luau alike, and markup
// creates the same reflected element types by tag (ElementTypes). This partition holds the entry
// points: the `gui` facade (`gui.root()` returns the root the host installed for the calling
// script) and the binding a host installs per script context. ElementTypes decides which element
// types can be created by name.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:gui_script;

import foundation.core;
import foundation.script;
import :node;
import :scene_root;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    inline constexpr core::StringView kGuiScriptService = u8"gui.runtime";

    /// What a host gives a script context: the root its scripts build under, or a function that
    /// finds the root at each call. The function takes precedence when set; a host uses it when
    /// the root depends on what is running the script (an editor runs a game per tab).
    struct GuiScriptBinding
    {
        SceneRoot* root = nullptr;
        core::Function<SceneRoot*()> resolve;

        [[nodiscard]] SceneRoot* Root() const { return resolve ? resolve() : root; }
    };

    inline void InstallGuiScriptService(foundation::script::IScriptContext& scriptContext, GuiScriptBinding& binding)
    {
        scriptContext.SetService(kGuiScriptService, &binding);
    }
    inline void ClearGuiScriptService(foundation::script::IScriptContext& scriptContext)
    {
        scriptContext.SetService(kGuiScriptService, nullptr);
    }

    /// The `gui` facade, with static members only (script name `gui`).
    class Gui : public Object
    {
        RTTI_OBJECT(Gui, Object)
    public:
        /// Returns the calling script's root (a Node handle), or null when its host installed none.
        [[nodiscard]] static Variant Root();
        /// Returns the GUI's data value `name` converted to a number, text or boolean (0, empty or
        /// false when it is unset or cannot be converted). A binding that reads it follows it.
        [[nodiscard]] static f64 GetNumber(core::StringView name);
        [[nodiscard]] static core::String GetText(core::StringView name);
        /// Returns the GUI's localized text for `key`, or the key itself when no table has it.
        [[nodiscard]] static core::String Translate(core::StringView key);
        /// Returns the key's plural form for `count`, with `{0}` replaced by the count.
        [[nodiscard]] static core::String TranslateCount(core::StringView key, i32 count);
        /// Returns the GUI's language ("en", "ar-EG"), or empty text with no localization.
        [[nodiscard]] static core::String Language();
        [[nodiscard]] static bool GetBool(core::StringView name);
        /// Returns what is driving the calling script's GUI now: "pointer", "keyboard" or "pad"
        /// (the root receives `inputkind` when it changes). A binding that reads it follows it.
        [[nodiscard]] static core::String InputKindNow();
        /// Sets the GUI's data value `name`; everything bound to it follows.
        static void Set(core::StringView name, const Variant& value);
        /// TweenData animates the GUI's data value `name` to `to` over `seconds`
        /// (GUIContext::AnimateData) and returns the running tween. CreateDataTween makes the same
        /// tween without starting it, as a step for a group. Both return null when the host
        /// installed no root.
        static RefPtr<Tween> TweenData(core::StringView name, f64 to, f32 seconds, core::StringView easing);
        static RefPtr<Tween> CreateDataTween(core::StringView name, f64 to, f32 seconds, core::StringView easing);
        /// Commands: define one with its label, enable or disable it, set its checked state, and
        /// run it along the focus path (which returns true when a handler ran).
        static void DefineCommand(core::StringView id, core::StringView label);
        static void SetCommandEnabled(core::StringView id, bool enabled);
        static void SetCommandChecked(core::StringView id, bool checked);
        /// Sets why the command is disabled. A control bound to it shows this while the command is
        /// disabled (as a tool bar button's tooltip).
        static void SetCommandDisabledReason(core::StringView id, core::StringView reason);
        [[nodiscard]] static bool IsCommandChecked(core::StringView id);
        static bool RunCommand(core::StringView id);
        /// Binds `keys` ("ctrl-s", "pad-east", a chord) to `command` where `context` holds (empty
        /// means everywhere). Returns false when the keys do not parse.
        static bool BindKey(core::StringView keys, core::StringView command, core::StringView context);
        /// Captures the next key or pad button before anything else sees it (for a rebinding
        /// screen) and calls `handler(keys)` with its binding name ("ctrl-s", "pad-north"), or with
        /// empty text when Escape cancelled. The handler has the shape
        /// `void KeyHandler(const string &in keys)`.
        static void CaptureKey(RefPtr<foundation::script::IScriptDelegate> handler);
        /// Returns a binding's keys as a menu shows them ("ctrl-s" becomes "Ctrl+S"), or empty text
        /// when they do not parse.
        [[nodiscard]] static core::String KeyText(core::StringView keys);
    };

    /// What a script handler (`node.on(name, fn)`) receives: the event's data, with positions in
    /// the GUI's own units (`x`, `y` relative to the root, `localX`, `localY` relative to the node
    /// the handler is on). A handler that returns true takes the event, and it goes no further on
    /// its route.
    struct GuiEvent
    {
        core::String type;  ///< the event name the handler listened for: "click", "mousedown", "keydown", ...
        core::String phase; ///< "capture", "target" or "bubble"
        core::String id;    ///< the id of the node the event is for (empty when it has none)
        core::String pointer; ///< "mouse" or "touch", for pointer events
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 localX = 0.0f;
        f32 localY = 0.0f;
        i32 button = 0; ///< 0 left, 1 right, 2 middle, 3 and 4 the extra buttons
        i32 clickCount = 0;
        i32 key = 0; ///< a KeyCode value for keys
        i32 modifiers = 0; ///< shift 1, ctrl 2, alt 4, super 8
        core::String text; ///< typed text, a link's id, or the property or @keyframes a transitionend or animationend is for
        f32 deltaX = 0.0f; ///< the wheel delta, or a gesture's translation since the last event
        f32 deltaY = 0.0f;
        // A gesture's totals since it began and this event's change; x and y are the midpoint of
        // its fingers.
        f32 scale = 1.0f;
        f32 rotation = 0.0f; ///< radians, clockwise
        f32 translationX = 0.0f;
        f32 translationY = 0.0f;
        f32 scaleDelta = 1.0f;
        f32 rotationDelta = 0.0f;
        core::String dragType; ///< a drag's payload type (its value is in `text`, its drop place in `index`)
        core::String effect;   ///< a drag's effect: "none", "copy", "move" or "link"
        i32 index = -1; ///< the tab for `tabclose`, or the row for `activate` and for a pointer event over a row
        f32 elapsed = 0.0f; ///< how many seconds the motion of a transitionend or animationend ran
    };

    /// Registers the GUI's own element types (Label, Button, the layouts and so on) with
    /// ElementTypes::Register, once. A host calls it at start-up, just as a library that adds its
    /// own widgets registers them.
    void RegisterGuiElements();

    /// Registers the GUI's element types and the `gui` facade for scripts, once. Call it before a
    /// script manager binds the reflection registry.
    void RegisterGuiScriptSurface();
}
