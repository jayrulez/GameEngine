// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Resource - the `foundation.resource` module.
//
// The resource manager: turns content-database *source* objects (ISerializable,
// full editor fidelity) into runtime *products* (lean Objects) via factories,
// hands them out behind replaceable handles, and caches/reloads them. This is
// the source->product split: the editor authors a `…Resource` in the content
// db; a factory builds the runtime product the game actually uses. Editor-only
// data (node positions, comments) lives on the source and never reaches the
// product.
//
// Sits at the top of the asset stack: Core/VFS -> content -> resource.

module;
#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"
#include "Core/Prelude.h"

export module foundation.resource;

import foundation.core;
import foundation.content;

using namespace foundation::core;

export namespace foundation::resource
{
    // Typed Guid naming a resource that binds to product type T.
    template <typename T>
    struct ResourceId
    {
        Guid id;

        ResourceId() = default;
        explicit ResourceId(const Guid& guid) noexcept : id(guid) {}
        [[nodiscard]] bool IsNull() const noexcept { return id.IsNil(); }
    };

    // Load state of a ResourceHandle (async loading, task #123). Unloaded = never built;
    // Pending = an async decode is in flight (Proxy Get() is null; poll State() or set OnReady);
    // Ready = product available; Failed = missing instance/factory or a build/decode/finalize
    // failure. Sync builds settle to Ready/Failed immediately.
    enum class ResourceState : u8
    {
        Unloaded,
        Pending,
        Ready,
        Failed,
    };

    // =======================================================================
    // ResourceHandle - a shared, replaceable slot holding one runtime product.
    // Proxies hold the handle (not the product), so a Reload that Replace()s the
    // product is seen by every holder. Remembers its product type so the manager
    // can rebuild it without being told the type again.
    // =======================================================================
    class ResourceHandle final : public RefCounted
    {
    public:
        [[nodiscard]] Object* Get() const noexcept { return m_product.Get(); }
        void Replace(RefPtr<Object> product) noexcept { m_product = Move(product); }
        void Flush() noexcept { m_product = nullptr; }

        [[nodiscard]] TypeId ProductTypeId() const noexcept { return m_productTypeId; }
        void SetProductTypeId(TypeId id) noexcept { m_productTypeId = id; }

        [[nodiscard]] ResourceState State() const noexcept { return m_state; }
        void SetState(ResourceState state) noexcept { m_state = state; }

        // Fired ONCE on the main thread when an async load reaches Ready (during Pump), then
        // cleared. Set before or during Pending; a load that is already Ready fires nothing.
        void SetOnReady(Function<void()> callback) noexcept { m_onReady = Move(callback); }
        void FireOnReady()
        {
            if (m_onReady)
            {
                Function<void()> callback = Move(m_onReady);
                m_onReady = nullptr;
                callback();
            }
        }

    private:
        RefPtr<Object> m_product;
        TypeId m_productTypeId{};
        ResourceState m_state = ResourceState::Unloaded;
        Function<void()> m_onReady;
    };

    // =======================================================================
    // Proxy<T> - typed accessor over a ResourceHandle. Cheap to copy/store; it
    // follows the handle, so it always sees the current product.
    // =======================================================================
    template <typename T>
    class Proxy
    {
    public:
        Proxy() = default;
        explicit Proxy(RefPtr<ResourceHandle> handle) noexcept : m_handle(Move(handle)) {}

        [[nodiscard]] T* Get() const noexcept
        {
            return (m_handle.Get() != nullptr) ? Cast<T>(m_handle->Get()) : nullptr;
        }
        [[nodiscard]] T* operator->() const noexcept { return Get(); }
        [[nodiscard]] T& operator*() const noexcept { return *Get(); }
        [[nodiscard]] explicit operator bool() const noexcept { return Get() != nullptr; }

        [[nodiscard]] ResourceHandle* Handle() const noexcept { return m_handle.Get(); }

    private:
        RefPtr<ResourceHandle> m_handle;
    };

    // =======================================================================
    // Ref<T> - the SERIALIZABLE resource reference components hold (the asset
    // pipeline's layer). Identity is a Guid (written by Serialize); at runtime
    // Bind() attaches a Proxy so hot reload's handle Replace() is visible to every
    // holder. Code-created resources (samples, procedural) assign a RefPtr<T>
    // directly - the direct object wins over the proxy and is never serialized.
    // =======================================================================
    class ResourceManager; // forward - Ref::Bind resolves through it

    template <typename T>
    class Ref
    {
    public:
        Guid id; // serialized identity (nil = unset / procedural-only)

        Ref() = default;
        Ref(const RefPtr<T>& object) : m_direct(object) {} // implicit: `c.mesh = meshPtr`
        Ref(T* object) : m_direct(RefPtr<T>(object)) {}    // implicit: raw runtime objects
        Ref& operator=(const RefPtr<T>& object)
        {
            m_direct = object;
            return *this;
        }
        Ref& operator=(T* object)
        {
            m_direct = RefPtr<T>(object);
            return *this;
        }

        [[nodiscard]] T* Get() const noexcept
        {
            return (m_direct.Get() != nullptr) ? m_direct.Get() : m_proxy.Get();
        }
        [[nodiscard]] T* operator->() const noexcept { return Get(); }
        [[nodiscard]] explicit operator bool() const noexcept { return Get() != nullptr; }

        void SetDirect(RefPtr<T> object) noexcept { m_direct = Move(object); }
        void SetId(const Guid& guid) noexcept { id = guid; }
        /// Adopt an already-bound proxy (runtime code that bound by hand): follows reloads;
        /// clears any direct override so the proxy is what Get() sees.
        void SetProxy(Proxy<T> proxy) noexcept
        {
            m_proxy = Move(proxy);
            m_direct = nullptr;
        }

        /// Re-point at `id` (editor pickers): drops the direct override AND the previous
        /// proxy, then binds the new id (nil = cleared reference).
        void Rebind(ResourceManager* manager)
        {
            m_direct = nullptr;
            m_proxy = Proxy<T>{};
            if (manager != nullptr && !id.IsNil())
            {
                Bind(*manager);
            }
        }
        [[nodiscard]] bool IsBound() const noexcept { return m_proxy.Handle() != nullptr; }

        /// Drop any runtime binding (proxy AND direct override). Deserialization calls this
        /// when the incoming identity REPLACES a different one - the old binding must not
        /// keep rendering the previous resource.
        void ClearBinding() noexcept
        {
            m_proxy = Proxy<T>{};
            m_direct = nullptr;
        }

        // Attach the runtime proxy for `id` (defined after ResourceManager below).
        void Bind(ResourceManager& manager);

        [[nodiscard]] const Proxy<T>& GetProxy() const noexcept { return m_proxy; }

    private:
        Proxy<T> m_proxy;   // guid-backed binding (runtime only)
        RefPtr<T> m_direct; // procedural override (runtime only)
    };

    // Ref<T> is a reference-shaped value: generic tooling (an editor's inspector, an agent's
    // entity_inspect) reads its identity through the TypeInfo, never naming T.
}
export namespace foundation::core
{
    template <typename T>
    struct ReferenceTraits<foundation::resource::Ref<T>>
    {
        static constexpr bool isReference = true;
        [[nodiscard]] static const ReferenceOps& Ops() noexcept
        {
            using RefT = foundation::resource::Ref<T>;
            static constexpr ReferenceOps ops{
                [](const void* value) -> const Guid* { return &static_cast<const RefT*>(value)->id; },
                [](void* value, const Guid& id) { static_cast<RefT*>(value)->SetId(id); },
                [](void* value) { static_cast<RefT*>(value)->ClearBinding(); },
                // The runtime type T, by the identity the resource manager keys its factories
                // with (StaticType, not TypeOf<T>: the two differ for an Object).
                []() -> const TypeInfo* { return &T::StaticType(); }};
            return ops;
        }
    };
}
export namespace foundation::resource
{
    // Serialization: identity only (found by ADL from component Serialize bodies). On READ,
    // blobs replay over LIVE components (paste / prefab revert / undo): when the incoming id
    // differs from the current one, the stale proxy/direct binding is dropped - critically,
    // reading a NIL id actually unbinds instead of leaving the old resource rendering.
    template <typename T>
    void Serialize(ISerializer& ar, Ref<T>& ref)
    {
        const Guid before = ref.id;
        foundation::core::Serialize(ar, ref.id);
        if (ref.id != before)
        {
            ref.ClearBinding();
        }
    }

    // =======================================================================
    // IResourceFactory - builds a runtime product from a content instance (its
    // source object + data streams). One factory per product type.
    // =======================================================================
    class ResourceManager; // forward - factories receive it to resolve child resources

    class IResourceFactory
    {
    public:
        virtual ~IResourceFactory() = default;

        [[nodiscard]] virtual const TypeInfo* ProductType() const = 0;
        /// The SERIALISED cooked form this factory reads out of the instance (the type the
        /// cook stamped: StaticMeshSource for a StaticMesh, TextureResource for a Texture) -
        /// the one link from a runtime type back to the builder that made it, which the scene
        /// format reference joins to the builder's product to name the asset type. A factory
        /// that reads the product type itself returns that.
        [[nodiscard]] virtual const TypeInfo* CookedType() const = 0;
        // Build the runtime product. `manager` lets a composite resource resolve its
        // child resources via manager.Bind<…>(childId) - and doing so AUTOMATICALLY
        // records a dependency edge, so reloading a child reloads this resource too.
        [[nodiscard]] virtual RefPtr<Object> Create(ResourceManager& manager,
                                                    foundation::content::Instance& instance) = 0;

        // --- Optional async two-stage path (task #123). Default: SupportsAsync() == false, so the
        //     manager builds synchronously via Create() (unchanged). A factory opts in by
        //     overriding all three. DecodeStage is a PURE function of the instance's (cheap,
        //     pak-backed) source bytes; it runs on a JobSystem worker and MUST NOT touch the
        //     ResourceManager, the GPU, or global mutable state. FinalizeStage runs on the MAIN
        //     thread and turns the decoded intermediate into the product (GPU upload, child
        //     manager.Bind()s, registry writes); a null return signals failure.
        [[nodiscard]] virtual bool SupportsAsync() const { return false; }
        [[nodiscard]] virtual RefPtr<Object> DecodeStage(foundation::content::Instance& instance)
        {
            (void)instance;
            return nullptr;
        }
        [[nodiscard]] virtual RefPtr<Object> FinalizeStage(ResourceManager& manager,
                                                           RefPtr<Object> decoded)
        {
            (void)manager;
            (void)decoded;
            return nullptr;
        }
    };

    // =======================================================================
    // ResourceManager - binds Guids to products over a content database, caching
    // handles and supporting hot reload. Factories are non-owning (registered by
    // the caller).
    // =======================================================================
    class ResourceManager
    {
    public:
        // `jobs` is the shared JobSystem used for async decode (BindAsync). Null = async degrades
        // to a synchronous Bind, so every existing caller keeps working unchanged. The constructing
        // thread is recorded as the main thread (async finalize / Pump must run on it).
        // The allocator (required - the owner decides) backs resource handles and
        // pending async loads.
        ResourceManager(IAllocator& allocator, foundation::content::IContentDatabase& database,
                        JobSystem* jobs = nullptr) noexcept
            : m_allocator(&allocator), m_database(&database), m_jobs(jobs),
              m_mainThreadId(Thread::CurrentId())
        {
        }

        // Drain outstanding decode jobs so none references this manager after destruction. Wait
        // participates in the pool, so a queued-but-unstarted decode still runs to completion; the
        // results are simply discarded (never finalized). Not finalizing here is deliberate -
        // FinalizeStage would touch factories/GPU, unsafe during teardown.
        ~ResourceManager()
        {
            if (m_jobs != nullptr)
            {
                for (auto& [id, record] : m_pending)
                {
                    (void)id;
                    m_jobs->Wait(record->counter);
                }
            }
        }

        /// The backing content database (path-addressed lookups: script facades and
        /// tooling resolve editor-visible content paths to instances, then Bind by id).
        [[nodiscard]] foundation::content::IContentDatabase& Database() const noexcept
        {
            return *m_database;
        }

        // === Diagnostics ===================================================================

        /// One row of the live-product report: everything the cache currently holds for one
        /// product type. `unreferenced` = handles whose product exists but has NO outside
        /// Proxy refs (cache-only - what a "purge unused" would release). Sizes arrive with
        /// the I5 allocator tagging; counts already answer "what is holding memory".
        struct LiveProductRow
        {
            const TypeInfo* type = nullptr; // null = handles whose type never resolved
            usize live = 0;                 // product present
            usize pending = 0;              // async decode in flight
            usize failed = 0;               // failed/empty handles still cached
            usize unreferenced = 0;         // live AND cache-only (evictable candidates)
        };

        /// Snapshot the cache into per-type rows (unsorted). The I4 instrumentation step:
        /// run it before designing eviction so "what is resident" is measured, not guessed.
        void ReportLiveProducts(Array<LiveProductRow>& outRows) const
        {
            outRows.Clear();
            for (const auto& pair : m_handles)
            {
                const RefPtr<ResourceHandle>& handle = pair.value;
                if (handle.Get() == nullptr)
                {
                    continue;
                }
                const TypeInfo* type = GlobalTypeRegistry().FindById(handle->ProductTypeId());
                LiveProductRow* row = nullptr;
                for (LiveProductRow& existing : outRows)
                {
                    if (existing.type == type)
                    {
                        row = &existing;
                        break;
                    }
                }
                if (row == nullptr)
                {
                    outRows.PushBack(LiveProductRow{});
                    row = &outRows[outRows.Size() - 1];
                    row->type = type;
                }
                if (handle->State() == ResourceState::Pending)
                {
                    ++row->pending;
                }
                else if (handle->Get() != nullptr)
                {
                    ++row->live;
                    // RefCount 2 = the cache map's ref + the report's iteration copy... the map
                    // stores RefPtr directly, so cache-only means EXACTLY one strong ref.
                    if (handle->RefCount() == 1)
                    {
                        ++row->unreferenced;
                    }
                }
                else
                {
                    ++row->failed;
                }
            }
        }

        /// Diagnostics/tripwires: is a factory registered for this product type? (The silent
        /// missing-factory class - the 2026-08-12 font incident - is what these guard.)
        [[nodiscard]] bool HasFactory(TypeId productTypeId) const noexcept
        {
            return m_factories.Find(productTypeId) != nullptr;
        }

        [[nodiscard]] usize FactoryCount() const noexcept { return m_factories.Size(); }
        /// Every registered factory, in no particular order (a tripwire, a schema join).
        template <typename Fn>
        void ForEachFactory(Fn&& fn) const
        {
            for (const auto& entry : m_factories)
            {
                fn(*entry.value);
            }
        }

        void AddFactory(IResourceFactory* factory)
        {
            if (factory != nullptr && factory->ProductType() != nullptr)
            {
                m_factories.InsertOrAssign(factory->ProductType()->id, factory);
            }
        }

        // Binds an id to a handle producing `productType`. Cached by id; a
        // flushed handle is rebuilt in place so existing proxies recover.
        [[nodiscard]] RefPtr<ResourceHandle> Bind(const TypeInfo& productType, const Guid& id)
        {
            // If a factory is mid-build and Binds this id, it's a dependency of the
            // resource currently building: record the edge so a reload propagates.
            if (!m_buildStack.IsEmpty())
            {
                RecordDependency(m_buildStack.Back(), id);
            }

            if (RefPtr<ResourceHandle>* cached = m_handles.Find(id))
            {
                // COPY the ref out of the map slot BEFORE building: the factory Binds child
                // resources, growing the handle map - a rehash dangles the slot pointer.
                // (The handle OBJECT itself is heap-stable; only the slot moves.)
                RefPtr<ResourceHandle> handle = *cached;
                // A sync Bind of a PENDING (async in-flight) id must return it READY: block-
                // complete the decode (participating in the pool) and finalize inline.
                if (handle->State() == ResourceState::Pending)
                {
                    CompletePending(id);
                    if (RefPtr<ResourceHandle>* refreshed = m_handles.Find(id))
                    {
                        return *refreshed; // re-find: finalize's child Binds may have rehashed
                    }
                    return handle;
                }
                if (handle->Get() == nullptr)
                {
                    BuildInto(*handle, productType.id, id);
                    SettleSyncState(*handle);
                }
                return handle;
            }
            RefPtr<ResourceHandle> handle = MakeRef<ResourceHandle>(*m_allocator);
            BuildInto(*handle, productType.id, id);
            SettleSyncState(*handle);
            m_handles.InsertOrAssign(id, handle);
            return handle;
        }

        template <typename T>
        [[nodiscard]] Proxy<T> Bind(const Guid& id)
        {
            return Proxy<T>(Bind(T::StaticType(), id));
        }

        template <typename T>
        [[nodiscard]] Proxy<T> Bind(const ResourceId<T>& rid)
        {
            return Bind<T>(rid.id);
        }

        // Asynchronous bind (task #123): returns the same handle/Proxy IMMEDIATELY with State()
        // == Pending (Get() null) and decodes on a JobSystem worker; Pump() finalizes it on the
        // main thread a frame or two later. Falls back to a synchronous, immediately-Ready build
        // when there is no JobSystem, no instance/factory, or the factory has not migrated
        // (SupportsAsync() == false). Concurrent BindAsync of the same id share the one handle.
        [[nodiscard]] RefPtr<ResourceHandle> BindAsync(const TypeInfo& productType, const Guid& id)
        {
            if (!m_buildStack.IsEmpty())
            {
                RecordDependency(m_buildStack.Back(), id);
            }
            if (RefPtr<ResourceHandle>* cached = m_handles.Find(id))
            {
                RefPtr<ResourceHandle> handle = *cached;
                // Dedup: a live product or an already-pending load is shared as-is.
                if (handle->Get() != nullptr || handle->State() == ResourceState::Pending)
                {
                    return handle;
                }
                StartAsyncBuild(handle, productType.id, id);
                return handle;
            }
            RefPtr<ResourceHandle> handle = MakeRef<ResourceHandle>(*m_allocator);
            m_handles.InsertOrAssign(id, handle);
            StartAsyncBuild(handle, productType.id, id);
            return handle;
        }

        template <typename T>
        [[nodiscard]] Proxy<T> BindAsync(const Guid& id)
        {
            return Proxy<T>(BindAsync(T::StaticType(), id));
        }

        template <typename T>
        [[nodiscard]] Proxy<T> BindAsync(const ResourceId<T>& rid)
        {
            return BindAsync<T>(rid.id);
        }

        // Finalize completed async decodes on the MAIN thread, FIFO, until `budgetSeconds` is
        // spent (default ~2ms). Tick once per frame BEFORE subsystem update so this-frame spawns
        // see ready resources. On a 0-worker (web) JobSystem it also drives queued decodes inline
        // within the budget (the pool has no threads to run them otherwise).
        void Pump(f64 budgetSeconds = 0.002)
        {
            AssertMainThread();
            const Stopwatch stopwatch = Stopwatch::StartNew();
            for (;;)
            {
                CompletedDecode entry;
                bool have = false;
                {
                    ScopedLock lock(m_completedMutex);
                    if (!m_completed.IsEmpty())
                    {
                        entry = Move(m_completed[0]);
                        m_completed.RemoveAt(0);
                        have = true;
                    }
                }
                if (!have)
                {
                    // 0-worker fallback: nothing has run the decode yet - drive one inline.
                    if (m_jobs != nullptr && m_jobs->WorkerCount() == 0 &&
                        stopwatch.Elapsed().AsSeconds() < budgetSeconds && DriveOneInlineDecode())
                    {
                        continue;
                    }
                    break;
                }

                const Stopwatch finalizeClock = Stopwatch::StartNew();
                FinalizeCompleted(entry);
                const f64 finalizeMs = finalizeClock.Elapsed().AsMilliseconds();
                if (finalizeMs >= 50.0)
                {
                    // One finalize past a frame, by type and id: a large GPU upload, a
                    // nested SYNC bind inside it, or (Debug, expected) a shader compile /
                    // pipeline creation under validation.
                    const TypeInfo* productType =
                        entry.factory != nullptr ? entry.factory->ProductType() : nullptr;
                    const char* typeName = productType != nullptr ? productType->name : "?";
                    LOG_INFO(u8"Resource", u8"finalize of {} {} took {} ms on the main thread",
                             StringView(reinterpret_cast<const char8_t*>(typeName)), entry.id,
                             static_cast<i64>(finalizeMs));
                }

                if (stopwatch.Elapsed().AsSeconds() >= budgetSeconds)
                {
                    break;
                }
            }
            ReapPending();
            ReportSettledBurst();
        }

        // Block the main thread until every pending async load has finalized (participating in the
        // pool). For loading screens and tests only - never call from the per-frame path.
        void WaitAll()
        {
            AssertMainThread();
            for (usize guard = 0; guard < 4096; ++guard)
            {
                bool anyOutstanding = false;
                if (m_jobs != nullptr)
                {
                    for (auto& [id, record] : m_pending)
                    {
                        (void)id;
                        if (!record->finalized)
                        {
                            anyOutstanding = true;
                            m_jobs->Wait(record->counter); // runs the decode inline if not yet done
                        }
                    }
                }
                Pump(1.0e9); // unbounded: finalize everything now decoded
                if (m_pending.IsEmpty() || !anyOutstanding)
                {
                    break;
                }
            }
        }

        // Number of async loads still pending (decoding or awaiting finalize). Main-thread only;
        // loading screens read (pending, total) to show progress.
        [[nodiscard]] usize PendingCount() const noexcept { return m_pending.Size(); }

        // When enabled, Ref<T>::Bind routes through BindAsync instead of Bind. A scene load flips
        // this on around ResolveSceneResources (see AsyncBindScope) so the whole scene's resource
        // set decodes on workers; the caller then Pumps to completion (AsyncLoadBatch / a loading
        // screen). Default off = every existing Ref bind stays synchronous.
        void SetAsyncBinds(bool enabled) noexcept { m_asyncBinds = enabled; }
        [[nodiscard]] bool AsyncBindsEnabled() const noexcept { return m_asyncBinds; }

        // Whether a bind of a product type no factory builds is logged, as a host wiring error.
        // Off for a manager that only collects references (a scene's reference scan): it has no
        // factories by design, and every bind landing unresolved is the answer it wants, not a
        // fault to report.
        void SetReportsMissingFactories(bool enabled) noexcept { m_reportsMissingFactories = enabled; }
        [[nodiscard]] bool ReportsMissingFactories() const noexcept { return m_reportsMissingFactories; }

        // Rebuilds the product for an already-bound id (e.g. after the source
        // changed on disk) AND, transitively, every resource that depends on it.
        // All proxies see the new products. False if `id` is unbound.
        bool Reload(const Guid& id)
        {
            if (m_handles.Find(id) == nullptr)
            {
                return false;
            }
            Array<Guid> visited;
            ReloadRecursive(id, visited);
            // RE-find: the recursive rebuild binds children, growing the handle map - the
            // pre-reload slot pointer dangles after a rehash. (This was the Sponza crash:
            // the post-cook mass reload of 167 products rehashed mid-cascade.)
            RefPtr<ResourceHandle>* handle = m_handles.Find(id);
            return handle != nullptr && (*handle)->Get() != nullptr;
        }

        // The ids that directly depend on `id` (introspection/tooling). Empty if none.
        // (Edges are recorded automatically when a factory Binds a child mid-build;
        // explicit declaration isn't exposed yet - every planned dependency, incl.
        // include resources, resolves through Bind.)
        [[nodiscard]] Span<const Guid> Dependents(const Guid& id) noexcept
        {
            Array<Guid>* d = m_dependents.Find(id);
            return (d != nullptr) ? Span<const Guid>(d->Data(), d->Size()) : Span<const Guid>{};
        }

        /// Appends every id that was requested (Bind) but has no product - the backing
        /// instance was missing or its factory failed. Editor tooling cooks exactly this
        /// set when a page opens over uncooked content (product guid == source guid, so
        /// an unresolved id IS a valid cook root).
        void CollectUnresolved(Array<Guid>& out)
        {
            for (auto& [id, handle] : m_handles)
            {
                if (handle->Get() == nullptr)
                {
                    out.PushBack(id);
                }
            }
        }

        /// Frame tick: ages the graveyard of hot-reloaded-away products and releases the
        /// ones old enough that no in-flight frame can still reference their GPU objects.
        void CollectGarbage()
        {
            // Take the graveyard OUT of the member first: releasing a product runs its
            // destructor, which can RE-ENTER the manager (a dying composite drops child
            // proxies; bookkeeping may push new graves) - mutating m_graveyard while a loop
            // walks it is a use-after-free (the Sponza mass-reload crash: hundreds of
            // products landing in one cook aged out together).
            Array<Grave> graves = Move(m_graveyard);
            m_graveyard = Array<Grave>{};

            Array<Grave> dropped;
            for (Grave& grave : graves)
            {
                if (grave.framesLeft <= 1)
                {
                    dropped.PushBack(Move(grave));
                    continue;
                }
                grave.framesLeft -= 1;
                m_graveyard.PushBack(Move(grave));
            }
            // Destructors run HERE, with m_graveyard consistent; re-entrant pushes append
            // to the fresh member array safely.
            dropped.Clear();
        }

        // Drops the product from a handle without unbinding it; a later Bind/
        // Reload rebuilds it. False if unbound.
        bool Flush(const Guid& id)
        {
            RefPtr<ResourceHandle>* handle = m_handles.Find(id);
            if (handle == nullptr)
            {
                return false;
            }
            (*handle)->Flush();
            return true;
        }

    private:
        // Async-load records (task #123). PendingLoad holds a non-movable Counter, so it lives in a
        // UniquePtr slot (heap-stable address the JobSystem signals). CompletedDecode is the
        // self-contained result a worker pushes and Pump finalizes - it carries its own handle +
        // factory, so finalize never has to touch the pending map.
        struct PendingLoad
        {
            RefPtr<ResourceHandle> handle;
            Guid id;
            Counter counter{1};     // 1 -> 0 when the decode job completes
            bool finalized = false; // product set on main; safe to reap once counter == 0
        };
        struct CompletedDecode
        {
            Guid id;
            RefPtr<ResourceHandle> handle;
            IResourceFactory* factory = nullptr;
            RefPtr<Object> decoded; // null => decode failed
            bool ok = false;
        };

        void BuildInto(ResourceHandle& handle, TypeId productTypeId, const Guid& id)
        {
            // A rebuild may resolve different children than before; drop the old
            // forward edges so they're re-recorded fresh during this build.
            ClearForwardDeps(id);

            handle.SetProductTypeId(productTypeId);
            // Park the outgoing product instead of destroying it now: GPU products own
            // views/buffers that in-flight frames may still reference - CollectGarbage
            // (ticked by the host once per frame) releases them a few frames later.
            if (Object* old = handle.Get())
            {
                m_graveyard.PushBack(Grave{RefPtr<Object>(old), kGraveFrames});
            }
            handle.Replace(nullptr);

            foundation::content::Instance* instance = m_database->GetInstance(id);
            if (instance == nullptr)
            {
                // A broken reference (the id is not in the content DB) - name it; a silent
                // null handle cost a live-repro session to diagnose (2026-08-12).
                LOG_WARNING(u8"Resource", u8"bind failed: no content instance for id {}", id);
                return;
            }

            IResourceFactory* const* factory = m_factories.Find(productTypeId);
            if (factory == nullptr)
            {
                // A HOST WIRING error, not a data error: the product type has no registered
                // IResourceFactory (RegisterStandardFactories or the host forgot AddFactory).
                if (m_reportsMissingFactories)
                {
                    LOG_WARNING(u8"Resource",
                                u8"bind failed: no resource factory registered for the product "
                                u8"type of '{}' ('{}'::'{}') - host is missing an AddFactory",
                                instance->Name(), instance->TypeNamespace(),
                                instance->TypeName());
                }
                return;
            }

            // While `id` is on the build stack, any Bind() the factory makes is
            // recorded as a dependency of `id` (see Bind).
            m_buildStack.PushBack(id);
            handle.Replace((*factory)->Create(*this, *instance));
            m_buildStack.PopBack();
        }

        // Kick off an async build: park any old product, mark Pending, submit the pure decode to a
        // worker. Degrades to a synchronous, immediately-settled BuildInto when async is impossible
        // (no job system / instance / factory, or a factory that has not migrated).
        void StartAsyncBuild(const RefPtr<ResourceHandle>& handle, TypeId productTypeId,
                             const Guid& id)
        {
            handle->SetProductTypeId(productTypeId);

            foundation::content::Instance* instance = m_database->GetInstance(id);
            IResourceFactory* const* factorySlot = m_factories.Find(productTypeId);
            IResourceFactory* factory = (factorySlot != nullptr) ? *factorySlot : nullptr;

            if (m_jobs == nullptr || instance == nullptr || factory == nullptr ||
                !factory->SupportsAsync())
            {
                BuildInto(*handle, productTypeId, id);
                SettleSyncState(*handle);
                return;
            }

            // Park the outgoing product (in-flight frames may still read its GPU objects), mark
            // pending, and drop stale forward edges so the rebuild re-records children fresh.
            if (Object* old = handle->Get())
            {
                m_graveyard.PushBack(Grave{RefPtr<Object>(old), kGraveFrames});
            }
            handle->Replace(nullptr);
            handle->SetState(ResourceState::Pending);
            ClearForwardDeps(id);

            UniquePtr<PendingLoad> pending = MakeUnique<PendingLoad>(*m_allocator);
            pending->handle = handle;
            pending->id = id;
            PendingLoad* record = pending.Get();
            m_pending.InsertOrAssign(id, Move(pending));
            NoteBurstGrowth();

            // The job captures only thread-safe data: the factory/instance pointers (stable for the
            // load's lifetime), a RefPtr copy of the handle (atomic refcount), and the id by value.
            // It NEVER touches the handle or pending maps - those stay main-thread. The counter
            // 1 -> 0 on completion lets WaitAll / the sync-upgrade path wait on this decode.
            m_jobs->Submit(
                [this, id, jobHandle = handle, factory, instance]() mutable
                {
                    RefPtr<Object> decoded = factory->DecodeStage(*instance);
                    const bool ok = decoded.Get() != nullptr;
                    ScopedLock lock(m_completedMutex);
                    m_completed.PushBack(
                        CompletedDecode{id, Move(jobHandle), factory, Move(decoded), ok});
                },
                &record->counter);
        }

        // Turn one decoded entry into its product on the main thread: FinalizeStage with the id on
        // the build stack (child Binds record dependency edges), settle state, fire OnReady on
        // success, and mark the pending record finalized.
        void FinalizeCompleted(CompletedDecode& entry)
        {
            RefPtr<Object> product;
            if (entry.ok)
            {
                m_buildStack.PushBack(entry.id);
                product = entry.factory->FinalizeStage(*this, Move(entry.decoded));
                m_buildStack.PopBack();
            }
            const bool ready = product.Get() != nullptr;
            entry.handle->Replace(product);
            entry.handle->SetState(ready ? ResourceState::Ready : ResourceState::Failed);
            if (UniquePtr<PendingLoad>* record = m_pending.Find(entry.id))
            {
                (*record)->finalized = true;
            }
            if (ready)
            {
                entry.handle->FireOnReady();
                // A settling child revives its dependents through the SAME edge a hot reload
                // uses (recorded when their factory bound this id mid-build): a material built
                // while its textures were still Pending skipped those slots - the reload
                // rebuilds it now that the child is live, and pop-in composes transitively.
                // COPY the list: Reload binds children, growing maps (the rehash lesson).
                // The rebuild runs with async binds ON whatever the caller's mode: a material
                // reloading for its first texture must NOT block-complete its other, still
                // pending textures - a sync Bind of a pending id waits out the decode and
                // finalized EVERYTHING decoded so far, nested, one 8 s stall on the Bistro
                // prefab open (2026-09-23). Pending slots stay skipped; each settle reloads.
                const Span<const Guid> dependentsView = Dependents(entry.id);
                if (!dependentsView.IsEmpty())
                {
                    Array<Guid> dependents;
                    for (const Guid& d : dependentsView)
                    {
                        dependents.PushBack(d);
                    }
                    const bool previousAsync = m_asyncBinds;
                    m_asyncBinds = true;
                    for (const Guid& d : dependents)
                    {
                        (void)Reload(d);
                    }
                    m_asyncBinds = previousAsync;
                }
            }
        }

        // Block-complete an in-flight async load, then finalize it (a sync Bind of a pending id).
        void CompletePending(const Guid& id)
        {
            if (UniquePtr<PendingLoad>* record = m_pending.Find(id))
            {
                if (m_jobs != nullptr)
                {
                    m_jobs->Wait((*record)->counter); // runs the decode inline if not yet done
                }
            }
            // Finalize THIS id only. Draining every decoded entry here (the old unbounded
            // Pump) made one sync Bind pay for the whole burst's finalizes - GPU uploads
            // included - on the caller's thread; the rest keep their FIFO turn in Pump.
            CompletedDecode entry;
            bool have = false;
            {
                ScopedLock lock(m_completedMutex);
                for (usize i = 0; i < m_completed.Size(); ++i)
                {
                    if (m_completed[i].id == id)
                    {
                        entry = Move(m_completed[i]);
                        m_completed.RemoveAt(i);
                        have = true;
                        break;
                    }
                }
            }
            if (have)
            {
                FinalizeCompleted(entry);
                ReapPending();
                ReportSettledBurst();
                return;
            }
            Pump(1.0e9); // not in the completed queue (a foreign wait raced it): drain
        }

        static void SettleSyncState(ResourceHandle& handle) noexcept
        {
            handle.SetState(handle.Get() != nullptr ? ResourceState::Ready : ResourceState::Failed);
        }

        // 0-worker fallback: run one not-yet-started queued decode inline (Wait participates,
        // executing the job). False when no undriven decode remains.
        bool DriveOneInlineDecode()
        {
            for (auto& [id, record] : m_pending)
            {
                (void)id;
                if (!record->finalized && record->counter.Value() != 0)
                {
                    m_jobs->Wait(record->counter);
                    return true;
                }
            }
            return false;
        }

        // Free finalized pending records. Freeing must go through the JobSystem's lifetime fence
        // (Wait), NOT a bare counter.Value()==0 check: the count is set to 0 INSIDE the JobSystem's
        // Counter lock (see RunJob), so a value of 0 is observable while the signaling worker is
        // still touching the Counter - destroying it then is a data race (TSAN-confirmed). Wait
        // returns only after that critical section is released; for a finalized record the decode
        // body is already done, so Wait is effectively non-blocking here.
        // A burst = the span from the first async bind while none was in flight to the Pump
        // that drains the last one: a scene open queues hundreds of binds in one frame and they
        // settle over the next seconds. The peak is sampled at BIND time (a pump may finalize
        // and reap part of a burst before it looks) and reported once, when the set empties.
        void NoteBurstGrowth()
        {
            if (!m_burstActive)
            {
                m_burstActive = true;
                m_burstClock = Stopwatch::StartNew();
                m_burstPeak = 0;
            }
            if (m_pending.Size() > m_burstPeak)
            {
                m_burstPeak = m_pending.Size();
            }
        }
        void ReportSettledBurst()
        {
            if (m_pending.IsEmpty() && m_burstActive)
            {
                m_burstActive = false;
                LOG_INFO(u8"Resource", u8"async loads settled: {} in flight at the peak, {} ms",
                         m_burstPeak, static_cast<i64>(m_burstClock.Elapsed().AsMilliseconds()));
            }
        }
        void ReapPending()
        {
            Array<Guid> reap;
            for (auto& [id, record] : m_pending)
            {
                if (record->finalized)
                {
                    reap.PushBack(id);
                }
            }
            for (const Guid& id : reap)
            {
                if (UniquePtr<PendingLoad>* record = m_pending.Find(id))
                {
                    if (m_jobs != nullptr)
                    {
                        m_jobs->Wait((*record)->counter); // lifetime fence before free
                    }
                    m_pending.Remove(id);
                }
            }
        }

        void AssertMainThread() const noexcept
        {
            DIAGNOSTIC_ASSERT_MSG(Thread::CurrentId() == m_mainThreadId,
                                "ResourceManager async finalize/pump must run on the main thread");
        }

        // Rebuild `id`, then transitively every resource that depends on it. The
        // visited list guards against cycles; dependents are snapshotted because a
        // dependent's rebuild mutates m_dependents[id] (clear+re-record its edges).
        void ReloadRecursive(const Guid& id, Array<Guid>& visited)
        {
            for (const Guid& v : visited)
            {
                if (v == id)
                {
                    return;
                }
            }
            visited.PushBack(id);

            if (RefPtr<ResourceHandle>* handle = m_handles.Find(id))
            {
                BuildInto(**handle, (*handle)->ProductTypeId(), id);
            }

            Array<Guid> dependents;
            if (Array<Guid>* d = m_dependents.Find(id))
            {
                for (const Guid& g : *d)
                {
                    dependents.PushBack(g);
                }
            }
            for (const Guid& dep : dependents)
            {
                ReloadRecursive(dep, visited);
            }
        }

        void RecordDependency(const Guid& dependent, const Guid& dependency)
        {
            if (dependent == dependency)
            {
                return;
            }
            AddEdgeUnique(m_dependencies, dependent, dependency);
            AddEdgeUnique(m_dependents, dependency, dependent);
        }

        // Drop `id`'s outgoing edges (and the matching reverse entries).
        void ClearForwardDeps(const Guid& id)
        {
            Array<Guid>* deps = m_dependencies.Find(id);
            if (deps == nullptr)
            {
                return;
            }
            for (const Guid& d : *deps)
            {
                if (Array<Guid>* rev = m_dependents.Find(d))
                {
                    for (usize i = 0; i < rev->Size(); ++i)
                    {
                        if ((*rev)[i] == id)
                        {
                            rev->RemoveAt(i);
                            break;
                        }
                    }
                }
            }
            deps->Clear();
        }

        static void AddEdgeUnique(HashMap<Guid, Array<Guid>>& map, const Guid& key,
                                  const Guid& value)
        {
            Array<Guid>* arr = map.Find(key);
            if (arr == nullptr)
            {
                map.InsertOrAssign(key, Array<Guid>{});
                arr = map.Find(key);
            }
            for (const Guid& g : *arr)
            {
                if (g == value)
                {
                    return;
                }
            }
            arr->PushBack(value);
        }

        IAllocator* m_allocator;
        foundation::content::IContentDatabase* m_database;
        HashMap<TypeId, IResourceFactory*> m_factories;
        HashMap<Guid, RefPtr<ResourceHandle>> m_handles;
        HashMap<Guid, Array<Guid>> m_dependencies; // id -> resources it depends on
        HashMap<Guid, Array<Guid>> m_dependents;   // id -> resources that depend on it
        Array<Guid> m_buildStack;                  // ids currently building (auto-edge source)

        // --- async load bookkeeping (task #123); PendingLoad/CompletedDecode declared above ---
        JobSystem* m_jobs = nullptr; // shared decode pool (null = sync-only)
        u64 m_mainThreadId = 0;      // thread that constructs/pumps; async finalize must run here
        bool m_asyncBinds = false;   // Ref<T>::Bind routes through BindAsync while set
        bool m_reportsMissingFactories = true; // off: a collector, quiet on a missing factory
        HashMap<Guid, UniquePtr<PendingLoad>> m_pending; // main-thread only (heap-stable Counter)
        bool m_burstActive = false;  // ReportSettledBurst: a burst of async loads is in flight
        usize m_burstPeak = 0;
        Stopwatch m_burstClock;
        Mutex m_completedMutex;                          // guards m_completed (worker <-> main)
        Array<CompletedDecode> m_completed;              // FIFO decode results, drained by Pump

        static constexpr u32 kGraveFrames = 8; // > max frames in flight, comfortably
        struct Grave
        {
            RefPtr<Object> product;
            u32 framesLeft = 0;
        };
        Array<Grave> m_graveyard; // hot-reloaded-away products awaiting release
    };

    template <typename T>
    void Ref<T>::Bind(ResourceManager& manager)
    {
        if (!id.IsNil())
        {
            // Route through BindAsync when the manager is in async-bind mode (scene load); the
            // proxy is null (Pending) until Pump finalizes it - Proxy already tolerates that.
            m_proxy = manager.AsyncBindsEnabled() ? manager.BindAsync<T>(id) : manager.Bind<T>(id);
        }
    }

    // RAII: turn on async Ref binds for a scope (e.g. around ResolveSceneResources), restoring the
    // previous mode on exit even on an early return. Nesting-safe (saves/restores the prior value).
    class AsyncBindScope
    {
    public:
        explicit AsyncBindScope(ResourceManager& manager) noexcept
            : m_manager(&manager), m_previous(manager.AsyncBindsEnabled())
        {
            manager.SetAsyncBinds(true);
        }
        ~AsyncBindScope() { m_manager->SetAsyncBinds(m_previous); }
        AsyncBindScope(const AsyncBindScope&) = delete;
        AsyncBindScope& operator=(const AsyncBindScope&) = delete;

    private:
        ResourceManager* m_manager;
        bool m_previous;
    };

    // Loading-screen helper: after issuing a batch of async binds (e.g. a scene resolve under an
    // AsyncBindScope), Snapshot() records the outstanding count as the batch total, then Progress()
    // reports 0..1 as loads finalize. Step() pumps within a per-frame budget; WaitComplete() blocks
    // to the end (non-interactive loading screen / tests). NOTE: Remaining() reads the manager's
    // TOTAL pending, so for accurate progress the manager should be dedicated to this batch (the
    // usual scene-load case); unrelated concurrent async loads would skew it.
    class AsyncLoadBatch
    {
    public:
        explicit AsyncLoadBatch(ResourceManager& manager) noexcept : m_manager(&manager) {}

        void Snapshot() noexcept { m_total = m_manager->PendingCount(); }

        [[nodiscard]] usize Total() const noexcept { return m_total; }
        [[nodiscard]] usize Remaining() const noexcept { return m_manager->PendingCount(); }
        [[nodiscard]] bool IsComplete() const noexcept { return m_manager->PendingCount() == 0; }

        [[nodiscard]] f32 Progress() const noexcept
        {
            const usize remaining = m_manager->PendingCount();
            if (m_total == 0 || remaining == 0)
            {
                return 1.0f;
            }
            const usize done = (remaining >= m_total) ? 0u : (m_total - remaining);
            return static_cast<f32>(done) / static_cast<f32>(m_total);
        }

        // Finalize completed loads within the budget; returns true once the batch is complete.
        bool Step(f64 budgetSeconds = 0.002)
        {
            m_manager->Pump(budgetSeconds);
            return IsComplete();
        }

        // Block until every outstanding load has finalized.
        void WaitComplete() { m_manager->WaitAll(); }

    private:
        ResourceManager* m_manager;
        usize m_total = 0;
    };

    // =======================================================================
    // Resource composition (Documentation/Specs/engine-composition.md, D1-D3). A resource
    // library DECLARES its module: its type registration and, per factory, a description -
    // product, cooked form, the service it needs beyond an allocator, and how to create it.
    // A composition creates the factories it can, OWNS them (ResourceFactorySet) and registers
    // them into a manager. Nobody collects factories by hand; a factory belongs to the
    // resource it produces, and a domain that names the resource module brings the factory.
    // =======================================================================

    /// What a factory may need beyond an allocator - a GPU device, a shader system - asked for
    /// BY TYPE: TypeOf<T>().id, process-single and stable across shared libraries. The host
    /// answers what it has; a factory whose service is absent is not created, and the set says so.
    class IResourceServices
    {
    public:
        virtual ~IResourceServices() = default;
        /// The service instance for `type`, or null when this host has none.
        [[nodiscard]] virtual void* Service(TypeId type) const noexcept = 0;
        template <typename T>
        [[nodiscard]] T* Get() const noexcept
        {
            return static_cast<T*>(Service(TypeOf<T>().id));
        }
    };

    /// A host with nothing to offer (headless tools): every gated factory is skipped.
    class NoResourceServices final : public IResourceServices
    {
    public:
        [[nodiscard]] void* Service(TypeId) const noexcept override { return nullptr; }
    };

    /// A host's services, by type: `Add<T>(&instance)` for each service it has (a device, a shader
    /// system), answered to any factory that asks for T. The one implementation every host needs.
    class ResourceServiceTable final : public IResourceServices
    {
    public:
        template <typename T>
        void Add(T* instance)
        {
            if (instance != nullptr)
            {
                m_services.PushBack(Entry{TypeOf<T>().id, static_cast<void*>(instance)});
            }
        }
        [[nodiscard]] void* Service(TypeId type) const noexcept override
        {
            for (const Entry& entry : m_services)
            {
                if (entry.type == type)
                {
                    return entry.instance;
                }
            }
            return nullptr;
        }

    private:
        struct Entry
        {
            TypeId type;
            void* instance;
        };
        Array<Entry> m_services;
    };

    /// What a factory IS before one exists: a constant, readable without constructing anything
    /// (the scene format reference joins on `product` and `cooked`). `service` names the type
    /// the factory needs beyond an allocator (null for most); `create` returns null when that
    /// service is absent from the host.
    struct ResourceFactoryDesc
    {
        const TypeInfo* (*product)();
        const TypeInfo* (*cooked)();
        const TypeInfo* (*service)();
        UniquePtr<IResourceFactory> (*create)(IAllocator& allocator, const IResourceServices& services);
        /// The further cooked forms the factory reads beside `cooked`, by index, null past the
        /// last (a render texture's record beside a texture's): each is another asset type the
        /// product is made from. Null when there are none.
        const TypeInfo* (*alsoCooked)(usize index) = nullptr;

        /// `cooked`, then every `alsoCooked` form.
        template <typename Visit>
        void ForEachCooked(Visit&& visit) const
        {
            visit(cooked());
            if (alsoCooked != nullptr)
            {
                for (usize i = 0; const TypeInfo* type = alsoCooked(i); ++i)
                {
                    visit(type);
                }
            }
        }
    };

    /// A description for a factory constructed from the allocator alone (`Factory(IAllocator&)`).
    template <typename Product, typename Cooked, typename Factory>
    [[nodiscard]] constexpr ResourceFactoryDesc FactoryWithAllocator() noexcept
    {
        return ResourceFactoryDesc{
            []() -> const TypeInfo* { return &Product::StaticType(); },
            []() -> const TypeInfo* { return &Cooked::StaticType(); },
            nullptr,
            [](IAllocator& allocator, const IResourceServices&) -> UniquePtr<IResourceFactory>
            { return MakeUnique<Factory>(allocator, allocator); }};
    }

    /// A description for a default-constructed factory (it allocates from nothing the host owns).
    template <typename Product, typename Cooked, typename Factory>
    [[nodiscard]] constexpr ResourceFactoryDesc FactoryByDefault() noexcept
    {
        return ResourceFactoryDesc{
            []() -> const TypeInfo* { return &Product::StaticType(); },
            []() -> const TypeInfo* { return &Cooked::StaticType(); },
            nullptr,
            [](IAllocator& allocator, const IResourceServices&) -> UniquePtr<IResourceFactory>
            { return MakeUnique<Factory>(allocator); }};
    }

    /// A description for a factory that needs a host `Service` (`Factory(IAllocator&, Service&)`):
    /// created only when the host answers for `TypeOf<Service>()`.
    template <typename Product, typename Cooked, typename Factory, typename Service>
    [[nodiscard]] constexpr ResourceFactoryDesc FactoryWithService() noexcept
    {
        return ResourceFactoryDesc{
            []() -> const TypeInfo* { return &Product::StaticType(); },
            []() -> const TypeInfo* { return &Cooked::StaticType(); },
            []() -> const TypeInfo* { return &TypeOf<Service>(); },
            [](IAllocator& allocator, const IResourceServices& services) -> UniquePtr<IResourceFactory>
            {
                Service* service = services.Get<Service>();
                if (service == nullptr)
                {
                    return UniquePtr<IResourceFactory>{};
                }
                return MakeUnique<Factory>(allocator, allocator, *service);
            }};
    }

    /// One resource library's declaration: an id, its type registration (idempotent; null when
    /// the library has none of its own) and its factory descriptions. Declared `inline constexpr`
    /// in the library's interface: a constant table, duplicated per image without harm.
    struct ResourceModule
    {
        StringView id;
        void (*registerTypes)();
        const ResourceFactoryDesc* factories;
        usize factoryCount;

        [[nodiscard]] Span<const ResourceFactoryDesc> Factories() const noexcept
        {
            return Span<const ResourceFactoryDesc>{factories, factoryCount};
        }
        void RegisterTypes() const
        {
            if (registerTypes != nullptr)
            {
                registerTypes();
            }
        }
    };

    /// The factories a composition created, owned here. `Create` is idempotent by product type:
    /// a second call with richer services fills what the first skipped and creates nothing twice.
    class ResourceFactorySet
    {
    public:
        /// Creates every description of `modules` whose product is not yet in the set and whose
        /// service (if any) `services` answers; the rest are recorded under Skipped().
        void Create(Span<const ResourceModule* const> modules, IAllocator& allocator,
                    const IResourceServices& services)
        {
            for (const ResourceModule* module : modules)
            {
                for (const ResourceFactoryDesc& desc : module->Factories())
                {
                    const TypeInfo* product = desc.product != nullptr ? desc.product() : nullptr;
                    if (product == nullptr || Has(product->id))
                    {
                        Forget(desc);
                        continue;
                    }
                    UniquePtr<IResourceFactory> factory =
                        desc.create != nullptr ? desc.create(allocator, services)
                                               : UniquePtr<IResourceFactory>{};
                    if (factory.Get() == nullptr)
                    {
                        Remember(desc);
                        continue;
                    }
                    Forget(desc);
                    m_factories.PushBack(Move(factory));
                }
            }
        }
        /// Registers every created factory into `manager` (non-owning, as AddFactory is).
        void Register(ResourceManager& manager) const
        {
            for (const UniquePtr<IResourceFactory>& factory : m_factories)
            {
                manager.AddFactory(factory.Get());
            }
        }
        template <typename Fn>
        void ForEach(Fn&& fn) const
        {
            for (const UniquePtr<IResourceFactory>& factory : m_factories)
            {
                fn(*factory);
            }
        }
        [[nodiscard]] usize Count() const noexcept { return m_factories.Size(); }
        /// Destroys every factory (a host does this while the device its factories used is alive).
        void Clear()
        {
            m_factories.Clear();
            m_skipped.Clear();
        }
        [[nodiscard]] bool Has(TypeId productId) const noexcept
        {
            for (const UniquePtr<IResourceFactory>& factory : m_factories)
            {
                if (factory->ProductType()->id == productId)
                {
                    return true;
                }
            }
            return false;
        }
        /// The descriptions the last Create calls could not honour (their `service` says why).
        [[nodiscard]] Span<const ResourceFactoryDesc* const> Skipped() const noexcept
        {
            return Span<const ResourceFactoryDesc* const>{m_skipped.Data(), m_skipped.Size()};
        }

    private:
        void Remember(const ResourceFactoryDesc& desc)
        {
            for (const ResourceFactoryDesc* known : m_skipped)
            {
                if (known == &desc)
                {
                    return;
                }
            }
            m_skipped.PushBack(&desc);
        }
        void Forget(const ResourceFactoryDesc& desc)
        {
            for (usize i = 0; i < m_skipped.Size(); ++i)
            {
                if (m_skipped[i] == &desc)
                {
                    m_skipped.RemoveAt(i);
                    return;
                }
            }
        }

        Array<UniquePtr<IResourceFactory>> m_factories;
        Array<const ResourceFactoryDesc*> m_skipped;
    };
}
