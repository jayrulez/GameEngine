// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests of ViewportToolManager and the provider registry: the activation state machine (the first
// tool added is the default and active, the ActivateById round trip, the gesture ending on a
// switch), the availability fallback (an active tool whose predicate lapses is deactivated before
// it sees another frame), routing (the consumed flag comes from the active tool), and the
// registry's behaviour (explicit registration, idempotent duplicates, CreateAll running after the
// default is added).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import editor.core;
import editor.viewporttools;

using namespace foundation::core;
using namespace editor;
namespace core = foundation::core;

namespace
{
    struct ToolLog
    {
        i32 activations = 0;
        i32 deactivations = 0;
        i32 updates = 0;
    };

    class TestTool final : public IViewportTool
    {
    public:
        TestTool(StringView id, ToolLog& log) : m_id(id), m_log(&log) {}

        StringView Id() const override { return m_id.AsView(); }
        StringView DisplayName() const override { return m_id.AsView(); }
        StringView Category() const override { return category.AsView(); }
        bool IsAvailable() const override { return available; }
        void OnActivate() override { ++m_log->activations; }
        void OnDeactivate() override { ++m_log->deactivations; }
        bool Update(const ViewportToolInput&) override
        {
            ++m_log->updates;
            return consume;
        }

        bool available = true;
        bool consume = false;
        String category; // empty when it stands alone (the interface default)

    private:
        String m_id;
        ToolLog* m_log;
    };
}

TEST_CASE("viewporttools: the first tool added is the default and starts active")
{
    ViewportToolManager manager;
    CHECK(manager.ActiveTool() == nullptr);
    CHECK(!manager.Update(ViewportToolInput{})); // empty manager consumes nothing

    ToolLog logA;
    IViewportTool* a = manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"select", logA));
    REQUIRE(a != nullptr);
    CHECK(manager.ActiveTool() == a);
    CHECK(logA.activations == 1); // activated on Add, not lazily

    ToolLog logB;
    IViewportTool* b = manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"brush", logB));
    CHECK(manager.ActiveTool() == a); // a later Add never steals activation
    CHECK(logB.activations == 0);
    CHECK(manager.Count() == 2);
    CHECK(manager.FindById(u8"brush") == b);
    CHECK(manager.FindById(u8"nope") == nullptr);
}

TEST_CASE("viewporttools: ActivateById switches with the gesture-end guarantee, Escape-shape "
          "returns to default")
{
    ViewportToolManager manager;
    ToolLog logA, logB;
    manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"select", logA));
    IViewportTool* b = manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"brush", logB));

    REQUIRE(manager.ActivateById(u8"brush"));
    CHECK(manager.ActiveTool() == b);
    CHECK(logA.deactivations == 1); // the old tool ended its gesture before the new one activated
    CHECK(logB.activations == 1);

    CHECK(manager.ActivateById(u8"brush")); // re-activating the active tool is a no-op success
    CHECK(logB.activations == 1);
    CHECK(logB.deactivations == 0);

    CHECK(!manager.ActivateById(u8"unknown")); // unknown id: refused, state unchanged
    CHECK(manager.ActiveTool() == b);

    manager.ActivateDefault();
    CHECK(manager.ActiveTool()->Id() == StringView(u8"select"));
    CHECK(logB.deactivations == 1);
    CHECK(logA.activations == 2);
}

TEST_CASE("viewporttools: an unavailable tool is refused; a lapsing active tool falls back "
          "before its next update")
{
    ViewportToolManager manager;
    ToolLog logA, logB;
    manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"select", logA));
    IViewportTool* b = manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"brush", logB));
    auto* brush = static_cast<TestTool*>(b);

    brush->available = false;
    CHECK(!manager.ActivateById(u8"brush")); // unavailable: activation refused

    brush->available = true;
    REQUIRE(manager.ActivateById(u8"brush"));
    brush->available = false; // as if the terrain were deleted mid-session
    (void)manager.Update(ViewportToolInput{});
    CHECK(manager.ActiveTool()->Id() == StringView(u8"select"));
    CHECK(logB.updates == 0); // the unavailable tool never saw another frame
    CHECK(logB.deactivations == 1);
    CHECK(logA.updates == 1); // the default took the same frame
}

TEST_CASE("viewporttools: Update returns the active tool's consumed flag")
{
    ViewportToolManager manager;
    ToolLog log;
    auto* tool =
        static_cast<TestTool*>(manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"select", log)));

    CHECK(!manager.Update(ViewportToolInput{}));
    tool->consume = true;
    CHECK(manager.Update(ViewportToolInput{}));
}

namespace
{
    class TestProvider final : public IViewportToolProvider
    {
    public:
        explicit TestProvider(ToolLog& log) : m_log(&log) {}

        void CreateTools(ViewportToolManager& manager, const ViewportToolHostContext&) override
        {
            manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"provided", *m_log));
        }

    private:
        ToolLog* m_log;
    };
}

TEST_CASE("viewporttools: provider registry - explicit, idempotent, never the default")
{
    // The registry is process-global, so this test uses its own provider instance and checks
    // only relative effects, which keeps it independent of the order tests run in.
    ToolLog log;
    TestProvider provider(log);
    ViewportToolProviderRegistry& registry = ViewportToolProviderRegistry::Get();
    const usize before = registry.Count();
    registry.Register(&provider);
    registry.Register(&provider); // duplicate pointer: ignored
    CHECK(registry.Count() == before + 1);

    ViewportToolManager manager;
    ToolLog defaultLog;
    manager.Add(MakeUnique<TestTool>(DefaultAllocator(), u8"select", defaultLog));
    ViewportToolHostContext context; // null members: providers must tolerate a bare host
    registry.CreateAll(manager, context);

    IViewportTool* provided = manager.FindById(u8"provided");
    REQUIRE(provided != nullptr);
    CHECK(manager.ActiveTool()->Id() == StringView(u8"select")); // provider tools never default
    CHECK(log.activations == 0);
}

TEST_CASE("viewporttools: Category defaults to empty and GroupViewportTools folds a category "
          "into one group in first-appearance order, skipping the default tool")
{
    ToolLog log;
    ViewportToolManager manager;
    auto add = [&](StringView id, StringView category)
    {
        UniquePtr<TestTool> tool = MakeUnique<TestTool>(DefaultAllocator(), id, log);
        tool->category = String(category);
        manager.Add(Move(tool));
    };
    add(u8"select", {});           // the default: never in a group
    add(u8"terrain.sculpt", u8"Terrain");
    add(u8"terrain.splat", u8"Terrain");
    add(u8"spline.edit", {});      // alone
    add(u8"vegetation.paint", u8"Vegetation");
    add(u8"terrain.hole", u8"Terrain"); // a late registration joins its group
    add(u8"vegetation.scatter", u8"Vegetation");

    CHECK(manager.ToolAt(3)->Category().IsEmpty()); // the interface default

    Array<ViewportToolGroup> groups;
    GroupViewportTools(manager, groups);
    REQUIRE(groups.Size() == 3u);
    CHECK(groups[0].category == u8"Terrain");
    REQUIRE(groups[0].toolIds.Size() == 3u);
    CHECK(groups[0].toolIds[0] == u8"terrain.sculpt");
    CHECK(groups[0].toolIds[1] == u8"terrain.splat");
    CHECK(groups[0].toolIds[2] == u8"terrain.hole");
    CHECK(groups[1].category.IsEmpty());
    REQUIRE(groups[1].toolIds.Size() == 1u);
    CHECK(groups[1].toolIds[0] == u8"spline.edit");
    CHECK(groups[2].category == u8"Vegetation");
    CHECK(groups[2].toolIds.Size() == 2u);
}

