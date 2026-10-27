// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - the input tools' bodies.
module;
#include "Core/Prelude.h"

module editor.app;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.gui;
import foundation.gui.inspector;
import foundation.gui.testing;
import editor.core;

using namespace foundation::core;
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolCall;
using foundation::mcp::ToolCallState;
using foundation::mcp::ToolOutcome;
namespace gui = foundation::gui;
namespace testing = foundation::gui::testing;

namespace editor::app
{
    namespace
    {
        constexpr usize kFindLimit = 50; // nodes editor_find lists; the count says how many there were
        constexpr usize kNamedLimit = 10; // nodes a refusal names when a target finds several

        // The seams, shared by every handler: they outlive the server through the handlers' holds.
        class InputToolState final : public RefCounted
        {
        public:
            explicit InputToolState(InputToolSeams seams) : seams(Move(seams)) {}
            InputToolSeams seams;

            [[nodiscard]] gui::SceneRoot* Root() const
            {
                gui::SceneRoot* root = seams.root ? seams.root() : nullptr;
                return root != nullptr && root->GetEventDispatcher() != nullptr ? root : nullptr;
            }
        };

        // An acting tool's call: done, its answer waits for the frame after it.
        class FrameWait final : public ToolCallState
        {
        public:
            JsonValue answer;
        };

        constexpr StringView kTargetHelp =
            u8"`target` names a node as the GUI's tests do: a CSS selector (\"#input-tree\", \"button.primary\", "
            u8"\"treeitem\"), \"text=Jump\" (shows exactly that) or \"text~=Jum\" (shows it in part); `within` "
            u8"narrows it to under what that finds; `index` picks one when it finds several (in document order, "
            u8"from 0). Or `x` and `y`, a point in editor_screenshot's pixels.";

        JsonValue NodeJson(gui::Node& node)
        {
            const gui::Rect box = node.GetScreenBounds();
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"node", JsonValue::MakeString(gui::inspector::Label(node)));
            const String text = testing::TextOf(node);
            if (!text.IsEmpty())
                out.Set(u8"text", JsonValue::MakeString(text));
            out.Set(u8"x", JsonValue::MakeNumber(static_cast<f64>(box.x)));
            out.Set(u8"y", JsonValue::MakeNumber(static_cast<f64>(box.y)));
            out.Set(u8"width", JsonValue::MakeNumber(static_cast<f64>(box.width)));
            out.Set(u8"height", JsonValue::MakeNumber(static_cast<f64>(box.height)));
            return out;
        }

        JsonValue PointJson(Float2 at)
        {
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"x", JsonValue::MakeNumber(static_cast<f64>(at.x)));
            out.Set(u8"y", JsonValue::MakeNumber(static_cast<f64>(at.y)));
            return out;
        }

        // What `args` names: a finder from `target` and `within`, all it finds.
        bool FindAll(gui::SceneRoot& root, const JsonValue& args, Array<gui::Node*>& found, String& error)
        {
            const String target = args.Get(u8"target").AsString();
            if (target.IsEmpty())
            {
                error = String(u8"name a `target` (a selector, text=..., text~=...)");
                return false;
            }
            testing::Finder finder = testing::Finder::Parse(target.AsView());
            const String within = args.Get(u8"within").AsString();
            if (!within.IsEmpty())
                finder = finder.Within(testing::Finder::Parse(within.AsView()));
            found = finder.All(root);
            return true;
        }

        // Where `args` aims: the middle of the one node it names (refused when something else lies
        // there), or its point. `node` is what the pointer reaches there.
        bool Aim(gui::SceneRoot& root, const JsonValue& args, Float2& at, gui::Node*& node, String& error)
        {
            if (!args.Has(u8"target"))
            {
                if (!args.Has(u8"x") || !args.Has(u8"y"))
                {
                    error = String(u8"name a `target`, or give `x` and `y`");
                    return false;
                }
                at = Float2{static_cast<f32>(args.Get(u8"x").AsNumber()), static_cast<f32>(args.Get(u8"y").AsNumber())};
                gui::EventDispatcher* dispatcher = root.GetEventDispatcher();
                node = dispatcher != nullptr ? dispatcher->NodeAt(at) : root.OverFind(at); // a popup's first, as input
                return true;
            }
            Array<gui::Node*> found;
            if (!FindAll(root, args, found, error))
                return false;
            const i64 index = args.Get(u8"index").AsInt(-1);
            if (found.IsEmpty() || (index < 0 && found.Size() > 1) || index >= static_cast<i64>(found.Size()))
            {
                error = Format(u8"{} shown node(s) match", found.Size());
                if (found.Size() > 1)
                    error += StringView(u8"; pass `index` or narrow it with `within`");
                for (usize i = 0; i < found.Size() && i < kNamedLimit; ++i)
                    error += Format(u8"\n  [{}] {} \"{}\"", i, gui::inspector::Label(*found[i]), testing::TextOf(*found[i]));
                if (found.IsEmpty())
                    error += StringView(u8" (editor_find lists what a target finds)");
                return false;
            }
            node = found[index < 0 ? 0 : static_cast<usize>(index)];
            gui::Node* over = nullptr;
            if (!testing::AimAt(root, *node, at, over))
            {
                error = Format(u8"{} is covered at its middle by {} (a popup or a dialog over it?)", gui::inspector::Label(*node),
                               over != nullptr ? gui::inspector::Label(*over) : String(u8"nothing"));
                return false;
            }
            return true;
        }

        [[nodiscard]] bool SameWord(StringView word, StringView name)
        {
            return word.Size() == name.Size() && word.ContainsIgnoreCase(name);
        }

        // "ctrl-shift" as the dispatcher's modifier bits; false for a word it does not know.
        bool ParseModifiers(StringView text, u32& modifiers)
        {
            modifiers = 0;
            usize start = 0;
            while (start < text.Size())
            {
                usize end = start;
                while (end < text.Size() && text[end] != u8'-' && text[end] != u8'+')
                    ++end;
                const StringView word = text.SubStr(start, end - start);
                if (SameWord(word, u8"ctrl") || SameWord(word, u8"control"))
                    modifiers |= gui::KeyModCtrl;
                else if (SameWord(word, u8"shift"))
                    modifiers |= gui::KeyModShift;
                else if (SameWord(word, u8"alt"))
                    modifiers |= gui::KeyModAlt;
                else if (SameWord(word, u8"super") || SameWord(word, u8"meta") || SameWord(word, u8"cmd"))
                    modifiers |= gui::KeyModSuper;
                else if (!word.IsEmpty())
                    return false;
                start = end + 1;
            }
            return true;
        }

        Array<String> Choices(Span<const StringView> values)
        {
            Array<String> out(editor::EditorRootAllocator());
            for (const StringView v : values)
                out.PushBack(String(v));
            return out;
        }

        // The focus and what lies under the pointer, as the frame after an act shows them.
        void AddAfter(gui::SceneRoot& root, JsonValue& answer)
        {
            gui::EventDispatcher* dispatcher = root.GetEventDispatcher();
            gui::Node* focus = dispatcher != nullptr ? dispatcher->GetFocusNode() : nullptr;
            answer.Set(u8"focus", focus != nullptr ? NodeJson(*focus) : JsonValue::MakeNull());
        }

        // An acting tool: `act` on its first entry (an answer, or an error), the answer a frame later.
        using Act = Function<bool(gui::SceneRoot& root, const JsonValue& args, JsonValue& answer, String& error)>;
        foundation::mcp::ToolCallHandler Acting(const RefPtr<InputToolState>& state, Act act)
        {
            return [state, act = Move(act)](ToolCall& call, const JsonValue& args) -> ToolOutcome
            {
                gui::SceneRoot* root = state->Root();
                if (root == nullptr)
                    return Err(String(u8"the editor has no window GUI now"));
                if (call.isReentry && call.State<FrameWait>() != nullptr)
                {
                    JsonValue answer = Move(call.State<FrameWait>()->answer);
                    AddAfter(*root, answer);
                    return answer;
                }
                JsonValue answer = JsonValue::MakeObject();
                String error;
                if (!act(*root, args, answer, error))
                    return Err(Move(error));
                auto wait = MakeUnique<FrameWait>(editor::EditorRootAllocator());
                wait->answer = Move(answer);
                call.state = UniquePtr<ToolCallState>(wait.Release(), editor::EditorRootAllocator());
                return ToolOutcome::NotFinished();
            };
        }
    }

    void RegisterInputTools(foundation::mcp::McpServer& server, InputToolSeams seams)
    {
        RefPtr<InputToolState> shared = MakeRef<InputToolState>(editor::EditorRootAllocator(), Move(seams));

        server.RegisterTool(
            u8"editor_find",
            Format(u8"The nodes of the editor's GUI a target names, shown ones only: {node, text, x, y, width, "
                   u8"height} each in document order (x, y, width and height in editor_screenshot's pixels), at "
                   u8"most {}; `count` says how many there were. What to aim editor_click at. {}",
                   kFindLimit, kTargetHelp),
            SchemaBuilder()
                .Str(u8"target", u8"a selector, text=..., or text~=...", true)
                .Str(u8"within", u8"only under what this finds")
                .Build(),
            ToolAnnotations::ReadOnly(),
            [shared](const JsonValue& args) -> ToolOutcome
            {
                gui::SceneRoot* root = shared->Root();
                if (root == nullptr)
                    return Err(String(u8"the editor has no window GUI now"));
                Array<gui::Node*> found;
                String error;
                if (!FindAll(*root, args, found, error))
                    return Err(Move(error));
                JsonValue list = JsonValue::MakeArray();
                for (usize i = 0; i < found.Size() && i < kFindLimit; ++i)
                    list.Add(NodeJson(*found[i]));
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"count", JsonValue::MakeNumber(static_cast<f64>(found.Size())));
                out.Set(u8"nodes", Move(list));
                return out;
            });

        constexpr StringView kButtons[] = {u8"left", u8"right", u8"middle"};
        server.RegisterTool(
            u8"editor_click",
            Format(u8"Clicks in the editor's GUI as a user would (the pointer moved there, pressed and released), "
                   u8"through the editor's own input: nothing outside this editor is touched. {} Refused when "
                   u8"the target finds no node or several, or something else lies over its middle. `button` "
                   u8"left (default), right (a context menu) or middle; `clicks` 2 for a double click; "
                   u8"`modifiers` held (\"ctrl\", \"shift\", \"ctrl-shift\"). Answers a frame later with "
                   u8"{{clicked, at, focus}}; then editor_screenshot to see it.",
                   kTargetHelp),
            SchemaBuilder()
                .Str(u8"target", u8"a selector, text=..., or text~=...")
                .Str(u8"within", u8"only under what this finds")
                .Integer(u8"index", u8"which of several matches, from 0")
                .Number(u8"x", u8"a point in the screenshot's pixels, in place of a target")
                .Number(u8"y", u8"a point in the screenshot's pixels, in place of a target")
                .Enum(u8"button", Choices(Span<const StringView>(kButtons, 3)), u8"left (default), right or middle")
                .Integer(u8"clicks", u8"1 (default), 2 for a double click")
                .Str(u8"modifiers", u8"held while clicking: ctrl, shift, alt, super, joined by -")
                .Build(),
            ToolAnnotations::Creates(),
            Acting(shared,
                   [](gui::SceneRoot& root, const JsonValue& args, JsonValue& answer, String& error)
                   {
                       Float2 at;
                       gui::Node* node = nullptr;
                       if (!Aim(root, args, at, node, error))
                           return false;
                       u32 modifiers = 0;
                       if (!ParseModifiers(args.Get(u8"modifiers").AsString().AsView(), modifiers))
                       {
                           error = String(u8"`modifiers` takes ctrl, shift, alt and super, joined by -");
                           return false;
                       }
                       const StringView name = args.Get(u8"button").AsString().AsView();
                       const gui::MouseButton button = name == StringView(u8"right")    ? gui::MouseButton::Right
                                                       : name == StringView(u8"middle") ? gui::MouseButton::Middle
                                                                                        : gui::MouseButton::Left;
                       const i64 clicks = Min<i64>(Max<i64>(args.Get(u8"clicks").AsInt(1), 1), 3);
                       gui::EventDispatcher& d = *root.GetEventDispatcher();
                       (void)d.InjectMouseMove(at, modifiers);
                       for (i64 i = 0; i < clicks; ++i)
                       {
                           (void)d.InjectMouseDown(at, button, modifiers);
                           (void)d.InjectMouseUp(at, button, modifiers);
                       }
                       answer.Set(u8"clicked", node != nullptr ? NodeJson(*node) : JsonValue::MakeNull());
                       answer.Set(u8"at", PointJson(at));
                       return true;
                   }));

        server.RegisterTool(
            u8"editor_hover",
            Format(u8"Moves the editor's pointer over a target or a point, pressing nothing: a hover's look, or a "
                   u8"tooltip after its delay (screenshot a moment later). {} Answers a frame later with {{over, "
                   u8"at, focus}}.",
                   kTargetHelp),
            SchemaBuilder()
                .Str(u8"target", u8"a selector, text=..., or text~=...")
                .Str(u8"within", u8"only under what this finds")
                .Integer(u8"index", u8"which of several matches, from 0")
                .Number(u8"x", u8"a point in the screenshot's pixels, in place of a target")
                .Number(u8"y", u8"a point in the screenshot's pixels, in place of a target")
                .Build(),
            ToolAnnotations::Adjusts(),
            Acting(shared,
                   [](gui::SceneRoot& root, const JsonValue& args, JsonValue& answer, String& error)
                   {
                       Float2 at;
                       gui::Node* node = nullptr;
                       if (!Aim(root, args, at, node, error))
                           return false;
                       (void)root.GetEventDispatcher()->InjectMouseMove(at);
                       answer.Set(u8"over", node != nullptr ? NodeJson(*node) : JsonValue::MakeNull());
                       answer.Set(u8"at", PointJson(at));
                       return true;
                   }));

        server.RegisterTool(
            u8"editor_scroll",
            Format(u8"Turns the wheel over a target or a point in the editor's GUI. `dy` in notches as the "
                   u8"platform reports them: negative scrolls down (the wheel toward the user), positive up; "
                   u8"`dx` sideways. {} Answers a frame later with {{over, at, focus}}.",
                   kTargetHelp),
            SchemaBuilder()
                .Str(u8"target", u8"a selector, text=..., or text~=...")
                .Str(u8"within", u8"only under what this finds")
                .Integer(u8"index", u8"which of several matches, from 0")
                .Number(u8"x", u8"a point in the screenshot's pixels, in place of a target")
                .Number(u8"y", u8"a point in the screenshot's pixels, in place of a target")
                .Number(u8"dy", u8"notches: negative down, positive up", true)
                .Number(u8"dx", u8"notches sideways")
                .Build(),
            ToolAnnotations::Adjusts(),
            Acting(shared,
                   [](gui::SceneRoot& root, const JsonValue& args, JsonValue& answer, String& error)
                   {
                       Float2 at;
                       gui::Node* node = nullptr;
                       if (!Aim(root, args, at, node, error))
                           return false;
                       const Float2 delta{static_cast<f32>(args.Get(u8"dx").AsNumber()), static_cast<f32>(args.Get(u8"dy").AsNumber())};
                       gui::EventDispatcher& d = *root.GetEventDispatcher();
                       (void)d.InjectMouseMove(at);
                       (void)d.InjectMouseWheel(at, delta);
                       answer.Set(u8"over", node != nullptr ? NodeJson(*node) : JsonValue::MakeNull());
                       answer.Set(u8"at", PointJson(at));
                       return true;
                   }));

        server.RegisterTool(
            u8"editor_type",
            u8"Types text into what has the focus in the editor's GUI (editor_click a field first), as the "
            u8"keyboard's text would arrive. A key that does something (Enter, Escape, Tab, a shortcut) is "
            u8"editor_key's. Refused when nothing has the focus. Answers a frame later with {focus}.",
            SchemaBuilder().Str(u8"text", u8"the text to type", true).Build(),
            ToolAnnotations::Creates(),
            Acting(shared,
                   [](gui::SceneRoot& root, const JsonValue& args, JsonValue&, String& error)
                   {
                       gui::EventDispatcher& d = *root.GetEventDispatcher();
                       if (d.GetFocusNode() == nullptr)
                       {
                           error = String(u8"nothing has the focus: editor_click the field first");
                           return false;
                       }
                       (void)d.InjectText(args.Get(u8"text").AsString().AsView());
                       return true;
                   }));

        server.RegisterTool(
            u8"editor_key",
            u8"Presses and releases a key or a chord in the editor's GUI, as the keyboard would: to what has "
            u8"the focus first, then the editor's shortcuts (\"return\", \"escape\", \"tab\", \"down\", "
            u8"\"ctrl-s\", \"ctrl-shift-z\", \"f2\"; the keymap's names). Answers a frame later with {focus}.",
            SchemaBuilder().Str(u8"chord", u8"the key or chord, as a keymap binding writes it", true).Build(),
            ToolAnnotations::Creates(),
            Acting(shared,
                   [](gui::SceneRoot& root, const JsonValue& args, JsonValue& answer, String& error)
                   {
                       const String chord = args.Get(u8"chord").AsString();
                       const Optional<gui::Keystroke> stroke = gui::ParseKeystroke(chord.AsView());
                       if (!stroke.HasValue() || stroke.Value().Pad)
                       {
                           error = Format(u8"no such key or chord: \"{}\" (the keymap's names: ctrl-s, return, f2)", chord);
                           return false;
                       }
                       gui::EventDispatcher& d = *root.GetEventDispatcher();
                       const bool taken = d.InjectKeyDown(stroke.Value().Code, stroke.Value().Modifiers);
                       (void)d.InjectKeyUp(stroke.Value().Code, stroke.Value().Modifiers);
                       answer.Set(u8"handled", JsonValue::MakeBool(taken));
                       return true;
                   }));
    }
}
