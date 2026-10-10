// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// EditorContext + EditorPageRegistry + Selection tests: nearest-type factory dispatch
// (Traktor's type_difference contest), open/focus/close page lifecycle, undo routing to the
// active page, selection semantics (dedup, primary, toggle).

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization;
import editor.core;
import foundation.mcp;
import foundation.json;

using namespace foundation::core;
using namespace editor;

namespace
{
    class BaseAsset : public ISerializable
    {
        RTTI_OBJECT(BaseAsset, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    class DerivedAsset : public BaseAsset
    {
        RTTI_OBJECT(DerivedAsset, BaseAsset)
    };

    class UnrelatedAsset : public ISerializable
    {
        RTTI_OBJECT(UnrelatedAsset, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    class TestPage final : public EditorPage
    {
    public:
        explicit TestPage(StringView title) : EditorPage(DefaultAllocator()), m_title(title) {}
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override
        {
            ClearDirty();
            return Status{};
        }

    private:
        String m_title;
    };

    class TestPageFactory final : public IEditorPageFactory
    {
    public:
        TestPageFactory(const TypeInfo& type, StringView title) : m_type(&type), m_title(title) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override { return m_type; }
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext&,
                                                       foundation::content::Instance&) override
        {
            return UniquePtr<EditorPage>(DefaultAllocator().New<TestPage>(m_title.AsView()),
                                         DefaultAllocator());
        }

    private:
        const TypeInfo* m_type;
        String m_title;
    };

    UniquePtr<IEditorPageFactory> MakeFactory(const TypeInfo& type, StringView title)
    {
        return UniquePtr<IEditorPageFactory>(DefaultAllocator().New<TestPageFactory>(type, title),
                                             DefaultAllocator());
    }

    void RegisterTestTypes()
    {
        GlobalTypeRegistry().Register(BaseAsset::StaticType());
        GlobalTypeRegistry().Register(DerivedAsset::StaticType());
        GlobalTypeRegistry().Register(UnrelatedAsset::StaticType());
        RegisterSerializable<BaseAsset>();
        RegisterSerializable<DerivedAsset>();
        RegisterSerializable<UnrelatedAsset>();
    }

    void RemoveDbTree(StringView root)
    {
        FileDelete(PathJoin(root, u8"a.xasset"));
        FileDelete(PathJoin(root, u8"b.xasset"));
        FileDelete(PathJoin(root, u8"c.xasset"));
        RemoveDirectory(root);
    }
}

RTTI_DEFINE_OBJECT(BaseAsset, "rtti::editor::editor::test")
RTTI_DEFINE_OBJECT(DerivedAsset, "rtti::editor::editor::test")
RTTI_DEFINE_OBJECT(UnrelatedAsset, "rtti::editor::editor::test")

TEST_CASE("editor-pages: registry nearest-type dispatch")
{
    EditorPageRegistry registry;
    registry.Register(MakeFactory(BaseAsset::StaticType(), u8"base"));

    // Base factory serves the derived type (base-chain walk)...
    IEditorPageFactory* found = registry.FindFactory(DerivedAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &BaseAsset::StaticType());

    // ...until a MORE SPECIFIC factory wins the distance contest.
    registry.Register(MakeFactory(DerivedAsset::StaticType(), u8"derived"));
    found = registry.FindFactory(DerivedAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &DerivedAsset::StaticType());

    // The base type still dispatches to the base factory.
    found = registry.FindFactory(BaseAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &BaseAsset::StaticType());

    // No factory covers an unrelated chain.
    CHECK(registry.FindFactory(UnrelatedAsset::StaticType()) == nullptr);
}

TEST_CASE("editor-context: open, focus, and close pages")
{
    RegisterTestTypes();

    const StringView dir = u8"scratch_editor_test_ctx_db";
    RemoveDbTree(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(),
                                          u8".xasset");

    auto* a = db.RootGroup()->CreateInstance(u8"a", BaseAsset::StaticType());
    auto* b = db.RootGroup()->CreateInstance(u8"b", DerivedAsset::StaticType());
    auto* c = db.RootGroup()->CreateInstance(u8"c", UnrelatedAsset::StaticType());
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);

    EditorContext ctx{DefaultAllocator()};
    i32 pagesChanged = 0;
    ctx.OnPagesChanged = [&pagesChanged]() { ++pagesChanged; };
    ctx.Pages().Register(MakeFactory(BaseAsset::StaticType(), u8"base"));

    // Open a page; it becomes active.
    EditorPage* pageA = ctx.OpenPage(*a);
    REQUIRE(pageA != nullptr);
    CHECK(ctx.ActivePage() == pageA);
    CHECK(ctx.OpenPages().Size() == 1);
    CHECK(pagesChanged == 1);

    // The derived instance dispatches through the base factory.
    EditorPage* pageB = ctx.OpenPage(*b);
    REQUIRE(pageB != nullptr);
    CHECK(ctx.ActivePage() == pageB);
    CHECK(ctx.OpenPages().Size() == 2);

    // Re-opening the same instance focuses the existing page instead of duplicating it.
    CHECK(ctx.OpenPage(*a) == pageA);
    CHECK(ctx.ActivePage() == pageA);
    CHECK(ctx.OpenPages().Size() == 2);

    // No factory for the unrelated type.
    CHECK(ctx.OpenPage(*c) == nullptr);

    // Closing the active page activates a surviving neighbor.
    ctx.ClosePage(pageA);
    CHECK(ctx.OpenPages().Size() == 1);
    CHECK(ctx.ActivePage() == pageB);
    ctx.ClosePage(pageB);
    CHECK(ctx.OpenPages().Size() == 0);
    CHECK(ctx.ActivePage() == nullptr);

    RemoveDbTree(dir);
}

TEST_CASE("editor-context: NotifyProjectSettingsChanged fires the subscribed hook")
{
    // The settings dialog fires this after a successful save; the app re-applies
    // settings-derived session state (default UI font/theme binds) - the fix for a
    // changed default font keeping its OLD bind until project reopen.
    EditorContext ctx{DefaultAllocator()};
    int fired = 0;
    ctx.NotifyProjectSettingsChanged(); // unsubscribed: safe no-op
    ctx.OnProjectSettingsChanged = [&fired]() { ++fired; };
    ctx.NotifyProjectSettingsChanged();
    ctx.NotifyProjectSettingsChanged();
    CHECK(fired == 2);
}

TEST_CASE("editor-context: IsCookBusy defaults to not-busy and reads the wired query")
{
    // PIE's cook gate (GameEditorPage::Play latches, OnUpdate polls this): unwired
    // (tests, no project) must read NOT busy so a deferred start never hangs.
    EditorContext ctx{DefaultAllocator()};
    CHECK_FALSE(ctx.IsCookBusy());
    bool busy = true;
    ctx.CookBusy = [&busy]() { return busy; };
    CHECK(ctx.IsCookBusy());
    busy = false;
    CHECK_FALSE(ctx.IsCookBusy());
    ctx.CookBusy = {};
    CHECK_FALSE(ctx.IsCookBusy()); // unwired again (project closed) - never busy
}

TEST_CASE("editor-context: open-asset interceptors claim newest-first and unregister cleanly")
{
    RegisterTestTypes();
    const StringView dir = u8"scratch_editor_test_ctx_intercept";
    RemoveDbTree(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(),
                                          u8".xasset");
    auto* a = db.RootGroup()->CreateInstance(u8"a", BaseAsset::StaticType());
    REQUIRE(a != nullptr);

    EditorContext ctx{DefaultAllocator()};

    // No interceptors -> the open is not claimed.
    CHECK(!ctx.TryInterceptOpenAsset(*a));

    // The NEWEST registration is consulted first; a false answer falls through to older ones.
    Array<i32> order;
    const u64 first = ctx.AddOpenAssetInterceptor(
        [&order](foundation::content::Instance&)
        {
            order.PushBack(1);
            return true;
        });
    const u64 second = ctx.AddOpenAssetInterceptor(
        [&order](foundation::content::Instance&)
        {
            order.PushBack(2);
            return false;
        });
    CHECK(ctx.TryInterceptOpenAsset(*a));
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == 2); // newest first
    CHECK(order[1] == 1); // fell through to the older claimer

    // A true answer short-circuits: older interceptors are never consulted.
    order.Clear();
    ctx.RemoveOpenAssetInterceptor(second);
    const u64 third = ctx.AddOpenAssetInterceptor(
        [&order](foundation::content::Instance&)
        {
            order.PushBack(3);
            return true;
        });
    CHECK(ctx.TryInterceptOpenAsset(*a));
    REQUIRE(order.Size() == 1);
    CHECK(order[0] == 3);

    // All removed -> back to unclaimed (removal by id, order-independent).
    ctx.RemoveOpenAssetInterceptor(first);
    ctx.RemoveOpenAssetInterceptor(third);
    CHECK(!ctx.TryInterceptOpenAsset(*a));

    RemoveDbTree(dir);
}

TEST_CASE("editor-context: adopted instance-less pages share the ownership flow")
{
    EditorContext context{DefaultAllocator()};

    // Adopt (the Game tab's path): owned by the context, becomes active, nil instance id.
    auto page = MakeUnique<TestPage>(DefaultAllocator(), u8"Game");
    EditorPage* raw = context.AdoptPage(UniquePtr<EditorPage>(page.Release(), DefaultAllocator()));
    REQUIRE(raw != nullptr);
    CHECK(context.OpenPages().Size() == 1);
    CHECK(context.ActivePage() == raw);
    CHECK(raw->InstanceId().IsNil());

    // Close destroys through the same path as instance pages.
    context.ClosePage(raw);
    CHECK(context.OpenPages().Size() == 0);
    CHECK(context.ActivePage() == nullptr);

    // Null adopt is a no-op.
    CHECK(context.AdoptPage(UniquePtr<EditorPage>{}) == nullptr);
    CHECK(context.OpenPages().Size() == 0);
}

TEST_CASE("editor-context: RevealPage makes a page active and asks the application to raise its panel")
{
    EditorContext context{DefaultAllocator()};
    class Page final : public EditorPage
    {
    public:
        Page() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"p"; }
        [[nodiscard]] Status Save() override { return Status{}; }
    };
    EditorPage* one = context.AdoptPage(UniquePtr<EditorPage>(DefaultAllocator().New<Page>(), DefaultAllocator()));
    EditorPage* two = context.AdoptPage(UniquePtr<EditorPage>(DefaultAllocator().New<Page>(), DefaultAllocator()));
    context.SetActivePage(two);
    // Headless: no hook, RevealPage is SetActivePage.
    context.RevealPage(one);
    CHECK(context.ActivePage() == one);
    // With the application's hook: the page is active first, then shown; a repeat still shows.
    Array<EditorPage*> shown;
    context.OnRevealPage = [&shown, &context](EditorPage* page)
    {
        CHECK(context.ActivePage() == page);
        shown.PushBack(page);
    };
    context.RevealPage(two);
    context.RevealPage(two);
    REQUIRE(shown.Size() == 2u);
    CHECK(shown[0] == two);
    context.RevealPage(nullptr); // clears the active page, shows nothing
    CHECK(context.ActivePage() == nullptr);
    CHECK(shown.Size() == 2u);
    context.ClosePage(one);
    context.ClosePage(two);
}

TEST_CASE("editor-context: a page owns its command stack, and the active page is the one Edit acts on")
{
    RegisterTestTypes();

    const StringView dir = u8"scratch_editor_test_ctx_undo_db";
    RemoveDbTree(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(),
                                          u8".xasset");
    auto* a = db.RootGroup()->CreateInstance(u8"a", BaseAsset::StaticType());
    REQUIRE(a != nullptr);

    EditorContext ctx{DefaultAllocator()};
    CHECK(ctx.ActivePage() == nullptr); // no active page: nothing to undo anywhere

    ctx.Pages().Register(MakeFactory(BaseAsset::StaticType(), u8"base"));
    EditorPage* page = ctx.OpenPage(*a);
    REQUIRE(page != nullptr);

    // A trivial command through the page's own stack.
    class Flip final : public IEditorCommand
    {
    public:
        explicit Flip(bool& b) : m_b(&b) {}
        [[nodiscard]] bool Execute() override
        {
            *m_b = !*m_b;
            return true;
        }
        void Undo() override { *m_b = !*m_b; }
        [[nodiscard]] StringView TypeId() const override { return u8"flip"; }

    private:
        bool* m_b;
    };

    bool flag = false;
    CHECK(page->Commands().Execute(
        UniquePtr<IEditorCommand>(DefaultAllocator().New<Flip>(flag), DefaultAllocator())));
    CHECK(flag);
    CHECK(page->IsDirty()); // command execution marks the page dirty

    // Edit > Undo / Redo are the edit.undo / edit.redo actions over the active page's stack
    // (the application declares them); the page owns the stack.
    REQUIRE(ctx.ActivePage() == page);
    CHECK(page->Commands().CanUndo());
    page->Commands().Undo();
    CHECK(!flag);
    CHECK(page->Commands().CanRedo());
    page->Commands().Redo();
    CHECK(flag);

    CHECK(page->Save().IsOk());
    CHECK(!page->IsDirty());

    RemoveDbTree(dir);
}

TEST_CASE("editor-selection: set, dedup, primary, toggle")
{
    Selection<i32> sel;
    i32 changes = 0;
    sel.OnChanged = [&changes]() { ++changes; };

    CHECK(sel.IsEmpty());
    CHECK(sel.Primary() == nullptr);

    sel.Set(5);
    CHECK(sel.Size() == 1);
    CHECK(*sel.Primary() == 5);
    CHECK(changes == 1);

    const i32 items[] = {3, 7, 3, 9}; // duplicate 3 removed, 3 stays primary
    sel.Set(Span<const i32>{items, 4});
    CHECK(sel.Size() == 3);
    CHECK(*sel.Primary() == 3);

    sel.Toggle(7); // present -> removed
    CHECK(sel.Size() == 2);
    CHECK(!sel.Contains(7));
    sel.Toggle(7); // absent -> added (at the back; primary unchanged)
    CHECK(sel.Contains(7));
    CHECK(*sel.Primary() == 3);

    sel.Clear();
    CHECK(sel.IsEmpty());

    const i32 changesAfterClear = changes;
    sel.Clear(); // clearing an empty selection does not notify
    CHECK(changes == changesAfterClear);
}

TEST_CASE("editor-context: Notify routes to OnNotice, falls back to the status bar")
{
    EditorContext ctx{DefaultAllocator()};
    Array<String> statuses;
    ctx.OnStatus = [&statuses](StringView text) { statuses.PushBack(String(text)); };

    // Unwired OnNotice -> status fallback (pages may Notify unconditionally).
    ctx.Notify(NoticeKind::Info, u8"hello");
    REQUIRE(statuses.Size() == 1u);
    CHECK(statuses[0].AsView() == StringView(u8"hello"));

    NoticeKind gotKind = NoticeKind::Info;
    String gotMessage;
    ctx.OnNotice = [&gotKind, &gotMessage](NoticeKind kind, StringView message)
    {
        gotKind = kind;
        gotMessage = String(message);
    };
    ctx.Notify(NoticeKind::Error, u8"cook failed");
    CHECK(gotKind == NoticeKind::Error);
    CHECK(gotMessage.AsView() == StringView(u8"cook failed"));
    CHECK(statuses.Size() == 1u); // wired notice does NOT double-post status
}

TEST_CASE("editor-context: script execution point set/clear + version stamps")
{
    EditorContext context{DefaultAllocator()};
    CHECK(!context.ScriptExecution().active);
    const u64 v0 = context.ScriptExecutionVersion();

    context.SetScriptExecutionPoint(u8"game.script", 12);
    CHECK(context.ScriptExecution().active);
    CHECK(context.ScriptExecution().file.AsView() == StringView(u8"game.script"));
    CHECK(context.ScriptExecution().line == 12);
    CHECK(context.ScriptExecutionVersion() != v0);

    // Re-set (a step to another line) bumps again; clear bumps once and goes inactive.
    const u64 v1 = context.ScriptExecutionVersion();
    context.SetScriptExecutionPoint(u8"game.script", 13);
    CHECK(context.ScriptExecutionVersion() != v1);
    const u64 v2 = context.ScriptExecutionVersion();
    context.ClearScriptExecutionPoint();
    CHECK(!context.ScriptExecution().active);
    CHECK(context.ScriptExecutionVersion() != v2);
    // Clearing while already clear is version-quiet (pollers stay idle).
    const u64 v3 = context.ScriptExecutionVersion();
    context.ClearScriptExecutionPoint();
    CHECK(context.ScriptExecutionVersion() == v3);
}

TEST_CASE("editor-context: script value probe slot")
{
    EditorContext context{DefaultAllocator()};
    CHECK(!context.ScriptValueProbe); // absent by default

    context.ScriptValueProbe = [](StringView identifier) -> String
    {
        if (identifier == StringView(u8"speed"))
        {
            return String(u8"4.5 : float");
        }
        return String();
    };
    REQUIRE(context.ScriptValueProbe);
    CHECK(context.ScriptValueProbe(u8"speed").AsView() == StringView(u8"4.5 : float"));
    CHECK(context.ScriptValueProbe(u8"unknown").IsEmpty());

    context.ScriptValueProbe = {}; // a run's end clears it
    CHECK(!context.ScriptValueProbe);
}

TEST_CASE("editor-context: pending asset-edit registry (register/replace/nil, drain, recook)")
{
    const StringView dir = u8"scratch_editor_test_assetedit_db";
    RemoveDbTree(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(),
                                          u8".xasset");

    EditorContext ctx{DefaultAllocator()};
    bool cooked = false;
    ctx.OnCookRequested = [&cooked](bool) { cooked = true; };

    const Guid a(1, 1);
    const Guid b(2, 2);
    int runsA = 0;
    int runsB = 0;

    CHECK_FALSE(ctx.HasPendingAssetEdits());
    ctx.RegisterAssetEdit(a, [&runsA](foundation::content::ContentDatabase&)
                          { ++runsA; return Status{}; });
    ctx.RegisterAssetEdit(a, [&runsA](foundation::content::ContentDatabase&)
                          { ++runsA; return Status{}; }); // same guid -> replaces (last wins)
    ctx.RegisterAssetEdit(b, [&runsB](foundation::content::ContentDatabase&)
                          { ++runsB; return Status{}; });
    ctx.RegisterAssetEdit(Guid{}, [](foundation::content::ContentDatabase&)
                          { return Status{}; }); // nil -> ignored (edit-live-only)
    CHECK(ctx.HasPendingAssetEdits());

    const Status drained = ctx.DrainAssetEdits(db);
    CHECK(drained.IsOk());
    CHECK(runsA == 1); // replaced, so ran once (not twice)
    CHECK(runsB == 1);
    CHECK(cooked);                      // a successful drain requests a recook
    CHECK_FALSE(ctx.HasPendingAssetEdits()); // drained clears

    cooked = false;
    CHECK(ctx.DrainAssetEdits(db).IsOk()); // empty drain is a no-op
    CHECK_FALSE(cooked);
}

TEST_CASE("context: MCP tool contributions register at boot and apply to a host's server in order")
{
    EditorContext ctx{DefaultAllocator()};
    CHECK(ctx.McpToolContributionCount() == 0u);
    Array<String> order;
    ctx.RegisterMcpToolContribution(
        [&order](foundation::mcp::McpServer& server)
        {
            order.PushBack(String(u8"scene"));
            server.RegisterTool(u8"selection_get", u8"x", foundation::mcp::SchemaBuilder().Build(),
                                foundation::mcp::ToolAnnotations::ReadOnly(),
                                [](const foundation::json::JsonValue&) -> foundation::mcp::ToolResult
                                { return foundation::json::JsonValue::MakeObject(); });
        });
    ctx.RegisterMcpToolContribution([&order](foundation::mcp::McpServer&)
                                    { order.PushBack(String(u8"other")); });
    CHECK(ctx.McpToolContributionCount() == 2u);

    foundation::mcp::McpServer server;
    ctx.ApplyMcpToolContributions(server);
    CHECK(server.ToolCount() == 1u);
    REQUIRE(order.Size() == 2u);
    CHECK(order[0] == u8"scene");
    CHECK(order[1] == u8"other");
    // Applying to a second host serves the same contributions again (one per project open).
    foundation::mcp::McpServer another;
    ctx.ApplyMcpToolContributions(another);
    CHECK(another.ToolCount() == 1u);
}

namespace
{
    // Two interfaces a page might publish: a page that is "a thing with a counter", and one
    // that is "a thing with a name". Interfaces, not owned objects: what a page IS to others.
    class ICounterPage : public IPageService
    {
    public:
        [[nodiscard]] virtual i32 Count() const = 0;
    };
    class INamedPage : public IPageService
    {
    public:
        [[nodiscard]] virtual StringView Name() const = 0;
    };
    class CountingPage final : public EditorPage, public ICounterPage
    {
    public:
        CountingPage() : EditorPage(DefaultAllocator()) { Provide<ICounterPage>(*this); }
        [[nodiscard]] StringView Title() const override { return u8"counting"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        [[nodiscard]] i32 Count() const override { return 42; }
    };
    class PlainPage final : public EditorPage
    {
    public:
        PlainPage() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"plain"; }
        [[nodiscard]] Status Save() override { return Status{}; }
    };
}

TEST_CASE("page: a page publishes the interfaces it implements, by type - a lookup for another "
          "interface, or on a page that publishes nothing, answers null")
{
    CountingPage counting;
    PlainPage plain;
    // Through the base pointer any holder of a page has: the published interface comes back
    // typed, and answers as the page.
    EditorPage* asPage = &counting;
    ICounterPage* counter = asPage->Service<ICounterPage>();
    REQUIRE(counter != nullptr);
    CHECK(counter->Count() == 42);
    // Not published: not that kind of page.
    CHECK(asPage->Service<INamedPage>() == nullptr);
    EditorPage* plainPage = &plain;
    CHECK(plainPage->Service<ICounterPage>() == nullptr);
    CHECK(plainPage->Service<INamedPage>() == nullptr);
}

namespace
{
    // A page that counts how often its asset changed under it.
    class WatchingPage final : public EditorPage
    {
    public:
        explicit WatchingPage(const Guid& asset) : EditorPage(DefaultAllocator())
        {
            SetInstanceId(asset);
        }
        [[nodiscard]] StringView Title() const override { return u8"watching"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        void OnAssetExternallyModified() override { ++told; }
        u32 told = 0;
    };
}

TEST_CASE("context: an asset changed outside its page tells every open page editing it, and no "
          "other")
{
    Random rng(11);
    const Guid edited = Guid::Generate(rng);
    const Guid other = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* first = static_cast<WatchingPage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<WatchingPage>(edited), DefaultAllocator())));
    auto* second = static_cast<WatchingPage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<WatchingPage>(edited), DefaultAllocator())));
    auto* elsewhere = static_cast<WatchingPage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<WatchingPage>(other), DefaultAllocator())));

    CHECK(context.NotifyAssetExternallyModified(edited) == 2u);
    CHECK(first->told == 1u);
    CHECK(second->told == 1u);
    CHECK(elsewhere->told == 0u);
    // An asset no page edits: nothing told, nothing wrong.
    CHECK(context.NotifyAssetExternallyModified(Guid::Generate(rng)) == 0u);

    context.ClosePage(elsewhere);
    context.ClosePage(second);
    context.ClosePage(first);
}


// Every editor filter field finds a thing by its name or its guid (editor.core :search_filter).
TEST_CASE("search filter: a name part, or a guid whole or by its first digits")
{
    Guid id;
    REQUIRE(Guid::TryParse(u8"3f2a9c1e-7b44-4d0e-9a51-0c6e2b8d7f13", id));
    CHECK(editor::NameOrGuidMatches(u8"Crate", id, u8""));
    CHECK(editor::NameOrGuidMatches(u8"Crate", id, u8"RAT"));
    CHECK_FALSE(editor::NameOrGuidMatches(u8"Crate", id, u8"barrel"));
    CHECK(editor::NameOrGuidMatches(u8"Crate", id, u8"3f2a"));
    CHECK(editor::NameOrGuidMatches(u8"Crate", id, u8"3F2A9C1E-7B44"));
    CHECK(editor::NameOrGuidMatches(u8"Crate", id, u8"{3f2a9c1e7b444d0e9a510c6e2b8d7f13}"));
    CHECK_FALSE(editor::NameOrGuidMatches(u8"Crate", id, u8"3f2"));  // under four digits: a name only
    CHECK_FALSE(editor::NameOrGuidMatches(u8"Crate", id, u8"4d0e")); // not the guid's beginning

    Guid whole;
    CHECK(editor::FilterAsGuid(u8" {3F2A9C1E-7B44-4D0E-9A51-0C6E2B8D7F13} ", whole));
    CHECK(whole == id);
    CHECK_FALSE(editor::FilterAsGuid(u8"3f2a9c1e", whole));
    CHECK_FALSE(editor::FilterAsGuid(u8"Crate", whole));
}
