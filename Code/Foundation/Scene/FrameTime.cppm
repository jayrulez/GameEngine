/// Foundation::Scene - the `:frame_time` partition.
///
/// FrameTime: the ONE value type carrying the scene lane's time-scale chain
/// (host x context x group x scene) plus the lane's configured fixed step
/// (scene-composition.md strain #4: the chain used to be hand-multiplied in
/// SceneManager, GameInstance::TickScript, and the subsystem bridge, drifting
/// independently). The fixed-step ACCUMULATOR stays per-scene (each Scene owns
/// its FixedStepper); `fixedStep` is configuration published alongside the
/// scales. Terminology: "context" is the app-level term of the documented
/// chain (game-instance.md §5) - a plain float handed DOWN by the driver;
/// foundation.scene stays runtime-free and the runtime layer never sees this
/// type (it is CONSTRUCTED at the engine bridge - the layering rules recorded
/// in scene-composition.md, 2026-08-19).
///
/// Own partition (not :composition) because BOTH :manager and :composition
/// consume it and :composition already imports :manager - a shared leaf
/// avoids the partition cycle.

module;
#include "Core/Prelude.h"

export module foundation.scene:frame_time;

import foundation.core;

using namespace foundation::core;

export namespace foundation::scene
{

    struct FrameTime
    {
        f32 rawDt = 0.0f;        // host dt, unscaled
        f32 contextScale = 1.0f; // app-wide term
        f32 groupScale = 1.0f;   // the group / instance term
        f32 sceneScale = 1.0f;   // the per-scene term
        f32 fixedStep = 1.0f / 60.0f;

        FrameTime() = default;
        FrameTime(f32 raw, f32 context = 1.0f, f32 group = 1.0f, f32 scene = 1.0f,
                  f32 step = 1.0f / 60.0f) noexcept
            : rawDt(raw), contextScale(context), groupScale(group), sceneScale(scene),
              fixedStep(step)
        {
        }

        // The dt a context-level subsystem sees for the variable lane (host x context).
        [[nodiscard]] f32 ContextDt() const noexcept { return rawDt * contextScale; }

        // The dt a scene's variable lane sees (host x context x group x scene).
        [[nodiscard]] f32 SceneDt() const noexcept
        {
            return rawDt * contextScale * groupScale * sceneScale;
        }
    };

} // namespace foundation::scene
