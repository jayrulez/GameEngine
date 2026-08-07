// Draconic::Resource - the `draconic.resource` module.
//
// The resource manager: turns content-database *source* objects (ISerializable,
// full editor fidelity) into runtime *products* (lean Objects) via factories,
// hands them out behind replaceable handles, and caches/reloads them. This is
// the source->product split: the editor authors a `…Resource` in the content
// db; a factory builds the runtime product the game actually uses. Editor-only
// data (node positions, comments) lives on the source and never reaches the
// product.
//
// Sits at the top of the asset stack: Foundation/VFS -> content -> resource.

module;
#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Prelude.h"

export module draconic.resource;

import draconic.foundation;
import draconic.content;

using namespace draconic::foundation;

export namespace draconic::resource
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
    // pipeline's §8 layer). Identity is a Guid (written by Serialize); at runtime
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

    // Serialization: identity only (found by ADL from component Serialize bodies). On READ,
    // blobs replay over LIVE components (paste / prefab revert / undo): when the incoming id
    // differs from the current one, the stale proxy/direct binding is dropped - critically,
    // reading a NIL id actually unbinds instead of leaving the old resource rendering.
    template <typename T>
    void Serialize(ISerializer& ar, Ref<T>& ref)
    {
        const Guid before = ref.id;
        draconic::foundation::Serialize(ar, ref.id);
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
        // Build the runtime product. `manager` lets a composite resource resolve its
        // child resources via manager.Bind<…>(childId) - and doing so AUTOMATICALLY
        // records a dependency edge, so reloading a child reloads this resource too.
        [[nodiscard]] virtual RefPtr<Object> Create(ResourceManager& manager,
                                                    draconic::content::Instance& instance) = 0;

        // --- Optional async two-stage path (task #123). Default: SupportsAsync() == false, so the
        //     manager builds synchronously via Create() (unchanged). A factory opts in by
        //     overriding all three. DecodeStage is a PURE function of the instance's (cheap,
        //     pak-backed) source bytes; it runs on a JobSystem worker and MUST NOT touch the
        //     ResourceManager, the GPU, or global mutable state. FinalizeStage runs on the MAIN
        //     thread and turns the decoded intermediate into the product (GPU upload, child
        //     manager.Bind()s, registry writes); a null return signals failure.
        [[nodiscard]] virtual bool SupportsAsync() const { return false; }
        [[nodiscard]] virtual RefPtr<Object> DecodeStage(draconic::content::Instance& instance)
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
        explicit ResourceManager(draconic::content::IContentDatabase& database,
                                 JobSystem* jobs = nullptr) noexcept
            : m_database(&database), m_jobs(jobs), m_mainThreadId(Thread::CurrentId())
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
        [[nodiscard]] draconic::content::IContentDatabase& Database() const noexcept
        {
            return *m_database;
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
            RefPtr<ResourceHandle> handle = MakeRef<ResourceHandle>(DefaultAllocator());
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
            RefPtr<ResourceHandle> handle = MakeRef<ResourceHandle>(DefaultAllocator());
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

                FinalizeCompleted(entry);

                if (stopwatch.Elapsed().AsSeconds() >= budgetSeconds)
                {
                    break;
                }
            }
            ReapPending();
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

            draconic::content::Instance* instance = m_database->GetInstance(id);
            if (instance == nullptr)
            {
                return;
            }

            IResourceFactory* const* factory = m_factories.Find(productTypeId);
            if (factory == nullptr)
            {
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

            draconic::content::Instance* instance = m_database->GetInstance(id);
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

            UniquePtr<PendingLoad> pending = MakeUnique<PendingLoad>(DefaultAllocator());
            pending->handle = handle;
            pending->id = id;
            PendingLoad* record = pending.Get();
            m_pending.InsertOrAssign(id, Move(pending));

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
            Pump(1.0e9); // unbounded: finalize this id (and any other now-decoded loads)
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
            DRACONIC_ASSERT_MSG(Thread::CurrentId() == m_mainThreadId,
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

        draconic::content::IContentDatabase* m_database;
        HashMap<TypeId, IResourceFactory*> m_factories;
        HashMap<Guid, RefPtr<ResourceHandle>> m_handles;
        HashMap<Guid, Array<Guid>> m_dependencies; // id -> resources it depends on
        HashMap<Guid, Array<Guid>> m_dependents;   // id -> resources that depend on it
        Array<Guid> m_buildStack;                  // ids currently building (auto-edge source)

        // --- async load bookkeeping (task #123); PendingLoad/CompletedDecode declared above ---
        JobSystem* m_jobs = nullptr; // shared decode pool (null = sync-only)
        u64 m_mainThreadId = 0;      // thread that constructs/pumps; async finalize must run here
        bool m_asyncBinds = false;   // Ref<T>::Bind routes through BindAsync while set
        HashMap<Guid, UniquePtr<PendingLoad>> m_pending; // main-thread only (heap-stable Counter)
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
}
