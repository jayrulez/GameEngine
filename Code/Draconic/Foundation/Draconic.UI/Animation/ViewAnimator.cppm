// Draconic UI - :view_animator partition
//
// Static convenience factories for common view animations. Returned animations are NOT automatically
// added to an AnimationManager - the caller adds them (ctx.Animations()->Add(ViewAnimator::FadeIn(...))).
// Ported from Sedulous.UI/src/Animation/ViewAnimator.bf. Beef `new FloatAnimation` + owned setter
// delegate -> UniquePtr<FloatAnimation> (converting-moves to UniquePtr<Animation>) + Function<void(f32)>
// capturing the borrowed View*.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:view_animator;

import draconic.foundation;
import :view;
import :view_transform;
import :animation;
import :float_animation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Static convenience methods for creating common view animations.
    struct ViewAnimator
    {
        /// Fade a view from one opacity to another.
        static UniquePtr<Animation> FadeTo(View* view, f32 from, f32 to, f32 duration,
                                           EasingFunction easing = nullptr)
        {
            view->Opacity = from;
            UniquePtr<FloatAnimation> anim = MakeUnique<FloatAnimation>(
                DefaultAllocator(), from, to, duration,
                Function<void(f32)>{[view](f32 v) { view->Opacity = v; }}, easing);
            anim->SetTarget(view);
            return anim;
        }

        /// Fade a view from 0 to 1 opacity.
        static UniquePtr<Animation> FadeIn(View* view, f32 duration,
                                           EasingFunction easing = nullptr)
        {
            return FadeTo(view, 0, 1, duration, easing);
        }

        /// Fade a view from 1 to 0 opacity.
        static UniquePtr<Animation> FadeOut(View* view, f32 duration,
                                            EasingFunction easing = nullptr)
        {
            return FadeTo(view, 1, 0, duration, easing);
        }

        /// Translate a view horizontally using ViewTransform.
        static UniquePtr<Animation> TranslateX(View* view, f32 from, f32 to, f32 duration,
                                               EasingFunction easing = nullptr)
        {
            UniquePtr<FloatAnimation> anim =
                MakeUnique<FloatAnimation>(DefaultAllocator(), from, to, duration,
                                           Function<void(f32)>{[view](f32 v)
                                                               {
                                                                   ViewTransform t =
                                                                       view->Transform;
                                                                   t.Translation.x = v;
                                                                   view->Transform = t;
                                                               }},
                                           easing);
            anim->SetTarget(view);
            return anim;
        }

        /// Translate a view vertically using ViewTransform.
        static UniquePtr<Animation> TranslateY(View* view, f32 from, f32 to, f32 duration,
                                               EasingFunction easing = nullptr)
        {
            UniquePtr<FloatAnimation> anim =
                MakeUnique<FloatAnimation>(DefaultAllocator(), from, to, duration,
                                           Function<void(f32)>{[view](f32 v)
                                                               {
                                                                   ViewTransform t =
                                                                       view->Transform;
                                                                   t.Translation.y = v;
                                                                   view->Transform = t;
                                                               }},
                                           easing);
            anim->SetTarget(view);
            return anim;
        }

        /// Scale a view uniformly using ViewTransform.
        static UniquePtr<Animation> ScaleTo(View* view, f32 from, f32 to, f32 duration,
                                            EasingFunction easing = nullptr)
        {
            UniquePtr<FloatAnimation> anim =
                MakeUnique<FloatAnimation>(DefaultAllocator(), from, to, duration,
                                           Function<void(f32)>{[view](f32 v)
                                                               {
                                                                   ViewTransform t =
                                                                       view->Transform;
                                                                   t.Scale = Float2{v, v};
                                                                   view->Transform = t;
                                                               }},
                                           easing);
            anim->SetTarget(view);
            return anim;
        }

        /// Rotate a view using ViewTransform (radians).
        static UniquePtr<Animation> RotateTo(View* view, f32 from, f32 to, f32 duration,
                                             EasingFunction easing = nullptr)
        {
            UniquePtr<FloatAnimation> anim =
                MakeUnique<FloatAnimation>(DefaultAllocator(), from, to, duration,
                                           Function<void(f32)>{[view](f32 v)
                                                               {
                                                                   ViewTransform t =
                                                                       view->Transform;
                                                                   t.Rotation = v;
                                                                   view->Transform = t;
                                                               }},
                                           easing);
            anim->SetTarget(view);
            return anim;
        }
    };
}
