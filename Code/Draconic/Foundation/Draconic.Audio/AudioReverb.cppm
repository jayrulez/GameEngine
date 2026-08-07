// Draconic::Audio - the `:reverb` partition.
//
// A Freeverb-style Schroeder reverberator (public-domain topology: 8 parallel damped
// combs + 4 series allpasses per channel, stereo-spread on the right) as PURE DSP -
// no miniaudio contact, so the state machine unit-tests headless. EngineImpl wraps it
// in a custom ma_node for bus-effect chains and per-scene zone reverb.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.audio:reverb;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::audio
{
    struct AudioReverbParams
    {
        f32 roomSize = 0.6f; // 0..1 (comb feedback)
        f32 damping = 0.4f;  // 0..1 (high-frequency decay inside the tail)
        f32 wet = 0.4f;      // 0..1 mix (0 = fully dry passthrough)
        // Dry passthrough level: < 0 = the classic INSERT mix (1 - wet); an explicit
        // 0 makes a WET-ONLY node (aux-send reverb: per-voice sends feed it in
        // parallel with the dry path, so the dry signal must not pass through again).
        f32 dry = -1.0f;
    };

    /// Stereo Freeverb state. Buffer lengths scale from the canonical 44.1 kHz tunings.
    class FreeverbState
    {
    public:
        void Initialize(u32 sampleRate)
        {
            static constexpr u32 kCombTunings[kCombCount] = {1116, 1188, 1277, 1356,
                                                             1422, 1491, 1557, 1617};
            static constexpr u32 kAllpassTunings[kAllpassCount] = {556, 441, 341, 225};
            static constexpr u32 kStereoSpread = 23;

            const f32 scale = static_cast<f32>(sampleRate) / 44100.0f;
            for (u32 channel = 0; channel < 2; ++channel)
            {
                const u32 spread = channel == 1 ? kStereoSpread : 0;
                for (u32 i = 0; i < kCombCount; ++i)
                {
                    const u32 length = Max<u32>(
                        4, static_cast<u32>(static_cast<f32>(kCombTunings[i] + spread) * scale));
                    m_comb[channel][i].buffer.Resize(length);
                    for (f32& sample : m_comb[channel][i].buffer)
                    {
                        sample = 0.0f;
                    }
                    m_comb[channel][i].cursor = 0;
                    m_comb[channel][i].filterStore = 0.0f;
                }
                for (u32 i = 0; i < kAllpassCount; ++i)
                {
                    const u32 length = Max<u32>(
                        2, static_cast<u32>(static_cast<f32>(kAllpassTunings[i] + spread) * scale));
                    m_allpass[channel][i].buffer.Resize(length);
                    for (f32& sample : m_allpass[channel][i].buffer)
                    {
                        sample = 0.0f;
                    }
                    m_allpass[channel][i].cursor = 0;
                }
            }
            m_initialized = true;
        }

        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

        void SetParams(const AudioReverbParams& params) noexcept
        {
            m_feedback = 0.7f + Clamp(params.roomSize, 0.0f, 1.0f) * 0.28f;
            m_damp = Clamp(params.damping, 0.0f, 1.0f) * 0.4f;
            m_wet = Clamp(params.wet, 0.0f, 1.0f);
            m_dry = params.dry < 0.0f ? 1.0f - m_wet : Clamp(params.dry, 0.0f, 1.0f);
        }

        [[nodiscard]] f32 Wet() const noexcept { return m_wet; }
        [[nodiscard]] f32 Dry() const noexcept { return m_dry; }

        /// Interleaved-stereo in-place processing: out = in * dry + tail * wet (dry
        /// defaults to 1 - wet, the classic insert mix; 0 = wet-only send node).
        /// Mono callers duplicate the channel. Denormal-flushed.
        void ProcessStereo(const f32* input, f32* output, u32 frameCount)
        {
            constexpr f32 kFixedGain = 0.015f; // canonical freeverb input gain
            for (u32 frame = 0; frame < frameCount; ++frame)
            {
                const f32 inL = input[frame * 2 + 0];
                const f32 inR = input[frame * 2 + 1];
                const f32 feed = (inL + inR) * kFixedGain;
                f32 outL = 0.0f;
                f32 outR = 0.0f;
                for (u32 i = 0; i < kCombCount; ++i)
                {
                    outL += CombProcess(m_comb[0][i], feed);
                    outR += CombProcess(m_comb[1][i], feed);
                }
                for (u32 i = 0; i < kAllpassCount; ++i)
                {
                    outL = AllpassProcess(m_allpass[0][i], outL);
                    outR = AllpassProcess(m_allpass[1][i], outR);
                }
                output[frame * 2 + 0] = inL * m_dry + outL * m_wet * 3.0f;
                output[frame * 2 + 1] = inR * m_dry + outR * m_wet * 3.0f;
            }
        }

    private:
        static constexpr u32 kCombCount = 8;
        static constexpr u32 kAllpassCount = 4;

        struct Comb
        {
            Array<f32> buffer;
            u32 cursor = 0;
            f32 filterStore = 0.0f;
        };
        struct Allpass
        {
            Array<f32> buffer;
            u32 cursor = 0;
        };

        [[nodiscard]] static f32 Undenormal(f32 value) noexcept
        {
            return (value > -1.0e-18f && value < 1.0e-18f) ? 0.0f : value;
        }

        f32 CombProcess(Comb& comb, f32 input) noexcept
        {
            const f32 output = comb.buffer[comb.cursor];
            comb.filterStore = Undenormal(output * (1.0f - m_damp) + comb.filterStore * m_damp);
            comb.buffer[comb.cursor] = Undenormal(input + comb.filterStore * m_feedback);
            if (++comb.cursor >= comb.buffer.Size())
            {
                comb.cursor = 0;
            }
            return output;
        }

        f32 AllpassProcess(Allpass& allpass, f32 input) noexcept
        {
            const f32 buffered = allpass.buffer[allpass.cursor];
            const f32 output = buffered - input;
            allpass.buffer[allpass.cursor] = Undenormal(input + buffered * 0.5f);
            if (++allpass.cursor >= allpass.buffer.Size())
            {
                allpass.cursor = 0;
            }
            return output;
        }

        Comb m_comb[2][kCombCount];
        Allpass m_allpass[2][kAllpassCount];
        f32 m_feedback = 0.84f;
        f32 m_damp = 0.2f;
        f32 m_wet = 0.4f;
        f32 m_dry = 0.6f; // tracks 1 - wet unless params.dry pins it (send mode)
        bool m_initialized = false;
    };
}
