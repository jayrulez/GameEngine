// Draconic::Shell - `:surface` partition.
//
// InputSurface + InputRouter: the viewport-input layer that sits above the raw
// shell devices (see docs/design/viewport-input.md §4).
//
//   InputSurface  - a rectangular slice of a window (a `ContentFit`) that presents
//                   the SAME device interfaces (IMouse/IKeyboard/IGamepad/ITouch)
//                   as the shell, but TRANSFORMED into the surface's content
//                   space and GATED by whether the surface is hovered/focused.
//                   Drop-in: any code written against IMouse works unchanged when
//                   handed a surface's mouse instead of the shell's.
//
//   InputRouter   - the single owner of hover/focus/capture across a set of
//                   surfaces. Once per frame (after the shell pumps events) it
//                   reads the raw pointer + event stream and updates every surface's
//                   gate. Exactly one surface is hovered, one focused, one captures
//                   the pointer while a button is held.
//
// Event-first: the raw device snapshots the surface reads are themselves a fold
// over IInputManager::Events() (phase 1), so polling a surface is polling the
// event stream through a transform. The router consults HoverWindow()/the event
// stream for its routing decisions.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell:surface;

import draconic.foundation;
import :input;
import :input_types;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    class InputSurface; // defined below; facades hold a back-pointer.

    // --- transformed + gated device facades -------------------------------------
    // Each wraps its owning surface. Method bodies are defined out-of-line once
    // InputSurface is complete.

    class SurfaceMouse final : public IMouse
    {
    public:
        explicit SurfaceMouse(InputSurface* s) noexcept : m_s(s) {}

        [[nodiscard]] foundation::f32 X() const override; // content-space
        [[nodiscard]] foundation::f32 Y() const override;
        [[nodiscard]] foundation::f32
        GlobalX() const override; // desktop-global, untransformed pass-through
        [[nodiscard]] foundation::f32 GlobalY() const override;
        [[nodiscard]] foundation::f32 DeltaX() const override; // content-space, gated
        [[nodiscard]] foundation::f32 DeltaY() const override;
        [[nodiscard]] foundation::f32 ScrollX() const override;
        [[nodiscard]] foundation::f32 ScrollY() const override;

        [[nodiscard]] bool IsButtonDown(MouseButton b) const override;
        [[nodiscard]] bool IsButtonPressed(MouseButton b) const override;
        [[nodiscard]] bool IsButtonReleased(MouseButton b) const override;

        // Cursor/relative state is global device state - pass through to the raw
        // mouse (the active surface drives it).
        [[nodiscard]] bool RelativeMode() const override;
        void SetRelativeMode(bool enabled) override;
        [[nodiscard]] bool CursorVisible() const override;
        void SetCursorVisible(bool visible) override;
        void SetCursor(CursorType cursor) override;
        void SetGlobalCapture(bool enabled) override;

    private:
        InputSurface* m_s;
    };

    class SurfaceKeyboard final : public IKeyboard
    {
    public:
        explicit SurfaceKeyboard(InputSurface* s) noexcept : m_s(s) {}

        [[nodiscard]] bool IsKeyDown(KeyCode k) const override;
        [[nodiscard]] bool IsKeyPressed(KeyCode k) const override;
        [[nodiscard]] bool IsKeyReleased(KeyCode k) const override;
        [[nodiscard]] KeyModifiers Modifiers() const override;

    private:
        InputSurface* m_s;
    };

    // Wraps one raw gamepad (by index), gated by the surface's keyboard focus.
    class SurfaceGamepad final : public IGamepad
    {
    public:
        SurfaceGamepad() noexcept = default;
        void Bind(InputSurface* s, foundation::i32 index) noexcept
        {
            m_s = s;
            m_index = index;
        }

        [[nodiscard]] foundation::i32 Index() const override;
        [[nodiscard]] foundation::StringView Name() const override;
        [[nodiscard]] bool Connected() const override;

        [[nodiscard]] bool IsButtonDown(GamepadButton b) const override;
        [[nodiscard]] bool IsButtonPressed(GamepadButton b) const override;
        [[nodiscard]] bool IsButtonReleased(GamepadButton b) const override;
        [[nodiscard]] foundation::f32 Axis(GamepadAxis a) const override;

        void SetRumble(foundation::f32 lo, foundation::f32 hi, foundation::u32 durationMs) override;

    private:
        [[nodiscard]] IGamepad* Raw() const; // resolves raw->GetGamepad(m_index)
        InputSurface* m_s = nullptr;
        foundation::i32 m_index = 0;
    };

    // Touch, transformed + spatially gated: raw finger coords (NORMALIZED window space,
    // SDL semantics) map through the surface's ContentFit into CONTENT-NORMALIZED [0,1]
    // points; fingers outside the drawn content (other panels, letterbox bars) are
    // FILTERED OUT entirely. The REGION is the gate - unlike mouse/keyboard, touch does
    // not follow hover (a touch-only device never moves the hover pointer), so a finger
    // in the rect is the surface's regardless of pointer state. Needs SetWindowSize
    // (pushed with the region each frame) to convert normalized->pixel space.
    // is mouse/keyboard).
    class SurfaceTouch final : public ITouch
    {
    public:
        explicit SurfaceTouch(InputSurface* s) noexcept : m_s(s) {}

        [[nodiscard]] foundation::i32 TouchCount() const override;
        [[nodiscard]] bool GetTouchPoint(foundation::i32 index, TouchPoint& out) const override;
        [[nodiscard]] bool HasTouch() const override;

    private:
        InputSurface* m_s;
    };

    // --- InputSurface -----------------------------------------------------------

    class InputSurface
    {
    public:
        InputSurface(IInputManager* raw, foundation::u32 window, const foundation::ContentFit& fit) noexcept
            : m_raw(raw), m_window(window), m_fit(fit)
        {
            for (foundation::i32 i = 0; i < kMaxGamepads; ++i)
            {
                m_gamepads[i].Bind(this, i);
            }
        }

        // Non-copyable, non-movable: facades hold a stable `this`.
        InputSurface(const InputSurface&) = delete;
        InputSurface& operator=(const InputSurface&) = delete;

        // --- configuration ---
        void SetFit(const foundation::ContentFit& fit) noexcept { m_fit = fit; }
        void SetRegion(foundation::Rectangle region) noexcept { m_fit.region = region; }
        void SetContentSize(foundation::Float2 size) noexcept { m_fit.contentSize = size; }
        void SetFitMode(foundation::FitMode mode) noexcept { m_fit.mode = mode; }
        void SetWindowSize(foundation::Float2 size) noexcept { m_windowSize = size; }
        [[nodiscard]] foundation::Float2 WindowSize() const noexcept { return m_windowSize; }
        // Re-target the surface to a different window (e.g. a dockable panel hosting the surface is
        // undocked into a floating OS window - the router hover-tests by window id).
        void SetWindow(foundation::u32 window) noexcept { m_window = window; }
        [[nodiscard]] const foundation::ContentFit& Fit() const noexcept { return m_fit; }
        [[nodiscard]] foundation::u32 Window() const noexcept { return m_window; }

        // --- gate state (read by anyone; set by InputRouter) ---
        [[nodiscard]] bool Hovered() const noexcept { return m_hovered; }
        [[nodiscard]] bool Focused() const noexcept { return m_focused; }
        [[nodiscard]] bool Captured() const noexcept { return m_captured; }
        [[nodiscard]] bool MouseActive() const noexcept { return m_hovered || m_captured; }

        [[nodiscard]] foundation::Float2 ContentMouse() const noexcept { return m_contentMouse; }
        [[nodiscard]] foundation::Float2 ContentDelta() const noexcept { return m_contentDelta; }

        // --- transformed + gated device facades ---
        [[nodiscard]] IMouse* Mouse() noexcept { return &m_mouse; }
        [[nodiscard]] IKeyboard* Keyboard() noexcept { return &m_keyboard; }
        [[nodiscard]] ITouch* Touch() noexcept { return &m_touch; }
        [[nodiscard]] IGamepad* Gamepad(foundation::i32 index) noexcept
        {
            return (index >= 0 && index < kMaxGamepads) ? &m_gamepads[index] : nullptr;
        }

        // --- internals the facades / router use ---
        [[nodiscard]] IInputManager* Raw() const noexcept { return m_raw; }

        // Called by InputRouter once per frame with this surface's resolved gate.
        void ApplyGate(bool hovered, bool focused, bool captured, foundation::Float2 contentMouse,
                       foundation::Float2 contentDelta) noexcept
        {
            m_hovered = hovered;
            m_focused = focused;
            m_captured = captured;
            m_contentMouse = contentMouse;
            m_contentDelta = contentDelta;
        }

    private:
        static constexpr foundation::i32 kMaxGamepads = 8;

        IInputManager* m_raw;
        foundation::u32 m_window;
        foundation::ContentFit m_fit;

        foundation::Float2 m_windowSize{0, 0}; // pixel size of the OS window (touch transform)
        bool m_hovered = false, m_focused = false, m_captured = false;
        foundation::Float2 m_contentMouse{0, 0};
        foundation::Float2 m_contentDelta{0, 0};

        SurfaceMouse m_mouse{this};
        SurfaceKeyboard m_keyboard{this};
        SurfaceTouch m_touch{this};
        SurfaceGamepad m_gamepads[kMaxGamepads];
    };

    // --- facade bodies (InputSurface now complete) ------------------------------

    inline foundation::f32 SurfaceMouse::X() const { return m_s->ContentMouse().x; }
    inline foundation::f32 SurfaceMouse::Y() const { return m_s->ContentMouse().y; }
    // Global position is desktop-space and surface-independent, so it passes through the raw device
    // untransformed (no ContentFit applied).
    inline foundation::f32 SurfaceMouse::GlobalX() const { return m_s->Raw()->Mouse()->GlobalX(); }
    inline foundation::f32 SurfaceMouse::GlobalY() const { return m_s->Raw()->Mouse()->GlobalY(); }
    inline foundation::f32 SurfaceMouse::DeltaX() const { return m_s->ContentDelta().x; }
    inline foundation::f32 SurfaceMouse::DeltaY() const { return m_s->ContentDelta().y; }
    inline foundation::f32 SurfaceMouse::ScrollX() const
    {
        return m_s->MouseActive() ? m_s->Raw()->Mouse()->ScrollX() : 0.0f;
    }
    inline foundation::f32 SurfaceMouse::ScrollY() const
    {
        return m_s->MouseActive() ? m_s->Raw()->Mouse()->ScrollY() : 0.0f;
    }
    inline bool SurfaceMouse::IsButtonDown(MouseButton b) const
    {
        return m_s->MouseActive() && m_s->Raw()->Mouse()->IsButtonDown(b);
    }
    inline bool SurfaceMouse::IsButtonPressed(MouseButton b) const
    {
        return m_s->MouseActive() && m_s->Raw()->Mouse()->IsButtonPressed(b);
    }
    inline bool SurfaceMouse::IsButtonReleased(MouseButton b) const
    {
        return m_s->MouseActive() && m_s->Raw()->Mouse()->IsButtonReleased(b);
    }
    inline bool SurfaceMouse::RelativeMode() const { return m_s->Raw()->Mouse()->RelativeMode(); }
    inline void SurfaceMouse::SetRelativeMode(bool e) { m_s->Raw()->Mouse()->SetRelativeMode(e); }
    inline bool SurfaceMouse::CursorVisible() const { return m_s->Raw()->Mouse()->CursorVisible(); }
    inline void SurfaceMouse::SetCursorVisible(bool v) { m_s->Raw()->Mouse()->SetCursorVisible(v); }
    inline void SurfaceMouse::SetCursor(CursorType c) { m_s->Raw()->Mouse()->SetCursor(c); }
    inline void SurfaceMouse::SetGlobalCapture(bool e) { m_s->Raw()->Mouse()->SetGlobalCapture(e); }

    inline bool SurfaceKeyboard::IsKeyDown(KeyCode k) const
    {
        return m_s->Focused() && m_s->Raw()->Keyboard()->IsKeyDown(k);
    }
    inline bool SurfaceKeyboard::IsKeyPressed(KeyCode k) const
    {
        return m_s->Focused() && m_s->Raw()->Keyboard()->IsKeyPressed(k);
    }
    inline bool SurfaceKeyboard::IsKeyReleased(KeyCode k) const
    {
        return m_s->Focused() && m_s->Raw()->Keyboard()->IsKeyReleased(k);
    }
    inline KeyModifiers SurfaceKeyboard::Modifiers() const
    {
        return m_s->Focused() ? m_s->Raw()->Keyboard()->Modifiers() : KeyModifiers::None;
    }

    inline IGamepad* SurfaceGamepad::Raw() const
    {
        return (m_s != nullptr) ? m_s->Raw()->GetGamepad(m_index) : nullptr;
    }
    inline foundation::i32 SurfaceGamepad::Index() const { return m_index; }
    inline foundation::StringView SurfaceGamepad::Name() const
    {
        IGamepad* g = Raw();
        return g ? g->Name() : foundation::StringView{};
    }
    inline bool SurfaceGamepad::Connected() const
    {
        IGamepad* g = Raw();
        return g != nullptr && g->Connected();
    }
    inline bool SurfaceGamepad::IsButtonDown(GamepadButton b) const
    {
        IGamepad* g = Raw();
        return m_s->Focused() && g != nullptr && g->IsButtonDown(b);
    }
    inline bool SurfaceGamepad::IsButtonPressed(GamepadButton b) const
    {
        IGamepad* g = Raw();
        return m_s->Focused() && g != nullptr && g->IsButtonPressed(b);
    }
    inline bool SurfaceGamepad::IsButtonReleased(GamepadButton b) const
    {
        IGamepad* g = Raw();
        return m_s->Focused() && g != nullptr && g->IsButtonReleased(b);
    }
    inline foundation::f32 SurfaceGamepad::Axis(GamepadAxis a) const
    {
        IGamepad* g = Raw();
        return (m_s->Focused() && g != nullptr) ? g->Axis(a) : 0.0f;
    }
    inline void SurfaceGamepad::SetRumble(foundation::f32 lo, foundation::f32 hi, foundation::u32 durationMs)
    {
        IGamepad* g = Raw();
        if (g != nullptr)
        {
            g->SetRumble(lo, hi, durationMs);
        }
    }

    namespace detail
    {
        // Window-normalized -> content-normalized through the surface's fit; false when the
        // finger is outside the drawn content (spatial gating).
        [[nodiscard]] inline bool TransformTouch(const InputSurface& s, const TouchPoint& raw,
                                                 TouchPoint& out)
        {
            const foundation::Float2 window = s.WindowSize();
            const foundation::Float2 content = s.Fit().contentSize;
            if (window.x <= 0.0f || window.y <= 0.0f)
            {
                return false;
            }
            if (content.x <= 0.0f || content.y <= 0.0f)
            {
                return false;
            }
            foundation::Float2 mapped;
            if (!s.Fit().ToContent(foundation::Float2{raw.x * window.x, raw.y * window.y}, mapped))
            {
                return false;
            }
            out = TouchPoint{raw.id, mapped.x / content.x, mapped.y / content.y, raw.pressure};
            return true;
        }
    }

    inline foundation::i32 SurfaceTouch::TouchCount() const
    {
        ITouch* raw = m_s->Raw() != nullptr ? m_s->Raw()->Touch() : nullptr;
        if (raw == nullptr)
        {
            return 0;
        }
        foundation::i32 count = 0;
        const foundation::i32 total = raw->TouchCount();
        for (foundation::i32 i = 0; i < total; ++i)
        {
            TouchPoint point;
            TouchPoint mapped;
            if (raw->GetTouchPoint(i, point) && detail::TransformTouch(*m_s, point, mapped))
            {
                ++count;
            }
        }
        return count;
    }
    inline bool SurfaceTouch::GetTouchPoint(foundation::i32 index, TouchPoint& out) const
    {
        ITouch* raw = m_s->Raw() != nullptr ? m_s->Raw()->Touch() : nullptr;
        if (raw == nullptr || index < 0)
        {
            return false;
        }
        foundation::i32 seen = 0;
        const foundation::i32 total = raw->TouchCount();
        for (foundation::i32 i = 0; i < total; ++i)
        {
            TouchPoint point;
            TouchPoint mapped;
            if (raw->GetTouchPoint(i, point) && detail::TransformTouch(*m_s, point, mapped))
            {
                if (seen == index)
                {
                    out = mapped;
                    return true;
                }
                ++seen;
            }
        }
        return false;
    }
    inline bool SurfaceTouch::HasTouch() const { return TouchCount() > 0; }

    // --- InputRouter ------------------------------------------------------------

    class InputRouter
    {
    public:
        explicit InputRouter(IInputManager* raw) noexcept : m_raw(raw) {}

        void AddSurface(InputSurface* s)
        {
            if (s != nullptr)
            {
                m_surfaces.PushBack(s);
            }
        }
        void RemoveSurface(InputSurface* s)
        {
            for (foundation::usize i = 0; i < m_surfaces.Size(); ++i)
            {
                if (m_surfaces[i] == s)
                {
                    m_surfaces.RemoveAt(i);
                    break;
                }
            }
            if (m_focused == s)
            {
                m_focused = nullptr;
            }
            if (m_captured == s)
            {
                m_captured = nullptr;
            }
        }

        // When on, hovering a surface also focuses it (keyboard/gamepad follow the
        // pointer). Off (default): focus changes on a click over a surface.
        void SetFocusFollowsHover(bool on) noexcept { m_focusFollowsHover = on; }

        // Let an external overlay (e.g. ImGui, via io.WantCaptureMouse/WantCaptureKeyboard) swallow input:
        // while set, no surface is hovered/focused, so the viewport cameras ignore the wheel/clicks/keys the
        // overlay is using. Call each frame before Update() (after the overlay's NewFrame).
        void SetExternalCapture(bool mouse, bool keyboard) noexcept
        {
            m_extMouseCapture = mouse;
            m_extKeyboardCapture = keyboard;
        }

        [[nodiscard]] InputSurface* Hovered() const noexcept { return m_hovered; }
        [[nodiscard]] InputSurface* Focused() const noexcept { return m_focused; }

        // Once per frame, after the shell pumps OS events. Resolves hover/focus/
        // capture and pushes each surface's gate.
        void Update()
        {
            IMouse* rawMouse = m_raw->Mouse();
            const foundation::Float2 pos{rawMouse->X(), rawMouse->Y()};
            const foundation::Float2 delta{rawMouse->DeltaX(), rawMouse->DeltaY()};
            const foundation::u32 hoverWindow = m_raw->HoverWindow();

            // Which surface is under the pointer? Last match wins (topmost added). When an external overlay
            // (e.g. an ImGui window under the pointer) has captured the mouse this frame, NO surface is
            // hovered - so the overlay swallows the wheel/clicks and the viewport cameras don't also react.
            m_hovered = nullptr;
            if (!m_extMouseCapture)
            {
                for (InputSurface* s : m_surfaces)
                {
                    if (s->Window() != hoverWindow)
                    {
                        continue;
                    }
                    if (s->Fit().DstRect().Contains(pos))
                    {
                        m_hovered = s;
                    }
                }
            }

            // Pointer capture: a held button pins the target to the surface the
            // press started on, so a drag that leaves the rect keeps reporting.
            const bool anyDown = rawMouse->IsButtonDown(MouseButton::Left) ||
                                 rawMouse->IsButtonDown(MouseButton::Right) ||
                                 rawMouse->IsButtonDown(MouseButton::Middle);
            const bool anyPressed = rawMouse->IsButtonPressed(MouseButton::Left) ||
                                    rawMouse->IsButtonPressed(MouseButton::Right) ||
                                    rawMouse->IsButtonPressed(MouseButton::Middle);

            if (m_captured != nullptr && (!anyDown || m_extMouseCapture))
            {
                m_captured = nullptr;
            }
            if (!m_extMouseCapture && m_captured == nullptr && m_hovered != nullptr && anyPressed)
            {
                m_captured = m_hovered;
            }

            InputSurface* target = (m_captured != nullptr) ? m_captured : m_hovered;

            // Focus resolution. An external overlay capturing the keyboard (e.g. an ImGui text field) drops
            // surface focus so typing doesn't also drive the viewport (WASD etc.).
            if (m_extKeyboardCapture)
            {
                m_focused = nullptr;
            }
            else if (m_focusFollowsHover)
            {
                if (m_hovered != nullptr)
                {
                    m_focused = m_hovered;
                }
                // else: keep last focus so keyboard survives a brief pointer exit.
            }
            else if (anyPressed && m_hovered != nullptr)
            {
                m_focused = m_hovered;
            }

            // Push each surface's gate.
            for (InputSurface* s : m_surfaces)
            {
                const bool hovered = (s == m_hovered);
                const bool captured = (s == m_captured);
                const bool focused = (s == m_focused);

                foundation::Float2 content = s->ContentMouse(); // keep last if not over this surface
                if (s->Window() == hoverWindow)
                {
                    foundation::Float2 c;
                    if (s->Fit().ToContent(pos, c))
                    {
                        content = c;
                    }
                }

                foundation::Float2 cdelta{0, 0};
                if (s == target)
                {
                    const foundation::Float2 scale = s->Fit().Scale();
                    cdelta = foundation::Float2{delta.x * scale.x, delta.y * scale.y};
                }

                s->ApplyGate(hovered, focused, captured, content, cdelta);
            }
        }

    private:
        IInputManager* m_raw;
        foundation::Array<InputSurface*> m_surfaces;
        InputSurface* m_hovered = nullptr;
        InputSurface* m_focused = nullptr;
        InputSurface* m_captured = nullptr;
        bool m_focusFollowsHover = false;
        bool m_extMouseCapture = false;    // external overlay (ImGui) owns the mouse this frame
        bool m_extKeyboardCapture = false; // external overlay owns the keyboard this frame
    };
}
