// Draconic::Audio - the `:cue` partition.
//
// SoundCue (audio.md P3): the container primitive - ONE trigger, one of N clip variants,
// weighted, with cue-level pitch/volume randomization (Godot's AudioStreamRandomizer
// shape; Traktor's grain banks stay the long-term north star). The cue itself is pure
// DATA + a pure resolution function: the caller owns the play state (last pick /
// sequential cursor) and the RNG, so resolution is deterministic under test and the
// engine stays clip-based - a resolved pick plays through the ordinary voice path.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.audio:cue;

import draconic.foundation;
import :clip;

using namespace draconic::foundation;

export namespace draconic::audio
{
    enum class SoundCueMode : u8
    {
        RandomNoRepeat = 0, // weighted random, never the SAME variant twice in a row
        Random,             // weighted random, repeats allowed
        Sequential,         // round-robin in slot order
    };

    struct SoundCueVariant
    {
        RefPtr<AudioClip> clip; // null = empty slot (skipped)
        f32 weight = 1.0f;      // <= 0 = disabled
    };

    /// The runtime cue product (resource-factory built; also hand-buildable in tests).
    class SoundCue final : public Object
    {
        DRACONIC_OBJECT(SoundCue, Object)
    public:
        Array<SoundCueVariant> variants;
        SoundCueMode mode = SoundCueMode::RandomNoRepeat;
        // Cue-level jitter, applied per trigger on top of the play params.
        f32 pitchMin = 1.0f;
        f32 pitchMax = 1.0f;
        f32 volumeMin = 1.0f;
        f32 volumeMax = 1.0f;
    };

    /// One resolved trigger: which variant, and this trigger's jittered pitch/volume
    /// MULTIPLIERS (fold them onto the caller's base params).
    struct SoundCuePick
    {
        i32 variantIndex = -1; // -1 = no playable variant
        f32 pitch = 1.0f;
        f32 volume = 1.0f;
    };

    /// Resolves one trigger. `lastVariantIndex` is the caller's previous pick (-1 =
    /// none) for RandomNoRepeat; `sequentialCursor` advances in Sequential mode. Both
    /// are caller-owned play state (per component / per one-shot cue identity).
    [[nodiscard]] inline SoundCuePick ResolveSoundCue(const SoundCue& cue, Random& rng,
                                                      i32 lastVariantIndex, u32& sequentialCursor)
    {
        SoundCuePick pick;

        // The eligible set: non-null clip, positive weight.
        i32 eligible[64];
        usize eligibleCount = 0;
        f32 totalWeight = 0.0f;
        const usize slotCount = Min<usize>(cue.variants.Size(), 64);
        for (usize i = 0; i < slotCount; ++i)
        {
            const SoundCueVariant& variant = cue.variants[i];
            if (variant.clip.Get() == nullptr || variant.weight <= 0.0f)
            {
                continue;
            }
            eligible[eligibleCount++] = static_cast<i32>(i);
            totalWeight += variant.weight;
        }
        if (eligibleCount == 0)
        {
            return pick;
        }

        if (cue.mode == SoundCueMode::Sequential)
        {
            pick.variantIndex = eligible[sequentialCursor % eligibleCount];
            ++sequentialCursor;
        }
        else
        {
            // No-repeat with >1 choice: excise the previous pick from the wheel.
            const bool avoidLast = cue.mode == SoundCueMode::RandomNoRepeat && eligibleCount > 1 &&
                                   lastVariantIndex >= 0;
            f32 wheelWeight = totalWeight;
            if (avoidLast)
            {
                for (usize i = 0; i < eligibleCount; ++i)
                {
                    if (eligible[i] == lastVariantIndex)
                    {
                        wheelWeight -= cue.variants[static_cast<usize>(lastVariantIndex)].weight;
                        break;
                    }
                }
            }
            f32 roll = rng.NextFloat(0.0f, wheelWeight);
            for (usize i = 0; i < eligibleCount; ++i)
            {
                const i32 index = eligible[i];
                if (avoidLast && index == lastVariantIndex)
                {
                    continue;
                }
                pick.variantIndex = index; // numeric-drift fallback = last VALID candidate
                const f32 weight = cue.variants[static_cast<usize>(index)].weight;
                if (roll < weight)
                {
                    break;
                }
                roll -= weight;
            }
        }

        pick.pitch =
            rng.NextFloat(Min(cue.pitchMin, cue.pitchMax), Max(cue.pitchMin, cue.pitchMax));
        pick.volume =
            rng.NextFloat(Min(cue.volumeMin, cue.volumeMax), Max(cue.volumeMin, cue.volumeMax));
        return pick;
    }

    DRACONIC_DEFINE_OBJECT(SoundCue, "draconic::audio")
}
