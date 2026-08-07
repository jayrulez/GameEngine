// Draconic Foundation - :fixed_stepper partition.
//
// FixedStepper: the fixed-timestep accumulator (pure, no clock - callers feed dt).
// Advance() drains whole steps from the accumulator, clamped to maxSteps per call
// (spiral-of-death guard: the EXCESS TIME IS DROPPED, not deferred). Alpha() is the
// leftover fraction of a step - the interpolation weight for consumers blending
// fixed-rate state (physics poses). Used per-SCENE (each scene owns its stepper and
// time scale - the simulatable unit) and by the application host's app-level lane.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:fixed_stepper;

import :base;

export namespace draconic::foundation
{
    struct FixedStepper
    {
        f32 step = 1.0f / 60.0f;
        u32 maxSteps = 4;
        f32 accumulator = 0.0f;

        [[nodiscard]] u32 Advance(f32 deltaTime)
        {
            if (step <= 0.0f)
            {
                accumulator = 0.0f;
                return 0;
            } // a zero step must not spin
            if (deltaTime > 0.0f)
            {
                accumulator += deltaTime;
            }
            u32 steps = 0;
            while (accumulator >= step)
            {
                accumulator -= step;
                ++steps;
            }
            if (steps > maxSteps)
            {
                steps = maxSteps;
            } // the excess was already drained: dropped
            return steps;
        }

        [[nodiscard]] f32 Alpha() const noexcept
        {
            return (step > 0.0f) ? (accumulator / step) : 0.0f;
        }
    };
}
