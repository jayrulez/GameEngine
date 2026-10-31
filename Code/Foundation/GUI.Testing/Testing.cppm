// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.gui.testing - helpers for tests of GUIs built with the GUI.
//
// A Finder picks out nodes by CSS selector (with the GUI's own selector engine) or by their text
// (a widget's reflected `text`; of a widget and its parts, the outermost is found). It can search
// within another finder's match, and finds only shown nodes unless asked. A Tester drives one root
// on a fixed clock: every action is followed by a frame (update, style, layout), so a test reads
// what a user would see. Its actions are a user's: click, right click, double click, hover, drag,
// type, a key chord, a held direction, a pad button, files dropped. Each aims at the middle of
// what it found, and fails when something else lies there. A finder that expects one node and
// finds none or several fails, listing what it found and the tree around where it looked.
// Failures go to a handler (a test's FAIL_CHECK) and are kept. CompareImages is the comparison
// the image tests use: two pictures match when every channel is within a tolerance on all but a
// small share of the pixels, and it can draw a picture of where they differ.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.testing;

import foundation.core;
import foundation.image;
import foundation.gui;
import foundation.gui.inspector;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui::testing
{
    /// Returns the text a node shows: its reflected `text` (on a label, a button or a field), or
    /// empty.
    [[nodiscard]] inline core::String TextOf(const Node& node)
    {
        for (const core::TypeInfo* t = node.GetType(); t != nullptr; t = t->base)
            for (usize i = 0; i < core::PropertyCount(*t); ++i)
            {
                const core::PropertyInfo& property = core::PropertyAt(*t, i);
                if (core::StringView(reinterpret_cast<const char8_t*>(property.name)) != core::StringView(u8"text"))
                    continue;
                const core::Variant value = ConvertVariant(core::GetProperty(property, core::Instance::From(const_cast<Node*>(&node))),
                                                           core::TypeOf<core::String>());
                const core::String* text = value.TryGet<core::String>();
                return text != nullptr ? *text : core::String{};
            }
        return {};
    }

    /// Sets `at` to the middle of `node` in `root`'s world, in device pixels as the dispatcher
    /// takes a pointer, and `over` to what a click there would hit (null for nothing). Returns
    /// whether that is the node or a part of it (a button's label).
    [[nodiscard]] inline bool AimAt(SceneRoot& root, Node& node, core::Float2& at, Node*& over)
    {
        const Rect box = node.GetScreenBounds();
        at = core::Float2{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
        // Hit-test as input does: an open popup first (a menu item, a dialog over a panel), then
        // the root.
        EventDispatcher* dispatcher = root.GetEventDispatcher();
        Node* hit = dispatcher != nullptr ? dispatcher->NodeAt(at) : root.OverFind(at);
        over = hit;
        for (Node* n = hit; n != nullptr; n = n->GetParent())
            if (n == &node)
                return true;
        for (Node* n = &node; hit != nullptr && n != nullptr; n = n->GetParent())
            if (n == hit && hit != &root)
                return true;
        return false;
    }

    /// Describes which nodes a test means: by selector or text, optionally within another
    /// finder's match, and only shown nodes unless IncludingHidden is used. A finder is a list of
    /// steps, outermost first; each step looks under the first node the step before it found.
    class Finder
    {
    public:
        /// Finds the widgets a CSS selector matches ("#ok", "button.primary", "panel > label").
        [[nodiscard]] static Finder Selector(core::StringView selector) { return Finder(Kind::Selector, selector); }
        /// Finds the nodes showing exactly `text`. Of a widget and the parts that show the text,
        /// only the outermost is found.
        [[nodiscard]] static Finder Text(core::StringView text) { return Finder(Kind::Text, text); }
        /// Finds the nodes whose text contains `text`.
        [[nodiscard]] static Finder TextContaining(core::StringView text) { return Finder(Kind::TextContaining, text); }
        /// Parses a finder: "text=Show" or "text~=Sho" finds by text, and anything else is a
        /// selector. The sandbox's --keys option uses this.
        [[nodiscard]] static Finder Parse(core::StringView spec)
        {
            const core::StringView exact(u8"text=");
            const core::StringView containing(u8"text~=");
            if (spec.StartsWith(exact))
                return Text(spec.SubStr(exact.Size(), spec.Size() - exact.Size()));
            if (spec.StartsWith(containing))
                return TextContaining(spec.SubStr(containing.Size(), spec.Size() - containing.Size()));
            return Selector(spec);
        }

        /// Limits the search to the node `scope` finds (the first one, when it finds several).
        [[nodiscard]] Finder Within(const Finder& scope) const
        {
            Finder f = scope;
            for (const Step& step : m_steps)
                f.m_steps.PushBack(step);
            return f;
        }
        /// Also finds hidden nodes: those not shown, or under a node that is not shown or is out of
        /// the layout.
        [[nodiscard]] Finder IncludingHidden() const
        {
            Finder f = *this;
            f.m_steps[f.m_steps.Size() - 1].Hidden = true;
            return f;
        }

        /// Returns what the finder finds under `root`, in document order.
        [[nodiscard]] Array<Node*> All(Node& root) const
        {
            Node* scope = &root;
            Array<Node*> found;
            for (usize i = 0; i < m_steps.Size(); ++i)
            {
                found = Run(m_steps[i], *scope);
                if (i + 1 < m_steps.Size())
                {
                    if (found.IsEmpty())
                        return {};
                    scope = found[0];
                }
            }
            return found;
        }

        /// Describes the finder for a failure message ("selector #ok within text=Settings").
        [[nodiscard]] core::String Describe() const
        {
            core::String out;
            for (usize i = m_steps.Size(); i-- > 0;)
            {
                const Step& step = m_steps[i];
                out += core::StringView(step.What == Kind::Selector ? u8"selector " : step.What == Kind::Text ? u8"text=" : u8"text~=");
                out += step.Pattern.AsView();
                if (step.Hidden)
                    out += core::StringView(u8" (hidden too)");
                if (i > 0)
                    out += core::StringView(u8" within ");
            }
            return out;
        }

    private:
        enum class Kind : u8
        {
            Selector,
            Text,
            TextContaining,
        };
        struct Step
        {
            Kind What = Kind::Selector;
            core::String Pattern;
            bool Hidden = false;
        };
        Finder(Kind kind, core::StringView pattern) { m_steps.PushBack(Step{kind, core::String(pattern), false}); }

        [[nodiscard]] static bool Holds(core::StringView text, core::StringView part)
        {
            for (usize i = 0; i + part.Size() <= text.Size(); ++i)
                if (text.SubStr(i, part.Size()) == part)
                    return true;
            return false;
        }
        [[nodiscard]] static Array<Node*> Run(const Step& step, Node& scope)
        {
            Array<Node*> out;
            if (step.What == Kind::Selector)
                out = inspector::Query(scope, step.Pattern.AsView());
            else
            {
                const auto walk = [&](auto& self, Node& node, bool matchedAbove) -> void
                {
                    const core::String text = TextOf(node);
                    const bool match = step.What == Kind::Text
                                           ? text.AsView() == step.Pattern.AsView()
                                           : text.Size() != 0 && Holds(text.AsView(), step.Pattern.AsView());
                    if (match && !matchedAbove)
                        out.PushBack(&node);
                    for (usize i = 0; i < node.ChildCount(); ++i)
                        self(self, *node.GetChildAt(i), matchedAbove || match);
                };
                walk(walk, scope, false);
            }
            if (!step.Hidden)
                for (usize i = out.Size(); i-- > 0;)
                    if (!out[i]->IsTreeVisible())
                        out.RemoveAt(i);
            return out;
        }

        Array<Step> m_steps;
    };

    /// Drives one root as a user would, a frame after every action on a fixed clock.
    class Tester
    {
    public:
        /// `styles` styles the root each frame; pass null when the root is styled elsewhere or not
        /// at all.
        explicit Tester(SceneRoot& root, StyleManager* styles = nullptr, core::Duration frame = core::Duration::FromSeconds(1.0 / 60.0))
            : m_root(root), m_styles(styles), m_frame(frame)
        {
            Frame();
        }

        /// Sets a handler that gets each failure as well as Failures; a test passes doctest's
        /// FAIL_CHECK.
        void SetFailureHandler(core::Function<void(core::StringView)> handler) { m_onFailure = core::Move(handler); }
        /// Sets what saves the screen when a failure happens, given a name for the picture
        /// (`failure-1`, ...). A host with a device (the sandbox's image tests) writes the frame;
        /// without a capture nothing is saved.
        void SetFailureCapture(core::Function<void(core::StringView)> capture) { m_capture = core::Move(capture); }
        [[nodiscard]] const Array<core::String>& Failures() const noexcept { return m_failures; }

        // === Time ===
        /// Runs one frame: the clock moves by the frame's length, then style and layout run.
        void Frame()
        {
            m_root.GetContext().Update(m_frame);
            if (m_styles != nullptr)
                m_styles->ApplyTree(m_root);
            m_root.UpdateLayout();
        }
        /// Runs frames until `time` has passed, so an animation runs or a held key repeats.
        void Advance(core::Duration time)
        {
            const f64 frames = time.AsSecondsF() / m_frame.AsSecondsF();
            for (i64 i = 0; i < static_cast<i64>(frames + 0.5); ++i)
                Frame();
        }

        // === Finding ===
        [[nodiscard]] Array<Node*> All(const Finder& finder) { return finder.All(m_root); }
        [[nodiscard]] bool Exists(const Finder& finder) { return !finder.All(m_root).IsEmpty(); }
        /// Returns the one node `finder` finds. Records a failure and returns null when it finds
        /// none or several.
        Node* One(const Finder& finder)
        {
            const Array<Node*> found = finder.All(m_root);
            if (found.Size() == 1)
                return found[0];
            core::String message = core::Format(u8"{} found {} nodes, expected one", finder.Describe(), found.Size());
            for (Node* n : found)
            {
                message += core::StringView(u8"\n  found: ");
                message += inspector::Label(*n).AsView();
            }
            message += core::StringView(u8"\n");
            message += Dump().AsView();
            Fail(message.AsView());
            return nullptr;
        }

        // === The user's actions (each followed by a frame) ===
        bool Click(const Finder& target) { return Press(target, MouseButton::Left, 1); }
        bool RightClick(const Finder& target) { return Press(target, MouseButton::Right, 1); }
        bool DoubleClick(const Finder& target) { return Press(target, MouseButton::Left, 2); }
        /// Moves the pointer over the middle of what `target` finds.
        bool Hover(const Finder& target)
        {
            core::Float2 at;
            if (!Aim(target, at))
                return false;
            (void)Dispatcher().InjectMouseMove(at);
            Frame();
            return true;
        }
        /// Moves the pointer out of the root, so the node it hovered is left.
        void Leave()
        {
            (void)Dispatcher().InjectMouseMove(core::Float2{-1.0f, -1.0f});
            Frame();
        }
        /// Turns the wheel over the middle of what `target` finds by `delta` notches, as the
        /// platform reports them. A negative y scrolls down (the wheel turned toward the user).
        bool Scroll(const Finder& target, core::Float2 delta, u32 modifiers = 0)
        {
            core::Float2 at;
            if (!Aim(target, at))
                return false;
            (void)Dispatcher().InjectMouseMove(at);
            (void)Dispatcher().InjectMouseWheel(at, delta, modifiers);
            Frame();
            return true;
        }
        /// Puts a finger down and lifts it on the middle of what `target` finds.
        bool Tap(const Finder& target)
        {
            core::Float2 at;
            if (!Aim(target, at))
                return false;
            EventDispatcher& d = Dispatcher();
            (void)d.InjectTouchDown(kFinger, at);
            Frame();
            (void)d.InjectTouchUp(kFinger, at);
            Frame();
            return true;
        }
        /// Puts a finger down on `from`'s middle, moves it by `by` in steps and lifts it there. The
        /// swipe pans what it is over, and the steps are fast enough to fling it.
        bool Swipe(const Finder& from, core::Float2 by)
        {
            core::Float2 start;
            if (!Aim(from, start))
                return false;
            EventDispatcher& d = Dispatcher();
            (void)d.InjectTouchDown(kFinger, start);
            Frame();
            constexpr int kSteps = 8;
            for (int i = 1; i <= kSteps; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(kSteps);
                (void)d.InjectTouchMove(kFinger, core::Float2{start.x + by.x * t, start.y + by.y * t});
                Frame();
            }
            (void)d.InjectTouchUp(kFinger, core::Float2{start.x + by.x, start.y + by.y});
            Frame();
            return true;
        }
        /// Puts two fingers `span` apart across `target`'s middle, pinches them to `toSpan` apart in
        /// steps and lifts them. It is a gesture where the target's touch-action accepts one.
        bool Pinch(const Finder& target, f32 span, f32 toSpan) { return TwoFingers(target, span, toSpan, 0.0f, core::Float2{0.0f, 0.0f}); }
        /// Turns two fingers on `target`'s middle by `radians` about it, clockwise, in steps.
        bool Rotate(const Finder& target, f32 radians, f32 span = 100.0f) { return TwoFingers(target, span, span, radians, core::Float2{0.0f, 0.0f}); }
        /// Moves two fingers on `target`'s middle together by `by` (device pixels), in steps.
        bool TwoFingerPan(const Finder& target, core::Float2 by, f32 span = 100.0f) { return TwoFingers(target, span, span, 0.0f, by); }
        /// Presses on `from`, moves in steps to `to`'s middle and releases there.
        bool DragTo(const Finder& from, const Finder& to)
        {
            core::Float2 end;
            Node* target = One(to);
            if (target == nullptr)
                return false;
            const Rect box = target->GetScreenBounds();
            end = core::Float2{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
            return DragTo(from, end);
        }
        /// Presses on `from`, moves in steps to `to` (in the root's world) and releases there.
        bool DragTo(const Finder& from, core::Float2 to)
        {
            core::Float2 start;
            if (!Aim(from, start))
                return false;
            EventDispatcher& d = Dispatcher();
            (void)d.InjectMouseMove(start);
            (void)d.InjectMouseDown(start, MouseButton::Left);
            Frame();
            constexpr int kSteps = 8;
            for (int i = 1; i <= kSteps; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(kSteps);
                (void)d.InjectMouseMove(core::Float2{start.x + (to.x - start.x) * t, start.y + (to.y - start.y) * t});
                Frame();
            }
            (void)d.InjectMouseUp(to, MouseButton::Left);
            Frame();
            return true;
        }
        /// Types `text` into the focused node.
        void Type(core::StringView text)
        {
            (void)Dispatcher().InjectText(text);
            Frame();
        }
        /// Presses a key chord ("return", "ctrl-a", "shift-tab", "pad-south"). Returns false for
        /// a chord it does not know.
        bool Press(core::StringView chord)
        {
            const Optional<Keystroke> stroke = ParseKeystroke(chord);
            if (!stroke.HasValue())
            {
                Fail(core::Format(u8"no such key chord: {}", chord).AsView());
                return false;
            }
            if (stroke.Value().Pad)
                Dispatcher().InjectPadButton(static_cast<PadButton>(stroke.Value().Code));
            else
            {
                (void)Dispatcher().InjectKeyDown(stroke.Value().Code, stroke.Value().Modifiers);
                (void)Dispatcher().InjectKeyUp(stroke.Value().Code, stroke.Value().Modifiers);
            }
            Frame();
            return true;
        }
        /// Holds an arrow, the d-pad or the stick one way for `time`; it repeats as it does for a
        /// user.
        void HoldDirection(NavDirection direction, core::Duration time)
        {
            Dispatcher().PressDirection(direction);
            Frame();
            Advance(time);
            Dispatcher().ReleaseDirection(direction);
            Frame();
        }
        /// Presses a pad button.
        void PadPress(PadButton button)
        {
            Dispatcher().InjectPadButton(button);
            Frame();
        }
        /// Drops files, as the OS does, on the middle of what `target` finds.
        bool DropFiles(const Finder& target, const Array<core::String>& paths)
        {
            core::Float2 at;
            if (!Aim(target, at))
                return false;
            (void)Dispatcher().InjectFileDrop(at, paths);
            Frame();
            return true;
        }

        /// Returns the tree under `around` (the root when null), one node a line, indented by
        /// depth.
        [[nodiscard]] core::String Dump(Node* around = nullptr)
        {
            core::String out;
            for (const inspector::TreeEntry& e : inspector::Tree(around != nullptr ? *around : static_cast<Node&>(m_root)))
            {
                for (u32 d = 0; d < e.Depth; ++d)
                    out += core::StringView(u8"  ");
                out += e.Label.AsView();
                if (const core::String text = TextOf(*e.Target); text.Size() != 0)
                {
                    out += core::StringView(u8" \"");
                    out += text.AsView();
                    out += core::StringView(u8"\"");
                }
                if (!e.Shown)
                    out += core::StringView(u8" (hidden)");
                out += core::StringView(u8"\n");
            }
            return out;
        }

    private:
        [[nodiscard]] EventDispatcher& Dispatcher() { return *m_root.GetEventDispatcher(); }
        void Fail(core::StringView message)
        {
            m_failures.PushBack(core::String(message));
            if (m_onFailure)
                m_onFailure(message);
            if (m_capture)
                m_capture(core::Format(u8"failure-{}", m_failures.Size()).AsView());
        }
        // Finds the middle of the one node `target` finds. Fails when a pointer there would hit
        // something covering the node instead.
        bool Aim(const Finder& target, core::Float2& at)
        {
            Node* node = One(target);
            if (node == nullptr)
                return false;
            Node* over = nullptr;
            if (AimAt(m_root, *node, at, over))
                return true;
            Fail(core::Format(u8"{} is covered at its middle by {}", target.Describe(),
                              over != nullptr ? inspector::Label(*over) : core::String(core::StringView(u8"nothing"))).AsView());
            return false;
        }
        bool Press(const Finder& target, MouseButton button, int clicks)
        {
            core::Float2 at;
            if (!Aim(target, at))
                return false;
            EventDispatcher& d = Dispatcher();
            (void)d.InjectMouseMove(at);
            for (int i = 0; i < clicks; ++i)
            {
                (void)d.InjectMouseDown(at, button);
                (void)d.InjectMouseUp(at, button);
                Frame();
            }
            return true;
        }

        SceneRoot& m_root;
        StyleManager* m_styles = nullptr;
        core::Duration m_frame;
        core::Function<void(core::StringView)> m_onFailure;
        core::Function<void(core::StringView)> m_capture;
        Array<core::String> m_failures;
        static constexpr u64 kFinger = 1; // the tester's one finger
        static constexpr u64 kSecondFinger = 2;
        // Moves two fingers about `target`'s middle from `span` apart to `toSpan` apart, while the
        // line between them turns by `turn` and their midpoint moves by `by`. It takes eight
        // steps, with a frame after each.
        bool TwoFingers(const Finder& target, f32 span, f32 toSpan, f32 turn, core::Float2 by)
        {
            core::Float2 middle;
            if (!Aim(target, middle))
                return false;
            const auto place = [&](f32 t, f32 side)
            {
                const f32 half = (span + (toSpan - span) * t) * 0.5f * side;
                const f32 angle = turn * t;
                return core::Float2{middle.x + by.x * t + half * core::Cos(angle), middle.y + by.y * t + half * core::Sin(angle)};
            };
            EventDispatcher& d = Dispatcher();
            (void)d.InjectTouchDown(kFinger, place(0.0f, -1.0f));
            (void)d.InjectTouchDown(kSecondFinger, place(0.0f, 1.0f));
            Frame();
            constexpr int kSteps = 8;
            for (int i = 1; i <= kSteps; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(kSteps);
                (void)d.InjectTouchMove(kFinger, place(t, -1.0f));
                (void)d.InjectTouchMove(kSecondFinger, place(t, 1.0f));
                Frame();
            }
            (void)d.InjectTouchUp(kSecondFinger, place(1.0f, 1.0f));
            (void)d.InjectTouchUp(kFinger, place(1.0f, -1.0f));
            Frame();
            return true;
        }
    };

    /// How a picture compares with its reference.
    struct ImageComparison
    {
        bool SameSize = false;
        u64 Differing = 0;   ///< pixels with a channel past the tolerance
        u64 Pixels = 0;
        u32 LargestDelta = 0; ///< the largest channel difference anywhere
        bool Matches = false;
    };

    /// Compares `picture` with `reference` (both RGBA8): they match when they are the same size
    /// and every channel is within `tolerance` on all but `allowedShare` of the pixels. `diff`,
    /// when given, gets a picture of the difference: the reference faded to grey, the differing
    /// pixels red.
    [[nodiscard]] inline ImageComparison CompareImages(const foundation::image::Image& picture, const foundation::image::Image& reference,
                                                       u32 tolerance = 8, f64 allowedShare = 0.001,
                                                       foundation::image::Image* diff = nullptr)
    {
        namespace image = foundation::image;
        ImageComparison out;
        out.SameSize = picture.Width() == reference.Width() && picture.Height() == reference.Height() &&
                       picture.Format() == image::PixelFormat::RGBA8 && reference.Format() == image::PixelFormat::RGBA8;
        if (!out.SameSize)
            return out;
        out.Pixels = static_cast<u64>(picture.Width()) * picture.Height();
        const Span<const u8> a = picture.PixelData();
        const Span<const u8> b = reference.PixelData();
        Array<u8> picturedDiff;
        if (diff != nullptr)
            picturedDiff.Resize(a.Size());
        for (usize i = 0; i + 3 < a.Size(); i += 4)
        {
            u32 delta = 0;
            for (usize c = 0; c < 4; ++c)
            {
                const i32 d = static_cast<i32>(a[i + c]) - static_cast<i32>(b[i + c]);
                delta = core::Max(delta, static_cast<u32>(d < 0 ? -d : d));
            }
            out.LargestDelta = core::Max(out.LargestDelta, delta);
            const bool differs = delta > tolerance;
            out.Differing += differs ? 1u : 0u;
            if (diff != nullptr)
            {
                const u8 grey = static_cast<u8>((static_cast<u32>(b[i]) + b[i + 1] + b[i + 2]) / 9u + 40u);
                picturedDiff[i] = differs ? u8{255} : grey;
                picturedDiff[i + 1] = differs ? u8{0} : grey;
                picturedDiff[i + 2] = differs ? u8{0} : grey;
                picturedDiff[i + 3] = 255;
            }
        }
        out.Matches = static_cast<f64>(out.Differing) <= allowedShare * static_cast<f64>(out.Pixels);
        if (diff != nullptr)
            *diff = image::Image(picture.Width(), picture.Height(), image::PixelFormat::RGBA8,
                                 Span<const u8>{picturedDiff.Data(), picturedDiff.Size()});
        return out;
    }
}
