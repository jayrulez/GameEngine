// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::UI.Script - :types partition
//
// The reflected typed VIEW HANDLES scripts get from the `ui` facade. Each is a copyable value struct
// holding an OWNING RefPtr to a CORE foundation.ui view (Objects are ref-counted; holding a handle keeps
// its subtree alive, so ops on a handle to a popped/detached screen are safe no-ops rather than a UAF).
// Reflected (REFLECT_VALUE, bodies in UiScriptImpl.cpp) so AngelScript/Luau bind them identically.
//
// Reads are parens-less COMPUTED PROPERTIES (`label.text`, `bar.value`); writes are METHODS
// (`label.setText(...)`, `bar.setValue(...)`) - the established facade idiom (the reflection has no
// computed read-write property). Typed finders return null-but-valid handles on a missing name OR a
// type mismatch (loud-null, not a silent wrong-type op).

module;
#include "Core/Prelude.h"

export module engine.ui.script:types;

import foundation.core;
import foundation.ui;
import foundation.script; // IScriptDelegate (a script function bound as a button click handler)

using namespace foundation::core;

export namespace engine::uiscript
{
    struct View;
    struct Label;
    struct Button;
    struct ProgressBar;
    struct Slider;
    struct TextBox;
    struct Image;
    struct ViewGroup;
    struct Screen;

    /// How a scripted tween moves through its time (`label.scaleTo(1.4f, 0.2f, Ease::OutBack)`).
    /// InOut, the default, starts and ends gently; Out arrives gently; OutBack overshoots and
    /// settles; OutBounce and OutElastic bounce and spring into place.
    enum class Ease : i32
    {
        Linear,
        In,
        Out,
        InOut,
        OutBack,
        OutBounce,
        OutElastic,
    };

    // Common accessors shared (by hand, value structs have no inheritance) across every handle.
#define UI_SCRIPT_COMMON_HANDLE_MEMBERS                                                                 \
    RefPtr<foundation::ui::View> view;                                                                  \
    [[nodiscard]] bool isValid() const;                                                                 \
    [[nodiscard]] String name() const;                                                                  \
    [[nodiscard]] bool visible() const;                                                                 \
    [[nodiscard]] bool enabled() const;                                                                 \
    void setVisible(bool value);                                                                        \
    void setEnabled(bool value);                                                                        \
    [[nodiscard]] f32 opacity() const;                                                                  \
    /* At once, clamped to 0..1; a running fade on the view stops. */                                   \
    void setOpacity(f32 value);                                                                         \
    /* From where it is to `target` over `seconds` of UI frame time, which runs while the game is */    \
    /* paused (time scale 0); zero seconds, or a view in no tree yet, is a set. */                      \
    void fadeTo(f32 target, f32 seconds);                                                               \
    /* The offset in pixels from where layout put the view, applied as it draws and hit tests, so */    \
    /* moving it costs no relayout (a marker on a minimap). */                                          \
    [[nodiscard]] Float2 translation() const;                                                           \
    void setTranslation(f32 x, f32 y);                                                                  \
    /* Degrees, clockwise on screen, about the view's centre. */                                        \
    [[nodiscard]] f32 rotation() const;                                                                 \
    void setRotation(f32 degrees);                                                                      \
    /* Tweens on the UI frame clock, as fadeTo. Each property runs its own: a new tween of one */      \
    /* property replaces the running one of that property and leaves the others (a label can rise */  \
    /* and fade at once). Zero seconds, or a view in no tree yet, is a set. */                         \
    void fadeTo(f32 target, f32 seconds, Ease ease);                                                    \
    void moveTo(f32 x, f32 y, f32 seconds);                                                             \
    void moveTo(f32 x, f32 y, f32 seconds, Ease ease);                                                  \
    /* A uniform scale about the view's centre, as drawn (layout is unchanged). */                      \
    [[nodiscard]] f32 scale() const;                                                                    \
    void setScale(f32 value);                                                                           \
    void scaleTo(f32 target, f32 seconds);                                                              \
    void scaleTo(f32 target, f32 seconds, Ease ease);                                                   \
    void rotateTo(f32 degrees, f32 seconds);                                                            \
    void rotateTo(f32 degrees, f32 seconds, Ease ease);                                                 \
    /* From the normal size out to `peak` times it and back over `seconds`: a counter that changed. */ \
    void pulse(f32 peak, f32 seconds);

    /// A bare view - identity + visibility/enabled. What find / childAt return.
    struct View
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
    };

    /// A text label. `label.text` reads, `label.setText(...)` writes.
    struct Label
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] String text() const;
        void setText(String value);
    };

    /// A button: a text Button, or a ContentButton drawing any view as its content (a picture card,
    /// whose labels the finders reach). `button.text` reads a text button's caption (empty for a
    /// content button); `button.onClick(ScriptDelegate(fn))` binds a script
    /// function as its click handler. The handler stays alive as long as the button (released when the
    /// screen pops). It always runs through the mutation queue (drained next frame), never inline during
    /// click dispatch - so it may do ANY structural mutation (screens, entities, ...), not just screen
    /// push/pop, without re-entrancy.
    struct Button
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] String text() const;
        void setText(String value);
        void onClick(RefPtr<foundation::script::IScriptDelegate> handler);
    };

    /// A progress/fill bar. `bar.value` is 0..1.
    struct ProgressBar
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] f64 value() const;
        void setValue(f64 value);
    };

    /// A slider: `slider.value` within `min`..`max`, and `onChanged` for a drag, a click, an arrow or a
    /// pad's left and right. A volume control, say.
    struct Slider
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] f64 value() const;
        /// Clamped and snapped to the range. Like any change it runs the onChanged handler, so a
        /// handler that writes the value back must not write a different one each time.
        void setValue(f64 value);
        [[nodiscard]] f64 min() const;
        [[nodiscard]] f64 max() const;
        void setRange(f64 min, f64 max);
        /// The amount an arrow or a pad press moves it; 0 is a twentieth of the range.
        void setStep(f64 step);
        /// The handler runs through the mutation queue, as a button's click does, and reads `value`
        /// itself; it stays alive as long as the slider.
        void onChanged(RefPtr<foundation::script::IScriptDelegate> handler);
    };

    /// A text input. `box.text` reads/writes the edited text.
    struct TextBox
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] String text() const;
        void setText(String value);
    };

    /// An image. `image.source` is the texture asset it shows (a render texture a camera draws
    /// into included); `image.setSource(id)` swaps it, a nil id clears it.
    struct Image
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] Guid source() const;
        void setSource(Guid value);
    };

    /// A container - the search surface. Every finder searches this group's subtree recursively, first
    /// match, and returns a null-but-valid handle when the name is missing or is the wrong control type.
    struct ViewGroup
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] i32 childCount() const;
        [[nodiscard]] View childAt(i32 index) const;
        [[nodiscard]] View find(String name) const;
        [[nodiscard]] Label findLabel(String name) const;
        [[nodiscard]] Button findButton(String name) const;
        [[nodiscard]] ProgressBar findProgressBar(String name) const;
        [[nodiscard]] Slider findSlider(String name) const;
        [[nodiscard]] TextBox findTextBox(String name) const;
        [[nodiscard]] Image findImage(String name) const;
        [[nodiscard]] ViewGroup findGroup(String name) const;
        [[nodiscard]] Screen findScreen(String name) const;
    };

    /// A screen (a gamekit UIScreen) - a container with the same finder surface as ViewGroup, scoped to
    /// one screen so name lookups can be narrowed (`ui.top().findLabel(...)`).
    struct Screen
    {
        UI_SCRIPT_COMMON_HANDLE_MEMBERS
        [[nodiscard]] i32 childCount() const;
        [[nodiscard]] View childAt(i32 index) const;
        [[nodiscard]] View find(String name) const;
        [[nodiscard]] Label findLabel(String name) const;
        [[nodiscard]] Button findButton(String name) const;
        [[nodiscard]] ProgressBar findProgressBar(String name) const;
        [[nodiscard]] Slider findSlider(String name) const;
        [[nodiscard]] TextBox findTextBox(String name) const;
        [[nodiscard]] Image findImage(String name) const;
        [[nodiscard]] ViewGroup findGroup(String name) const;
    };

#undef UI_SCRIPT_COMMON_HANDLE_MEMBERS

    // Register the reflected view-handle value types with the global registry. Called by
    // RegisterUiScriptSurface; idempotent.
    void RegisterUiScriptTypes();
}
