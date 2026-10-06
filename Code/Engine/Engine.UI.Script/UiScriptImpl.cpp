// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::UI.Script - implementation unit: view-handle + `ui` facade bodies, the REFLECT_VALUE /
// REFLECT_MEMBERS registrations, and the RegisterUiScriptSurface entry point. Kept out of the interface
// units (the REFLECT bodies + foundation.ui contact never sit in a module interface - GCC gcm-cluster
// rule).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.ui.script;
import engine.domain;

import foundation.core;
import foundation.ui;
import foundation.ui.gamekit;
import foundation.script;
import foundation.script.facades; // RegisterExtraFacadeName

using namespace foundation::core;

namespace engine::uiscript
{
    namespace ui = foundation::ui;
    namespace gamekit = foundation::ui::gamekit;

    // ------------------------------------------------------------------- handle helpers (file-local) --
    namespace
    {
        template <typename T>
        [[nodiscard]] T* As(const RefPtr<ui::View>& view)
        {
            return Cast<T>(view.Get());
        }

        // Wrap a borrowed CORE view in a script handle of type H (H.view is an owning RefPtr).
        template <typename H>
        [[nodiscard]] H Wrap(ui::View* v)
        {
            H handle;
            handle.view = (v != nullptr) ? RefPtr<ui::View>(v) : RefPtr<ui::View>{};
            return handle;
        }

        // Find the first descendant named `name` and cast to CoreT; wrap in handle H (null on miss or
        // type mismatch - loud-null).
        template <typename CoreT, typename H>
        [[nodiscard]] H FindAs(const RefPtr<ui::View>& group, StringView name)
        {
            if (auto* g = Cast<ui::ViewGroup>(group.Get()))
            {
                return Wrap<H>(g->template FindByName<CoreT>(name));
            }
            return H{};
        }

        RefPtr<gamekit::UIScreen> MakeScreenFromDocument(UiScreenScriptBinding* b,
                                                         const Guid& document)
        {
            if (b == nullptr || !b->instantiate || document.IsNil())
            {
                return {};
            }
            RefPtr<ui::View> tree = b->instantiate(document);
            if (!tree)
            {
                return {};
            }
            if (auto* s = Cast<gamekit::UIScreen>(tree.Get()))
            {
                return RefPtr<gamekit::UIScreen>(s); // the document root IS a <screen>
            }
            RefPtr<gamekit::UIScreen> wrapper = MakeRef<gamekit::UIScreen>(DefaultAllocator());
            wrapper->AddView(tree.Get()); // wrap a plain root in a default (Modal) screen
            return wrapper;
        }
    }

    // ============================================================================ common handle ops ===
    namespace
    {
        // A view's opacity at once, clamped; a running fade on it stops. Nothing for null.
        void SetViewOpacity(ui::View* view, f32 value)
        {
            if (view == nullptr)
            {
                return;
            }
            if (view->Context != nullptr)
            {
                view->Context->Animations()->CancelForView(view, ui::AnimationChannel::Opacity);
            }
            view->Opacity = Clamp(value, 0.0f, 1.0f);
        }

        EasingFunction EasingOf(Ease ease)
        {
            switch (ease)
            {
            case Ease::Linear:
                return ui::Easing::Linear;
            case Ease::In:
                return ui::Easing::EaseIn;
            case Ease::Out:
                return ui::Easing::EaseOut;
            case Ease::OutBack:
                return ui::Easing::BackOut;
            case Ease::OutBounce:
                return ui::Easing::BounceOut;
            case Ease::OutElastic:
                return ui::Easing::ElasticOut;
            case Ease::InOut:
                break;
            }
            return ui::Easing::EaseInOut;
        }

        // Runs `animation` in place of the view's running one on the same property. A view in no
        // tree yet has no clock to run it on: the caller sets the end value instead.
        bool StartTween(ui::View* view, UniquePtr<ui::Animation> animation)
        {
            if (view->Context == nullptr)
            {
                return false;
            }
            view->Context->Animations()->CancelForView(view, animation->Channel());
            view->Context->Animations()->Add(Move(animation));
            return true;
        }

        void StopTween(ui::View* view, ui::AnimationChannel channel)
        {
            if (view->Context != nullptr)
            {
                view->Context->Animations()->CancelForView(view, channel);
            }
        }

        // A fade on the UI's frame clock (UIContext::BeginFrame), which keeps running while the
        // game sits at time scale 0, so a pause menu or a fade to black animates.
        void FadeViewTo(ui::View* view, f32 target, f32 seconds, Ease ease = Ease::InOut)
        {
            if (view == nullptr)
            {
                return;
            }
            const f32 to = Clamp(target, 0.0f, 1.0f);
            if (seconds <= 0.0f ||
                !StartTween(view, ui::ViewAnimator::FadeTo(view, view->Opacity, to, seconds, EasingOf(ease))))
            {
                SetViewOpacity(view, to);
            }
        }

        void MoveViewTo(ui::View* view, Float2 target, f32 seconds, Ease ease)
        {
            if (view == nullptr)
            {
                return;
            }
            if (seconds <= 0.0f ||
                !StartTween(view, ui::ViewAnimator::TranslateTo(view, view->Transform.Translation, target,
                                                                seconds, EasingOf(ease))))
            {
                StopTween(view, ui::AnimationChannel::Translation);
                ui::ViewTransform t = view->Transform;
                t.Translation = target;
                view->Transform = t;
            }
        }

        void SetViewScale(ui::View* view, f32 value)
        {
            if (view == nullptr)
            {
                return;
            }
            StopTween(view, ui::AnimationChannel::Scale);
            ui::ViewTransform t = view->Transform;
            t.Scale = Float2{value, value};
            view->Transform = t;
        }

        void ScaleViewTo(ui::View* view, f32 target, f32 seconds, Ease ease)
        {
            if (view == nullptr)
            {
                return;
            }
            if (seconds <= 0.0f ||
                !StartTween(view, ui::ViewAnimator::ScaleTo(view, view->Transform.Scale.x, target, seconds,
                                                            EasingOf(ease))))
            {
                SetViewScale(view, target);
            }
        }

        void RotateViewTo(ui::View* view, f32 degrees, f32 seconds, Ease ease)
        {
            if (view == nullptr)
            {
                return;
            }
            const f32 to = DegreesToRadians(degrees);
            if (seconds <= 0.0f ||
                !StartTween(view, ui::ViewAnimator::RotateTo(view, view->Transform.Rotation, to, seconds,
                                                             EasingOf(ease))))
            {
                StopTween(view, ui::AnimationChannel::Rotation);
                ui::ViewTransform t = view->Transform;
                t.Rotation = to;
                view->Transform = t;
            }
        }

        // From its normal size out to `peak` and back, half the time each way: the return is the
        // same tween played backward, so it always settles at the normal size, even when a pulse
        // starts over one still running.
        void PulseView(ui::View* view, f32 peak, f32 seconds)
        {
            if (view == nullptr || view->Context == nullptr || seconds <= 0.0f)
            {
                return;
            }
            UniquePtr<ui::Animation> swell =
                ui::ViewAnimator::ScaleTo(view, 1.0f, peak, seconds * 0.5f, ui::Easing::EaseOut);
            swell->SetAutoReverse(true);
            swell->SetRepeatCount(1);
            (void)StartTween(view, Move(swell));
        }
    }

    // Defined once via a macro (the value structs share the members but have no inheritance).
#define UI_SCRIPT_DEFINE_COMMON(H)                                                                      \
    bool H::isValid() const { return static_cast<bool>(view); }                                         \
    String H::name() const { return view ? String(view->Name.AsView()) : String{}; }                   \
    bool H::visible() const { return view && view->Visibility == ui::Visibility::Visible; }             \
    bool H::enabled() const { return view && view->IsEnabled; }                                         \
    void H::setVisible(bool value)                                                                      \
    {                                                                                                   \
        if (view)                                                                                       \
        {                                                                                               \
            view->Visibility = value ? ui::Visibility::Visible : ui::Visibility::Hidden;                \
        }                                                                                               \
    }                                                                                                   \
    void H::setEnabled(bool value)                                                                      \
    {                                                                                                   \
        if (view)                                                                                       \
        {                                                                                               \
            view->IsEnabled = value;                                                                    \
        }                                                                                               \
    }                                                                                                   \
    f32 H::opacity() const { return view ? view->Opacity : 0.0f; }                                      \
    void H::setOpacity(f32 value) { SetViewOpacity(view.Get(), value); }                                \
    void H::fadeTo(f32 target, f32 seconds) { FadeViewTo(view.Get(), target, seconds); }                \
    Float2 H::translation() const { return view ? view->Transform.Translation : Float2{0.0f, 0.0f}; }  \
    void H::setTranslation(f32 x, f32 y)                                                                \
    {                                                                                                   \
        if (view)                                                                                       \
        {                                                                                               \
            view->Transform.Translation = Float2{x, y};                                                 \
        }                                                                                               \
    }                                                                                                   \
    f32 H::rotation() const { return view ? RadiansToDegrees(view->Transform.Rotation) : 0.0f; }        \
    void H::setRotation(f32 degrees)                                                                    \
    {                                                                                                   \
        if (view)                                                                                       \
        {                                                                                               \
            view->Transform.Rotation = DegreesToRadians(degrees);                                       \
        }                                                                                               \
    }                                                                                                   \
    void H::fadeTo(f32 target, f32 seconds, Ease ease) { FadeViewTo(view.Get(), target, seconds, ease); } \
    void H::moveTo(f32 x, f32 y, f32 seconds) { MoveViewTo(view.Get(), Float2{x, y}, seconds, Ease::InOut); } \
    void H::moveTo(f32 x, f32 y, f32 seconds, Ease ease) { MoveViewTo(view.Get(), Float2{x, y}, seconds, ease); } \
    f32 H::scale() const { return view ? view->Transform.Scale.x : 1.0f; }                              \
    void H::setScale(f32 value) { SetViewScale(view.Get(), value); }                                    \
    void H::scaleTo(f32 target, f32 seconds) { ScaleViewTo(view.Get(), target, seconds, Ease::InOut); } \
    void H::scaleTo(f32 target, f32 seconds, Ease ease) { ScaleViewTo(view.Get(), target, seconds, ease); } \
    void H::rotateTo(f32 degrees, f32 seconds) { RotateViewTo(view.Get(), degrees, seconds, Ease::InOut); } \
    void H::rotateTo(f32 degrees, f32 seconds, Ease ease) { RotateViewTo(view.Get(), degrees, seconds, ease); } \
    void H::pulse(f32 peak, f32 seconds) { PulseView(view.Get(), peak, seconds); }

    UI_SCRIPT_DEFINE_COMMON(View)
    UI_SCRIPT_DEFINE_COMMON(Label)
    UI_SCRIPT_DEFINE_COMMON(Button)
    UI_SCRIPT_DEFINE_COMMON(ProgressBar)
    UI_SCRIPT_DEFINE_COMMON(Slider)
    UI_SCRIPT_DEFINE_COMMON(TextBox)
    UI_SCRIPT_DEFINE_COMMON(Image)
    UI_SCRIPT_DEFINE_COMMON(ViewGroup)
    UI_SCRIPT_DEFINE_COMMON(Screen)
#undef UI_SCRIPT_DEFINE_COMMON

    // ------------------------------------------------------------------------------- Label / Button ---
    String Label::text() const
    {
        auto* l = As<ui::Label>(view);
        return (l != nullptr) ? String(l->Text.Value().AsView()) : String{};
    }
    void Label::setText(String value)
    {
        if (auto* l = As<ui::Label>(view))
        {
            l->Text.SetValue(Move(value));
        }
    }
    Color Label::textColor() const
    {
        auto* l = As<ui::Label>(view);
        return (l != nullptr) ? l->ResolvedTextColor() : Color::Transparent;
    }
    void Label::setTextColor(Color value)
    {
        if (auto* l = As<ui::Label>(view))
        {
            l->TextColor.SetValue(Optional<Color>(value));
        }
    }
    void Label::clearTextColor()
    {
        if (auto* l = As<ui::Label>(view))
        {
            l->TextColor.SetValue(Optional<Color>{});
        }
    }

    String Button::text() const
    {
        auto* b = As<ui::Button>(view);
        return (b != nullptr) ? String(b->Text.Value().AsView()) : String{};
    }
    void Button::setText(String value)
    {
        if (auto* b = As<ui::Button>(view))
        {
            b->Text.SetValue(Move(value));
        }
    }
    void Button::onClick(RefPtr<foundation::script::IScriptDelegate> handler)
    {
        auto* b = As<ui::ButtonBase>(view); // any button: a text Button or a ContentButton
        if (b == nullptr || !handler)
        {
            return; // null-but-valid handle, or no handler: a safe no-op (loud-null idiom).
        }
        // Capturing the delegate RefPtr keeps the script function alive for exactly the lifetime of this
        // button's OnClick subscription (released when the screen - and button - is destroyed).
        //
        // A click is dispatched while the view tree is being traversed, and the handler is ARBITRARY
        // script: it may pop/push a screen, but also despawn an entity, tear down UI, etc. - ANY
        // structural mutation, none of which may run re-entrantly during that traversal. Input is
        // dispatched at the Idle phase (the InputManager sets no phase of its own), so a phase check
        // would NOT catch this. So the handler NEVER runs inline: it goes through the UIContext mutation
        // queue and runs at the next frame's drain - a quiescent point where every structural op is safe
        // (the same queue the ScreenStack and dialogs use). With no context (a headless / unattached
        // button) nothing is dispatching, so running directly is safe.
        b->OnClick.Add(
            [handler](ui::ButtonBase* btn)
            {
                ui::UIContext* ctx = (btn != nullptr) ? btn->Context : nullptr;
                if (ctx != nullptr)
                {
                    ctx->MutationQueueRef().QueueAction(
                        [handler] { (void)handler->Invoke(Span<Variant>{}); });
                }
                else
                {
                    (void)handler->Invoke(Span<Variant>{});
                }
            });
    }

    // --------------------------------------------------------------------------------- ProgressBar ---
    f64 ProgressBar::value() const
    {
        auto* p = As<ui::ProgressBar>(view);
        return (p != nullptr) ? static_cast<f64>(p->Value.Value()) : 0.0;
    }
    void ProgressBar::setValue(f64 value)
    {
        if (auto* p = As<ui::ProgressBar>(view))
        {
            p->Value.SetValue(static_cast<f32>(value));
        }
    }

    // -------------------------------------------------------------------------------------- Slider ---
    f64 Slider::value() const
    {
        auto* s = As<ui::Slider>(view);
        return (s != nullptr) ? static_cast<f64>(s->Value.Value()) : 0.0;
    }
    void Slider::setValue(f64 value)
    {
        if (auto* s = As<ui::Slider>(view))
        {
            s->Value.SetValue(static_cast<f32>(value));
        }
    }
    f64 Slider::min() const
    {
        auto* s = As<ui::Slider>(view);
        return (s != nullptr) ? static_cast<f64>(s->Min.Value()) : 0.0;
    }
    f64 Slider::max() const
    {
        auto* s = As<ui::Slider>(view);
        return (s != nullptr) ? static_cast<f64>(s->Max.Value()) : 0.0;
    }
    void Slider::setRange(f64 min, f64 max)
    {
        if (auto* s = As<ui::Slider>(view))
        {
            s->Min.SetValue(static_cast<f32>(min));
            s->Max.SetValue(static_cast<f32>(max));
        }
    }
    void Slider::setStep(f64 step)
    {
        if (auto* s = As<ui::Slider>(view))
        {
            s->Step.SetValue(static_cast<f32>(step));
        }
    }
    void Slider::onChanged(RefPtr<foundation::script::IScriptDelegate> handler)
    {
        auto* s = As<ui::Slider>(view);
        if (s == nullptr || !handler)
        {
            return; // null-but-valid handle, or no handler: a safe no-op
        }
        // As Button::onClick: the delegate lives with the subscription, and the handler never runs
        // inline during dispatch but through the mutation queue (directly with no context).
        s->OnValueChanged.Add(
            [handler](ui::Slider* changed, f32)
            {
                ui::UIContext* ctx = (changed != nullptr) ? changed->Context : nullptr;
                if (ctx != nullptr)
                {
                    ctx->MutationQueueRef().QueueAction([handler] { (void)handler->Invoke(Span<Variant>{}); });
                }
                else
                {
                    (void)handler->Invoke(Span<Variant>{});
                }
            });
    }

    // ------------------------------------------------------------------------------------- TextBox ---
    String TextBox::text() const
    {
        auto* e = As<ui::EditText>(view);
        return (e != nullptr) ? String(e->Text()) : String{};
    }
    void TextBox::setText(String value)
    {
        if (auto* e = As<ui::EditText>(view))
        {
            e->SetText(value.AsView());
        }
    }

    // ------------------------------------------------------------------------------------- Image ---
    Guid Image::source() const
    {
        auto* i = As<ui::ImageView>(view);
        Guid id;
        if (i == nullptr)
        {
            return id;
        }
        StringView text = i->Source.Value().AsView();
        if (text.Size() == 38 && text[0] == utf8char('{') && text[37] == utf8char('}'))
        {
            text = text.SubStr(1, 36);
        }
        (void)Guid::TryParse(text, id);
        return id;
    }
    void Image::setSource(Guid value)
    {
        if (auto* i = As<ui::ImageView>(view))
        {
            i->Source.SetValue(value.IsNil() ? String{} : Format(u8"{}", value));
        }
    }

    // --------------------------------------------------------- ViewGroup / Screen finders (shared) ---
#define UI_SCRIPT_DEFINE_FINDERS(H)                                                                     \
    i32 H::childCount() const                                                                           \
    {                                                                                                   \
        auto* g = As<ui::ViewGroup>(view);                                                              \
        return (g != nullptr) ? static_cast<i32>(g->ChildCount()) : 0;                                  \
    }                                                                                                   \
    View H::childAt(i32 index) const                                                                    \
    {                                                                                                   \
        auto* g = As<ui::ViewGroup>(view);                                                              \
        if (g == nullptr || index < 0 || index >= static_cast<i32>(g->ChildCount()))                    \
        {                                                                                               \
            return View{};                                                                              \
        }                                                                                               \
        return Wrap<View>(g->GetChildAt(static_cast<usize>(index)));                                    \
    }                                                                                                   \
    View H::find(String name) const { return FindAs<ui::View, View>(view, name.AsView()); }             \
    Label H::findLabel(String name) const { return FindAs<ui::Label, Label>(view, name.AsView()); }     \
    Button H::findButton(String name) const                                                             \
    {                                                                                                   \
        return FindAs<ui::ButtonBase, Button>(view, name.AsView());                                     \
    }                                                                                                   \
    ProgressBar H::findProgressBar(String name) const                                                   \
    {                                                                                                   \
        return FindAs<ui::ProgressBar, ProgressBar>(view, name.AsView());                               \
    }                                                                                                   \
    Slider H::findSlider(String name) const { return FindAs<ui::Slider, Slider>(view, name.AsView()); } \
    TextBox H::findTextBox(String name) const                                                           \
    {                                                                                                   \
        return FindAs<ui::EditText, TextBox>(view, name.AsView());                                      \
    }                                                                                                   \
    Image H::findImage(String name) const                                                               \
    {                                                                                                   \
        return FindAs<ui::ImageView, Image>(view, name.AsView());                                       \
    }                                                                                                   \
    ViewGroup H::findGroup(String name) const                                                           \
    {                                                                                                   \
        return FindAs<ui::ViewGroup, ViewGroup>(view, name.AsView());                                   \
    }

    UI_SCRIPT_DEFINE_FINDERS(ViewGroup)
    UI_SCRIPT_DEFINE_FINDERS(Screen)
#undef UI_SCRIPT_DEFINE_FINDERS

    // findScreen lives on ViewGroup only (a screen inside a screen is not a v1 shape).
    Screen ViewGroup::findScreen(String name) const
    {
        return FindAs<gamekit::UIScreen, Screen>(view, name.AsView());
    }

    // ================================================================================= `ui` facade ===
    UiScreenScriptBinding* Ui::Resolve()
    {
        foundation::script::IScriptContext* context = foundation::script::CurrentScriptContext();
        return (context != nullptr)
                   ? static_cast<UiScreenScriptBinding*>(context->GetService(kUiScreenScriptService))
                   : nullptr;
    }

    foundation::ui::gamekit::ScreenStack* Ui::Stack(UiScreenScriptBinding* b)
    {
        if (b == nullptr)
        {
            return nullptr;
        }
        return b->stackForRun ? b->stackForRun(foundation::script::CurrentRun()) : b->stack;
    }

    ViewGroup Ui::root()
    {
        UiScreenScriptBinding* b = Resolve();
        if (b == nullptr)
        {
            return Wrap<ViewGroup>(nullptr);
        }
        // The ScreenStack's attached root is the SOURCE OF TRUTH for where pushed screens live
        // (Push adds to it). A separately-captured raw RootView* can diverge in the embedded host
        // (captured stale/null while the stack points at the live root) and dangle after a UI
        // shutdown, so the binding carries none - the stack is the only root source.
        foundation::ui::gamekit::ScreenStack* stack = Stack(b);
        foundation::ui::RootView* root = stack != nullptr ? stack->Root() : nullptr;
        return Wrap<ViewGroup>(root);
    }
    Screen Ui::top()
    {
        UiScreenScriptBinding* b = Resolve();
        return Wrap<Screen>(Stack(b) != nullptr ? Stack(b)->Top() : nullptr);
    }
    i32 Ui::count()
    {
        UiScreenScriptBinding* b = Resolve();
        return Stack(b) != nullptr ? static_cast<i32>(Stack(b)->Count()) : 0;
    }

    View Ui::find(String name) { return root().find(Move(name)); }
    Label Ui::findLabel(String name) { return root().findLabel(Move(name)); }
    Button Ui::findButton(String name) { return root().findButton(Move(name)); }
    ProgressBar Ui::findProgressBar(String name) { return root().findProgressBar(Move(name)); }
    Slider Ui::findSlider(String name) { return root().findSlider(Move(name)); }
    TextBox Ui::findTextBox(String name) { return root().findTextBox(Move(name)); }
    Image Ui::findImage(String name) { return root().findImage(Move(name)); }
    ViewGroup Ui::findGroup(String name) { return root().findGroup(Move(name)); }

    Screen Ui::push(Guid document)
    {
        UiScreenScriptBinding* b = Resolve();
        if (Stack(b) == nullptr)
        {
            return Screen{};
        }
        RefPtr<gamekit::UIScreen> screen = MakeScreenFromDocument(b, document);
        if (!screen)
        {
            return Screen{};
        }
        return Wrap<Screen>(Stack(b)->Push(screen));
    }
    void Ui::pop()
    {
        UiScreenScriptBinding* b = Resolve();
        if (Stack(b) != nullptr)
        {
            Stack(b)->Pop();
        }
    }
    Screen Ui::replace(Guid document)
    {
        UiScreenScriptBinding* b = Resolve();
        if (Stack(b) == nullptr)
        {
            return Screen{};
        }
        RefPtr<gamekit::UIScreen> screen = MakeScreenFromDocument(b, document);
        if (!screen)
        {
            return Screen{};
        }
        return Wrap<Screen>(Stack(b)->Replace(screen));
    }
    void Ui::clear()
    {
        UiScreenScriptBinding* b = Resolve();
        if (Stack(b) != nullptr)
        {
            Stack(b)->Clear();
        }
    }
    bool Ui::back()
    {
        UiScreenScriptBinding* b = Resolve();
        return Stack(b) != nullptr ? Stack(b)->HandleBack() : false;
    }

    // ================================================================================== reflection ===
    // The tweens every handle shares; each overload pair is an arity family (with or without an
    // Ease), so both backends bind both.
#define UI_SCRIPT_REFLECT_MOTION(H)                                                                     \
    builder.Method<static_cast<void (H::*)(f32, f32, Ease)>(&H::fadeTo)>("fadeTo",                    \
                                                                        {"opacity", "seconds", "ease"}); \
    builder.Method<static_cast<void (H::*)(f32, f32, f32)>(&H::moveTo)>("moveTo", {"x", "y", "seconds"}); \
    builder.Method<static_cast<void (H::*)(f32, f32, f32, Ease)>(&H::moveTo)>(                         \
        "moveTo", {"x", "y", "seconds", "ease"});                                                       \
    builder.ComputedProperty<&H::scale>("scale");                                                      \
    builder.Method<&H::setScale>("setScale", {"value"});                                                \
    builder.Method<static_cast<void (H::*)(f32, f32)>(&H::scaleTo)>("scaleTo", {"scale", "seconds"});  \
    builder.Method<static_cast<void (H::*)(f32, f32, Ease)>(&H::scaleTo)>("scaleTo",                  \
                                                                         {"scale", "seconds", "ease"}); \
    builder.Method<static_cast<void (H::*)(f32, f32)>(&H::rotateTo)>("rotateTo", {"degrees", "seconds"}); \
    builder.Method<static_cast<void (H::*)(f32, f32, Ease)>(&H::rotateTo)>(                            \
        "rotateTo", {"degrees", "seconds", "ease"});                                                    \
    builder.Method<&H::pulse>("pulse", {"peak", "seconds"})

    REFLECT_ENUM(Ease, "rtti::engine.ui.script")
    {
        builder.Value("Linear", Ease::Linear);
        builder.Value("In", Ease::In);
        builder.Value("Out", Ease::Out);
        builder.Value("InOut", Ease::InOut);
        builder.Value("OutBack", Ease::OutBack);
        builder.Value("OutBounce", Ease::OutBounce);
        builder.Value("OutElastic", Ease::OutElastic);
    }

    REFLECT_VALUE(View, "rtti::engine.ui.script")
    {
        builder.Method<&View::isValid>("isValid");
        builder.ComputedProperty<&View::name>("name");
        builder.ComputedProperty<&View::visible>("visible");
        builder.ComputedProperty<&View::enabled>("enabled");
        builder.Method<&View::setVisible>("setVisible", {"value"});
        builder.Method<&View::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&View::opacity>("opacity");
        builder.Method<&View::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (View::*)(f32, f32)>(&View::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&View::translation>("translation");
        builder.Method<&View::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&View::rotation>("rotation");
        builder.Method<&View::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(View);
        builder.Constructor();
    }
    REFLECT_VALUE(Label, "rtti::engine.ui.script")
    {
        builder.Method<&Label::isValid>("isValid");
        builder.ComputedProperty<&Label::name>("name");
        builder.ComputedProperty<&Label::visible>("visible");
        builder.ComputedProperty<&Label::enabled>("enabled");
        builder.ComputedProperty<&Label::text>("text");
        builder.Method<&Label::setVisible>("setVisible", {"value"});
        builder.Method<&Label::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&Label::opacity>("opacity");
        builder.Method<&Label::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (Label::*)(f32, f32)>(&Label::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&Label::translation>("translation");
        builder.Method<&Label::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&Label::rotation>("rotation");
        builder.Method<&Label::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(Label);
        builder.Method<&Label::setText>("setText", {"value"});
        builder.ComputedProperty<&Label::textColor>("textColor");
        builder.Method<&Label::setTextColor>("setTextColor", {"value"});
        builder.Method<&Label::clearTextColor>("clearTextColor");
        builder.Constructor();
    }
    REFLECT_VALUE(Button, "rtti::engine.ui.script")
    {
        builder.Method<&Button::isValid>("isValid");
        builder.ComputedProperty<&Button::name>("name");
        builder.ComputedProperty<&Button::visible>("visible");
        builder.ComputedProperty<&Button::enabled>("enabled");
        builder.ComputedProperty<&Button::text>("text");
        builder.Method<&Button::setVisible>("setVisible", {"value"});
        builder.Method<&Button::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&Button::opacity>("opacity");
        builder.Method<&Button::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (Button::*)(f32, f32)>(&Button::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&Button::translation>("translation");
        builder.Method<&Button::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&Button::rotation>("rotation");
        builder.Method<&Button::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(Button);
        builder.Method<&Button::setText>("setText", {"value"});
        builder.Method<&Button::onClick>("onClick", {"handler"});
        builder.Constructor();
    }
    REFLECT_VALUE(Slider, "rtti::engine.ui.script")
    {
        builder.Method<&Slider::isValid>("isValid");
        builder.ComputedProperty<&Slider::name>("name");
        builder.ComputedProperty<&Slider::visible>("visible");
        builder.ComputedProperty<&Slider::enabled>("enabled");
        builder.ComputedProperty<&Slider::value>("value");
        builder.ComputedProperty<&Slider::min>("min");
        builder.ComputedProperty<&Slider::max>("max");
        builder.Method<&Slider::setVisible>("setVisible", {"value"});
        builder.Method<&Slider::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&Slider::opacity>("opacity");
        builder.Method<&Slider::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (Slider::*)(f32, f32)>(&Slider::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&Slider::translation>("translation");
        builder.Method<&Slider::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&Slider::rotation>("rotation");
        builder.Method<&Slider::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(Slider);
        builder.Method<&Slider::setValue>("setValue", {"value"});
        builder.Method<&Slider::setRange>("setRange", {"min", "max"});
        builder.Method<&Slider::setStep>("setStep", {"step"});
        builder.Method<&Slider::onChanged>("onChanged", {"handler"});
        builder.Constructor();
    }

    REFLECT_VALUE(ProgressBar, "rtti::engine.ui.script")
    {
        builder.Method<&ProgressBar::isValid>("isValid");
        builder.ComputedProperty<&ProgressBar::name>("name");
        builder.ComputedProperty<&ProgressBar::visible>("visible");
        builder.ComputedProperty<&ProgressBar::enabled>("enabled");
        builder.ComputedProperty<&ProgressBar::value>("value");
        builder.Method<&ProgressBar::setVisible>("setVisible", {"value"});
        builder.Method<&ProgressBar::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&ProgressBar::opacity>("opacity");
        builder.Method<&ProgressBar::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (ProgressBar::*)(f32, f32)>(&ProgressBar::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&ProgressBar::translation>("translation");
        builder.Method<&ProgressBar::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&ProgressBar::rotation>("rotation");
        builder.Method<&ProgressBar::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(ProgressBar);
        builder.Method<&ProgressBar::setValue>("setValue", {"value"});
        builder.Constructor();
    }
    REFLECT_VALUE(TextBox, "rtti::engine.ui.script")
    {
        builder.Method<&TextBox::isValid>("isValid");
        builder.ComputedProperty<&TextBox::name>("name");
        builder.ComputedProperty<&TextBox::visible>("visible");
        builder.ComputedProperty<&TextBox::enabled>("enabled");
        builder.ComputedProperty<&TextBox::text>("text");
        builder.Method<&TextBox::setVisible>("setVisible", {"value"});
        builder.Method<&TextBox::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&TextBox::opacity>("opacity");
        builder.Method<&TextBox::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (TextBox::*)(f32, f32)>(&TextBox::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&TextBox::translation>("translation");
        builder.Method<&TextBox::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&TextBox::rotation>("rotation");
        builder.Method<&TextBox::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(TextBox);
        builder.Method<&TextBox::setText>("setText", {"value"});
        builder.Constructor();
    }
    REFLECT_VALUE(Image, "rtti::engine.ui.script")
    {
        builder.Method<&Image::isValid>("isValid");
        builder.ComputedProperty<&Image::name>("name");
        builder.ComputedProperty<&Image::visible>("visible");
        builder.ComputedProperty<&Image::enabled>("enabled");
        builder.ComputedProperty<&Image::source>("source");
        builder.Method<&Image::setVisible>("setVisible", {"value"});
        builder.Method<&Image::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&Image::opacity>("opacity");
        builder.Method<&Image::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (Image::*)(f32, f32)>(&Image::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&Image::translation>("translation");
        builder.Method<&Image::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&Image::rotation>("rotation");
        builder.Method<&Image::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(Image);
        builder.Method<&Image::setSource>("setSource", {"value"});
        builder.Constructor();
    }
    REFLECT_VALUE(ViewGroup, "rtti::engine.ui.script")
    {
        builder.Method<&ViewGroup::isValid>("isValid");
        builder.ComputedProperty<&ViewGroup::name>("name");
        builder.ComputedProperty<&ViewGroup::visible>("visible");
        builder.ComputedProperty<&ViewGroup::enabled>("enabled");
        builder.ComputedProperty<&ViewGroup::childCount>("childCount");
        builder.Method<&ViewGroup::setVisible>("setVisible", {"value"});
        builder.Method<&ViewGroup::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&ViewGroup::opacity>("opacity");
        builder.Method<&ViewGroup::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (ViewGroup::*)(f32, f32)>(&ViewGroup::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&ViewGroup::translation>("translation");
        builder.Method<&ViewGroup::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&ViewGroup::rotation>("rotation");
        builder.Method<&ViewGroup::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(ViewGroup);
        builder.Method<&ViewGroup::childAt>("childAt", {"index"});
        builder.Method<&ViewGroup::find>("find", {"name"});
        builder.Method<&ViewGroup::findLabel>("findLabel", {"name"});
        builder.Method<&ViewGroup::findButton>("findButton", {"name"});
        builder.Method<&ViewGroup::findProgressBar>("findProgressBar", {"name"});
        builder.Method<&ViewGroup::findSlider>("findSlider", {"name"});
        builder.Method<&ViewGroup::findTextBox>("findTextBox", {"name"});
        builder.Method<&ViewGroup::findImage>("findImage", {"name"});
        builder.Method<&ViewGroup::findGroup>("findGroup", {"name"});
        builder.Method<&ViewGroup::findScreen>("findScreen", {"name"});
        builder.Constructor();
    }
    REFLECT_VALUE(Screen, "rtti::engine.ui.script")
    {
        builder.Method<&Screen::isValid>("isValid");
        builder.ComputedProperty<&Screen::name>("name");
        builder.ComputedProperty<&Screen::visible>("visible");
        builder.ComputedProperty<&Screen::enabled>("enabled");
        builder.ComputedProperty<&Screen::childCount>("childCount");
        builder.Method<&Screen::setVisible>("setVisible", {"value"});
        builder.Method<&Screen::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&Screen::opacity>("opacity");
        builder.Method<&Screen::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (Screen::*)(f32, f32)>(&Screen::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&Screen::translation>("translation");
        builder.Method<&Screen::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&Screen::rotation>("rotation");
        builder.Method<&Screen::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(Screen);
        builder.Method<&Screen::childAt>("childAt", {"index"});
        builder.Method<&Screen::find>("find", {"name"});
        builder.Method<&Screen::findLabel>("findLabel", {"name"});
        builder.Method<&Screen::findButton>("findButton", {"name"});
        builder.Method<&Screen::findProgressBar>("findProgressBar", {"name"});
        builder.Method<&Screen::findSlider>("findSlider", {"name"});
        builder.Method<&Screen::findTextBox>("findTextBox", {"name"});
        builder.Method<&Screen::findImage>("findImage", {"name"});
        builder.Method<&Screen::findGroup>("findGroup", {"name"});
        builder.Constructor();
    }

    REFLECT_MEMBERS(Ui, "rtti::engine.ui.script")
    {
        builder.Attribute("scriptName", "ui"); // reserved lowercase `ui` (ScriptName alias)
        builder.Method<&Ui::root>("root");
        builder.Method<&Ui::top>("top");
        builder.Method<&Ui::count>("count");
        builder.Method<&Ui::find>("find", {"name"});
        builder.Method<&Ui::findLabel>("findLabel", {"name"});
        builder.Method<&Ui::findButton>("findButton", {"name"});
        builder.Method<&Ui::findProgressBar>("findProgressBar", {"name"});
        builder.Method<&Ui::findSlider>("findSlider", {"name"});
        builder.Method<&Ui::findTextBox>("findTextBox", {"name"});
        builder.Method<&Ui::findImage>("findImage", {"name"});
        builder.Method<&Ui::findGroup>("findGroup", {"name"});
        builder.Method<&Ui::push>("push", {"document"});
        builder.Method<&Ui::pop>("pop");
        builder.Method<&Ui::replace>("replace", {"document"});
        builder.Method<&Ui::clear>("clear");
        builder.Method<&Ui::back>("back");
        builder.Constructor();
    }

    void RegisterUiScriptTypes()
    {
        static const bool once = []()
        {
            RttiRegisterValue_View();
            RttiRegisterValue_Label();
            RttiRegisterValue_Button();
            RttiRegisterValue_ProgressBar();
            RttiRegisterValue_Slider();
            RttiRegisterValue_TextBox();
            RttiRegisterValue_Image();
            RttiRegisterValue_ViewGroup();
            RttiRegisterValue_Screen();
            GlobalTypeRegistry().Register(TypeOf<View>());
            GlobalTypeRegistry().Register(TypeOf<Label>());
            GlobalTypeRegistry().Register(TypeOf<Button>());
            GlobalTypeRegistry().Register(TypeOf<ProgressBar>());
            GlobalTypeRegistry().Register(TypeOf<Slider>());
            GlobalTypeRegistry().Register(TypeOf<TextBox>());
            GlobalTypeRegistry().Register(TypeOf<Image>());
            GlobalTypeRegistry().Register(TypeOf<ViewGroup>());
            GlobalTypeRegistry().Register(TypeOf<Screen>());
            RttiRegisterEnum_Ease();                         // the tweens' easing choice
            GlobalTypeRegistry().Register(TypeOf<Ease>());
            return true;
        }();
        (void)once;
    }

    void RegisterUiScriptSurface()
    {
        static const bool once = []()
        {
            RegisterUiScriptTypes();                           // finders return these value handles
            GlobalTypeRegistry().Register(Ui::StaticType());   // binds as `ui` (its scriptName alias)
            foundation::script::RegisterExtraFacadeName(u8"ui"); // prelude imports the ALIAS, not Ui
            return true;
        }();
        (void)once;
    }
}

namespace engine::uiscript
{
    const engine::DomainModule& UiScriptDomain() noexcept
    {
        static const engine::DomainModule kModule{
            .id = u8"ui.script",
            .registerScriptFacade = &RegisterUiScriptSurface};
        return kModule;
    }
}
