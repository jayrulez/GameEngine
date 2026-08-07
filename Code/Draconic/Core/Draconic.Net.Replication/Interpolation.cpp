// Draconic::NetReplication - implementation unit: client-side snapshot interpolation (LerpFieldValue
// + InterpolationBuffer). Separate from ReplicationImpl.cpp - a distinct concern (playback smoothing),
// same module. Depends only on Core reflection (no wire).

module;
#include "Draconic.Foundation/Prelude.h"
#include <cmath>

module draconic.net.replication;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::net
{

    namespace
    {
        [[nodiscard]] f32 Lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

        // Normalized-lerp with shortest-path selection - cheap, stable for the small per-tick deltas
        // interpolation sees (full slerp is overkill at 10-20 Hz).
        [[nodiscard]] Quaternion NLerp(const Quaternion& a, const Quaternion& b, f32 t) noexcept
        {
            const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
            const f32 s = dot < 0.0f ? -1.0f : 1.0f; // shortest path
            Quaternion r{Lerp(a.x, s * b.x, t), Lerp(a.y, s * b.y, t), Lerp(a.z, s * b.z, t),
                         Lerp(a.w, s * b.w, t)};
            const f32 len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (len > 1e-6f)
            {
                r.x /= len;
                r.y /= len;
                r.z /= len;
                r.w /= len;
            }
            return r;
        }
    }

    bool IsInterpolatableType(const TypeInfo* type) noexcept
    {
        return type == &TypeOf<f32>() || type == &TypeOf<f64>() || type == &TypeOf<Float2>() ||
               type == &TypeOf<Float3>() || type == &TypeOf<Float4>() ||
               type == &TypeOf<Quaternion>();
    }

    Variant LerpFieldValue(const Variant& a, const Variant& b, f32 t)
    {
        if (const f32* x = a.TryGet<f32>())
        {
            const f32* y = b.TryGet<f32>();
            return Variant::From<f32>(Lerp(*x, y ? *y : *x, t));
        }
        if (const f64* x = a.TryGet<f64>())
        {
            const f64* y = b.TryGet<f64>();
            const f64 yv = y ? *y : *x;
            return Variant::From<f64>(*x + (yv - *x) * static_cast<f64>(t));
        }
        if (const Float2* x = a.TryGet<Float2>())
        {
            const Float2* y = b.TryGet<Float2>();
            const Float2 yy = y ? *y : *x;
            return Variant::From<Float2>(Float2{Lerp(x->x, yy.x, t), Lerp(x->y, yy.y, t)});
        }
        if (const Float3* x = a.TryGet<Float3>())
        {
            const Float3* y = b.TryGet<Float3>();
            const Float3 yy = y ? *y : *x;
            return Variant::From<Float3>(
                Float3{Lerp(x->x, yy.x, t), Lerp(x->y, yy.y, t), Lerp(x->z, yy.z, t)});
        }
        if (const Float4* x = a.TryGet<Float4>())
        {
            const Float4* y = b.TryGet<Float4>();
            const Float4 yy = y ? *y : *x;
            return Variant::From<Float4>(Float4{Lerp(x->x, yy.x, t), Lerp(x->y, yy.y, t),
                                                Lerp(x->z, yy.z, t), Lerp(x->w, yy.w, t)});
        }
        if (const Quaternion* x = a.TryGet<Quaternion>())
        {
            const Quaternion* y = b.TryGet<Quaternion>();
            return Variant::From<Quaternion>(NLerp(*x, y ? *y : *x, t));
        }
        return a; // non-interpolatable: snap to the value in effect at render time (the earlier sample)
    }

    void InterpolationBuffer::Record(NetworkId id, u32 componentTypeHash, f64 timestampMs,
                                     const Instance& component)
    {
        if (component.Type() == nullptr)
        {
            return;
        }
        if (m_entities.Find(id.value) == nullptr)
        {
            m_entities.InsertOrAssign(id.value, HashMap<u32, Timeline>{});
        }
        HashMap<u32, Timeline>& byType = *m_entities.Find(id.value);
        if (byType.Find(componentTypeHash) == nullptr)
        {
            byType.InsertOrAssign(componentTypeHash, Timeline{});
        }
        Timeline& tl = *byType.Find(componentTypeHash);

        StateSample s;
        s.time = timestampMs;
        for (const PropertyInfo* property : ReplicatedProperties(*component.Type()))
        {
            s.fields.PushBack(GetProperty(*property, component));
        }
        tl.samples.PushBack(Move(s));

        // Prune samples older than the history window, always keeping at least the last two (to bracket).
        const f64 cutoff = timestampMs - m_historyMs;
        while (tl.samples.Size() > 2 && tl.samples[0].time < cutoff)
        {
            tl.samples.RemoveAt(0);
        }
    }

    bool InterpolationBuffer::Sample(NetworkId id, u32 componentTypeHash, f64 renderTimeMs,
                                     const Instance& component) const
    {
        if (component.Type() == nullptr)
        {
            return false;
        }
        const HashMap<u32, Timeline>* byType = m_entities.Find(id.value);
        if (byType == nullptr)
        {
            return false;
        }
        const Timeline* tl = byType->Find(componentTypeHash);
        if (tl == nullptr || tl->samples.IsEmpty())
        {
            return false;
        }
        const Array<StateSample>& ss = tl->samples;

        // Bracket renderTime: bi = first sample at/after it.
        usize bi = ss.Size();
        for (usize i = 0; i < ss.Size(); ++i)
        {
            if (ss[i].time >= renderTimeMs)
            {
                bi = i;
                break;
            }
        }

        const StateSample* a = nullptr;
        const StateSample* b = nullptr;
        f32 t = 0.0f;
        if (bi == 0)
        {
            a = &ss[0];
            b = &ss[0];
        } // before window -> earliest
        else if (bi >= ss.Size())
        {
            a = &ss[ss.Size() - 1];
            b = a;
        } // after window -> latest
        else
        {
            a = &ss[bi - 1];
            b = &ss[bi];
            const f64 span = b->time - a->time;
            t = span > 1e-9 ? static_cast<f32>((renderTimeMs - a->time) / span) : 0.0f;
        }

        const Span<const PropertyInfo* const> layout = ReplicatedProperties(*component.Type());
        for (usize i = 0; i < layout.Size() && i < a->fields.Size() && i < b->fields.Size(); ++i)
        {
            const Variant v = LerpFieldValue(a->fields[i], b->fields[i], t);
            (void)SetProperty(*layout[i], component, v);
        }
        return true;
    }

    void InterpolationBuffer::Forget(NetworkId id) { (void)m_entities.Remove(id.value); }

}
