// Async resource loading (task #123): BindAsync / Pump / WaitAll, the two-stage factory protocol,
// dedup, sync-upgrade of a pending id, the decode-failure path, budget-bounded finalize, and the
// destructor drain. The fake factory is TSAN-safe: DecodeStage runs on a worker and only reads
// pre-populated maps + constructs objects whose StaticType() was force-initialized on the main
// thread; it never calls ReadObject or touches the ResourceManager (per the protocol contract).
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

#include <atomic>
#include <chrono>
#include <thread>

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;

namespace
{
    // Source (serializable, in the content DB), lean runtime product, and the decoded intermediate
    // the worker produces and the main thread finalizes into a product.
    class AsyncSource final : public ISerializable
    {
        DRACONIC_OBJECT(AsyncSource, ISerializable)
    public:
        i32 value = 0;
        void Serialize(ISerializer& ar) override { draconic::foundation::Serialize(ar, "value", value); }
    };

    class AsyncProduct final : public Object
    {
        DRACONIC_OBJECT(AsyncProduct, Object)
    public:
        i32 value = 0;
    };

    class DecodedBlob final : public Object
    {
        DRACONIC_OBJECT(DecodedBlob, Object)
    public:
        i32 value = 0;
        Guid id; // so FinalizeStage (which only gets the blob) can record completion order
    };

    constexpr int kMaxSlots = 16;

    class AsyncFactory final : public IResourceFactory
    {
    public:
        AsyncFactory()
        {
            for (int i = 0; i < kMaxSlots; ++i)
            {
                gates[i].store(true, std::memory_order_relaxed);      // open by default
                decoded[i].store(false, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AsyncProduct::StaticType();
        }

        // Synchronous fallback (un-migrated factory / no job system): reads the source directly.
        [[nodiscard]] RefPtr<Object> Create(ResourceManager& /*manager*/,
                                            draconic::content::Instance& instance) override
        {
            createCalls.fetch_add(1, std::memory_order_relaxed);
            RefPtr<ISerializable> source = instance.ReadObject();
            AsyncSource* src = Cast<AsyncSource>(source.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<AsyncProduct> product = MakeRef<AsyncProduct>(DefaultAllocator());
            product->value = src->value;
            return product;
        }

        [[nodiscard]] bool SupportsAsync() const override { return supportsAsync; }

        // Worker thread: pure, no manager/GPU/globals. Reads only pre-populated maps (main writes
        // them before any BindAsync, never during) and builds a DecodedBlob whose type was
        // force-initialized on the main thread.
        [[nodiscard]] RefPtr<Object> DecodeStage(draconic::content::Instance& instance) override
        {
            decodeCalls.fetch_add(1, std::memory_order_relaxed);
            const Guid id = instance.Id();
            int slot = 0;
            if (const int* found = indexOf.Find(id))
            {
                slot = *found;
            }
            while (!gates[slot].load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            if (failDecode)
            {
                decoded[slot].store(true, std::memory_order_release);
                return RefPtr<Object>{};
            }
            i32 value = 0;
            if (const i32* found = valueOf.Find(id))
            {
                value = *found;
            }
            RefPtr<DecodedBlob> blob = MakeRef<DecodedBlob>(DefaultAllocator());
            blob->value = value;
            blob->id = id;
            decoded[slot].store(true, std::memory_order_release);
            return blob;
        }

        // Main thread: turn the blob into the product; record completion order + the calling thread.
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager& /*manager*/,
                                                   RefPtr<Object> decodedObject) override
        {
            finalizeCalls.fetch_add(1, std::memory_order_relaxed);
            finalizeThread.store(Thread::CurrentId(), std::memory_order_relaxed);
            if (finalizeSleepMs > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(finalizeSleepMs));
            }
            DecodedBlob* blob = Cast<DecodedBlob>(decodedObject.Get());
            if (blob == nullptr)
            {
                return RefPtr<Object>{};
            }
            finalizeOrder.PushBack(blob->id); // finalize is main-only, so no lock needed
            RefPtr<AsyncProduct> product = MakeRef<AsyncProduct>(DefaultAllocator());
            product->value = blob->value;
            return product;
        }

        std::atomic<bool> gates[kMaxSlots];
        std::atomic<bool> decoded[kMaxSlots];
        std::atomic<int> decodeCalls{0};
        std::atomic<int> finalizeCalls{0};
        std::atomic<int> createCalls{0};
        std::atomic<u64> finalizeThread{0};
        bool supportsAsync = true;
        bool failDecode = false;
        int finalizeSleepMs = 0;
        HashMap<Guid, int> indexOf; // main-populated, worker read-only
        HashMap<Guid, i32> valueOf; // main-populated, worker read-only
        Array<Guid> finalizeOrder;  // main-only
    };

    // Registers the reflected types once and forces every StaticType() the worker will touch to
    // initialize on the MAIN thread (so a worker only ever reads them).
    void RegisterAsyncTypes()
    {
        GlobalTypeRegistry().Register(AsyncSource::StaticType());
        RegisterSerializable<AsyncSource>();
        (void)AsyncProduct::StaticType();
        (void)DecodedBlob::StaticType();
    }

    Guid MakeInstance(draconic::content::ContentDatabase& db, AsyncFactory& factory, StringView name,
                      i32 value, int slot)
    {
        auto* instance = db.RootGroup()->CreateInstance(name, AsyncSource::StaticType());
        REQUIRE(instance != nullptr);
        AsyncSource source;
        source.value = value;
        REQUIRE(instance->WriteObject(source).IsOk());
        const Guid id = instance->Id();
        factory.valueOf.InsertOrAssign(id, value);
        factory.indexOf.InsertOrAssign(id, slot);
        return id;
    }

    void CleanDir(StringView dir) { RemoveDirectory(dir); }
}

DRACONIC_DEFINE_OBJECT(AsyncSource, "draconic::resource::test")
DRACONIC_DEFINE_OBJECT(AsyncProduct, "draconic::resource::test")
DRACONIC_DEFINE_OBJECT(DecodedBlob, "draconic::resource::test")

TEST_CASE("resource.async: BindAsync is pending until Pump finalizes it on the main thread")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    factory.gates[0].store(false, std::memory_order_relaxed); // hold the decode closed
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);
    const Guid id = MakeInstance(db, factory, u8"a", 7, 0);

    Proxy<AsyncProduct> product = manager.BindAsync<AsyncProduct>(id);
    // Immediately pending: the handle exists, the product does not.
    CHECK(product.Get() == nullptr);
    CHECK(product.Handle()->State() == ResourceState::Pending);
    CHECK(manager.PendingCount() == 1u);

    factory.gates[0].store(true, std::memory_order_release); // release the decode
    manager.WaitAll();

    REQUIRE(product.Get() != nullptr);
    CHECK(product->value == 7);
    CHECK(product.Handle()->State() == ResourceState::Ready);
    CHECK(manager.PendingCount() == 0u);
    CHECK(factory.decodeCalls.load() == 1);
    CHECK(factory.finalizeCalls.load() == 1);
    CHECK(factory.finalizeThread.load() == Thread::CurrentId()); // finalize ran on main
}

TEST_CASE("resource.async: concurrent BindAsync of one id shares a single decode + finalize")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    factory.gates[0].store(false, std::memory_order_relaxed);
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);
    const Guid id = MakeInstance(db, factory, u8"a", 42, 0);

    Proxy<AsyncProduct> a = manager.BindAsync<AsyncProduct>(id);
    Proxy<AsyncProduct> b = manager.BindAsync<AsyncProduct>(id); // dedup while pending
    CHECK(a.Handle() == b.Handle());
    CHECK(manager.PendingCount() == 1u);

    factory.gates[0].store(true, std::memory_order_release);
    manager.WaitAll();

    CHECK(factory.decodeCalls.load() == 1);
    CHECK(factory.finalizeCalls.load() == 1);
    REQUIRE(a.Get() != nullptr);
    CHECK(a->value == 42);
    CHECK(a.Get() == b.Get());
}

TEST_CASE("resource.async: a sync Bind of a pending id block-completes it to Ready")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory; // gate open: the sync upgrade waits on / drives the decode
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);
    const Guid id = MakeInstance(db, factory, u8"a", 99, 0);

    Proxy<AsyncProduct> async = manager.BindAsync<AsyncProduct>(id);
    Proxy<AsyncProduct> sync = manager.Bind<AsyncProduct>(id); // must return Ready

    REQUIRE(sync.Get() != nullptr);
    CHECK(sync->value == 99);
    CHECK(sync.Handle()->State() == ResourceState::Ready);
    CHECK(async.Handle() == sync.Handle());
    CHECK(manager.PendingCount() == 0u);
    CHECK(factory.decodeCalls.load() == 1);
    CHECK(factory.finalizeCalls.load() == 1);
}

TEST_CASE("resource.async: a failed decode settles the handle to Failed, no finalize")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    factory.failDecode = true;
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);
    const Guid id = MakeInstance(db, factory, u8"a", 1, 0);

    Proxy<AsyncProduct> product = manager.BindAsync<AsyncProduct>(id);
    manager.WaitAll();

    CHECK(product.Get() == nullptr);
    CHECK(product.Handle()->State() == ResourceState::Failed);
    CHECK(factory.finalizeCalls.load() == 0); // decode failed -> never finalized
    CHECK(manager.PendingCount() == 0u);
}

TEST_CASE("resource.async: BindAsync falls back to a synchronous Ready build when async is off")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");

    SUBCASE("un-migrated factory (SupportsAsync == false)")
    {
        AsyncFactory factory;
        factory.supportsAsync = false;
        JobSystem jobs;
        ResourceManager manager(db, &jobs);
        manager.AddFactory(&factory);
        const Guid id = MakeInstance(db, factory, u8"a", 5, 0);

        Proxy<AsyncProduct> product = manager.BindAsync<AsyncProduct>(id);
        REQUIRE(product.Get() != nullptr); // immediately ready via sync Create
        CHECK(product->value == 5);
        CHECK(product.Handle()->State() == ResourceState::Ready);
        CHECK(factory.createCalls.load() == 1);
        CHECK(factory.decodeCalls.load() == 0);
    }

    SUBCASE("no JobSystem")
    {
        AsyncFactory factory; // supports async, but the manager has no pool
        ResourceManager manager(db);
        manager.AddFactory(&factory);
        const Guid id = MakeInstance(db, factory, u8"b", 6, 0);

        Proxy<AsyncProduct> product = manager.BindAsync<AsyncProduct>(id);
        REQUIRE(product.Get() != nullptr);
        CHECK(product->value == 6);
        CHECK(product.Handle()->State() == ResourceState::Ready);
        CHECK(factory.createCalls.load() == 1);
        CHECK(factory.decodeCalls.load() == 0);
    }
}

TEST_CASE("resource.async: Pump respects its time budget and resumes on the next tick")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    factory.finalizeSleepMs = 5; // each finalize (>1ms budget) => at most one per Pump
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);

    constexpr int kCount = 4;
    for (int i = 0; i < kCount; ++i)
    {
        char8_t name[2] = {static_cast<char8_t>(u8'a' + i), 0};
        const Guid id = MakeInstance(db, factory, StringView(name), i, i);
        (void)manager.BindAsync<AsyncProduct>(id);
    }

    int pumps = 0;
    while (manager.PendingCount() > 0 && pumps < 10000)
    {
        manager.Pump(0.001); // 1ms budget; a 5ms finalize forces multiple ticks
        ++pumps;
    }

    CHECK(manager.PendingCount() == 0u);
    CHECK(factory.finalizeCalls.load() == kCount);
    CHECK(pumps >= kCount); // budget prevented finalizing all in a single Pump
}

TEST_CASE("resource.async: finalize follows decode-completion order (FIFO)")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    for (int i = 0; i < 3; ++i)
    {
        factory.gates[i].store(false, std::memory_order_relaxed); // hold all three decodes
    }
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);

    const Guid id0 = MakeInstance(db, factory, u8"a", 10, 0);
    const Guid id1 = MakeInstance(db, factory, u8"b", 11, 1);
    const Guid id2 = MakeInstance(db, factory, u8"c", 12, 2);
    (void)manager.BindAsync<AsyncProduct>(id0);
    (void)manager.BindAsync<AsyncProduct>(id1);
    (void)manager.BindAsync<AsyncProduct>(id2);

    // Release decodes in a non-bind order, finalizing each before releasing the next, so the
    // completion order is deterministic: 2, then 0, then 1.
    const int releaseSlots[3] = {2, 0, 1};
    const Guid releaseIds[3] = {id2, id0, id1};
    for (int step = 0; step < 3; ++step)
    {
        const int slot = releaseSlots[step];
        factory.gates[slot].store(true, std::memory_order_release);
        // Only this slot's gate is open, so only its decode can reach the completion queue. Pump
        // until it is actually finalized - waiting on the decoded[] flag alone would race the
        // manager's post-decode push into the queue (the flag is set inside DecodeStage, before
        // the manager enqueues the result), so Pump could run before the entry lands and miss it.
        const usize target = static_cast<usize>(step) + 1u;
        while (factory.finalizeOrder.Size() < target)
        {
            manager.Pump(1.0e9);
            std::this_thread::yield();
        }
    }

    REQUIRE(factory.finalizeOrder.Size() == 3u);
    CHECK(factory.finalizeOrder[0] == releaseIds[0]);
    CHECK(factory.finalizeOrder[1] == releaseIds[1]);
    CHECK(factory.finalizeOrder[2] == releaseIds[2]);
}

TEST_CASE("resource.async: OnReady fires once on the main thread when the load becomes ready")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);
    const Guid id = MakeInstance(db, factory, u8"a", 3, 0);

    Proxy<AsyncProduct> product = manager.BindAsync<AsyncProduct>(id);
    int readyCount = 0;
    u64 readyThread = 0;
    product.Handle()->SetOnReady(
        [&readyCount, &readyThread]()
        {
            ++readyCount;
            readyThread = Thread::CurrentId();
        });

    manager.WaitAll();
    manager.Pump(); // a second pump must NOT re-fire

    CHECK(readyCount == 1);
    CHECK(readyThread == Thread::CurrentId());
}

TEST_CASE("resource.async: destroying the manager with an in-flight decode drains cleanly")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    JobSystem jobs;
    {
        ResourceManager manager(db, &jobs);
        manager.AddFactory(&factory);
        const Guid id = MakeInstance(db, factory, u8"a", 1, 0);
        (void)manager.BindAsync<AsyncProduct>(id);
        // Leave it pending (never pumped): the destructor must wait out the decode so no job
        // references the freed manager. ASAN/TSAN prove no use-after-free / data race here.
    }
    CHECK(factory.decodeCalls.load() == 1); // the decode ran to completion during teardown
}

TEST_CASE("resource.async: Ref::Bind routes through BindAsync under an AsyncBindScope")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    factory.gates[0].store(false, std::memory_order_relaxed); // hold the decode
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);
    const Guid id = MakeInstance(db, factory, u8"a", 5, 0);

    Ref<AsyncProduct> ref;
    ref.SetId(id);
    {
        AsyncBindScope scope(manager);
        CHECK(manager.AsyncBindsEnabled());
        ref.Bind(manager);
    }
    CHECK_FALSE(manager.AsyncBindsEnabled());  // scope restored the mode
    CHECK(manager.PendingCount() == 1u);       // routed to BindAsync: pending, not built yet
    CHECK(ref.Get() == nullptr);               // proxy is null while pending

    factory.gates[0].store(true, std::memory_order_release);
    manager.WaitAll();

    CHECK(manager.PendingCount() == 0u);
    REQUIRE(ref.Get() != nullptr); // the proxy now resolves to the finalized product
    CHECK(ref.Get()->value == 5);
}

TEST_CASE("resource.async: AsyncLoadBatch reports progress as loads finalize")
{
    RegisterAsyncTypes();
    CleanDir(u8"draconic_async_db");
    NativeFileSystem mount(u8"draconic_async_db");
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    AsyncFactory factory;
    for (int i = 0; i < 3; ++i)
    {
        factory.gates[i].store(false, std::memory_order_relaxed); // hold all decodes
    }
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);

    const Guid a = MakeInstance(db, factory, u8"a", 1, 0);
    const Guid b = MakeInstance(db, factory, u8"b", 2, 1);
    const Guid c = MakeInstance(db, factory, u8"c", 3, 2);
    (void)manager.BindAsync<AsyncProduct>(a);
    (void)manager.BindAsync<AsyncProduct>(b);
    (void)manager.BindAsync<AsyncProduct>(c);

    AsyncLoadBatch batch(manager);
    batch.Snapshot();
    CHECK(batch.Total() == 3u);
    CHECK(batch.Remaining() == 3u);
    CHECK(batch.Progress() == doctest::Approx(0.0f));
    CHECK_FALSE(batch.IsComplete());

    for (int i = 0; i < 3; ++i)
    {
        factory.gates[i].store(true, std::memory_order_release);
    }
    batch.WaitComplete();

    CHECK(batch.IsComplete());
    CHECK(batch.Remaining() == 0u);
    CHECK(batch.Progress() == doctest::Approx(1.0f));
}
