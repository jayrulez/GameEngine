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
    // A sheet animation's end or iteration on `view`, handed to a script with the @keyframes' name.
    // The view already delivers it through the mutation queue after the frame's tick, so the
    // handler may change the tree; the delegate lives as long as the subscription.
    static void OnViewAnimation(ui::View* view, RefPtr<foundation::script::IScriptDelegate> handler, bool ended)
    {
        if (view == nullptr || !handler)
        {
            return; // null-but-valid handle, or no handler: a safe no-op
        }
        auto deliver = [handler](ui::View*, StringView name)
        {
            Variant args[1] = {Variant::From(String(name))};
            (void)handler->Invoke(Span<Variant>(args, 1));
        };
        if (ended)
        {
            view->OnAnimationEnd.Add(Move(deliver));
        }
        else
        {
            view->OnAnimationIteration.Add(Move(deliver));
        }
    }

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
    void H::pulse(f32 peak, f32 seconds) { PulseView(view.Get(), peak, seconds); }                    \
    void H::onAnimationEnd(RefPtr<foundation::script::IScriptDelegate> handler)                         \
    {                                                                                                   \
        OnViewAnimation(view.Get(), Move(handler), true);                                               \
    }                                                                                                   \
    void H::onAnimationIteration(RefPtr<foundation::script::IScriptDelegate> handler)                   \
    {                                                                                                   \
        OnViewAnimation(view.Get(), Move(handler), false);                                              \
    }

    UI_SCRIPT_DEFINE_COMMON(UIView)
    UI_SCRIPT_DEFINE_COMMON(UILabel)
    UI_SCRIPT_DEFINE_COMMON(UIButton)
    UI_SCRIPT_DEFINE_COMMON(UIProgressBar)
    UI_SCRIPT_DEFINE_COMMON(UISlider)
    UI_SCRIPT_DEFINE_COMMON(UIRichText)
    UI_SCRIPT_DEFINE_COMMON(UITextBox)
    UI_SCRIPT_DEFINE_COMMON(UIImage)
    UI_SCRIPT_DEFINE_COMMON(UIViewGroup)
    UI_SCRIPT_DEFINE_COMMON(UIScreen)
#undef UI_SCRIPT_DEFINE_COMMON

    // --------------------------------------------------------------------------- UILabel / UIButton ---
    String UILabel::text() const
    {
        auto* l = As<ui::Label>(view);
        return (l != nullptr) ? String(l->Text.Value().AsView()) : String{};
    }
    void UILabel::setText(String value)
    {
        if (auto* l = As<ui::Label>(view))
        {
            l->Text.SetValue(Move(value));
        }
    }
    Color UILabel::textColor() const
    {
        auto* l = As<ui::Label>(view);
        return (l != nullptr) ? l->ResolvedTextColor() : Color::Transparent;
    }
    void UILabel::setTextColor(Color value)
    {
        if (auto* l = As<ui::Label>(view))
        {
            l->TextColor.SetValue(Optional<Color>(value));
        }
    }
    void UILabel::clearTextColor()
    {
        if (auto* l = As<ui::Label>(view))
        {
            l->TextColor.SetValue(Optional<Color>{});
        }
    }

    String UIButton::text() const
    {
        auto* b = As<ui::Button>(view);
        return (b != nullptr) ? String(b->Text.Value().AsView()) : String{};
    }
    void UIButton::setText(String value)
    {
        if (auto* b = As<ui::Button>(view))
        {
            b->Text.SetValue(Move(value));
        }
    }
    void UIButton::onClick(RefPtr<foundation::script::IScriptDelegate> handler)
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

    // ------------------------------------------------------------------------------- UIProgressBar ---
    f64 UIProgressBar::value() const
    {
        auto* p = As<ui::ProgressBar>(view);
        return (p != nullptr) ? static_cast<f64>(p->Value.Value()) : 0.0;
    }
    void UIProgressBar::setValue(f64 value)
    {
        if (auto* p = As<ui::ProgressBar>(view))
        {
            p->Value.SetValue(static_cast<f32>(value));
        }
    }

    // ------------------------------------------------------------------------------------ UISlider ---
    f64 UISlider::value() const
    {
        auto* s = As<ui::Slider>(view);
        return (s != nullptr) ? static_cast<f64>(s->Value.Value()) : 0.0;
    }
    void UISlider::setValue(f64 value)
    {
        if (auto* s = As<ui::Slider>(view))
        {
            s->Value.SetValue(static_cast<f32>(value));
        }
    }
    f64 UISlider::min() const
    {
        auto* s = As<ui::Slider>(view);
        return (s != nullptr) ? static_cast<f64>(s->Min.Value()) : 0.0;
    }
    f64 UISlider::max() const
    {
        auto* s = As<ui::Slider>(view);
        return (s != nullptr) ? static_cast<f64>(s->Max.Value()) : 0.0;
    }
    void UISlider::setRange(f64 min, f64 max)
    {
        if (auto* s = As<ui::Slider>(view))
        {
            s->Min.SetValue(static_cast<f32>(min));
            s->Max.SetValue(static_cast<f32>(max));
        }
    }
    void UISlider::setStep(f64 step)
    {
        if (auto* s = As<ui::Slider>(view))
        {
            s->Step.SetValue(static_cast<f32>(step));
        }
    }
    void UISlider::onChanged(RefPtr<foundation::script::IScriptDelegate> handler)
    {
        auto* s = As<ui::Slider>(view);
        if (s == nullptr || !handler)
        {
            return; // null-but-valid handle, or no handler: a safe no-op
        }
        // As UIButton::onClick: the delegate lives with the subscription, and the handler never runs
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

    // ---------------------------------------------------------------------------------- UIRichText ---
    String UIRichText::markup() const
    {
        auto* t = As<ui::RichText>(view);
        return (t != nullptr) ? String(t->Markup.Value().AsView()) : String{};
    }
    void UIRichText::setMarkup(String value)
    {
        if (auto* t = As<ui::RichText>(view))
        {
            t->SetMarkup(value.AsView());
        }
    }
    String UIRichText::plainText() const
    {
        auto* t = As<ui::RichText>(view);
        return (t != nullptr) ? t->Document().PlainText() : String{};
    }
    String UIRichText::clickedLink() const
    {
        auto* t = As<ui::RichText>(view);
        return (t != nullptr) ? String(t->LastLink()) : String{};
    }
    void UIRichText::onLink(RefPtr<foundation::script::IScriptDelegate> handler)
    {
        auto* t = As<ui::RichText>(view);
        if (t == nullptr || !handler)
        {
            return;
        }
        // As UIButton::onClick: deferred through the mutation queue; the id waits in LastLink.
        t->OnLink.Add(
            [handler](ui::RichText* clicked, StringView)
            {
                ui::UIContext* ctx = (clicked != nullptr) ? clicked->Context : nullptr;
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
    void UIRichText::clearRuns()
    {
        if (auto* t = As<ui::RichText>(view))
        {
            t->SetRuns(Array<ui::TextRun>{});
        }
    }
    void UIRichText::addRun(String text)
    {
        if (auto* t = As<ui::RichText>(view))
        {
            ui::TextRun run;
            run.text = Move(text);
            t->AppendRun(Move(run));
        }
    }
    void UIRichText::addRun(String text, Color color)
    {
        if (auto* t = As<ui::RichText>(view))
        {
            ui::TextRun run;
            run.text = Move(text);
            run.color = color;
            t->AppendRun(Move(run));
        }
    }
    void UIRichText::addRun(String text, Color color, f32 size, bool bold, bool italic)
    {
        if (auto* t = As<ui::RichText>(view))
        {
            ui::TextRun run;
            run.text = Move(text);
            run.color = color;
            if (size > 0.0f)
            {
                run.size = size;
            }
            run.bold = bold;
            run.italic = italic;
            t->AppendRun(Move(run));
        }
    }

    // ----------------------------------------------------------------------------------- UITextBox ---
    String UITextBox::text() const
    {
        auto* e = As<ui::EditText>(view);
        return (e != nullptr) ? String(e->Text()) : String{};
    }
    void UITextBox::setText(String value)
    {
        if (auto* e = As<ui::EditText>(view))
        {
            e->SetText(value.AsView());
        }
    }

    // ----------------------------------------------------------------------------------- UIImage ---
    Guid UIImage::source() const
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
    void UIImage::setSource(Guid value)
    {
        if (auto* i = As<ui::ImageView>(view))
        {
            i->Source.SetValue(value.IsNil() ? String{} : Format(u8"{}", value));
        }
    }

    // ----------------------------------------------------- UIViewGroup / UIScreen finders (shared) ---
#define UI_SCRIPT_DEFINE_FINDERS(H)                                                                     \
    i32 H::childCount() const                                                                           \
    {                                                                                                   \
        auto* g = As<ui::ViewGroup>(view);                                                              \
        return (g != nullptr) ? static_cast<i32>(g->ChildCount()) : 0;                                  \
    }                                                                                                   \
    UIView H::childAt(i32 index) const                                                                    \
    {                                                                                                   \
        auto* g = As<ui::ViewGroup>(view);                                                              \
        if (g == nullptr || index < 0 || index >= static_cast<i32>(g->ChildCount()))                    \
        {                                                                                               \
            return UIView{};                                                                              \
        }                                                                                               \
        return Wrap<UIView>(g->GetChildAt(static_cast<usize>(index)));                                    \
    }                                                                                                   \
    UIView H::find(String name) const { return FindAs<ui::View, UIView>(view, name.AsView()); }             \
    UILabel H::findLabel(String name) const { return FindAs<ui::Label, UILabel>(view, name.AsView()); }     \
    UIButton H::findButton(String name) const                                                             \
    {                                                                                                   \
        return FindAs<ui::ButtonBase, UIButton>(view, name.AsView());                                     \
    }                                                                                                   \
    UIProgressBar H::findProgressBar(String name) const                                                   \
    {                                                                                                   \
        return FindAs<ui::ProgressBar, UIProgressBar>(view, name.AsView());                               \
    }                                                                                                   \
    UISlider H::findSlider(String name) const { return FindAs<ui::Slider, UISlider>(view, name.AsView()); } \
    UIRichText H::findRichText(String name) const                                                         \
    {                                                                                                   \
        return FindAs<ui::RichText, UIRichText>(view, name.AsView());                                     \
    }                                                                                                   \
    UITextBox H::findTextBox(String name) const                                                           \
    {                                                                                                   \
        return FindAs<ui::EditText, UITextBox>(view, name.AsView());                                      \
    }                                                                                                   \
    UIImage H::findImage(String name) const                                                               \
    {                                                                                                   \
        return FindAs<ui::ImageView, UIImage>(view, name.AsView());                                       \
    }                                                                                                   \
    UIViewGroup H::findGroup(String name) const                                                           \
    {                                                                                                   \
        return FindAs<ui::ViewGroup, UIViewGroup>(view, name.AsView());                                   \
    }

    UI_SCRIPT_DEFINE_FINDERS(UIViewGroup)
    UI_SCRIPT_DEFINE_FINDERS(UIScreen)
#undef UI_SCRIPT_DEFINE_FINDERS

    // findScreen lives on UIViewGroup only (a screen inside a screen is not a v1 shape).
    UIScreen UIViewGroup::findScreen(String name) const
    {
        return FindAs<gamekit::UIScreen, UIScreen>(view, name.AsView());
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

    UIViewGroup Ui::root()
    {
        UiScreenScriptBinding* b = Resolve();
        if (b == nullptr)
        {
            return Wrap<UIViewGroup>(nullptr);
        }
        // The ScreenStack's attached root is the SOURCE OF TRUTH for where pushed screens live
        // (Push adds to it). A separately-captured raw RootView* can diverge in the embedded host
        // (captured stale/null while the stack points at the live root) and dangle after a UI
        // shutdown, so the binding carries none - the stack is the only root source.
        foundation::ui::gamekit::ScreenStack* stack = Stack(b);
        foundation::ui::RootView* root = stack != nullptr ? stack->Root() : nullptr;
        return Wrap<UIViewGroup>(root);
    }
    UIScreen Ui::top()
    {
        UiScreenScriptBinding* b = Resolve();
        return Wrap<UIScreen>(Stack(b) != nullptr ? Stack(b)->Top() : nullptr);
    }
    i32 Ui::count()
    {
        UiScreenScriptBinding* b = Resolve();
        return Stack(b) != nullptr ? static_cast<i32>(Stack(b)->Count()) : 0;
    }

    UIView Ui::find(String name) { return root().find(Move(name)); }
    UILabel Ui::findLabel(String name) { return root().findLabel(Move(name)); }
    UIButton Ui::findButton(String name) { return root().findButton(Move(name)); }
    UIProgressBar Ui::findProgressBar(String name) { return root().findProgressBar(Move(name)); }
    UISlider Ui::findSlider(String name) { return root().findSlider(Move(name)); }
    UIRichText Ui::findRichText(String name) { return root().findRichText(Move(name)); }
    UITextBox Ui::findTextBox(String name) { return root().findTextBox(Move(name)); }
    UIImage Ui::findImage(String name) { return root().findImage(Move(name)); }
    UIViewGroup Ui::findGroup(String name) { return root().findGroup(Move(name)); }

    UIScreen Ui::push(Guid document)
    {
        UiScreenScriptBinding* b = Resolve();
        if (Stack(b) == nullptr)
        {
            return UIScreen{};
        }
        RefPtr<gamekit::UIScreen> screen = MakeScreenFromDocument(b, document);
        if (!screen)
        {
            return UIScreen{};
        }
        return Wrap<UIScreen>(Stack(b)->Push(screen));
    }
    void Ui::pop()
    {
        UiScreenScriptBinding* b = Resolve();
        if (Stack(b) != nullptr)
        {
            Stack(b)->Pop();
        }
    }
    UIScreen Ui::replace(Guid document)
    {
        UiScreenScriptBinding* b = Resolve();
        if (Stack(b) == nullptr)
        {
            return UIScreen{};
        }
        RefPtr<gamekit::UIScreen> screen = MakeScreenFromDocument(b, document);
        if (!screen)
        {
            return UIScreen{};
        }
        return Wrap<UIScreen>(Stack(b)->Replace(screen));
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
    builder.Method<&H::pulse>("pulse", {"peak", "seconds"});                                            \
    builder.Method<&H::onAnimationEnd>("onAnimationEnd", {"handler"});                                  \
    builder.Method<&H::onAnimationIteration>("onAnimationIteration", {"handler"})

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

    REFLECT_VALUE(UIView, "rtti::engine.ui.script")
    {
        builder.Method<&UIView::isValid>("isValid");
        builder.ComputedProperty<&UIView::name>("name");
        builder.ComputedProperty<&UIView::visible>("visible");
        builder.ComputedProperty<&UIView::enabled>("enabled");
        builder.Method<&UIView::setVisible>("setVisible", {"value"});
        builder.Method<&UIView::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UIView::opacity>("opacity");
        builder.Method<&UIView::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UIView::*)(f32, f32)>(&UIView::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UIView::translation>("translation");
        builder.Method<&UIView::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UIView::rotation>("rotation");
        builder.Method<&UIView::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UIView);
        builder.Constructor();
    }
    REFLECT_VALUE(UILabel, "rtti::engine.ui.script")
    {
        builder.Method<&UILabel::isValid>("isValid");
        builder.ComputedProperty<&UILabel::name>("name");
        builder.ComputedProperty<&UILabel::visible>("visible");
        builder.ComputedProperty<&UILabel::enabled>("enabled");
        builder.ComputedProperty<&UILabel::text>("text");
        builder.Method<&UILabel::setVisible>("setVisible", {"value"});
        builder.Method<&UILabel::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UILabel::opacity>("opacity");
        builder.Method<&UILabel::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UILabel::*)(f32, f32)>(&UILabel::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UILabel::translation>("translation");
        builder.Method<&UILabel::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UILabel::rotation>("rotation");
        builder.Method<&UILabel::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UILabel);
        builder.Method<&UILabel::setText>("setText", {"value"});
        builder.ComputedProperty<&UILabel::textColor>("textColor");
        builder.Method<&UILabel::setTextColor>("setTextColor", {"value"});
        builder.Method<&UILabel::clearTextColor>("clearTextColor");
        builder.Constructor();
    }
    REFLECT_VALUE(UIButton, "rtti::engine.ui.script")
    {
        builder.Method<&UIButton::isValid>("isValid");
        builder.ComputedProperty<&UIButton::name>("name");
        builder.ComputedProperty<&UIButton::visible>("visible");
        builder.ComputedProperty<&UIButton::enabled>("enabled");
        builder.ComputedProperty<&UIButton::text>("text");
        builder.Method<&UIButton::setVisible>("setVisible", {"value"});
        builder.Method<&UIButton::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UIButton::opacity>("opacity");
        builder.Method<&UIButton::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UIButton::*)(f32, f32)>(&UIButton::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UIButton::translation>("translation");
        builder.Method<&UIButton::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UIButton::rotation>("rotation");
        builder.Method<&UIButton::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UIButton);
        builder.Method<&UIButton::setText>("setText", {"value"});
        builder.Method<&UIButton::onClick>("onClick", {"handler"});
        builder.Constructor();
    }
    REFLECT_VALUE(UISlider, "rtti::engine.ui.script")
    {
        builder.Method<&UISlider::isValid>("isValid");
        builder.ComputedProperty<&UISlider::name>("name");
        builder.ComputedProperty<&UISlider::visible>("visible");
        builder.ComputedProperty<&UISlider::enabled>("enabled");
        builder.ComputedProperty<&UISlider::value>("value");
        builder.ComputedProperty<&UISlider::min>("min");
        builder.ComputedProperty<&UISlider::max>("max");
        builder.Method<&UISlider::setVisible>("setVisible", {"value"});
        builder.Method<&UISlider::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UISlider::opacity>("opacity");
        builder.Method<&UISlider::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UISlider::*)(f32, f32)>(&UISlider::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UISlider::translation>("translation");
        builder.Method<&UISlider::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UISlider::rotation>("rotation");
        builder.Method<&UISlider::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UISlider);
        builder.Method<&UISlider::setValue>("setValue", {"value"});
        builder.Method<&UISlider::setRange>("setRange", {"min", "max"});
        builder.Method<&UISlider::setStep>("setStep", {"step"});
        builder.Method<&UISlider::onChanged>("onChanged", {"handler"});
        builder.Constructor();
    }

    REFLECT_VALUE(UIRichText, "rtti::engine.ui.script")
    {
        builder.Method<&UIRichText::isValid>("isValid");
        builder.ComputedProperty<&UIRichText::name>("name");
        builder.ComputedProperty<&UIRichText::visible>("visible");
        builder.ComputedProperty<&UIRichText::enabled>("enabled");
        builder.Method<&UIRichText::setVisible>("setVisible", {"value"});
        builder.Method<&UIRichText::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UIRichText::opacity>("opacity");
        builder.Method<&UIRichText::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UIRichText::*)(f32, f32)>(&UIRichText::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UIRichText::translation>("translation");
        builder.Method<&UIRichText::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UIRichText::rotation>("rotation");
        builder.Method<&UIRichText::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UIRichText);
        builder.ComputedProperty<&UIRichText::markup>("markup");
        builder.Method<&UIRichText::setMarkup>("setMarkup", {"value"});
        builder.ComputedProperty<&UIRichText::plainText>("plainText");
        builder.ComputedProperty<&UIRichText::clickedLink>("clickedLink");
        builder.Method<&UIRichText::onLink>("onLink", {"handler"});
        builder.Method<&UIRichText::clearRuns>("clearRuns");
        builder.Method<static_cast<void (UIRichText::*)(String)>(&UIRichText::addRun)>("addRun", {"text"});
        builder.Method<static_cast<void (UIRichText::*)(String, Color)>(&UIRichText::addRun)>("addRun", {"text", "color"});
        builder.Method<static_cast<void (UIRichText::*)(String, Color, f32, bool, bool)>(&UIRichText::addRun)>(
            "addRun", {"text", "color", "size", "bold", "italic"});
        builder.Constructor();
    }

    REFLECT_VALUE(UIProgressBar, "rtti::engine.ui.script")
    {
        builder.Method<&UIProgressBar::isValid>("isValid");
        builder.ComputedProperty<&UIProgressBar::name>("name");
        builder.ComputedProperty<&UIProgressBar::visible>("visible");
        builder.ComputedProperty<&UIProgressBar::enabled>("enabled");
        builder.ComputedProperty<&UIProgressBar::value>("value");
        builder.Method<&UIProgressBar::setVisible>("setVisible", {"value"});
        builder.Method<&UIProgressBar::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UIProgressBar::opacity>("opacity");
        builder.Method<&UIProgressBar::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UIProgressBar::*)(f32, f32)>(&UIProgressBar::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UIProgressBar::translation>("translation");
        builder.Method<&UIProgressBar::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UIProgressBar::rotation>("rotation");
        builder.Method<&UIProgressBar::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UIProgressBar);
        builder.Method<&UIProgressBar::setValue>("setValue", {"value"});
        builder.Constructor();
    }
    REFLECT_VALUE(UITextBox, "rtti::engine.ui.script")
    {
        builder.Method<&UITextBox::isValid>("isValid");
        builder.ComputedProperty<&UITextBox::name>("name");
        builder.ComputedProperty<&UITextBox::visible>("visible");
        builder.ComputedProperty<&UITextBox::enabled>("enabled");
        builder.ComputedProperty<&UITextBox::text>("text");
        builder.Method<&UITextBox::setVisible>("setVisible", {"value"});
        builder.Method<&UITextBox::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UITextBox::opacity>("opacity");
        builder.Method<&UITextBox::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UITextBox::*)(f32, f32)>(&UITextBox::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UITextBox::translation>("translation");
        builder.Method<&UITextBox::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UITextBox::rotation>("rotation");
        builder.Method<&UITextBox::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UITextBox);
        builder.Method<&UITextBox::setText>("setText", {"value"});
        builder.Constructor();
    }
    REFLECT_VALUE(UIImage, "rtti::engine.ui.script")
    {
        builder.Method<&UIImage::isValid>("isValid");
        builder.ComputedProperty<&UIImage::name>("name");
        builder.ComputedProperty<&UIImage::visible>("visible");
        builder.ComputedProperty<&UIImage::enabled>("enabled");
        builder.ComputedProperty<&UIImage::source>("source");
        builder.Method<&UIImage::setVisible>("setVisible", {"value"});
        builder.Method<&UIImage::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UIImage::opacity>("opacity");
        builder.Method<&UIImage::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UIImage::*)(f32, f32)>(&UIImage::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UIImage::translation>("translation");
        builder.Method<&UIImage::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UIImage::rotation>("rotation");
        builder.Method<&UIImage::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UIImage);
        builder.Method<&UIImage::setSource>("setSource", {"value"});
        builder.Constructor();
    }
    REFLECT_VALUE(UIViewGroup, "rtti::engine.ui.script")
    {
        builder.Method<&UIViewGroup::isValid>("isValid");
        builder.ComputedProperty<&UIViewGroup::name>("name");
        builder.ComputedProperty<&UIViewGroup::visible>("visible");
        builder.ComputedProperty<&UIViewGroup::enabled>("enabled");
        builder.ComputedProperty<&UIViewGroup::childCount>("childCount");
        builder.Method<&UIViewGroup::setVisible>("setVisible", {"value"});
        builder.Method<&UIViewGroup::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UIViewGroup::opacity>("opacity");
        builder.Method<&UIViewGroup::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UIViewGroup::*)(f32, f32)>(&UIViewGroup::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UIViewGroup::translation>("translation");
        builder.Method<&UIViewGroup::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UIViewGroup::rotation>("rotation");
        builder.Method<&UIViewGroup::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UIViewGroup);
        builder.Method<&UIViewGroup::childAt>("childAt", {"index"});
        builder.Method<&UIViewGroup::find>("find", {"name"});
        builder.Method<&UIViewGroup::findLabel>("findLabel", {"name"});
        builder.Method<&UIViewGroup::findButton>("findButton", {"name"});
        builder.Method<&UIViewGroup::findProgressBar>("findProgressBar", {"name"});
        builder.Method<&UIViewGroup::findSlider>("findSlider", {"name"});
        builder.Method<&UIViewGroup::findRichText>("findRichText", {"name"});
        builder.Method<&UIViewGroup::findTextBox>("findTextBox", {"name"});
        builder.Method<&UIViewGroup::findImage>("findImage", {"name"});
        builder.Method<&UIViewGroup::findGroup>("findGroup", {"name"});
        builder.Method<&UIViewGroup::findScreen>("findScreen", {"name"});
        builder.Constructor();
    }
    REFLECT_VALUE(UIScreen, "rtti::engine.ui.script")
    {
        builder.Method<&UIScreen::isValid>("isValid");
        builder.ComputedProperty<&UIScreen::name>("name");
        builder.ComputedProperty<&UIScreen::visible>("visible");
        builder.ComputedProperty<&UIScreen::enabled>("enabled");
        builder.ComputedProperty<&UIScreen::childCount>("childCount");
        builder.Method<&UIScreen::setVisible>("setVisible", {"value"});
        builder.Method<&UIScreen::setEnabled>("setEnabled", {"value"});
        builder.ComputedProperty<&UIScreen::opacity>("opacity");
        builder.Method<&UIScreen::setOpacity>("setOpacity", {"value"});
        builder.Method<static_cast<void (UIScreen::*)(f32, f32)>(&UIScreen::fadeTo)>("fadeTo", {"opacity", "seconds"});
        builder.ComputedProperty<&UIScreen::translation>("translation");
        builder.Method<&UIScreen::setTranslation>("setTranslation", {"x", "y"});
        builder.ComputedProperty<&UIScreen::rotation>("rotation");
        builder.Method<&UIScreen::setRotation>("setRotation", {"degrees"});
        UI_SCRIPT_REFLECT_MOTION(UIScreen);
        builder.Method<&UIScreen::childAt>("childAt", {"index"});
        builder.Method<&UIScreen::find>("find", {"name"});
        builder.Method<&UIScreen::findLabel>("findLabel", {"name"});
        builder.Method<&UIScreen::findButton>("findButton", {"name"});
        builder.Method<&UIScreen::findProgressBar>("findProgressBar", {"name"});
        builder.Method<&UIScreen::findSlider>("findSlider", {"name"});
        builder.Method<&UIScreen::findRichText>("findRichText", {"name"});
        builder.Method<&UIScreen::findTextBox>("findTextBox", {"name"});
        builder.Method<&UIScreen::findImage>("findImage", {"name"});
        builder.Method<&UIScreen::findGroup>("findGroup", {"name"});
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
        builder.Method<&Ui::findRichText>("findRichText", {"name"});
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
            RttiRegisterValue_UIView();
            RttiRegisterValue_UILabel();
            RttiRegisterValue_UIButton();
            RttiRegisterValue_UIProgressBar();
            RttiRegisterValue_UISlider();
            RttiRegisterValue_UIRichText();
            RttiRegisterValue_UITextBox();
            RttiRegisterValue_UIImage();
            RttiRegisterValue_UIViewGroup();
            RttiRegisterValue_UIScreen();
            GlobalTypeRegistry().Register(TypeOf<UIView>());
            GlobalTypeRegistry().Register(TypeOf<UILabel>());
            GlobalTypeRegistry().Register(TypeOf<UIButton>());
            GlobalTypeRegistry().Register(TypeOf<UIProgressBar>());
            GlobalTypeRegistry().Register(TypeOf<UISlider>());
            GlobalTypeRegistry().Register(TypeOf<UIRichText>());
            GlobalTypeRegistry().Register(TypeOf<UITextBox>());
            GlobalTypeRegistry().Register(TypeOf<UIImage>());
            GlobalTypeRegistry().Register(TypeOf<UIViewGroup>());
            GlobalTypeRegistry().Register(TypeOf<UIScreen>());
            RttiRegisterEnum_Ease();                         // the tweens' easing choice
            GlobalTypeRegistry().Register(TypeOf<Ease>());
            // onAnimationEnd / onAnimationIteration's handler, so an AngelScript screen passes one
            // without declaring it.
            foundation::script::RegisterDelegateShape(u8"void UiAnimationHandler(const string &in name)");
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
