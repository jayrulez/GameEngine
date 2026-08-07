/// Draconic::Scene - the `:component` partition.
///
/// The value-pool component contract. A component is plain value data; the typed
/// ComponentManager<T> owns its storage, lifecycle, iteration, and serialization, and
/// is the only thing that touches the concrete type. Storage is a **sparse set**:
///   * m_dense  - a truly contiguous Array<T> (no holes), for cache-friendly ForEach /
///                Dense() iteration and direct GPU/system extraction.
///   * m_owners - the owning EntityHandle per dense slot (parallel to m_dense).
///   * m_sparse - entity-index -> dense-index, for O(1) Has/Get/Remove by entity.
/// Removal is swap-with-last (keeps m_dense packed). Components are referenced by their
/// owning entity (the EntityHandle's generation is the staleness check) - never by a
/// stashed pointer (the pools move). At most one component of type T per entity.
///
/// A specific manager that needs stable addresses or in-pool polymorphism can swap its
/// internal storage (e.g. to UniquePtr<T> slots) behind this same API - the escape
/// hatch from the storage decision - with no consumer impact.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.scene:component;

import draconic.foundation;
import draconic.resource;
import :entity;
import :system;

using namespace draconic::foundation;

export namespace draconic::scene
{

    // Non-generic interface a Scene uses to hold heterogeneous managers and route by type.
    class ComponentManagerBase : public SceneSystem
    {
    public:
        [[nodiscard]] ComponentManagerBase* AsComponentManager() noexcept override { return this; }

        [[nodiscard]] virtual bool HasComponent(EntityHandle entity) const = 0;
        virtual void RemoveComponent(EntityHandle entity) = 0;
        virtual void InitializePendingComponents() {}
        [[nodiscard]] virtual u32 ComponentCount() const = 0;
        // Stable per-component-type id (for type -> manager routing on load).
        [[nodiscard]] virtual const TypeInfo* ComponentType() const = 0;
        // Owning entity of each stored component (parallel to the pool).
        [[nodiscard]] virtual Span<const EntityHandle> OwnerHandles() const noexcept = 0;

        // --- reflection access (tools: the editor inspector edits components generically) ---
        // The entity's component as a typed reflection Instance (empty when absent).
        [[nodiscard]] virtual Instance GetComponentInstance(EntityHandle /*entity*/) { return {}; }
        // Adds a default-constructed component (false if the entity already has one).
        virtual bool AddDefaultComponent(EntityHandle /*entity*/) { return false; }

        // --- serialization (a manager opts in via SerializableComponentManager) ---
        // Whether this manager's components persist, and a stable on-disk type id for
        // routing on load (TypeOf<T>() is not disk-stable, so the id is explicit).
        [[nodiscard]] virtual bool IsSerializable() const noexcept { return false; }
        [[nodiscard]] virtual StringView SerializationTypeId() const noexcept { return {}; }
        // Writes / reads one component's data for `entity` (read adds the component first).
        virtual void WriteComponent(ISerializer& /*ar*/, EntityHandle /*entity*/) {}
        virtual void ReadComponent(ISerializer& /*ar*/, EntityHandle /*entity*/) {}

        // Bind every component's resource::Ref fields through the manager (the post-load
        // resolve pass; asset-pipeline design §8). Default: nothing to resolve.
        void ResolveResources(draconic::resource::ResourceManager& /*manager*/) override {}

        // Destroying an entity destroys its component in this manager.
        void OnEntityDestroyed(EntityHandle entity) override { RemoveComponent(entity); }
    };

    // Typed, sparse-set component pool. T is plain value data.
    template <typename T>
    class ComponentManager : public ComponentManagerBase
    {
    public:
        // Adds a component for `entity` (one per type per entity - asserts otherwise) and
        // returns a transient reference. Do not stash the reference across structural
        // changes; re-resolve via Get(entity). Initialization is deferred to the next
        // InitializePendingComponents (so sibling components can be set up first).
        T& Add(EntityHandle entity)
        {
            DRACONIC_ASSERT(!HasComponent(entity));
            const u32 dense = static_cast<u32>(m_dense.Size());
            m_dense.PushBack(T{});
            m_owners.PushBack(entity);
            m_sparse.InsertOrAssign(entity.index, dense);
            m_pendingInit.PushBack(entity);
            OnComponentCreated(m_dense[dense], entity);
            return m_dense[dense];
        }

        [[nodiscard]] bool HasComponent(EntityHandle entity) const override
        {
            return DenseIndex(entity) != kInvalid;
        }
        [[nodiscard]] bool Has(EntityHandle entity) const { return HasComponent(entity); }

        // Effective component for `entity`, or null if absent / the handle is stale.
        [[nodiscard]] Instance GetComponentInstance(EntityHandle entity) override
        {
            T* c = Get(entity);
            return (c != nullptr) ? Instance::From(c) : Instance{};
        }
        bool AddDefaultComponent(EntityHandle entity) override
        {
            if (HasComponent(entity))
            {
                return false;
            }
            (void)Add(entity);
            return true;
        }

        [[nodiscard]] T* Get(EntityHandle entity)
        {
            const u32 i = DenseIndex(entity);
            return (i != kInvalid) ? &m_dense[i] : nullptr;
        }
        [[nodiscard]] const T* Get(EntityHandle entity) const
        {
            const u32 i = DenseIndex(entity);
            return (i != kInvalid) ? &m_dense[i] : nullptr;
        }

        void RemoveComponent(EntityHandle entity) override
        {
            const u32 i = DenseIndex(entity);
            if (i == kInvalid)
            {
                return;
            }
            OnComponentDestroyed(m_dense[i], entity);

            const u32 last = static_cast<u32>(m_dense.Size()) - 1u;
            if (i != last)
            { // swap the last element into the hole
                m_dense[i] = Move(m_dense[last]);
                m_owners[i] = m_owners[last];
                m_sparse.InsertOrAssign(m_owners[i].index, i);
            }
            m_dense.PopBack();
            m_owners.PopBack();
            m_sparse.Remove(entity.index);
        }
        void Remove(EntityHandle entity) { RemoveComponent(entity); }

        [[nodiscard]] u32 ComponentCount() const override
        {
            return static_cast<u32>(m_dense.Size());
        }
        [[nodiscard]] u32 Count() const { return static_cast<u32>(m_dense.Size()); }

        // Contiguous fast-path: the packed component array + the parallel owner handles.
        [[nodiscard]] Span<T> Dense() noexcept { return {m_dense.Data(), m_dense.Size()}; }
        [[nodiscard]] Span<const T> Dense() const noexcept
        {
            return {m_dense.Data(), m_dense.Size()};
        }
        [[nodiscard]] Span<const EntityHandle> Owners() const noexcept
        {
            return {m_owners.Data(), m_owners.Size()};
        }

        // Visits every component with its owning entity (dense, no holes).
        template <typename Fn>
        void ForEach(Fn&& fn)
        {
            for (usize i = 0; i < m_dense.Size(); ++i)
            {
                fn(m_dense[i], m_owners[i]);
            }
        }

        // Runs OnComponentInitialized for components added since the last call (skipping any
        // already removed). Deferred init lets a component see its siblings before it runs.
        void InitializePendingComponents() override
        {
            for (EntityHandle e : m_pendingInit)
            {
                T* c = Get(e);
                if (c != nullptr)
                {
                    OnComponentInitialized(*c, e);
                }
            }
            m_pendingInit.Clear();
        }

        [[nodiscard]] const TypeInfo* ComponentType() const override { return &TypeOf<T>(); }
        [[nodiscard]] Span<const EntityHandle> OwnerHandles() const noexcept override
        {
            return Owners();
        }

    protected:
        // Manager-driven lifecycle hooks (value-pool: hooks live on the manager, not the
        // component). Override in a concrete manager subclass.
        virtual void OnComponentCreated(T& /*c*/, EntityHandle /*e*/) {}
        virtual void OnComponentInitialized(T& /*c*/, EntityHandle /*e*/) {}
        virtual void OnComponentDestroyed(T& /*c*/, EntityHandle /*e*/) {}

    private:
        static constexpr u32 kInvalid = 0xFFFFFFFFu;

        // Dense index for a live, generation-matching component, or kInvalid.
        [[nodiscard]] u32 DenseIndex(EntityHandle entity) const
        {
            const u32* found = m_sparse.Find(entity.index);
            if (found == nullptr)
            {
                return kInvalid;
            }
            const u32 dense = *found;
            // The stored owner carries the full handle - a stale (reused-slot) entity
            // handle won't match its generation, so it reads as absent.
            return (m_owners[dense] == entity) ? dense : kInvalid;
        }

        Array<T> m_dense;                  // packed component data (no holes)
        Array<EntityHandle> m_owners;      // owning entity per dense slot
        HashMap<u32, u32> m_sparse;        // entity.index -> dense index
        Array<EntityHandle> m_pendingInit; // entities awaiting OnComponentInitialized
    };

    namespace detail
    {
        // Fallback for components without resource::Ref fields (see ResolveOne).
        template <typename T>
        void ResolveResources(draconic::resource::ResourceManager&, T&)
        {
        }
    }

    // A ComponentManager<T> whose components persist. The component type T must have a
    // `Serialize(ISerializer&, T&)` overload (found by ADL); the manager drives it per
    // component. Construct with a stable on-disk type id (used to route records to this
    // manager on load). Subclass this (instead of ComponentManager<T>) for serializable
    // components - keeping serialization opt-in. Components with resource::Ref fields
    // additionally define `ResolveResources(ResourceManager&, T&)` (ADL, like Serialize).
    template <typename T>
    class SerializableComponentManager : public ComponentManager<T>
    {
    public:
        explicit SerializableComponentManager(StringView typeId) : m_typeId(typeId) {}

        [[nodiscard]] bool IsSerializable() const noexcept override { return true; }
        [[nodiscard]] StringView SerializationTypeId() const noexcept override
        {
            return m_typeId.AsView();
        }

        void WriteComponent(ISerializer& ar, EntityHandle entity) override
        {
            if (T* c = this->Get(entity))
            {
                // Records carry the component type's data version (TypeOf<T> - the reflected
                // TypeInfo, patched by DRACONIC_REFLECT_VALUE's builder.DataVersion), so
                // component Serialize bodies can migrate old scenes/blobs.
                BeginVersionedPayload(ar, TypeOf<T>());
                SerializeOne(ar, *c);
                EndVersionedPayload(ar);
            }
        }
        void ReadComponent(ISerializer& ar, EntityHandle entity) override
        {
            // Overwrite when present: duplicate records (corrupt saves recovered by the scene
            // loader) must consume their payload instead of asserting in Add.
            T* existing = this->Get(entity);
            T& c = (existing != nullptr) ? *existing : this->Add(entity);
            BeginVersionedPayload(ar, TypeOf<T>());
            SerializeOne(ar, c);
            EndVersionedPayload(ar);
        }

        // ADL hook like Serialize: components with resource::Ref fields define
        // `ResolveResources(ResourceManager&, T&)` in their namespace; others resolve to
        // the no-op fallback below.
        void ResolveResources(draconic::resource::ResourceManager& manager) override
        {
            this->ForEach([&](T& c, EntityHandle) { ResolveOne(manager, c); });
        }

    private:
        // Unqualified call so ADL finds the user's Serialize(ar, T&); the using-declaration
        // brings the core overloads into scope for the component's own field serialization.
        static void SerializeOne(ISerializer& ar, T& value)
        {
            using draconic::foundation::Serialize;
            Serialize(ar, value);
        }
        static void ResolveOne(draconic::resource::ResourceManager& manager, T& value)
        {
            using draconic::scene::detail::ResolveResources; // no-op fallback
            ResolveResources(manager, value);
        }
        String m_typeId;
    };

} // namespace draconic::scene
