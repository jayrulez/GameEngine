// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :keymap partition
//
// Bindings from keys and gamepad buttons to commands (G7 of Specs/vg-and-gui-lab.md), after GPUI's
// keymap (`zed/crates/gpui/src/keymap.rs`, `keymap/context.rs`). A binding names a keystroke or a
// chord ("ctrl-s", "ctrl-k ctrl-c", "escape", "pad-east"), a command (empty: the keystroke is
// unbound there) and, optionally, the contexts it applies in as a small predicate over the key
// contexts the focus path declares ("menu", "editor && mode == insert", "!modal"). Each node on the
// focus path may declare a context (Node::SetKeyContext: "editor mode=insert"); a binding matches at
// the deepest depth whose contexts, root to there, satisfy it. Of the bindings for a keystroke, the
// deepest match wins, then the later binding, so a screen's binding beats the GUI's and a later one
// overrides an earlier.

module;
#include "Core/Prelude.h"

export module experimental.gui:keymap;

import foundation.core; // String, Array, Optional, Span
import :event;          // KeyCode, KeyModifier
import :style_value;    // SplitTopLevel

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// A gamepad's buttons as keystrokes take them (the d-pad moves focus, not through bindings).
    enum class PadButton : u32
    {
        South, // A / Cross: activates when unbound
        East,  // B / Circle
        West,
        North,
        Start,
        Back,
        LeftShoulder,
        RightShoulder,
    };

    /// One key with its modifiers, or one gamepad button.
    struct Keystroke
    {
        u32 Code = 0; ///< a KeyCode, or a PadButton when Pad
        u32 Modifiers = 0;
        bool Pad = false;

        [[nodiscard]] bool operator==(const Keystroke&) const noexcept = default;
    };

    namespace detail
    {
        struct NamedCode
        {
            const char8_t* Name;
            u32 Code;
        };
        inline constexpr NamedCode kKeyNames[] = {
            {u8"enter", static_cast<u32>(KeyCode::Return)},    {u8"return", static_cast<u32>(KeyCode::Return)},
            {u8"escape", static_cast<u32>(KeyCode::Escape)},   {u8"backspace", static_cast<u32>(KeyCode::Backspace)},
            {u8"tab", static_cast<u32>(KeyCode::Tab)},         {u8"space", static_cast<u32>(KeyCode::Space)},
            {u8"delete", static_cast<u32>(KeyCode::Delete)},   {u8"insert", static_cast<u32>(KeyCode::Insert)},
            {u8"home", static_cast<u32>(KeyCode::Home)},       {u8"end", static_cast<u32>(KeyCode::End)},
            {u8"pageup", static_cast<u32>(KeyCode::PageUp)},   {u8"pagedown", static_cast<u32>(KeyCode::PageDown)},
            {u8"left", static_cast<u32>(KeyCode::Left)},       {u8"right", static_cast<u32>(KeyCode::Right)},
            {u8"up", static_cast<u32>(KeyCode::Up)},           {u8"down", static_cast<u32>(KeyCode::Down)},
        };
        inline constexpr NamedCode kPadNames[] = {
            {u8"south", static_cast<u32>(PadButton::South)},   {u8"east", static_cast<u32>(PadButton::East)},
            {u8"west", static_cast<u32>(PadButton::West)},     {u8"north", static_cast<u32>(PadButton::North)},
            {u8"start", static_cast<u32>(PadButton::Start)},   {u8"back", static_cast<u32>(PadButton::Back)},
            {u8"lb", static_cast<u32>(PadButton::LeftShoulder)}, {u8"rb", static_cast<u32>(PadButton::RightShoulder)},
        };
    }

    /// "ctrl-shift-s", "escape", "pad-east": modifiers (ctrl, shift, alt, super) then a key, joined
    /// by '-'; "pad-" names a gamepad button. Empty on an unknown name.
    [[nodiscard]] inline Optional<Keystroke> ParseKeystroke(core::StringView text)
    {
        Keystroke stroke;
        core::StringView rest = Trim(text);
        if (rest.Size() > 4 && rest.SubStr(0, 4) == core::StringView(u8"pad-"))
        {
            const core::StringView name = rest.SubStr(4, rest.Size() - 4);
            for (const detail::NamedCode& pad : detail::kPadNames)
                if (name == core::StringView(pad.Name))
                    return Keystroke{pad.Code, 0, true};
            return {};
        }
        for (;;)
        {
            usize dash = rest.Size();
            for (usize i = 0; i < rest.Size(); ++i)
                if (rest[i] == u8'-')
                {
                    dash = i;
                    break;
                }
            if (dash == rest.Size())
                break;
            const core::StringView modifier = rest.SubStr(0, dash);
            if (modifier == core::StringView(u8"ctrl"))
                stroke.Modifiers |= KeyModCtrl;
            else if (modifier == core::StringView(u8"shift"))
                stroke.Modifiers |= KeyModShift;
            else if (modifier == core::StringView(u8"alt"))
                stroke.Modifiers |= KeyModAlt;
            else if (modifier == core::StringView(u8"super"))
                stroke.Modifiers |= KeyModSuper;
            else
                return {};
            rest = rest.SubStr(dash + 1, rest.Size() - dash - 1);
        }
        for (const detail::NamedCode& key : detail::kKeyNames)
            if (rest == core::StringView(key.Name))
            {
                stroke.Code = key.Code;
                return stroke;
            }
        // A letter, a digit, or f1 to f12.
        if (rest.Size() == 1 && rest[0] >= u8'a' && rest[0] <= u8'z')
        {
            stroke.Code = static_cast<u32>(kLetterKeys[rest[0] - u8'a']);
            return stroke;
        }
        if (rest.Size() == 1 && rest[0] >= u8'0' && rest[0] <= u8'9')
        {
            stroke.Code = static_cast<u32>(KeyCode::Num0) + static_cast<u32>(rest[0] - u8'0');
            return stroke;
        }
        if (rest.Size() >= 2 && rest.Size() <= 3 && rest[0] == u8'f')
        {
            u32 n = 0;
            for (usize i = 1; i < rest.Size(); ++i)
            {
                if (rest[i] < u8'0' || rest[i] > u8'9')
                    return {};
                n = n * 10 + static_cast<u32>(rest[i] - u8'0');
            }
            if (n >= 1 && n <= 12)
            {
                stroke.Code = static_cast<u32>(KeyCode::F1) + n - 1;
                return stroke;
            }
        }
        return {};
    }

    /// What one node's key context declares: identifiers ("menu") and key=value pairs
    /// ("mode=insert"), separated by spaces. A layer of the stack the focus path builds holds the
    /// declarations of every node from the root to there, a deeper value replacing an outer one.
    struct KeyContextLayer
    {
        Array<core::String> Keys;
        Array<core::String> Values; ///< parallel to Keys; empty for a bare identifier

        void Declare(core::StringView declaration)
        {
            for (const core::StringView word : SplitTopLevel(declaration, u8' '))
            {
                core::StringView key = word, value{};
                for (usize i = 0; i < word.Size(); ++i)
                    if (word[i] == u8'=')
                    {
                        key = word.SubStr(0, i);
                        value = word.SubStr(i + 1, word.Size() - i - 1);
                        break;
                    }
                bool replaced = false;
                for (usize k = 0; k < Keys.Size(); ++k)
                    if (Keys[k].AsView() == key)
                    {
                        Values[k] = core::String(value);
                        replaced = true;
                    }
                if (!replaced)
                {
                    Keys.PushBack(core::String(key));
                    Values.PushBack(core::String(value));
                }
            }
        }
        [[nodiscard]] const core::String* Find(core::StringView key) const
        {
            for (usize k = 0; k < Keys.Size(); ++k)
                if (Keys[k].AsView() == key)
                    return &Values[k];
            return nullptr;
        }
    };

    /// A context predicate: identifiers, `a == b`, `a != b`, `!p`, `p && q`, `p || q`, parentheses.
    class ContextPredicate
    {
    public:
        /// Parses `text`; false (and matching nothing) when it does not parse. Empty text matches
        /// everywhere.
        bool Parse(core::StringView text)
        {
            m_nodes.Clear();
            m_tokens.Clear();
            m_root = -1;
            if (Trim(text).Size() == 0)
                return true;
            if (!Tokenize(text))
                return false;
            usize at = 0;
            m_root = ParseOr(at);
            if (m_root < 0 || at != m_tokens.Size())
            {
                m_root = -2; // malformed: matches nothing
                return false;
            }
            return true;
        }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_root == -1; }
        [[nodiscard]] bool Eval(const KeyContextLayer& layer) const
        {
            if (m_root == -1)
                return true;
            return m_root >= 0 && EvalNode(m_root, layer);
        }

    private:
        enum class Op : u8
        {
            Identifier,
            Equal,
            NotEqual,
            Not,
            And,
            Or,
        };
        struct PNode
        {
            Op Kind;
            core::String A, B; // an identifier; or the key and value of a comparison
            i32 Left = -1, Right = -1;
        };
        Array<PNode> m_nodes;
        Array<core::String> m_tokens;
        i32 m_root = -1;

        bool Tokenize(core::StringView text)
        {
            for (usize i = 0; i < text.Size();)
            {
                const char8_t c = text[i];
                if (c == u8' ' || c == u8'\t')
                {
                    ++i;
                    continue;
                }
                const auto two = [&](const char8_t* op) { return i + 1 < text.Size() && text[i] == op[0] && text[i + 1] == op[1]; };
                if (two(u8"&&") || two(u8"||") || two(u8"==") || two(u8"!="))
                {
                    m_tokens.PushBack(core::String(text.SubStr(i, 2)));
                    i += 2;
                    continue;
                }
                if (c == u8'!' || c == u8'(' || c == u8')')
                {
                    m_tokens.PushBack(core::String(text.SubStr(i, 1)));
                    ++i;
                    continue;
                }
                const usize start = i;
                while (i < text.Size() &&
                       ((text[i] >= u8'a' && text[i] <= u8'z') || (text[i] >= u8'A' && text[i] <= u8'Z') ||
                        (text[i] >= u8'0' && text[i] <= u8'9') || text[i] == u8'_' || text[i] == u8'-' || text[i] == u8'.'))
                    ++i;
                if (i == start)
                    return false;
                m_tokens.PushBack(core::String(text.SubStr(start, i - start)));
            }
            return true;
        }
        [[nodiscard]] bool Is(usize at, const char8_t* token) const
        {
            return at < m_tokens.Size() && m_tokens[at].AsView() == core::StringView(token);
        }
        i32 Add(PNode node)
        {
            m_nodes.PushBack(Move(node));
            return static_cast<i32>(m_nodes.Size() - 1);
        }
        i32 ParseOr(usize& at)
        {
            i32 left = ParseAnd(at);
            while (left >= 0 && Is(at, u8"||"))
            {
                ++at;
                const i32 right = ParseAnd(at);
                if (right < 0)
                    return -1;
                left = Add(PNode{Op::Or, {}, {}, left, right});
            }
            return left;
        }
        i32 ParseAnd(usize& at)
        {
            i32 left = ParseUnary(at);
            while (left >= 0 && Is(at, u8"&&"))
            {
                ++at;
                const i32 right = ParseUnary(at);
                if (right < 0)
                    return -1;
                left = Add(PNode{Op::And, {}, {}, left, right});
            }
            return left;
        }
        i32 ParseUnary(usize& at)
        {
            if (Is(at, u8"!"))
            {
                ++at;
                const i32 inner = ParseUnary(at);
                return inner < 0 ? -1 : Add(PNode{Op::Not, {}, {}, inner, -1});
            }
            if (Is(at, u8"("))
            {
                ++at;
                const i32 inner = ParseOr(at);
                if (inner < 0 || !Is(at, u8")"))
                    return -1;
                ++at;
                return inner;
            }
            if (at >= m_tokens.Size() || Is(at, u8")") || Is(at, u8"&&") || Is(at, u8"||") || Is(at, u8"==") || Is(at, u8"!="))
                return -1;
            core::String name = m_tokens[at++];
            if (Is(at, u8"==") || Is(at, u8"!="))
            {
                const Op op = Is(at, u8"==") ? Op::Equal : Op::NotEqual;
                ++at;
                if (at >= m_tokens.Size())
                    return -1;
                return Add(PNode{op, Move(name), m_tokens[at++], -1, -1});
            }
            return Add(PNode{Op::Identifier, Move(name), {}, -1, -1});
        }
        [[nodiscard]] bool EvalNode(i32 index, const KeyContextLayer& layer) const
        {
            const PNode& n = m_nodes[static_cast<usize>(index)];
            switch (n.Kind)
            {
            case Op::Identifier: return layer.Find(n.A.AsView()) != nullptr;
            case Op::Equal:
            {
                const core::String* v = layer.Find(n.A.AsView());
                return v != nullptr && v->AsView() == n.B.AsView();
            }
            case Op::NotEqual:
            {
                const core::String* v = layer.Find(n.A.AsView());
                return v == nullptr || v->AsView() != n.B.AsView();
            }
            case Op::Not: return !EvalNode(n.Left, layer);
            case Op::And: return EvalNode(n.Left, layer) && EvalNode(n.Right, layer);
            case Op::Or: return EvalNode(n.Left, layer) || EvalNode(n.Right, layer);
            }
            return false;
        }
    };

    enum class KeymapMatch : u8
    {
        None,    ///< no binding: the keystroke goes on (to navigation)
        Pending, ///< the start of a chord: wait for the next keystroke
        Matched, ///< a command to run (or an unbinding: an empty command)
    };

    class Keymap
    {
    public:
        /// Binds `keys` (a keystroke, or a chord of them separated by spaces) to `command` where
        /// `context` holds (empty: everywhere); an empty command unbinds the keys there. False when
        /// the keys or the context do not parse.
        bool Bind(core::StringView keys, core::StringView command, core::StringView context = {})
        {
            KeyBinding binding;
            for (const core::StringView part : SplitTopLevel(keys, u8' '))
            {
                const Optional<Keystroke> stroke = ParseKeystroke(part);
                if (!stroke.HasValue())
                    return false;
                binding.Sequence.PushBack(stroke.Value());
            }
            if (binding.Sequence.IsEmpty() || !binding.Context.Parse(context))
                return false;
            binding.Command = core::String(command);
            m_bindings.PushBack(Move(binding));
            return true;
        }
        /// Binds one keystroke (made, not parsed: from an input map) to `command` where `context`
        /// holds; false when the context does not parse.
        bool Bind(const Keystroke& stroke, core::StringView command, core::StringView context = {})
        {
            KeyBinding binding;
            binding.Sequence.PushBack(stroke);
            if (!binding.Context.Parse(context))
                return false;
            binding.Command = core::String(command);
            m_bindings.PushBack(Move(binding));
            return true;
        }
        [[nodiscard]] usize BindingCount() const noexcept { return m_bindings.Size(); }

        /// Matches what was typed (a chord so far) against the bindings in the context `stack`
        /// (root first). On Matched, `command` is the winner's (empty for an unbinding).
        [[nodiscard]] KeymapMatch Match(Span<const Keystroke> typed, Span<const KeyContextLayer> stack,
                                        core::String& command) const
        {
            i32 bestDepth = -2;
            const KeyBinding* best = nullptr;
            bool pending = false;
            for (const KeyBinding& binding : m_bindings)
            {
                if (binding.Sequence.Size() < typed.Size())
                    continue;
                bool prefix = true;
                for (usize i = 0; i < typed.Size(); ++i)
                    if (!(binding.Sequence[i] == typed[i]))
                    {
                        prefix = false;
                        break;
                    }
                if (!prefix)
                    continue;
                const i32 depth = MatchDepth(binding, stack);
                if (depth < -1)
                    continue; // its context holds nowhere on the focus path
                if (binding.Sequence.Size() > typed.Size())
                {
                    pending = true;
                    continue;
                }
                if (depth >= bestDepth) // deeper wins; at a tie, the later binding
                {
                    bestDepth = depth;
                    best = &binding;
                }
            }
            if (pending && (best == nullptr || bestDepth < 0))
                return KeymapMatch::Pending; // a chord may follow (a contextual full match still wins)
            if (best == nullptr)
                return KeymapMatch::None;
            command = best->Command;
            return KeymapMatch::Matched;
        }

    private:
        struct KeyBinding
        {
            Array<Keystroke> Sequence;
            core::String Command;
            ContextPredicate Context;
        };
        Array<KeyBinding> m_bindings;

        // The deepest layer whose contexts satisfy the binding's (-1 for one with no context: the
        // least specific; -2 when none does).
        [[nodiscard]] static i32 MatchDepth(const KeyBinding& binding, Span<const KeyContextLayer> stack)
        {
            if (binding.Context.IsEmpty())
                return -1;
            for (usize d = stack.Size(); d-- > 0;)
                if (binding.Context.Eval(stack[d]))
                    return static_cast<i32>(d);
            return -2;
        }
    };
}
