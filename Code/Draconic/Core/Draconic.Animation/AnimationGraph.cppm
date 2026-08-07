/// Draconic::Animation - the `:graph` partition.
///
/// The animation graph stack, ported faithfully from Sedulous.Animation: state nodes (clip + 1D/2D
/// blend trees), per-bone masks, parameters + conditions + transitions, states, layers, the graph
/// definition, and the per-instance AnimationGraphPlayer (state machine + layer blending). Resource
/// refs (editor/serialization) are dropped - this is the runtime foundation. Polymorphic node
/// dispatch uses a NodeType tag + static_cast (the engine builds with -fno-rtti, so no dynamic_cast).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.animation:graph;

import draconic.foundation;
import :skeleton; // Skeleton, Bone, BoneTransform
import :clip;     // AnimationClip, AnimationEventHandler
import :sampler;  // SampleClip, BlendPoses

using namespace draconic::foundation;

export namespace draconic::animation
{

    // ---- state nodes ---------------------------------------------------------------------------

    enum class NodeType
    {
        Clip,
        BlendTree1D,
        BlendTree2D
    };

    // A node that produces an animation pose. Implemented by ClipStateNode + the blend trees.
    class IAnimationStateNode
    {
    public:
        virtual ~IAnimationStateNode() = default;
        [[nodiscard]] virtual NodeType Type() const noexcept = 0;
        // Evaluate at normalized time [0..1] into outPoses.
        virtual void Evaluate(const Skeleton& skeleton, f32 normalizedTime,
                              Span<BoneTransform> outPoses) const = 0;
        [[nodiscard]] virtual f32 Duration() const noexcept = 0;
        virtual void FireEvents(f32 prevNormalizedTime, f32 currentNormalizedTime, bool looping,
                                const AnimationEventHandler& handler) const = 0;
    };

    // Wraps a single AnimationClip (borrowed).
    class ClipStateNode final : public IAnimationStateNode
    {
    public:
        explicit ClipStateNode(AnimationClip* clip) : m_clip(clip) {}
        [[nodiscard]] NodeType Type() const noexcept override { return NodeType::Clip; }
        [[nodiscard]] AnimationClip* Clip() const noexcept { return m_clip; }

        void Evaluate(const Skeleton& skeleton, f32 normalizedTime,
                      Span<BoneTransform> outPoses) const override
        {
            if (m_clip == nullptr)
            {
                return;
            }
            SampleClip(*m_clip, skeleton, normalizedTime * m_clip->duration, outPoses);
        }
        [[nodiscard]] f32 Duration() const noexcept override
        {
            return m_clip != nullptr ? m_clip->duration : 0.0f;
        }

        void FireEvents(f32 prevNorm, f32 currentNorm, bool looping,
                        const AnimationEventHandler& handler) const override
        {
            if (m_clip == nullptr || !handler || m_clip->Events().IsEmpty() ||
                m_clip->duration <= 0.0f)
            {
                return;
            }
            const f32 prevAbs = prevNorm * m_clip->duration;
            const f32 curAbs = currentNorm * m_clip->duration;
            if (looping && currentNorm < prevNorm)
            {
                // wrapped: (prevAbs, Duration] then [0, curAbs]
                for (const AnimationEvent& e : m_clip->Events())
                {
                    if (e.time > prevAbs && e.time <= m_clip->duration)
                    {
                        handler(StringView{e.name}, e.time);
                    }
                }
                for (const AnimationEvent& e : m_clip->Events())
                {
                    if (e.time <= curAbs)
                    {
                        handler(StringView{e.name}, e.time);
                    }
                }
            }
            else
            {
                m_clip->FireEvents(prevAbs, curAbs, handler);
            }
        }

    private:
        AnimationClip* m_clip = nullptr;
    };

    // ---- bone mask -----------------------------------------------------------------------------

    class BoneMask
    {
    public:
        explicit BoneMask(i32 boneCount, f32 defaultWeight = 1.0f)
        {
            m_weights.Resize(boneCount < 0 ? 0 : static_cast<usize>(boneCount));
            for (usize i = 0; i < m_weights.Size(); ++i)
            {
                m_weights[i] = defaultWeight;
            }
        }
        [[nodiscard]] i32 BoneCount() const noexcept { return static_cast<i32>(m_weights.Size()); }
        [[nodiscard]] f32 GetWeight(i32 boneIndex) const
        {
            return (boneIndex >= 0 && static_cast<usize>(boneIndex) < m_weights.Size())
                       ? m_weights[static_cast<usize>(boneIndex)]
                       : 0.0f;
        }
        void SetWeight(i32 boneIndex, f32 weight)
        {
            if (boneIndex >= 0 && static_cast<usize>(boneIndex) < m_weights.Size())
            {
                m_weights[static_cast<usize>(boneIndex)] = Clamp(weight, 0.0f, 1.0f);
            }
        }
        void SetAll(f32 weight)
        {
            const f32 c = Clamp(weight, 0.0f, 1.0f);
            for (usize i = 0; i < m_weights.Size(); ++i)
            {
                m_weights[i] = c;
            }
        }
        void SetBoneChainWeight(const Skeleton& skeleton, i32 boneIndex, f32 weight)
        {
            if (boneIndex < 0 || boneIndex >= skeleton.BoneCount())
            {
                return;
            }
            const f32 c = Clamp(weight, 0.0f, 1.0f);
            SetWeight(boneIndex, c);
            if (const Bone* bone = skeleton.GetBone(boneIndex))
            {
                for (i32 child : bone->children)
                {
                    SetBoneChainWeight(skeleton, child, c);
                }
            }
        }
        [[nodiscard]] Span<const f32> Weights() const noexcept
        {
            return {m_weights.Data(), m_weights.Size()};
        }

    private:
        Array<f32> m_weights;
    };

    // ---- blend trees ---------------------------------------------------------------------------

    struct BlendTree1DEntry
    {
        f32 threshold = 0.0f;
        AnimationClip* clip = nullptr;
    };

    // Blends clips along one float axis (Parameter). Entries kept sorted by threshold.
    class BlendTree1D final : public IAnimationStateNode
    {
    public:
        i32 parameterIndex = -1; // graph parameter that drives this tree
        f32 parameter = 0.0f;

        [[nodiscard]] NodeType Type() const noexcept override { return NodeType::BlendTree1D; }
        [[nodiscard]] Array<BlendTree1DEntry>& Entries() noexcept { return m_entries; }

        void AddEntry(f32 threshold, AnimationClip* clip)
        {
            // Append then bubble left into sorted-by-threshold position (Array has no Insert).
            m_entries.PushBack(BlendTree1DEntry{threshold, clip});
            for (usize i = m_entries.Size() - 1; i > 0; --i)
            {
                if (m_entries[i - 1].threshold <= m_entries[i].threshold)
                {
                    break;
                }
                const BlendTree1DEntry tmp = m_entries[i - 1];
                m_entries[i - 1] = m_entries[i];
                m_entries[i] = tmp;
            }
        }

        void Evaluate(const Skeleton& skeleton, f32 normalizedTime,
                      Span<BoneTransform> outPoses) const override
        {
            if (m_entries.IsEmpty())
            {
                return;
            }
            if (m_entries.Size() == 1)
            {
                SampleEntry(0, skeleton, normalizedTime, outPoses);
                return;
            }
            if (parameter <= m_entries[0].threshold)
            {
                SampleEntry(0, skeleton, normalizedTime, outPoses);
                return;
            }
            if (parameter >= m_entries[m_entries.Size() - 1].threshold)
            {
                SampleEntry(m_entries.Size() - 1, skeleton, normalizedTime, outPoses);
                return;
            }

            usize lowIdx = 0, highIdx = 1;
            for (usize i = 0; i + 1 < m_entries.Size(); ++i)
            {
                if (parameter >= m_entries[i].threshold && parameter <= m_entries[i + 1].threshold)
                {
                    lowIdx = i;
                    highIdx = i + 1;
                    break;
                }
            }
            AnimationClip* clipA = m_entries[lowIdx].clip;
            AnimationClip* clipB = m_entries[highIdx].clip;
            if (clipA == nullptr && clipB == nullptr)
            {
                return;
            }
            if (clipA == nullptr)
            {
                SampleEntry(highIdx, skeleton, normalizedTime, outPoses);
                return;
            }
            if (clipB == nullptr)
            {
                SampleEntry(lowIdx, skeleton, normalizedTime, outPoses);
                return;
            }

            const f32 range = m_entries[highIdx].threshold - m_entries[lowIdx].threshold;
            const f32 blend =
                (range > 0.0f) ? (parameter - m_entries[lowIdx].threshold) / range : 0.0f;
            const usize n = static_cast<usize>(skeleton.BoneCount());
            Array<BoneTransform> a;
            a.Resize(n);
            Array<BoneTransform> b;
            b.Resize(n);
            SampleClip(*clipA, skeleton, normalizedTime * clipA->duration,
                       Span<BoneTransform>{a.Data(), n});
            SampleClip(*clipB, skeleton, normalizedTime * clipB->duration,
                       Span<BoneTransform>{b.Data(), n});
            BlendPoses(Span<const BoneTransform>{a.Data(), n},
                       Span<const BoneTransform>{b.Data(), n}, blend, outPoses);
        }

        [[nodiscard]] f32 Duration() const noexcept override
        {
            f32 bestDist = 3.4e38f, bestDuration = 0.0f;
            for (const BlendTree1DEntry& e : m_entries)
            {
                const f32 dist = Abs(e.threshold - parameter);
                if (dist < bestDist && e.clip != nullptr)
                {
                    bestDist = dist;
                    bestDuration = e.clip->duration;
                }
            }
            return bestDuration;
        }
        void FireEvents(f32, f32, bool, const AnimationEventHandler&) const override {
        } // blend trees don't fire clip events

    private:
        void SampleEntry(usize idx, const Skeleton& skeleton, f32 normalizedTime,
                         Span<BoneTransform> outPoses) const
        {
            AnimationClip* clip = m_entries[idx].clip;
            if (clip != nullptr)
            {
                SampleClip(*clip, skeleton, normalizedTime * clip->duration, outPoses);
            }
        }
        Array<BlendTree1DEntry> m_entries;
    };

    struct BlendTree2DEntry
    {
        Float2 position = Float2{0, 0};
        AnimationClip* clip = nullptr;
    };

    // Blends clips in a 2D parameter space via inverse-distance weighting.
    class BlendTree2D final : public IAnimationStateNode
    {
    public:
        i32 parameterIndexX = -1, parameterIndexY = -1;
        f32 parameterX = 0.0f, parameterY = 0.0f;

        [[nodiscard]] NodeType Type() const noexcept override { return NodeType::BlendTree2D; }
        [[nodiscard]] Array<BlendTree2DEntry>& Entries() noexcept { return m_entries; }
        void AddEntry(Float2 position, AnimationClip* clip)
        {
            m_entries.PushBack(BlendTree2DEntry{position, clip});
        }
        void AddEntry(f32 x, f32 y, AnimationClip* clip)
        {
            m_entries.PushBack(BlendTree2DEntry{Float2{x, y}, clip});
        }

        void Evaluate(const Skeleton& skeleton, f32 normalizedTime,
                      Span<BoneTransform> outPoses) const override
        {
            if (m_entries.IsEmpty())
            {
                return;
            }
            if (m_entries.Size() == 1)
            {
                SampleEntry(0, skeleton, normalizedTime, outPoses);
                return;
            }

            const Float2 paramPos{parameterX, parameterY};
            Array<f32> weights;
            weights.Resize(m_entries.Size());
            f32 totalWeight = 0.0f;
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                const f32 dist = Length(paramPos - m_entries[i].position);
                if (dist < 0.0001f)
                {
                    SampleEntry(i, skeleton, normalizedTime, outPoses);
                    return;
                } // on top of an entry
                weights[i] = 1.0f / dist;
                totalWeight += weights[i];
            }
            if (totalWeight > 0.0f)
            {
                for (usize i = 0; i < weights.Size(); ++i)
                {
                    weights[i] /= totalWeight;
                }
            }

            const usize n = static_cast<usize>(skeleton.BoneCount());
            Array<BoneTransform> temp;
            temp.Resize(n);
            bool firstSample = true;
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (weights[i] < 0.001f || m_entries[i].clip == nullptr)
                {
                    continue;
                }
                AnimationClip* clip = m_entries[i].clip;
                SampleClip(*clip, skeleton, normalizedTime * clip->duration,
                           Span<BoneTransform>{temp.Data(), n});
                if (firstSample)
                {
                    for (usize b = 0; b < n && b < outPoses.Size(); ++b)
                    {
                        outPoses[b] = temp[b];
                    }
                    firstSample = false;
                    if (weights[i] > 0.999f)
                    {
                        return;
                    }
                    continue;
                }
                f32 accumulated = 0.0f;
                for (usize j = 0; j < i; ++j)
                {
                    if (weights[j] >= 0.001f && m_entries[j].clip != nullptr)
                    {
                        accumulated += weights[j];
                    }
                }
                const f32 relative = weights[i] / (accumulated + weights[i]);
                BlendPoses(Span<const BoneTransform>{outPoses.Data(), outPoses.Size()},
                           Span<const BoneTransform>{temp.Data(), n}, relative, outPoses);
            }
        }

        [[nodiscard]] f32 Duration() const noexcept override
        {
            const Float2 paramPos{parameterX, parameterY};
            f32 totalWeight = 0.0f, totalDuration = 0.0f;
            for (const BlendTree2DEntry& e : m_entries)
            {
                if (e.clip == nullptr)
                {
                    continue;
                }
                const f32 dist = Length(paramPos - e.position);
                if (dist < 0.0001f)
                {
                    return e.clip->duration;
                }
                const f32 w = 1.0f / dist;
                totalWeight += w;
                totalDuration += w * e.clip->duration;
            }
            return totalWeight > 0.0f ? totalDuration / totalWeight : 0.0f;
        }
        void FireEvents(f32, f32, bool, const AnimationEventHandler&) const override {}

    private:
        void SampleEntry(usize idx, const Skeleton& skeleton, f32 normalizedTime,
                         Span<BoneTransform> outPoses) const
        {
            AnimationClip* clip = m_entries[idx].clip;
            if (clip != nullptr)
            {
                SampleClip(*clip, skeleton, normalizedTime * clip->duration, outPoses);
            }
        }
        Array<BlendTree2DEntry> m_entries;
    };

    // ---- parameters + conditions + transitions -------------------------------------------------

    enum class AnimationParameterType
    {
        Float,
        Int,
        Bool,
        Trigger
    };

    // A named value driving transitions/blend trees. Value type (copied into the player's runtime set).
    class AnimationGraphParameter
    {
    public:
        AnimationGraphParameter() = default;
        AnimationGraphParameter(StringView name, AnimationParameterType t) : type(t), m_name(name)
        {
        }

        AnimationParameterType type = AnimationParameterType::Float;
        f32 floatValue = 0.0f;
        i32 intValue = 0;
        bool boolValue = false;

        [[nodiscard]] const String& Name() const noexcept { return m_name; }
        void ConsumeTrigger()
        {
            if (type == AnimationParameterType::Trigger)
            {
                boolValue = false;
            }
        }

    private:
        String m_name;
    };

    enum class ComparisonOp
    {
        Equal,
        NotEqual,
        Greater,
        Less,
        GreaterEqual,
        LessEqual
    };

    // One transition condition: compare a parameter against a threshold.
    struct AnimationGraphCondition
    {
        i32 parameterIndex = 0;
        ComparisonOp op = ComparisonOp::Equal;
        f32 threshold = 0.0f;

        AnimationGraphCondition() = default;
        AnimationGraphCondition(i32 paramIndex, ComparisonOp o, f32 thr = 0.0f)
            : parameterIndex(paramIndex), op(o), threshold(thr)
        {
        }

        [[nodiscard]] bool Evaluate(const AnimationGraphParameter* param) const
        {
            if (param == nullptr)
            {
                return false;
            }
            switch (param->type)
            {
            case AnimationParameterType::Float:
                return CompareFloat(param->floatValue);
            case AnimationParameterType::Int:
                return CompareFloat(static_cast<f32>(param->intValue));
            case AnimationParameterType::Bool:
            case AnimationParameterType::Trigger:
                switch (op)
                {
                case ComparisonOp::Equal:
                    return param->boolValue == (threshold > 0.5f);
                case ComparisonOp::NotEqual:
                    return param->boolValue != (threshold > 0.5f);
                default:
                    return param->boolValue;
                }
            }
            return false;
        }

    private:
        [[nodiscard]] bool CompareFloat(f32 value) const
        {
            switch (op)
            {
            case ComparisonOp::Equal:
                return Abs(value - threshold) < 0.0001f;
            case ComparisonOp::NotEqual:
                return Abs(value - threshold) >= 0.0001f;
            case ComparisonOp::Greater:
                return value > threshold;
            case ComparisonOp::Less:
                return value < threshold;
            case ComparisonOp::GreaterEqual:
                return value >= threshold;
            case ComparisonOp::LessEqual:
                return value <= threshold;
            }
            return false;
        }
    };

    // A transition between states; fires when all conditions hold (+ optional exit-time gate).
    class AnimationGraphTransition
    {
    public:
        i32 sourceStateIndex = -1; // -1 = "Any State"
        i32 destStateIndex = 0;
        f32 duration = 0.25f; // cross-fade seconds
        bool hasExitTime = false;
        f32 exitTime = 1.0f;
        i32 priority = 0; // lower = higher priority

        [[nodiscard]] Array<AnimationGraphCondition>& Conditions() noexcept { return m_conditions; }
        [[nodiscard]] const Array<AnimationGraphCondition>& Conditions() const noexcept
        {
            return m_conditions;
        }

        void AddBoolCondition(i32 parameterIndex, bool expected = true)
        {
            m_conditions.PushBack(AnimationGraphCondition{parameterIndex, ComparisonOp::Equal,
                                                          expected ? 1.0f : 0.0f});
        }
        void AddFloatCondition(i32 parameterIndex, ComparisonOp op, f32 threshold)
        {
            m_conditions.PushBack(AnimationGraphCondition{parameterIndex, op, threshold});
        }
        void AddIntCondition(i32 parameterIndex, ComparisonOp op, i32 threshold)
        {
            m_conditions.PushBack(
                AnimationGraphCondition{parameterIndex, op, static_cast<f32>(threshold)});
        }

        // All conditions must hold against the parameter set. Empty -> unconditional (true).
        [[nodiscard]] bool EvaluateConditions(Span<const AnimationGraphParameter> parameters) const
        {
            for (const AnimationGraphCondition& c : m_conditions)
            {
                if (c.parameterIndex < 0 ||
                    static_cast<usize>(c.parameterIndex) >= parameters.Size())
                {
                    return false;
                }
                if (!c.Evaluate(&parameters[static_cast<usize>(c.parameterIndex)]))
                {
                    return false;
                }
            }
            return true;
        }

    private:
        Array<AnimationGraphCondition> m_conditions;
    };

    // ---- states + layers + graph ---------------------------------------------------------------

    // A state wraps a node (clip or blend tree) + playback settings. Owns the node when constructed with
    // a UniquePtr; borrows it (no delete) when constructed with a raw pointer.
    class AnimationGraphState
    {
    public:
        AnimationGraphState(StringView name, IAnimationStateNode* node) : m_name(name), m_node(node)
        {
        }
        AnimationGraphState(StringView name, UniquePtr<IAnimationStateNode> node)
            : m_name(name), m_node(node.Get()),
              m_owned(static_cast<UniquePtr<IAnimationStateNode>&&>(node)), m_ownsNode(true)
        {
        }

        f32 speed = 1.0f;
        bool loop = true;

        [[nodiscard]] const String& Name() const noexcept { return m_name; }
        [[nodiscard]] IAnimationStateNode* Node() const noexcept { return m_node; }
        [[nodiscard]] bool OwnsNode() const noexcept { return m_ownsNode; }
        [[nodiscard]] f32 Duration() const noexcept
        {
            return m_node != nullptr ? m_node->Duration() : 0.0f;
        }

    private:
        String m_name;
        IAnimationStateNode* m_node = nullptr;  // active node (borrowed or == m_owned)
        UniquePtr<IAnimationStateNode> m_owned; // set when owning
        bool m_ownsNode = false;
    };

    enum class LayerBlendMode
    {
        Override,
        Additive
    };

    // A layer: states + transitions + an optional bone mask. Layer 0 is the base; others blend on top.
    class AnimationLayer
    {
    public:
        explicit AnimationLayer(StringView name) : m_name(name) {}

        i32 defaultStateIndex = 0;
        LayerBlendMode blendMode = LayerBlendMode::Override;
        f32 weight = 1.0f;

        [[nodiscard]] const String& Name() const noexcept { return m_name; }
        [[nodiscard]] Array<UniquePtr<AnimationGraphState>>& States() noexcept { return m_states; }
        [[nodiscard]] Array<UniquePtr<AnimationGraphTransition>>& Transitions() noexcept
        {
            return m_transitions;
        }
        [[nodiscard]] BoneMask* Mask() const noexcept { return m_mask.Get(); }
        void SetMask(UniquePtr<BoneMask> mask)
        {
            m_mask = static_cast<UniquePtr<BoneMask>&&>(mask);
        }

        i32 AddState(UniquePtr<AnimationGraphState> state)
        {
            const i32 idx = static_cast<i32>(m_states.Size());
            m_states.PushBack(static_cast<UniquePtr<AnimationGraphState>&&>(state));
            return idx;
        }
        void AddTransition(UniquePtr<AnimationGraphTransition> t)
        {
            m_transitions.PushBack(static_cast<UniquePtr<AnimationGraphTransition>&&>(t));
        }
        [[nodiscard]] AnimationGraphState* GetState(i32 index) const
        {
            return (index >= 0 && static_cast<usize>(index) < m_states.Size())
                       ? m_states[static_cast<usize>(index)].Get()
                       : nullptr;
        }
        [[nodiscard]] i32 FindStateIndex(StringView name) const
        {
            for (usize i = 0; i < m_states.Size(); ++i)
            {
                if (m_states[i]->Name() == name)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

    private:
        String m_name;
        Array<UniquePtr<AnimationGraphState>> m_states;
        Array<UniquePtr<AnimationGraphTransition>> m_transitions;
        UniquePtr<BoneMask> m_mask;
    };

    // Shared graph definition: parameters + layers. Multiple players can reference one graph.
    // A resource product (Object) so a cooked AnimationGraphSource can build into it.
    class AnimationGraph : public Object
    {
        DRACONIC_OBJECT(AnimationGraph, Object)
    public:
        [[nodiscard]] Array<AnimationGraphParameter>& Parameters() noexcept { return m_parameters; }
        [[nodiscard]] const Array<AnimationGraphParameter>& Parameters() const noexcept
        {
            return m_parameters;
        }
        [[nodiscard]] Array<UniquePtr<AnimationLayer>>& Layers() noexcept { return m_layers; }

        i32 AddParameter(StringView name, AnimationParameterType type)
        {
            const i32 idx = static_cast<i32>(m_parameters.Size());
            m_parameters.PushBack(AnimationGraphParameter{name, type});
            return idx;
        }
        [[nodiscard]] i32 FindParameter(StringView name) const
        {
            for (usize i = 0; i < m_parameters.Size(); ++i)
            {
                if (m_parameters[i].Name() == name)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }
        [[nodiscard]] AnimationGraphParameter* GetParameter(i32 index)
        {
            return (index >= 0 && static_cast<usize>(index) < m_parameters.Size())
                       ? &m_parameters[static_cast<usize>(index)]
                       : nullptr;
        }
        i32 AddLayer(UniquePtr<AnimationLayer> layer)
        {
            const i32 idx = static_cast<i32>(m_layers.Size());
            m_layers.PushBack(static_cast<UniquePtr<AnimationLayer>&&>(layer));
            return idx;
        }

    private:
        Array<AnimationGraphParameter> m_parameters;
        Array<UniquePtr<AnimationLayer>> m_layers;
    };

    // ---- graph player --------------------------------------------------------------------------

    // Per-layer runtime state (current/previous state + cross-fade + scratch poses).
    struct AnimationGraphLayerRuntime
    {
        i32 currentStateIndex = -1;
        f32 currentTime = 0.0f;
        i32 previousStateIndex = -1;
        f32 previousTime = 0.0f;
        f32 transitionElapsed = 0.0f;
        f32 transitionDuration = 0.0f;
        bool isTransitioning = false;
        Array<BoneTransform> poses;
        Array<BoneTransform> prevPoses;

        void Init(usize boneCount)
        {
            poses.Resize(boneCount);
            prevPoses.Resize(boneCount);
        }
        void Reset(i32 defaultStateIndex)
        {
            currentStateIndex = defaultStateIndex;
            currentTime = 0.0f;
            previousStateIndex = -1;
            previousTime = 0.0f;
            transitionElapsed = 0.0f;
            transitionDuration = 0.0f;
            isTransitioning = false;
        }
    };

    // Evaluates an AnimationGraph for one skeleton instance (state machine + layer blending).
    class AnimationGraphPlayer
    {
    public:
        AnimationGraphPlayer(AnimationGraph& graph, Skeleton& skeleton)
            : m_graph(&graph), m_skeleton(&skeleton)
        {
            const usize boneCount = static_cast<usize>(skeleton.BoneCount());
            m_finalPoses.Resize(boneCount);
            m_skinningMatrices.Resize(boneCount);
            m_prevSkinningMatrices.Resize(boneCount);
            for (usize i = 0; i < boneCount; ++i)
            {
                m_skinningMatrices[i] = Float4x4::Identity();
                m_prevSkinningMatrices[i] = Float4x4::Identity();
            }

            // Runtime parameter copy (each player has independent values).
            for (const AnimationGraphParameter& p : graph.Parameters())
            {
                m_parameters.PushBack(p);
            }

            for (auto& layer : graph.Layers())
            {
                AnimationGraphLayerRuntime rt;
                rt.Init(boneCount);
                rt.Reset(layer->defaultStateIndex);
                m_layerRuntimes.PushBack(static_cast<AnimationGraphLayerRuntime&&>(rt));
            }

            // Auto-link blend trees from their stored parameter indices (NodeType tag - no RTTI).
            for (auto& layer : graph.Layers())
            {
                for (auto& state : layer->States())
                {
                    IAnimationStateNode* node = state->Node();
                    if (node == nullptr)
                    {
                        continue;
                    }
                    if (node->Type() == NodeType::BlendTree1D)
                    {
                        auto* t = static_cast<BlendTree1D*>(node);
                        if (t->parameterIndex >= 0)
                        {
                            m_links1D.PushBack(Link1D{t, t->parameterIndex});
                        }
                    }
                    else if (node->Type() == NodeType::BlendTree2D)
                    {
                        auto* t = static_cast<BlendTree2D*>(node);
                        if (t->parameterIndexX >= 0 || t->parameterIndexY >= 0)
                        {
                            m_links2D.PushBack(Link2D{t, t->parameterIndexX, t->parameterIndexY});
                        }
                    }
                }
            }
            ResetToBind();
        }

        ~AnimationGraphPlayer() = default;
        AnimationGraphPlayer(const AnimationGraphPlayer&) = delete;
        AnimationGraphPlayer& operator=(const AnimationGraphPlayer&) = delete;

        [[nodiscard]] Skeleton& GetSkeleton() noexcept { return *m_skeleton; }
        [[nodiscard]] AnimationGraph& GetGraph() noexcept { return *m_graph; }

        // --- parameter access ---
        void SetFloat(i32 i, f32 v)
        {
            if (Valid(i))
            {
                m_parameters[static_cast<usize>(i)].floatValue = v;
            }
        }
        void SetFloat(StringView n, f32 v) { SetFloat(m_graph->FindParameter(n), v); }
        [[nodiscard]] f32 GetFloat(i32 i) const
        {
            return Valid(i) ? m_parameters[static_cast<usize>(i)].floatValue : 0.0f;
        }
        void SetInt(i32 i, i32 v)
        {
            if (Valid(i))
            {
                m_parameters[static_cast<usize>(i)].intValue = v;
            }
        }
        void SetInt(StringView n, i32 v) { SetInt(m_graph->FindParameter(n), v); }
        [[nodiscard]] i32 GetInt(i32 i) const
        {
            return Valid(i) ? m_parameters[static_cast<usize>(i)].intValue : 0;
        }
        void SetBool(i32 i, bool v)
        {
            if (Valid(i))
            {
                m_parameters[static_cast<usize>(i)].boolValue = v;
            }
        }
        void SetBool(StringView n, bool v) { SetBool(m_graph->FindParameter(n), v); }
        [[nodiscard]] bool GetBool(i32 i) const
        {
            return Valid(i) ? m_parameters[static_cast<usize>(i)].boolValue : false;
        }
        void SetTrigger(i32 i)
        {
            if (Valid(i))
            {
                m_parameters[static_cast<usize>(i)].boolValue = true;
            }
        }
        void SetTrigger(StringView n) { SetTrigger(m_graph->FindParameter(n)); }

        void SetEventHandler(AnimationEventHandler handler)
        {
            m_eventHandler = static_cast<AnimationEventHandler&&>(handler);
        }

        void Update(f32 deltaTime)
        {
            for (usize i = 0; i < m_skinningMatrices.Size(); ++i)
            {
                m_prevSkinningMatrices[i] = m_skinningMatrices[i];
            }
            SyncBlendTreeParameters();
            for (usize i = 0; i < m_graph->Layers().Size() && i < m_layerRuntimes.Size(); ++i)
            {
                UpdateLayer(*m_graph->Layers()[i], m_layerRuntimes[i], deltaTime);
            }
            for (AnimationGraphParameter& p : m_parameters)
            {
                p.ConsumeTrigger();
            }
            CombineLayers();
            m_matricesDirty = true;
        }

        [[nodiscard]] Span<const Float4x4> GetSkinningMatrices()
        {
            if (m_matricesDirty)
            {
                m_skeleton->ComputeSkinningMatrices(
                    Span<const BoneTransform>{m_finalPoses.Data(), m_finalPoses.Size()},
                    Span<Float4x4>{m_skinningMatrices.Data(), m_skinningMatrices.Size()});
                m_matricesDirty = false;
            }
            return Span<const Float4x4>{m_skinningMatrices.Data(), m_skinningMatrices.Size()};
        }
        [[nodiscard]] Span<const Float4x4> GetPrevSkinningMatrices() const
        {
            return {m_prevSkinningMatrices.Data(), m_prevSkinningMatrices.Size()};
        }
        [[nodiscard]] Span<BoneTransform> GetLocalPoses() noexcept
        {
            return {m_finalPoses.Data(), m_finalPoses.Size()};
        }

        // --- state query ---
        [[nodiscard]] i32 GetCurrentStateIndex(i32 layerIndex = 0) const
        {
            return (layerIndex >= 0 && static_cast<usize>(layerIndex) < m_layerRuntimes.Size())
                       ? m_layerRuntimes[static_cast<usize>(layerIndex)].currentStateIndex
                       : -1;
        }
        [[nodiscard]] bool IsTransitioning(i32 layerIndex = 0) const
        {
            return (layerIndex >= 0 && static_cast<usize>(layerIndex) < m_layerRuntimes.Size())
                       ? m_layerRuntimes[static_cast<usize>(layerIndex)].isTransitioning
                       : false;
        }
        [[nodiscard]] f32 GetCurrentNormalizedTime(i32 layerIndex = 0) const
        {
            return (layerIndex >= 0 && static_cast<usize>(layerIndex) < m_layerRuntimes.Size())
                       ? m_layerRuntimes[static_cast<usize>(layerIndex)].currentTime
                       : 0.0f;
        }

        void ResetToBind()
        {
            for (i32 i = 0;
                 i < m_skeleton->BoneCount() && static_cast<usize>(i) < m_finalPoses.Size(); ++i)
            {
                const Bone* bone = m_skeleton->GetBone(i);
                m_finalPoses[static_cast<usize>(i)] =
                    (bone != nullptr) ? bone->localBindPose : BoneTransform{};
            }
            m_matricesDirty = true;
        }
        void ForceState(i32 stateIndex, i32 layerIndex = 0)
        {
            if (layerIndex >= 0 && static_cast<usize>(layerIndex) < m_layerRuntimes.Size())
            {
                AnimationGraphLayerRuntime& rt = m_layerRuntimes[static_cast<usize>(layerIndex)];
                rt.currentStateIndex = stateIndex;
                rt.currentTime = 0.0f;
                rt.isTransitioning = false;
                rt.previousStateIndex = -1;
            }
        }

    private:
        [[nodiscard]] bool Valid(i32 i) const noexcept
        {
            return i >= 0 && static_cast<usize>(i) < m_parameters.Size();
        }
        [[nodiscard]] Span<const AnimationGraphParameter> Params() const noexcept
        {
            return {m_parameters.Data(), m_parameters.Size()};
        }

        void UpdateLayer(AnimationLayer& layer, AnimationGraphLayerRuntime& rt, f32 deltaTime)
        {
            if (rt.currentStateIndex < 0 ||
                static_cast<usize>(rt.currentStateIndex) >= layer.States().Size())
            {
                return;
            }
            AnimationGraphState* currentState = layer.GetState(rt.currentStateIndex);

            if (!rt.isTransitioning)
            {
                EvaluateTransitions(layer, rt);
            }

            if (rt.isTransitioning)
            {
                const f32 prevNorm = rt.currentTime;
                AdvanceStateTime(*currentState, rt.currentTime, deltaTime);
                if (AnimationGraphState* prevState = layer.GetState(rt.previousStateIndex))
                {
                    AdvanceStateTime(*prevState, rt.previousTime, deltaTime);
                }
                currentState = layer.GetState(rt.currentStateIndex); // (re-fetch; index unchanged)
                if (m_eventHandler && currentState != nullptr && currentState->Node() != nullptr)
                {
                    currentState->Node()->FireEvents(prevNorm, rt.currentTime, currentState->loop,
                                                     m_eventHandler);
                }
                rt.transitionElapsed += deltaTime;
                if (rt.transitionElapsed >= rt.transitionDuration)
                {
                    rt.isTransitioning = false;
                    rt.previousStateIndex = -1;
                }
            }
            else
            {
                const f32 prevNorm = rt.currentTime;
                AdvanceStateTime(*currentState, rt.currentTime, deltaTime);
                if (m_eventHandler && currentState->Node() != nullptr)
                {
                    currentState->Node()->FireEvents(prevNorm, rt.currentTime, currentState->loop,
                                                     m_eventHandler);
                }
            }
            SampleLayerPoses(layer, rt);
        }

        void AdvanceStateTime(AnimationGraphState& state, f32& normalizedTime, f32 deltaTime)
        {
            if (state.Duration() <= 0.0f)
            {
                return;
            }
            normalizedTime += (deltaTime * state.speed) / state.Duration();
            if (state.loop)
            {
                while (normalizedTime >= 1.0f)
                {
                    normalizedTime -= 1.0f;
                }
                while (normalizedTime < 0.0f)
                {
                    normalizedTime += 1.0f;
                }
            }
            else
            {
                normalizedTime = Clamp(normalizedTime, 0.0f, 1.0f);
            }
        }

        void EvaluateTransitions(AnimationLayer& layer, AnimationGraphLayerRuntime& rt)
        {
            AnimationGraphTransition* best = nullptr;
            i32 bestPriority = 2147483647;
            for (auto& tr : layer.Transitions())
            {
                if (tr->sourceStateIndex != -1 && tr->sourceStateIndex != rt.currentStateIndex)
                {
                    continue;
                }
                if (tr->destStateIndex == rt.currentStateIndex)
                {
                    continue;
                }
                if (tr->hasExitTime && rt.currentTime < tr->exitTime)
                {
                    continue;
                }
                if (!tr->EvaluateConditions(Params()))
                {
                    continue;
                }
                if (tr->priority < bestPriority)
                {
                    bestPriority = tr->priority;
                    best = tr.Get();
                }
            }
            if (best != nullptr)
            {
                rt.previousStateIndex = rt.currentStateIndex;
                rt.previousTime = rt.currentTime;
                rt.currentStateIndex = best->destStateIndex;
                rt.currentTime = 0.0f;
                rt.transitionElapsed = 0.0f;
                rt.transitionDuration = Max(best->duration, 0.001f);
                rt.isTransitioning = true;
            }
        }

        void SampleLayerPoses(AnimationLayer& layer, AnimationGraphLayerRuntime& rt)
        {
            if (rt.currentStateIndex < 0)
            {
                return;
            }
            AnimationGraphState* currentState = layer.GetState(rt.currentStateIndex);
            if (currentState == nullptr || currentState->Node() == nullptr)
            {
                return;
            }

            if (rt.isTransitioning && rt.previousStateIndex >= 0)
            {
                AnimationGraphState* prevState = layer.GetState(rt.previousStateIndex);
                if (prevState != nullptr && prevState->Node() != nullptr)
                {
                    prevState->Node()->Evaluate(
                        *m_skeleton, rt.previousTime,
                        Span<BoneTransform>{rt.prevPoses.Data(), rt.prevPoses.Size()});
                    currentState->Node()->Evaluate(
                        *m_skeleton, rt.currentTime,
                        Span<BoneTransform>{rt.poses.Data(), rt.poses.Size()});
                    const f32 blend =
                        Clamp(rt.transitionElapsed / rt.transitionDuration, 0.0f, 1.0f);
                    BlendPoses(Span<const BoneTransform>{rt.prevPoses.Data(), rt.prevPoses.Size()},
                               Span<const BoneTransform>{rt.poses.Data(), rt.poses.Size()}, blend,
                               Span<BoneTransform>{rt.poses.Data(), rt.poses.Size()});
                    return;
                }
            }
            currentState->Node()->Evaluate(*m_skeleton, rt.currentTime,
                                           Span<BoneTransform>{rt.poses.Data(), rt.poses.Size()});
        }

        void SyncBlendTreeParameters()
        {
            for (const Link1D& l : m_links1D)
            {
                if (Valid(l.paramIndex))
                {
                    l.tree->parameter = m_parameters[static_cast<usize>(l.paramIndex)].floatValue;
                }
            }
            for (const Link2D& l : m_links2D)
            {
                if (Valid(l.paramIndexX))
                {
                    l.tree->parameterX = m_parameters[static_cast<usize>(l.paramIndexX)].floatValue;
                }
                if (Valid(l.paramIndexY))
                {
                    l.tree->parameterY = m_parameters[static_cast<usize>(l.paramIndexY)].floatValue;
                }
            }
        }

        void CombineLayers()
        {
            if (m_layerRuntimes.IsEmpty())
            {
                return;
            }
            // Base layer writes directly.
            const AnimationGraphLayerRuntime& base = m_layerRuntimes[0];
            for (usize i = 0; i < m_finalPoses.Size() && i < base.poses.Size(); ++i)
            {
                m_finalPoses[i] = base.poses[i];
            }

            for (usize layerIdx = 1;
                 layerIdx < m_layerRuntimes.Size() && layerIdx < m_graph->Layers().Size();
                 ++layerIdx)
            {
                AnimationLayer& layer = *m_graph->Layers()[layerIdx];
                const AnimationGraphLayerRuntime& rt = m_layerRuntimes[layerIdx];
                if (layer.weight <= 0.0f)
                {
                    continue;
                }
                const BoneMask* mask = layer.Mask();

                if (layer.blendMode == LayerBlendMode::Override)
                {
                    for (usize b = 0; b < m_finalPoses.Size() && b < rt.poses.Size(); ++b)
                    {
                        const f32 w =
                            layer.weight *
                            (mask != nullptr ? mask->GetWeight(static_cast<i32>(b)) : 1.0f);
                        if (w > 0.0f)
                        {
                            m_finalPoses[b] = BoneTransform::Lerp(m_finalPoses[b], rt.poses[b], w);
                        }
                    }
                }
                else
                { // Additive: delta from bind pose
                    for (usize b = 0; b < m_finalPoses.Size() && b < rt.poses.Size(); ++b)
                    {
                        const f32 w =
                            layer.weight *
                            (mask != nullptr ? mask->GetWeight(static_cast<i32>(b)) : 1.0f);
                        if (w <= 0.0f)
                        {
                            continue;
                        }
                        const Bone* bone = m_skeleton->GetBone(static_cast<i32>(b));
                        const BoneTransform bind =
                            (bone != nullptr) ? bone->localBindPose : BoneTransform{};
                        const Float3 deltaPos = rt.poses[b].position - bind.position;
                        const Quaternion deltaRot = rt.poses[b].rotation * Inverse(bind.rotation);
                        const Float3 deltaScale = rt.poses[b].scale / bind.scale; // component-wise
                        m_finalPoses[b].position = m_finalPoses[b].position + deltaPos * w;
                        m_finalPoses[b].rotation =
                            Slerp(Quaternion::Identity, deltaRot, w) * m_finalPoses[b].rotation;
                        m_finalPoses[b].scale =
                            m_finalPoses[b].scale * Lerp(Float3::One, deltaScale, w);
                    }
                }
            }
        }

        struct Link1D
        {
            BlendTree1D* tree;
            i32 paramIndex;
        };
        struct Link2D
        {
            BlendTree2D* tree;
            i32 paramIndexX;
            i32 paramIndexY;
        };

        AnimationGraph* m_graph = nullptr;
        Skeleton* m_skeleton = nullptr;
        Array<AnimationGraphLayerRuntime> m_layerRuntimes;
        Array<AnimationGraphParameter> m_parameters; // runtime copy
        Array<BoneTransform> m_finalPoses;
        Array<Float4x4> m_skinningMatrices;
        Array<Float4x4> m_prevSkinningMatrices;
        bool m_matricesDirty = true;
        AnimationEventHandler m_eventHandler;
        Array<Link1D> m_links1D;
        Array<Link2D> m_links2D;
    };

    DRACONIC_DEFINE_OBJECT(AnimationGraph, "draconic::animation")

} // namespace draconic::animation
