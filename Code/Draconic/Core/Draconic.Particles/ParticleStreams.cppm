// draconic.particles:streams - the SoA particle stream container, ported from
// Sedulous.Particles (ParticleStream.bf, CPUStream.bf, ParticleStreamContainer.bf).
//
// Particle state is Structure-of-Arrays: one typed stream per attribute, held in a fixed
// 64-slot array indexed by ParticleStreamId, allocated lazily by whichever module declares
// it. AliveCount is shared across every stream; death is O(1) swap-remove (order not
// preserved). This is the CPU representation; the GPU-compute path (Phase 6) mirrors it with
// one SSBO per attribute behind the same ids.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.particles:streams;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::particles
{
    // Attribute channels. Core three (Position/Age/Lifetime) are always allocated; the rest are
    // added on demand by the modules that need them. Custom..MaxStreams is user-reserved space.
    enum class ParticleStreamId : u8
    {
        Position = 0,
        Velocity = 1,
        StartVelocity = 2,
        Color = 3,
        Size = 4,
        Age = 5,
        Lifetime = 6,
        Rotation = 7,
        RotationSpeed = 8,
        Axis = 9,
        Custom = 32,
        MaxStreams = 64,
    };

    enum class StreamElementType : u8
    {
        Float,
        Float2,
        Float3,
        Float4,
        Int32
    };

    // Maps a C++ element type to its StreamElementType tag (for checked typed access).
    template <typename T>
    struct StreamElementOf;
    template <>
    struct StreamElementOf<f32>
    {
        static constexpr StreamElementType value = StreamElementType::Float;
    };
    template <>
    struct StreamElementOf<Float2>
    {
        static constexpr StreamElementType value = StreamElementType::Float2;
    };
    template <>
    struct StreamElementOf<Float3>
    {
        static constexpr StreamElementType value = StreamElementType::Float3;
    };
    template <>
    struct StreamElementOf<Float4>
    {
        static constexpr StreamElementType value = StreamElementType::Float4;
    };
    template <>
    struct StreamElementOf<i32>
    {
        static constexpr StreamElementType value = StreamElementType::Int32;
    };

    // Abstract stream: identity + element type + capacity, plus a type-erased swap-remove so the
    // container can compact without knowing T.
    class ParticleStream
    {
    public:
        ParticleStream(ParticleStreamId id, StreamElementType elementType, i32 capacity) noexcept
            : m_id(id), m_elementType(elementType), m_capacity(capacity)
        {
        }
        virtual ~ParticleStream() = default;

        ParticleStream(const ParticleStream&) = delete;
        ParticleStream& operator=(const ParticleStream&) = delete;

        [[nodiscard]] ParticleStreamId Id() const noexcept { return m_id; }
        [[nodiscard]] StreamElementType ElementType() const noexcept { return m_elementType; }
        [[nodiscard]] i32 Capacity() const noexcept { return m_capacity; }
        [[nodiscard]] virtual bool IsCPU() const noexcept = 0;
        [[nodiscard]] bool IsGPU() const noexcept { return !IsCPU(); }

        // Move the last alive element into `index` (swap-remove). Caller decrements AliveCount.
        virtual void SwapRemoveElement(i32 index, i32 aliveCount) noexcept = 0;

    protected:
        ParticleStreamId m_id;
        StreamElementType m_elementType;
        i32 m_capacity;
    };

    // A CPU stream: a system-memory array of `capacity` elements.
    template <typename T>
    class CPUStream final : public ParticleStream
    {
    public:
        CPUStream(ParticleStreamId id, StreamElementType elementType, i32 capacity)
            : ParticleStream(id, elementType, capacity)
        {
            m_data.Resize(static_cast<usize>(capacity));
        }

        [[nodiscard]] bool IsCPU() const noexcept override { return true; }

        [[nodiscard]] T& operator[](i32 index) noexcept
        {
            return m_data[static_cast<usize>(index)];
        }
        [[nodiscard]] const T& operator[](i32 index) const noexcept
        {
            return m_data[static_cast<usize>(index)];
        }
        [[nodiscard]] T* Data() noexcept { return m_data.Data(); }
        [[nodiscard]] Span<T> Slice(i32 count) noexcept
        {
            return Span<T>{m_data.Data(), static_cast<usize>(count)};
        }

        void SwapRemoveElement(i32 index, i32 aliveCount) noexcept override
        {
            const i32 last = aliveCount - 1;
            if (index < last)
            {
                m_data[static_cast<usize>(index)] = m_data[static_cast<usize>(last)];
            }
        }

    private:
        Array<T> m_data;
    };

    // The SoA container: a fixed 64-slot array of streams + a shared AliveCount. Owns its streams
    // (raw pointers freed in the destructor, mirroring Sedulous's manual lifetime).
    class ParticleStreamContainer
    {
    public:
        explicit ParticleStreamContainer(i32 capacity) : m_capacity(capacity)
        {
            // Core streams every system has.
            EnsureStream(ParticleStreamId::Position, StreamElementType::Float3);
            EnsureStream(ParticleStreamId::Age, StreamElementType::Float);
            EnsureStream(ParticleStreamId::Lifetime, StreamElementType::Float);
        }

        ~ParticleStreamContainer()
        {
            IAllocator& alloc = DefaultAllocator();
            for (ParticleStream*& s : m_streams)
            {
                if (s != nullptr)
                {
                    alloc.Delete(s);
                    s = nullptr;
                }
            }
        }

        ParticleStreamContainer(const ParticleStreamContainer&) = delete;
        ParticleStreamContainer& operator=(const ParticleStreamContainer&) = delete;

        // Move: transfer stream ownership (used by ParticleSystem::SetMaxParticles to swap in a
        // fresh, larger/smaller container). The source is left empty so its dtor frees nothing.
        ParticleStreamContainer(ParticleStreamContainer&& other) noexcept
            : aliveCount(other.aliveCount), m_capacity(other.m_capacity)
        {
            for (usize i = 0; i < static_cast<usize>(ParticleStreamId::MaxStreams); ++i)
            {
                m_streams[i] = other.m_streams[i];
                other.m_streams[i] = nullptr;
            }
            other.aliveCount = 0;
        }
        ParticleStreamContainer& operator=(ParticleStreamContainer&& other) noexcept
        {
            if (this != &other)
            {
                IAllocator& alloc = DefaultAllocator();
                for (ParticleStream*& s : m_streams)
                {
                    if (s != nullptr)
                    {
                        alloc.Delete(s);
                        s = nullptr;
                    }
                }
                for (usize i = 0; i < static_cast<usize>(ParticleStreamId::MaxStreams); ++i)
                {
                    m_streams[i] = other.m_streams[i];
                    other.m_streams[i] = nullptr;
                }
                m_capacity = other.m_capacity;
                aliveCount = other.aliveCount;
                other.aliveCount = 0;
            }
            return *this;
        }

        [[nodiscard]] i32 Capacity() const noexcept { return m_capacity; }

        i32 aliveCount = 0; // shared across all streams; index [0, aliveCount) is live

        [[nodiscard]] ParticleStream* GetStream(ParticleStreamId id) const noexcept
        {
            return m_streams[static_cast<usize>(id)];
        }

        // Typed access - returns null if the stream isn't allocated or the element type mismatches.
        template <typename T>
        [[nodiscard]] CPUStream<T>* GetCPUStream(ParticleStreamId id) const noexcept
        {
            ParticleStream* s = m_streams[static_cast<usize>(id)];
            if (s == nullptr || !s->IsCPU() || s->ElementType() != StreamElementOf<T>::value)
            {
                return nullptr;
            }
            return static_cast<CPUStream<T>*>(s);
        }

        // Allocate a stream in its slot if absent (idempotent). Modules call this in DeclareStreams.
        void EnsureStream(ParticleStreamId id, StreamElementType elementType)
        {
            const usize slot = static_cast<usize>(id);
            if (m_streams[slot] != nullptr)
            {
                return;
            }
            IAllocator& alloc = DefaultAllocator();
            switch (elementType)
            {
            case StreamElementType::Float:
                m_streams[slot] = alloc.New<CPUStream<f32>>(id, elementType, m_capacity);
                break;
            case StreamElementType::Float2:
                m_streams[slot] = alloc.New<CPUStream<Float2>>(id, elementType, m_capacity);
                break;
            case StreamElementType::Float3:
                m_streams[slot] = alloc.New<CPUStream<Float3>>(id, elementType, m_capacity);
                break;
            case StreamElementType::Float4:
                m_streams[slot] = alloc.New<CPUStream<Float4>>(id, elementType, m_capacity);
                break;
            case StreamElementType::Int32:
                m_streams[slot] = alloc.New<CPUStream<i32>>(id, elementType, m_capacity);
                break;
            }
        }

        // Typed accessors for the standard channels (null when not allocated).
        [[nodiscard]] CPUStream<Float3>* Positions() const noexcept
        {
            return GetCPUStream<Float3>(ParticleStreamId::Position);
        }
        [[nodiscard]] CPUStream<f32>* Ages() const noexcept
        {
            return GetCPUStream<f32>(ParticleStreamId::Age);
        }
        [[nodiscard]] CPUStream<f32>* Lifetimes() const noexcept
        {
            return GetCPUStream<f32>(ParticleStreamId::Lifetime);
        }
        [[nodiscard]] CPUStream<Float3>* Velocities() const noexcept
        {
            return GetCPUStream<Float3>(ParticleStreamId::Velocity);
        }
        [[nodiscard]] CPUStream<Float3>* StartVelocities() const noexcept
        {
            return GetCPUStream<Float3>(ParticleStreamId::StartVelocity);
        }
        [[nodiscard]] CPUStream<Float4>* Colors() const noexcept
        {
            return GetCPUStream<Float4>(ParticleStreamId::Color);
        }
        [[nodiscard]] CPUStream<Float2>* Sizes() const noexcept
        {
            return GetCPUStream<Float2>(ParticleStreamId::Size);
        }
        [[nodiscard]] CPUStream<f32>* Rotations() const noexcept
        {
            return GetCPUStream<f32>(ParticleStreamId::Rotation);
        }
        [[nodiscard]] CPUStream<f32>* RotationSpeeds() const noexcept
        {
            return GetCPUStream<f32>(ParticleStreamId::RotationSpeed);
        }
        [[nodiscard]] CPUStream<Float3>* Axes() const noexcept
        {
            return GetCPUStream<Float3>(ParticleStreamId::Axis);
        }

        // Normalized life [0,1] of a particle (age/lifetime), 1 when lifetime is non-positive.
        [[nodiscard]] f32 GetLifeRatio(i32 index) const noexcept
        {
            const CPUStream<f32>* ages = Ages();
            const CPUStream<f32>* lifetimes = Lifetimes();
            if (ages == nullptr || lifetimes == nullptr)
            {
                return 1.0f;
            }
            const f32 life = (*lifetimes)[index];
            if (life <= 0.0f)
            {
                return 1.0f;
            }
            return Min((*ages)[index] / life, 1.0f);
        }

        // Swap-remove one particle across every allocated stream, then shrink AliveCount.
        void SwapRemove(i32 index) noexcept
        {
            for (ParticleStream* s : m_streams)
            {
                if (s != nullptr)
                {
                    s->SwapRemoveElement(index, aliveCount);
                }
            }
            --aliveCount;
        }

        // Reverse-scan swap-remove of all dead particles (age >= lifetime). Returns the count removed.
        i32 CompactDead() noexcept
        {
            const CPUStream<f32>* ages = Ages();
            const CPUStream<f32>* lifetimes = Lifetimes();
            if (ages == nullptr || lifetimes == nullptr)
            {
                return 0;
            }
            i32 removed = 0;
            for (i32 i = aliveCount - 1; i >= 0; --i)
            {
                if ((*ages)[i] >= (*lifetimes)[i])
                {
                    SwapRemove(i);
                    ++removed;
                }
            }
            return removed;
        }

    private:
        ParticleStream* m_streams[static_cast<usize>(ParticleStreamId::MaxStreams)] = {};
        i32 m_capacity;
    };
}
