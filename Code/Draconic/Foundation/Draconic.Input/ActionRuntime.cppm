// Draconic::Input - :runtime partition.
//
// ActionRuntime (docs/design/input.md §3.2): per-frame evaluation of an InputMap against
// polled shell device facades. Godot's value model (per-device OR/MAX folding, [0,1]
// strength, frame-counter-exact edges, circular dead zones) on ez's set model (all enabled
// sets evaluate; queries resolve flat by priority; exclusive-set push with HELD-SUPPRESSION
// latching - an action suppressed while physically held stays released until the physical
// release, so closing a menu never re-fires a held "Fire").
//
// Devices arrive through IInputSourceProvider: the game host passes the raw shell, play-in-
// editor passes the Game viewport's gated InputSurface facades - fixing, by construction,
// Sedulous's "editor viewport forwards nothing".

module;
#include "Draconic.Foundation/Prelude.h"
#include <cmath>

export module draconic.input:action_runtime;

import draconic.foundation;
import draconic.shell;
import :input_map;

using namespace draconic::foundation;

export namespace draconic::input
{
    namespace shell = draconic::shell;

    // The per-context script service key: the Input facade resolves an ActionRuntime under this key
    // (each context can read a DIFFERENT runtime - the shared editor runtime, or a per-GameInstance
    // one). Lives here (not the subsystem) so a GameInstance can install its own runtime without
    // pulling the whole InputSubsystem in.
    inline constexpr foundation::StringView kInputScriptService = u8"input.runtime";

    // The device seam. All accessors may return null / zero - devices come and go (hotplug,
    // unfocused editor viewport) and evaluation treats absence as "released".
    class IInputSourceProvider
    {
    public:
        virtual ~IInputSourceProvider() = default;
        [[nodiscard]] virtual shell::IKeyboard* Keyboard() = 0;
        [[nodiscard]] virtual shell::IMouse* Mouse() = 0;
        [[nodiscard]] virtual i32 GamepadCount() const = 0;
        [[nodiscard]] virtual shell::IGamepad* Gamepad(i32 index) = 0;
        // Defaulted (not every provider has one): touch coordinates are NORMALIZED window
        // space, matching the touch bindings' region model.
        [[nodiscard]] virtual shell::ITouch* Touch() { return nullptr; }
        // This frame's tagged shell event stream, gated like the device facades (a
        // viewport provider returns it only while it owns keyboard focus). Consumers
        // needing ORDER or PAYLOADS polling cannot carry - key sequence, TextInput
        // characters - read these; the span is valid until the next shell pump.
        // Defaulted empty: pure-polling providers (tests, fakes) stay valid.
        [[nodiscard]] virtual Span<const shell::InputEvent> Events() { return {}; }
    };

    // The common case: the whole app's devices, straight off the shell.
    class ShellInputSource final : public IInputSourceProvider
    {
    public:
        explicit ShellInputSource(shell::IInputManager* input) : m_input(input) {}
        void SetInput(shell::IInputManager* input) noexcept { m_input = input; }
        [[nodiscard]] shell::IKeyboard* Keyboard() override
        {
            return m_input != nullptr ? m_input->Keyboard() : nullptr;
        }
        [[nodiscard]] shell::IMouse* Mouse() override
        {
            return m_input != nullptr ? m_input->Mouse() : nullptr;
        }
        [[nodiscard]] i32 GamepadCount() const override
        {
            return m_input != nullptr ? m_input->GamepadCount() : 0;
        }
        [[nodiscard]] shell::IGamepad* Gamepad(i32 index) override
        {
            return m_input != nullptr ? m_input->GetGamepad(index) : nullptr;
        }
        [[nodiscard]] shell::ITouch* Touch() override
        {
            return m_input != nullptr ? m_input->Touch() : nullptr;
        }
        [[nodiscard]] Span<const shell::InputEvent> Events() override
        {
            return m_input != nullptr ? m_input->Events() : Span<const shell::InputEvent>{};
        }

    private:
        shell::IInputManager* m_input = nullptr; // borrowed
    };

    // A resolved action name: hash computed once, candidates (same name across sets) cached
    // sorted by set priority. Queries are array lookups, never per-call string compares.
    struct ActionRef
    {
        u32 index = kInvalid; // into the runtime's candidate table
        static constexpr u32 kInvalid = 0xFFFFFFFFu;
        [[nodiscard]] bool IsValid() const noexcept { return index != kInvalid; }
    };

    // Rebind capture (ez GetPressedInputSlot shape): poll once per frame from the rebind
    // UI; the first ACTIVATED input matching the filter comes back as a ready-made Binding.
    // Returns false while nothing qualifies (the UI keeps listening; Esc-to-cancel is the
    // UI's affair). Axis/stick activation threshold is deliberately high (0.6) so drift
    // never binds.
    struct CaptureFilter
    {
        bool keys = true;
        bool mouseButtons = true;
        bool gamepadButtons = true;
        bool gamepadAxes = false; // axis rebinds opt in (a key rebind must ignore stick noise)
        bool gamepadSticks = false;
    };

    [[nodiscard]] inline bool CaptureBinding(IInputSourceProvider& devices,
                                             const CaptureFilter& filter, Binding& out)
    {
        constexpr f32 kActivate = 0.6f;
        if (filter.keys)
        {
            if (shell::IKeyboard* keyboard = devices.Keyboard())
            {
                for (u32 code = 1; code < static_cast<u32>(shell::KeyCode::Count); ++code)
                {
                    if (keyboard->IsKeyPressed(static_cast<shell::KeyCode>(code)))
                    {
                        out = Binding{};
                        out.source = BindingSource::Key;
                        out.code = code;
                        return true;
                    }
                }
            }
        }
        if (filter.mouseButtons)
        {
            if (shell::IMouse* mouse = devices.Mouse())
            {
                for (u32 code = 0; code < static_cast<u32>(shell::MouseButton::Count); ++code)
                {
                    if (mouse->IsButtonPressed(static_cast<shell::MouseButton>(code)))
                    {
                        out = Binding{};
                        out.source = BindingSource::MouseButton;
                        out.code = code;
                        return true;
                    }
                }
            }
        }
        const i32 pads = devices.GamepadCount();
        for (i32 p = 0; p < pads; ++p)
        {
            shell::IGamepad* pad = devices.Gamepad(p);
            if (pad == nullptr || !pad->Connected())
            {
                continue;
            }
            if (filter.gamepadButtons)
            {
                for (u32 code = 0; code < static_cast<u32>(shell::GamepadButton::Count); ++code)
                {
                    if (pad->IsButtonPressed(static_cast<shell::GamepadButton>(code)))
                    {
                        out = Binding{};
                        out.source = BindingSource::GamepadButton;
                        out.code = code;
                        return true;
                    }
                }
            }
            if (filter.gamepadSticks)
            {
                const f32 lx = pad->Axis(shell::GamepadAxis::LeftX);
                const f32 ly = pad->Axis(shell::GamepadAxis::LeftY);
                const f32 rx = pad->Axis(shell::GamepadAxis::RightX);
                const f32 ry = pad->Axis(shell::GamepadAxis::RightY);
                if (lx * lx + ly * ly > kActivate * kActivate)
                {
                    out = Binding{};
                    out.source = BindingSource::GamepadStick;
                    out.code = static_cast<u32>(StickCode::Left);
                    return true;
                }
                if (rx * rx + ry * ry > kActivate * kActivate)
                {
                    out = Binding{};
                    out.source = BindingSource::GamepadStick;
                    out.code = static_cast<u32>(StickCode::Right);
                    return true;
                }
            }
            if (filter.gamepadAxes)
            {
                for (u32 code = 0; code < static_cast<u32>(shell::GamepadAxis::Count); ++code)
                {
                    if (std::fabs(pad->Axis(static_cast<shell::GamepadAxis>(code))) > kActivate)
                    {
                        out = Binding{};
                        out.source = BindingSource::GamepadAxis;
                        out.code = code;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    class ActionRuntime
    {
    public:
        static constexpr f32 kPressPoint = 0.5f;

        /// Installs (copies) the map and rebuilds all state. Every set starts ENABLED.
        void SetMap(const InputMap& map)
        {
            m_map = map;
            m_states.Clear();
            m_setEnabled.Clear();
            m_exclusiveStack.Clear();
            m_refs.Clear();
            usize actionTotal = 0;
            for (const ActionSet& set : m_map.sets)
            {
                actionTotal += set.actions.Size();
            }
            m_states.Resize(actionTotal);
            m_setEnabled.Resize(m_map.sets.Size());
            for (usize i = 0; i < m_setEnabled.Size(); ++i)
            {
                m_setEnabled[i] = 1u;
            }
        }
        [[nodiscard]] const InputMap& Map() const noexcept { return m_map; }

        // ---- sets ----
        void EnableSet(StringView name, bool enabled = true)
        {
            for (usize i = 0; i < m_map.sets.Size(); ++i)
            {
                if (m_map.sets[i].name.AsView() == name)
                {
                    m_setEnabled[i] = enabled ? 1u : 0u;
                }
            }
        }
        void DisableSet(StringView name) { EnableSet(name, false); }
        [[nodiscard]] bool IsSetEnabled(StringView name) const
        {
            for (usize i = 0; i < m_map.sets.Size(); ++i)
            {
                if (m_map.sets[i].name.AsView() == name)
                {
                    return m_setEnabled[i] != 0u;
                }
            }
            return false;
        }

        /// Modal contexts: while the stack is non-empty, only the TOP set's actions read
        /// active; everything else reads released (with proper release edges + latching).
        /// Every exclusive TRANSITION additionally latches all physically-held actions -
        /// ez's require-key-up-on-activation: a held Fire neither Confirms the menu that
        /// just opened nor re-fires when it closes; modal boundaries demand a fresh press.
        void PushExclusiveSet(StringView name)
        {
            m_exclusiveStack.PushBack(String(name));
            m_latchHeldOnce = true;
        }
        void PopExclusiveSet()
        {
            if (!m_exclusiveStack.IsEmpty())
            {
                m_exclusiveStack.PopBack();
                m_latchHeldOnce = true;
            }
        }
        [[nodiscard]] usize ExclusiveDepth() const noexcept { return m_exclusiveStack.Size(); }

        // ---- resolution ----
        /// Flat resolution (the approved model): the name is looked up across ALL sets;
        /// at query time the highest-priority candidate in an ENABLED set answers.
        [[nodiscard]] ActionRef Resolve(StringView name)
        {
            for (u32 i = 0; i < static_cast<u32>(m_refs.Size()); ++i)
            {
                if (m_refs[i].name.AsView() == name)
                {
                    return ActionRef{i};
                }
            }
            RefEntry entry;
            entry.name = String(name);
            usize flat = 0;
            for (u32 s = 0; s < static_cast<u32>(m_map.sets.Size()); ++s)
            {
                for (u32 a = 0; a < static_cast<u32>(m_map.sets[s].actions.Size()); ++a, ++flat)
                {
                    if (m_map.sets[s].actions[a].name.AsView() == name)
                    {
                        entry.candidates.PushBack(Candidate{s, static_cast<u32>(flat)});
                    }
                }
            }
            // Highest set priority first (stable for ties: map order).
            for (usize i = 1; i < entry.candidates.Size(); ++i)
            {
                for (usize j = i; j > 0; --j)
                {
                    const i32 pa = m_map.sets[entry.candidates[j - 1].set].priority;
                    const i32 pb = m_map.sets[entry.candidates[j].set].priority;
                    if (pb > pa)
                    {
                        Swap(entry.candidates[j - 1], entry.candidates[j]);
                    }
                    else
                    {
                        break;
                    }
                }
            }
            m_refs.PushBack(static_cast<RefEntry&&>(entry));
            return ActionRef{static_cast<u32>(m_refs.Size() - 1)};
        }

        // ---- per-frame evaluation ----
        void Update(IInputSourceProvider& devices, f32 deltaTime)
        {
            ++m_frame;
            const i32 exclusiveTop = ExclusiveTopSet();
            usize flat = 0;
            for (usize s = 0; s < m_map.sets.Size(); ++s)
            {
                // Disabled behaves exactly like exclusive-suppressed: released + latching.
                const bool suppressed = (m_setEnabled[s] == 0u) ||
                                        (exclusiveTop >= 0 && static_cast<i32>(s) != exclusiveTop);
                for (usize a = 0; a < m_map.sets[s].actions.Size(); ++a, ++flat)
                {
                    EvaluateAction(m_map.sets[s].actions[a], m_states[flat], devices, deltaTime,
                                   suppressed, m_latchHeldOnce);
                }
            }
            m_latchHeldOnce = false;
        }
        [[nodiscard]] u64 Frame() const noexcept { return m_frame; }

        /// Engine time scale, applied to actions whose processors set `timeScale` (their
        /// VALUE multiplies by it - a rate driving per-second gameplay slows with the
        /// world). Mouse-delta-style rates should simply not set the flag (the ez rule).
        void SetTimeScale(f32 scale) noexcept { m_timeScale = scale < 0.0f ? 0.0f : scale; }
        [[nodiscard]] f32 TimeScale() const noexcept { return m_timeScale; }

        /// UI consumption (game-ui.md §3.3): device CLASSES the UI consumed this frame -
        /// bindings on those classes read RELEASED through actions (raw facades stay
        /// unfiltered). Separate classes so a menu eating the mouse doesn't mute gamepad
        /// movement. Republished every frame by the UI subsystem; sticky until changed.
        struct ConsumptionMask
        {
            bool pointer = false;  // MouseButton/MouseAxis/MouseDelta/TouchButton/TouchStick
            bool keyboard = false; // Key (+ Composite2D, which is four keys)
        };
        void SetConsumptionMask(ConsumptionMask mask) noexcept { m_consumed = mask; }
        [[nodiscard]] ConsumptionMask GetConsumptionMask() const noexcept { return m_consumed; }

        [[nodiscard]] static bool IsPointerSource(BindingSource source) noexcept
        {
            return source == BindingSource::MouseButton || source == BindingSource::MouseAxis ||
                   source == BindingSource::MouseDelta || source == BindingSource::TouchButton ||
                   source == BindingSource::TouchStick;
        }
        [[nodiscard]] static bool IsKeyboardSource(BindingSource source) noexcept
        {
            return source == BindingSource::Key || source == BindingSource::Composite2D;
        }
        [[nodiscard]] bool IsConsumed(BindingSource source) const noexcept
        {
            return (m_consumed.pointer && IsPointerSource(source)) ||
                   (m_consumed.keyboard && IsKeyboardSource(source));
        }

        // ---- queries ----
        [[nodiscard]] bool IsDown(ActionRef ref) const
        {
            const ActionState* state = StateFor(ref);
            return state != nullptr && state->pressed;
        }
        [[nodiscard]] bool WasPressed(ActionRef ref) const
        {
            const ActionState* state = StateFor(ref);
            return state != nullptr && state->pressedFrame == m_frame;
        }
        [[nodiscard]] bool WasReleased(ActionRef ref) const
        {
            const ActionState* state = StateFor(ref);
            return state != nullptr && state->releasedFrame == m_frame;
        }
        [[nodiscard]] f32 Value(ActionRef ref) const
        {
            const ActionState* state = StateFor(ref);
            return state != nullptr ? state->value.x : 0.0f;
        }
        [[nodiscard]] Float2 Value2D(ActionRef ref) const
        {
            const ActionState* state = StateFor(ref);
            return state != nullptr ? state->value : Float2{0.0f, 0.0f};
        }

        /// Ad-hoc composition helpers (Godot): a signed axis from two actions, a vector
        /// from four, with circular length clamping.
        [[nodiscard]] f32 Axis(ActionRef negative, ActionRef positive) const
        {
            return Value(positive) - Value(negative);
        }
        [[nodiscard]] Float2 Vector2(ActionRef negX, ActionRef posX, ActionRef negY,
                                     ActionRef posY) const
        {
            Float2 v{Value(posX) - Value(negX), Value(posY) - Value(negY)};
            const f32 length = std::sqrt(v.x * v.x + v.y * v.y);
            if (length > 1.0f)
            {
                v.x /= length;
                v.y /= length;
            }
            return v;
        }

    private:
        struct ActionState
        {
            Float2 value{0.0f, 0.0f};    // post-processor, post-suppression
            Float2 smoothed{0.0f, 0.0f}; // smoothing integrator (pre-suppression)
            bool pressed = false;
            bool latched = false; // held through a suppression: stays released
            u64 pressedFrame = 0;
            u64 releasedFrame = 0;
            // Interaction machine (Button actions with kind != None):
            bool rawHeld = false; // last frame's effective press, pre-interaction
            f32 heldSeconds = 0.0f;
            f32 sinceLastTap = 1.0e9f; // DoubleTap window timer
            bool holdFired = false;
            // Virtual-stick tracking (TouchStick): the owning touch + its anchor.
            u64 touchId = 0;
            Float2 touchAnchor{0.0f, 0.0f};
        };
        struct Candidate
        {
            u32 set = 0;
            u32 flatIndex = 0;
        };
        struct RefEntry
        {
            String name;
            Array<Candidate> candidates;
        };

        [[nodiscard]] i32 ExclusiveTopSet() const
        {
            if (m_exclusiveStack.IsEmpty())
            {
                return -1;
            }
            const StringView top = m_exclusiveStack[m_exclusiveStack.Size() - 1].AsView();
            for (usize i = 0; i < m_map.sets.Size(); ++i)
            {
                if (m_map.sets[i].name.AsView() == top)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        [[nodiscard]] const ActionState* StateFor(ActionRef ref) const
        {
            if (!ref.IsValid() || ref.index >= m_refs.Size())
            {
                return nullptr;
            }
            const RefEntry& entry = m_refs[ref.index];
            for (const Candidate& candidate : entry.candidates)
            {
                if (m_setEnabled[candidate.set] != 0u)
                {
                    return &m_states[candidate.flatIndex];
                }
            }
            return nullptr;
        }

        [[nodiscard]] static f32 ApplyDeadZone(f32 v, f32 deadZone)
        {
            const f32 magnitude = std::fabs(v);
            if (magnitude <= deadZone)
            {
                return 0.0f;
            }
            const f32 rescaled = (magnitude - deadZone) / (1.0f - deadZone);
            return v < 0.0f ? -rescaled : rescaled;
        }

        [[nodiscard]] static Float2 ApplyCircularDeadZone(Float2 v, f32 deadZone)
        {
            const f32 length = std::sqrt(v.x * v.x + v.y * v.y);
            if (length <= deadZone)
            {
                return Float2{0.0f, 0.0f};
            }
            const f32 rescaled = Min((length - deadZone) / (1.0f - deadZone), 1.0f);
            const f32 factor = rescaled / length;
            return Float2{v.x * factor, v.y * factor};
        }

        struct Contribution
        {
            Float2 value{0.0f, 0.0f};
            bool digitalDown = false;
        };

        [[nodiscard]] static bool KeyDown(shell::IKeyboard* keyboard, u32 code, u32 modifiers)
        {
            if (keyboard == nullptr)
            {
                return false;
            }
            if (!keyboard->IsKeyDown(static_cast<shell::KeyCode>(code)))
            {
                return false;
            }
            if (modifiers != 0u &&
                (static_cast<u32>(keyboard->Modifiers()) & modifiers) != modifiers)
            {
                return false;
            }
            return true;
        }

        [[nodiscard]] static Contribution EvaluateBinding(const Binding& b,
                                                          IInputSourceProvider& devices)
        {
            Contribution out;
            const f32 sign = b.invert ? -1.0f : 1.0f;
            switch (b.source)
            {
            case BindingSource::Key:
            {
                if (KeyDown(devices.Keyboard(), b.code, b.modifiers))
                {
                    out.value.x = b.scale * sign;
                    out.digitalDown = true;
                }
                break;
            }
            case BindingSource::MouseButton:
            {
                shell::IMouse* mouse = devices.Mouse();
                if (mouse != nullptr &&
                    mouse->IsButtonDown(static_cast<shell::MouseButton>(b.code)))
                {
                    out.value.x = b.scale * sign;
                    out.digitalDown = true;
                }
                break;
            }
            case BindingSource::MouseAxis:
            {
                shell::IMouse* mouse = devices.Mouse();
                if (mouse != nullptr)
                {
                    f32 v = 0.0f;
                    switch (static_cast<MouseAxisCode>(b.code))
                    {
                    case MouseAxisCode::DeltaX:
                        v = mouse->DeltaX();
                        break;
                    case MouseAxisCode::DeltaY:
                        v = mouse->DeltaY();
                        break;
                    case MouseAxisCode::Wheel:
                        v = mouse->ScrollY();
                        break;
                    }
                    out.value.x = v * b.scale * sign;
                }
                break;
            }
            case BindingSource::MouseDelta:
            {
                shell::IMouse* mouse = devices.Mouse();
                if (mouse != nullptr)
                {
                    out.value.x = mouse->DeltaX() * b.scale;
                    out.value.y = mouse->DeltaY() * b.scale * sign;
                }
                break;
            }
            case BindingSource::GamepadButton:
            {
                ForEachPad(devices, b.device,
                           [&](shell::IGamepad& pad)
                           {
                               if (pad.IsButtonDown(static_cast<shell::GamepadButton>(b.code)))
                               {
                                   out.value.x = b.scale * sign;
                                   out.digitalDown = true;
                               }
                           });
                break;
            }
            case BindingSource::GamepadAxis:
            {
                ForEachPad(devices, b.device,
                           [&](shell::IGamepad& pad)
                           {
                               const f32 v =
                                   ApplyDeadZone(pad.Axis(static_cast<shell::GamepadAxis>(b.code)),
                                                 b.deadZone) *
                                   b.scale * sign;
                               if (std::fabs(v) > std::fabs(out.value.x))
                               {
                                   out.value.x = v;
                               }
                           });
                break;
            }
            case BindingSource::GamepadStick:
            {
                const shell::GamepadAxis axisX = static_cast<StickCode>(b.code) == StickCode::Left
                                                     ? shell::GamepadAxis::LeftX
                                                     : shell::GamepadAxis::RightX;
                const shell::GamepadAxis axisY = static_cast<StickCode>(b.code) == StickCode::Left
                                                     ? shell::GamepadAxis::LeftY
                                                     : shell::GamepadAxis::RightY;
                ForEachPad(devices, b.device,
                           [&](shell::IGamepad& pad)
                           {
                               Float2 v = ApplyCircularDeadZone(
                                   Float2{pad.Axis(axisX), pad.Axis(axisY)}, b.deadZone);
                               v.x *= b.scale;
                               v.y *= b.scale * sign;
                               if (v.x * v.x + v.y * v.y >
                                   out.value.x * out.value.x + out.value.y * out.value.y)
                               {
                                   out.value = v;
                               }
                           });
                break;
            }
            case BindingSource::TouchButton:
            case BindingSource::TouchStick:
                break; // stateful: handled by EvaluateTouchBinding at the call site
            case BindingSource::Composite2D:
            {
                shell::IKeyboard* keyboard = devices.Keyboard();
                const f32 x = (KeyDown(keyboard, b.posX, 0u) ? 1.0f : 0.0f) -
                              (KeyDown(keyboard, b.negX, 0u) ? 1.0f : 0.0f);
                const f32 y = (KeyDown(keyboard, b.posY, 0u) ? 1.0f : 0.0f) -
                              (KeyDown(keyboard, b.negY, 0u) ? 1.0f : 0.0f);
                out.value = Float2{x * b.scale, y * b.scale * sign};
                if (b.normalize)
                {
                    const f32 length =
                        std::sqrt(out.value.x * out.value.x + out.value.y * out.value.y);
                    if (length > 1.0f)
                    {
                        out.value.x /= length;
                        out.value.y /= length;
                    }
                }
                out.digitalDown = x != 0.0f || y != 0.0f;
                break;
            }
            }
            return out;
        }

        template <typename Fn>
        static void ForEachPad(IInputSourceProvider& devices, i32 wanted, Fn&& fn)
        {
            const i32 count = devices.GamepadCount();
            for (i32 i = 0; i < count; ++i)
            {
                shell::IGamepad* pad = devices.Gamepad(i);
                if (pad == nullptr || !pad->Connected())
                {
                    continue;
                }
                if (wanted >= 0 && pad->Index() != wanted)
                {
                    continue;
                }
                fn(*pad);
            }
        }

        // Touch bindings carry per-action STATE (the stick's owning touch + anchor), so
        // they evaluate here rather than in the stateless per-binding helper.
        [[nodiscard]] static Contribution
        EvaluateTouchBinding(const Binding& b, IInputSourceProvider& devices, ActionState& state)
        {
            Contribution out;
            shell::ITouch* touch = devices.Touch();
            if (touch == nullptr)
            {
                state.touchId = 0;
                return out;
            }
            auto inRegion = [&](f32 x, f32 y)
            {
                return x >= b.regionX && x <= b.regionX + b.regionW && y >= b.regionY &&
                       y <= b.regionY + b.regionH;
            };
            if (b.source == BindingSource::TouchButton)
            {
                const i32 count = touch->TouchCount();
                for (i32 i = 0; i < count; ++i)
                {
                    shell::TouchPoint point;
                    if (touch->GetTouchPoint(i, point) && inRegion(point.x, point.y))
                    {
                        out.value.x = b.scale;
                        out.digitalDown = true;
                        break;
                    }
                }
                return out;
            }

            // TouchStick: a floating stick anchored where its owning touch STARTED.
            const i32 count = touch->TouchCount();
            if (state.touchId != 0)
            {
                bool alive = false;
                shell::TouchPoint point;
                for (i32 i = 0; i < count; ++i)
                {
                    if (touch->GetTouchPoint(i, point) && point.id == state.touchId)
                    {
                        alive = true;
                        break;
                    }
                }
                if (!alive)
                {
                    state.touchId = 0;
                }
                else
                {
                    const f32 radius = b.stickRadius > 0.0f ? b.stickRadius : 0.15f;
                    Float2 v{(point.x - state.touchAnchor.x) / radius,
                             (point.y - state.touchAnchor.y) / radius};
                    const f32 length = std::sqrt(v.x * v.x + v.y * v.y);
                    if (length > 1.0f)
                    {
                        v.x /= length;
                        v.y /= length;
                    }
                    v = ApplyCircularDeadZone(v, b.deadZone);
                    out.value.x = v.x * b.scale;
                    out.value.y = v.y * b.scale * (b.invert ? -1.0f : 1.0f);
                    out.digitalDown = length > b.deadZone;
                }
            }
            if (state.touchId == 0)
            {
                for (i32 i = 0; i < count; ++i)
                {
                    shell::TouchPoint point;
                    if (touch->GetTouchPoint(i, point) && inRegion(point.x, point.y))
                    {
                        state.touchId = point.id;
                        state.touchAnchor = Float2{point.x, point.y};
                        break; // value starts at 0 this frame (anchor == position)
                    }
                }
            }
            return out;
        }

        [[nodiscard]] static f32 ApplyResponse(f32 v, f32 exponent)
        {
            if (exponent == 1.0f || v == 0.0f)
            {
                return v;
            }
            const f32 curved = std::pow(std::fabs(v), exponent);
            return v < 0.0f ? -curved : curved;
        }

        [[nodiscard]] static f32 MoveToward(f32 current, f32 target, f32 maxDelta)
        {
            const f32 diff = target - current;
            if (std::fabs(diff) <= maxDelta)
            {
                return target;
            }
            return current + (diff > 0.0f ? maxDelta : -maxDelta);
        }

        void EvaluateAction(const Action& action, ActionState& state, IInputSourceProvider& devices,
                            f32 deltaTime, bool suppressed, bool latchHeld)
        {
            // Fold bindings: per-component max magnitude (Godot MAX), digital OR.
            Float2 target{0.0f, 0.0f};
            bool digitalDown = false;
            for (const Binding& b : action.bindings)
            {
                if (IsConsumed(b.source))
                {
                    continue;
                } // UI ate this device class
                const Contribution c = (b.source == BindingSource::TouchButton ||
                                        b.source == BindingSource::TouchStick)
                                           ? EvaluateTouchBinding(b, devices, state)
                                           : EvaluateBinding(b, devices);
                if (std::fabs(c.value.x) > std::fabs(target.x))
                {
                    target.x = c.value.x;
                }
                if (std::fabs(c.value.y) > std::fabs(target.y))
                {
                    target.y = c.value.y;
                }
                digitalDown = digitalDown || c.digitalDown;
            }

            // Processors: response curve, then key-axis smoothing (sensitivity ramp,
            // gravity recenter, snap-on-flip). sensitivity==0 = instant.
            const ActionProcessors& proc = action.processors;
            target.x = ApplyResponse(target.x, proc.responseExponent);
            target.y = ApplyResponse(target.y, proc.responseExponent);
            if (proc.sensitivity > 0.0f && action.kind != ActionKind::Button)
            {
                auto smooth = [&](f32 current, f32 wanted)
                {
                    if (proc.snap && wanted != 0.0f && current != 0.0f &&
                        ((wanted > 0.0f) != (current > 0.0f)))
                    {
                        current = 0.0f;
                    }
                    const f32 rate =
                        (wanted == 0.0f && proc.gravity > 0.0f) ? proc.gravity : proc.sensitivity;
                    return MoveToward(current, wanted, rate * deltaTime);
                };
                state.smoothed.x = smooth(state.smoothed.x, target.x);
                state.smoothed.y = smooth(state.smoothed.y, target.y);
            }
            else
            {
                state.smoothed = target;
            }

            const f32 strength = Max(std::fabs(state.smoothed.x), std::fabs(state.smoothed.y));
            const bool physicallyPressed = digitalDown || strength > kPressPoint;

            // Suppression latching (ez RequireKeyUp): a press that lives through a
            // suppression window must not re-fire when the window ends; an exclusive
            // TRANSITION latches every held action (modal boundaries demand a fresh press).
            if ((suppressed || latchHeld) && physicallyPressed)
            {
                state.latched = true;
            }
            if (!physicallyPressed)
            {
                state.latched = false;
            }
            const bool effective = physicallyPressed && !suppressed && !state.latched;

            Float2 reportedValue = state.smoothed;
            if (proc.timeScale)
            {
                reportedValue.x *= m_timeScale;
                reportedValue.y *= m_timeScale;
            }
            state.value = (suppressed || state.latched) ? Float2{0.0f, 0.0f} : reportedValue;

            // Interactions reshape the EFFECTIVE press into the reported one (Hold delays
            // it, Tap/DoubleTap turn it into one-frame pulses); None passes through.
            bool reported = effective;
            switch (action.interaction.kind)
            {
            case InteractionKind::None:
                break;
            case InteractionKind::Hold:
            {
                if (effective)
                {
                    state.heldSeconds += deltaTime;
                    reported = state.holdFired || state.heldSeconds >= action.interaction.seconds;
                    state.holdFired = reported;
                }
                else
                {
                    state.heldSeconds = 0.0f;
                    state.holdFired = false;
                    reported = false;
                }
                break;
            }
            case InteractionKind::Tap:
            {
                reported = false;
                if (effective)
                {
                    state.heldSeconds += deltaTime;
                }
                else
                {
                    // Release: a short-enough press pulses for exactly this frame.
                    if (state.rawHeld && state.heldSeconds <= action.interaction.seconds)
                    {
                        reported = true;
                    }
                    state.heldSeconds = 0.0f;
                }
                break;
            }
            case InteractionKind::DoubleTap:
            {
                state.sinceLastTap += deltaTime;
                reported = false;
                if (effective && !state.rawHeld) // a fresh press
                {
                    if (state.sinceLastTap <= action.interaction.seconds)
                    {
                        reported = true;
                        state.sinceLastTap = 1.0e9f; // consumed
                    }
                    else
                    {
                        state.sinceLastTap = 0.0f; // first tap: arm the window
                    }
                }
                break;
            }
            }
            state.rawHeld = effective;

            if (reported && !state.pressed)
            {
                state.pressedFrame = m_frame;
            }
            if (!reported && state.pressed)
            {
                state.releasedFrame = m_frame;
            }
            state.pressed = reported;
        }

        InputMap m_map;
        Array<ActionState> m_states; // flat, parallel to (set, action) in map order
        Array<u8> m_setEnabled;
        Array<String> m_exclusiveStack;
        Array<RefEntry> m_refs;
        u64 m_frame = 0;
        f32 m_timeScale = 1.0f;
        ConsumptionMask m_consumed;
        bool m_latchHeldOnce = false; // set by exclusive push/pop, consumed next Update
    };
}
