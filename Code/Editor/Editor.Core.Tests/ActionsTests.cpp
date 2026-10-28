// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Core tests - the action registry on a real EditorContext: declarations bound
// through the active page and the interface it publishes, the pulled enabled and checked
// states, Execute as the one funnel with its refusals, the shortcut table (defaults,
// overrides, collisions, reset) and the change notification; the refusals of a bad or
// duplicate registration.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui; // Effect (what changes), Menu (action items)
import foundation.settings;
import foundation.xml.serialization;
import editor.core;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    // A page kind an action can address: it publishes ISimulating (start / stop a run).
    class ISimulating : public IPageService
    {
    public:
        virtual void Start() = 0;
        virtual void Stop() = 0;
        [[nodiscard]] virtual bool Running() const = 0;
    };
    class SimPage final : public EditorPage, public ISimulating
    {
    public:
        SimPage() : EditorPage(DefaultAllocator()) { Provide<ISimulating>(*this); }
        [[nodiscard]] StringView Title() const override { return u8"sim"; }
        [[nodiscard]] Status Save() override
        {
            ++saves;
            ClearDirty();
            return Status{};
        }
        void Start() override { running = true; }
        void Stop() override { running = false; }
        [[nodiscard]] bool Running() const override { return running; }
        u32 saves = 0;
        bool running = false;
    };
    class PlainPage final : public EditorPage
    {
    public:
        PlainPage() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"plain"; }
        [[nodiscard]] Status Save() override
        {
            ++saves;
            ClearDirty();
            return Status{};
        }
        u32 saves = 0;
    };

    EditorActionDeclaration Declare(StringView id, StringView label)
    {
        EditorActionDeclaration d;
        d.id = String(id);
        d.label = String(label);
        return d;
    }
    // A menu's items, in order (the rows AppendActionItems added).
    Array<gui::MenuItem*> ItemsOf(foundation::gui::Node& node)
    {
        Array<gui::MenuItem*> out;
        if (auto* item = Cast<gui::MenuItem>(&node))
            out.PushBack(item);
        for (usize i = 0; i < node.ChildCount(); ++i)
            for (gui::MenuItem* inner : ItemsOf(*node.GetChildAt(i)))
                out.PushBack(inner);
        return out;
    }
    const EditorShortcut CtrlS = EditorShortcut::Of(u8"ctrl-s");
    const EditorShortcut F5 = EditorShortcut::Of(u8"f5");
}

TEST_CASE("actions: declarations bind through the active page and its published interface; "
          "enabled and checked are pulled; Execute is the funnel and refuses what it must")
{
    EditorContext context{DefaultAllocator()};
    EditorActionRegistry& actions = context.Actions();
    u32 changes = 0;
    gui::Effect watch([&]() { actions.TrackChanges(); ++changes; }); // a surface following the actions
    changes = 0; // its first run reads them; the changes are what follow

    // Editor-wide, always enabled.
    u32 exits = 0;
    EditorActionDeclaration exit = Declare(u8"file.exit", u8"Exit");
    exit.menuPath = String(u8"File/Exit");
    exit.execute = [&exits](EditorPage*) { ++exits; };
    REQUIRE(actions.Register(Move(exit)));

    // Over the subject page: enabled while it is dirty, executes its Save.
    EditorActionDeclaration save = Declare(u8"page.save", u8"Save");
    save.shortcut = CtrlS;
    save.enabled = [](EditorPage* page) { return page != nullptr && page->IsDirty(); };
    save.execute = [](EditorPage* page) { (void)page->Save(); };
    REQUIRE(actions.Register(Move(save)));

    // Over the interface the subject page publishes: a Toggle whose checked state is the run.
    EditorActionDeclaration simulate = Declare(u8"sim.run", u8"Simulate");
    simulate.kind = EditorActionKind::Toggle;
    simulate.shortcut = F5;
    simulate.enabled = [](EditorPage* page) { return ServiceOf<ISimulating>(page) != nullptr; };
    simulate.checked = [](EditorPage* page)
    {
        ISimulating* sim = ServiceOf<ISimulating>(page);
        return sim != nullptr && sim->Running();
    };
    simulate.execute = [](EditorPage* page)
    {
        ISimulating* sim = ServiceOf<ISimulating>(page);
        if (sim->Running())
        {
            sim->Stop();
        }
        else
        {
            sim->Start();
        }
    };
    REQUIRE(actions.Register(Move(simulate)));
    CHECK(changes == 3u);
    CHECK(actions.Count() == 3u);
    REQUIRE(actions.Actions().Size() == 3u);
    CHECK(actions.Actions()[0].id == u8"file.exit"); // registration order
    CHECK(actions.Actions()[2].id == u8"sim.run");
    REQUIRE(actions.Find(u8"page.save") != nullptr);
    CHECK(actions.Find(u8"page.save")->label == u8"Save");
    CHECK(actions.Find(u8"nobody.home") == nullptr);

    // No page: the editor-wide action runs, the page-bound ones are disabled and refused.
    CHECK(actions.IsEnabled(u8"file.exit"));
    CHECK_FALSE(actions.IsEnabled(u8"page.save"));
    CHECK_FALSE(actions.IsEnabled(u8"sim.run"));
    CHECK_FALSE(actions.IsEnabled(u8"nobody.home"));
    CHECK(actions.Execute(u8"file.exit").IsOk());
    CHECK(exits == 1u);
    CHECK(actions.Execute(u8"page.save").Code() == ErrorCode::NotSupported);
    CHECK(actions.Execute(u8"nobody.home").Code() == ErrorCode::NotFound);

    // A plain page, dirty: save is enabled and saves IT; simulate stays disabled (it is not
    // that kind of page).
    auto* plain = static_cast<PlainPage*>(context.AdoptPage(
        UniquePtr<EditorPage>(DefaultAllocator().New<PlainPage>(), DefaultAllocator())));
    CHECK_FALSE(actions.IsEnabled(u8"page.save")); // clean
    plain->MarkDirty();
    CHECK(actions.IsEnabled(u8"page.save"));
    CHECK_FALSE(actions.IsEnabled(u8"sim.run"));
    CHECK(actions.Execute(u8"page.save").IsOk());
    CHECK(plain->saves == 1u);
    CHECK_FALSE(actions.IsEnabled(u8"page.save")); // clean again, pulled
    CHECK(actions.Execute(u8"sim.run").Code() == ErrorCode::NotSupported);

    // A sim page active: the toggle is enabled, its checked state follows the run.
    auto* sim = static_cast<SimPage*>(context.AdoptPage(
        UniquePtr<EditorPage>(DefaultAllocator().New<SimPage>(), DefaultAllocator())));
    REQUIRE(context.ActivePage() == sim);
    CHECK(actions.IsEnabled(u8"sim.run"));
    CHECK_FALSE(actions.IsChecked(u8"sim.run"));
    CHECK(actions.Execute(u8"sim.run").IsOk());
    CHECK(sim->running);
    CHECK(actions.IsChecked(u8"sim.run"));
    CHECK(actions.Execute(u8"sim.run").IsOk());
    CHECK_FALSE(sim->running);
    CHECK_FALSE(actions.IsChecked(u8"file.exit")); // a Command is never checked
    // Back on the plain page, the toggle is out of reach again through the active subject;
    // a surface that names the sim page as its subject still reaches it (a page's own
    // toolbar in a split layout), and a named subject answers about ITSELF.
    context.SetActivePage(plain);
    CHECK_FALSE(actions.IsEnabled(u8"sim.run"));
    CHECK(actions.IsEnabled(u8"sim.run", sim));
    CHECK(actions.Execute(u8"sim.run", sim).IsOk());
    CHECK(sim->running);
    CHECK(actions.IsChecked(u8"sim.run", sim));
    CHECK_FALSE(actions.IsChecked(u8"sim.run")); // the active subject is the plain page
    CHECK(actions.Execute(u8"page.save", plain).Code() == ErrorCode::NotSupported); // clean
    plain->MarkDirty();
    context.SetActivePage(sim);
    CHECK_FALSE(actions.IsEnabled(u8"page.save"));  // the active sim page is clean
    CHECK(actions.IsEnabled(u8"page.save", plain)); // the named plain page is dirty
    CHECK(actions.Execute(u8"page.save", plain).IsOk());
    CHECK(plain->saves == 2u);
    // A bare registry has no subject: page-bound actions see null.
    EditorActionRegistry bare;
    EditorActionDeclaration needsPage = Declare(u8"needs.page", u8"Needs a page");
    needsPage.enabled = [](EditorPage* page) { return page != nullptr; };
    needsPage.execute = [](EditorPage*) {};
    REQUIRE(bare.Register(Move(needsPage)));
    CHECK(bare.Subject() == nullptr);
    CHECK_FALSE(bare.IsEnabled(u8"needs.page"));
    CHECK(bare.IsEnabled(u8"needs.page", plain));

    context.ClosePage(sim);
    context.ClosePage(plain);
}

TEST_CASE("actions: the shortcut table - the default chord, the user's override, a collision "
          "refused naming the holder, clearing and resetting, each a change")
{
    EditorContext context{DefaultAllocator()};
    EditorActionRegistry& actions = context.Actions();
    EditorActionDeclaration save = Declare(u8"page.save", u8"Save");
    save.shortcut = CtrlS;
    save.execute = [](EditorPage*) {};
    REQUIRE(actions.Register(Move(save)));
    EditorActionDeclaration run = Declare(u8"sim.run", u8"Simulate");
    run.shortcut = F5;
    run.alternateShortcut = EditorShortcut::Of(u8"f6");
    run.execute = [](EditorPage*) {};
    REQUIRE(actions.Register(Move(run)));
    EditorActionDeclaration bare = Declare(u8"view.reset", u8"Reset Layout"); // no chord
    bare.execute = [](EditorPage*) {};
    REQUIRE(actions.Register(Move(bare)));
    u32 changes = 0;
    gui::Effect watch([&]() { actions.TrackChanges(); ++changes; }); // a surface following the actions
    changes = 0; // its first run reads them; the changes are what follow

    CHECK(actions.Shortcut(u8"page.save") == CtrlS);
    CHECK_FALSE(actions.Shortcut(u8"view.reset").IsSet());
    CHECK_FALSE(actions.Shortcut(u8"nobody.home").IsSet());
    CHECK(actions.HolderOf(CtrlS) == actions.Find(u8"page.save"));
    CHECK(actions.HolderOf(EditorShortcut{}) == nullptr); // the unset chord is nobody's
    CHECK_FALSE(actions.HasOverride(u8"page.save"));
    // The alternate is the action's too: taken, reported, never rebound.
    const EditorShortcut f6 = EditorShortcut::Of(u8"f6");
    CHECK(actions.AlternateShortcut(u8"sim.run") == f6);
    CHECK_FALSE(actions.AlternateShortcut(u8"page.save").IsSet());
    CHECK(actions.HolderOf(f6) == actions.Find(u8"sim.run"));
    CHECK(actions.Rebind(u8"view.reset", f6).Code() == ErrorCode::AlreadyExists);

    // A free chord binds; the declaration's default no longer applies.
    const EditorShortcut ctrlShiftS = EditorShortcut::Of(u8"ctrl-shift-s");
    REQUIRE(actions.Rebind(u8"page.save", ctrlShiftS).IsOk());
    CHECK(changes == 1u);
    CHECK(actions.Shortcut(u8"page.save") == ctrlShiftS);
    CHECK(actions.HasOverride(u8"page.save"));
    CHECK(actions.HolderOf(CtrlS) == nullptr);
    CHECK(actions.HolderOf(ctrlShiftS) == actions.Find(u8"page.save"));

    // Another action's chord is refused, naming the holder; nothing changes.
    const EditorActionDeclaration* holder = nullptr;
    CHECK(actions.Rebind(u8"view.reset", F5, &holder).Code() == ErrorCode::AlreadyExists);
    REQUIRE(holder != nullptr);
    CHECK(holder->id == u8"sim.run");
    CHECK(changes == 1u);
    CHECK_FALSE(actions.Shortcut(u8"view.reset").IsSet());
    // An action may keep its own chord through Rebind (the settings page re-applies).
    CHECK(actions.Rebind(u8"sim.run", F5).IsOk());
    CHECK(actions.Shortcut(u8"sim.run") == F5);
    // The chord an override freed is available to another action.
    REQUIRE(actions.Rebind(u8"view.reset", CtrlS).IsOk());
    CHECK(actions.HolderOf(CtrlS) == actions.Find(u8"view.reset"));

    // Clearing = an override that is unset: "no shortcut", on purpose, distinct from the default.
    REQUIRE(actions.Rebind(u8"sim.run", EditorShortcut{}).IsOk());
    CHECK_FALSE(actions.Shortcut(u8"sim.run").IsSet());
    CHECK(actions.HasOverride(u8"sim.run"));
    CHECK(actions.HolderOf(F5) == nullptr);
    // Resetting forgets the override: the default is back.
    actions.ResetShortcut(u8"sim.run");
    CHECK(actions.Shortcut(u8"sim.run") == F5);
    CHECK_FALSE(actions.HasOverride(u8"sim.run"));
    const u32 before = changes;
    actions.ResetShortcut(u8"sim.run"); // nothing to forget: no change
    CHECK(changes == before);
    CHECK(actions.Rebind(u8"nobody.home", F5).Code() == ErrorCode::NotFound);
}

TEST_CASE("actions: a registration without an id, a label or execute is refused, and so is a "
          "second declaration of an id - the first stands")
{
    EditorActionRegistry actions;
    EditorActionDeclaration noId = Declare(u8"", u8"Nameless");
    noId.execute = [](EditorPage*) {};
    CHECK_FALSE(actions.Register(Move(noId)));
    EditorActionDeclaration noLabel = Declare(u8"x.y", u8"");
    noLabel.execute = [](EditorPage*) {};
    CHECK_FALSE(actions.Register(Move(noLabel)));
    CHECK_FALSE(actions.Register(Declare(u8"x.y", u8"No body"))); // execute unset
    CHECK(actions.Count() == 0u);

    u32 first = 0;
    u32 second = 0;
    EditorActionDeclaration a = Declare(u8"x.y", u8"First");
    a.execute = [&first](EditorPage*) { ++first; };
    EditorActionDeclaration b = Declare(u8"x.y", u8"Second");
    b.execute = [&second](EditorPage*) { ++second; };
    REQUIRE(actions.Register(Move(a)));
    CHECK_FALSE(actions.Register(Move(b)));
    CHECK(actions.Count() == 1u);
    CHECK(actions.Find(u8"x.y")->label == u8"First");
    CHECK(actions.Execute(u8"x.y").IsOk());
    CHECK(first == 1u);
    CHECK(second == 0u);
}

TEST_CASE("actions: AppendActionItems adds a context menu's items from the declarations over a "
          "subject - labelled, enabled as answered now, executing through the registry; an "
          "unknown id adds nothing")
{
    EditorActionRegistry actions;
    u32 runs = 0;
    EditorActionDeclaration run = Declare(u8"sim.run", u8"Simulate");
    run.shortcut = F5;
    run.enabled = [](EditorPage* page) { return page != nullptr; };
    run.execute = [&runs](EditorPage*) { ++runs; };
    REQUIRE(actions.Register(Move(run)));
    EditorActionDeclaration exit = Declare(u8"file.exit", u8"Exit");
    exit.execute = [](EditorPage*) {};
    REQUIRE(actions.Register(Move(exit)));

    SimPage page;
    auto menu = MakeRef<gui::Menu>(DefaultAllocator());
    const StringView ids[] = {u8"sim.run", u8"nobody.home", u8"file.exit"};
    CHECK(AppendActionItems(*menu, actions, &page, Span<const StringView>(ids, 3)) == 2u);
    const Array<gui::MenuItem*> items = ItemsOf(*menu);
    REQUIRE(items.Size() == 2u);
    CHECK(items[0]->GetText() == u8"Simulate");
    CHECK(items[0]->GetShortcut() == u8"F5"); // its chord, as the menu shows it
    CHECK(items[0]->IsEnabled());
    CHECK(items[1]->GetText() == u8"Exit");
    (void)items[0]->HandleActivate();
    CHECK(runs == 1u);
    // Over no subject the page-bound item is disabled; its click is refused by the registry.
    auto bare = MakeRef<gui::Menu>(DefaultAllocator());
    CHECK(AppendActionItems(*bare, actions, nullptr, Span<const StringView>(ids, 1)) == 1u);
    const Array<gui::MenuItem*> bareItems = ItemsOf(*bare);
    REQUIRE(bareItems.Size() == 1u);
    CHECK_FALSE(bareItems[0]->IsEnabled());
    (void)bareItems[0]->HandleActivate();
    CHECK(runs == 1u);
}

TEST_CASE("actions: FormatShortcut spells a chord the way a shortcut column shows it")
{
    CHECK(FormatShortcut(EditorShortcut{}).IsEmpty());
    CHECK(FormatShortcut(CtrlS) == u8"Ctrl+S");
    CHECK(FormatShortcut(F5) == u8"F5");
    CHECK(FormatShortcut(EditorShortcut::Of(u8"ctrl-shift-z")) ==
          u8"Ctrl+Shift+Z");
    CHECK(FormatShortcut(EditorShortcut::Of(u8"alt-pageup")) == u8"Alt+PgUp");
    CHECK(FormatShortcut(EditorShortcut::Of(u8"delete")) == u8"Del");
}


TEST_CASE("actions: shortcut overrides persist - captured from the registry into the settings section, "
          "round-tripped through the store, applied back with unknown ids kept and collisions skipped")
{
    RegisterEditorSettingsTypes();
    EditorContext context{DefaultAllocator()};
    EditorActionRegistry& actions = context.Actions();
    const auto declare = [&](StringView id, EditorShortcut chord)
    {
        EditorActionDeclaration d = Declare(id, id);
        d.shortcut = chord;
        d.execute = [](EditorPage*) {};
        REQUIRE(actions.Register(Move(d)));
    };
    declare(u8"page.save", CtrlS);
    declare(u8"sim.run", F5);
    declare(u8"view.reset", EditorShortcut{});
    const EditorShortcut ctrlShiftS = EditorShortcut::Of(u8"ctrl-shift-s");
    REQUIRE(actions.Rebind(u8"page.save", ctrlShiftS).IsOk());
    REQUIRE(actions.Rebind(u8"sim.run", EditorShortcut{}).IsOk()); // cleared on purpose

    // Capture: only the overridden ones; an entry for an id this build never registered stays.
    foundation::settings::Settings store(DefaultAllocator());
    EditorShortcutSettings& section = store.Section<EditorShortcutSettings>();
    ShortcutOverrideEntry foreign;
    foreign.id = String(u8"terrain.sculpt"); // a domain not loaded here
    foreign.keys = String(u8"t");
    section.overrides.PushBack(Move(foreign));
    CHECK(CaptureShortcutOverrides(actions, section) == 3u);
    REQUIRE(section.Find(u8"page.save") != nullptr);
    CHECK(section.Find(u8"page.save")->Chord() == ctrlShiftS);
    REQUIRE(section.Find(u8"sim.run") != nullptr);
    CHECK_FALSE(section.Find(u8"sim.run")->Chord().IsSet());
    CHECK(section.Find(u8"view.reset") == nullptr);
    CHECK(section.Find(u8"terrain.sculpt") != nullptr);
    // A reset override leaves the section on the next capture.
    actions.ResetShortcut(u8"sim.run");
    CHECK(CaptureShortcutOverrides(actions, section) == 2u);
    CHECK(section.Find(u8"sim.run") == nullptr);

    // Through the store and back.
    MemoryStream buffer;
    REQUIRE(store.Save(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buffer.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings loaded(DefaultAllocator());
    REQUIRE(loaded.Load(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    const EditorShortcutSettings* back = loaded.Find<EditorShortcutSettings>();
    REQUIRE(back != nullptr);
    REQUIRE(back->overrides.Size() == 2u);
    CHECK(back->Find(u8"page.save")->Chord() == ctrlShiftS);
    CHECK(back->Find(u8"terrain.sculpt")->Chord() == EditorShortcut::Of(u8"t"));

    // Apply to a fresh registry: the known override binds, the unknown id is skipped, and a
    // chord another action holds is refused and skipped (the holder keeps it).
    EditorContext other{DefaultAllocator()};
    EditorActionRegistry& fresh = other.Actions();
    {
        EditorActionDeclaration d = Declare(u8"page.save", u8"Save");
        d.shortcut = CtrlS;
        d.execute = [](EditorPage*) {};
        REQUIRE(fresh.Register(Move(d)));
        EditorActionDeclaration taken = Declare(u8"edit.other", u8"Other");
        taken.shortcut = ctrlShiftS; // holds the chord the override wants? no - page.save's own
        taken.execute = [](EditorPage*) {};
        REQUIRE(fresh.Register(Move(taken)));
    }
    // edit.other holds Ctrl+Shift+S, so page.save's override collides and is skipped.
    CHECK(ApplyShortcutOverrides(*back, fresh) == 0u);
    CHECK(fresh.Shortcut(u8"page.save") == CtrlS);
    CHECK_FALSE(fresh.HasOverride(u8"page.save"));
    // Free the chord: the override applies on the next apply; the unknown id still counts nothing.
    REQUIRE(fresh.Rebind(u8"edit.other", EditorShortcut{}).IsOk());
    CHECK(ApplyShortcutOverrides(*back, fresh) == 1u);
    CHECK(fresh.Shortcut(u8"page.save") == ctrlShiftS);
    CHECK(fresh.Find(u8"terrain.sculpt") == nullptr);
}

TEST_CASE("actions: ShortcutEdits apply together - a chosen chord, a swap between two actions, a reset, "
          "and a collision that gives the staged action back what it had and names the holder")
{
    RegisterEditorSettingsTypes();
    EditorContext context{DefaultAllocator()};
    EditorActionRegistry& actions = context.Actions();
    const auto declare = [&](StringView id, EditorShortcut chord)
    {
        EditorActionDeclaration d = Declare(id, id);
        d.shortcut = chord;
        d.execute = [](EditorPage*) {};
        REQUIRE(actions.Register(Move(d)));
    };
    const EditorShortcut CtrlZ = EditorShortcut::Of(u8"ctrl-z");
    const EditorShortcut CtrlY = EditorShortcut::Of(u8"ctrl-y");
    declare(u8"page.save", CtrlS);
    declare(u8"edit.undo", CtrlZ);
    declare(u8"edit.redo", CtrlY);
    declare(u8"sim.run", F5);
    declare(u8"view.reset", EditorShortcut{});
    foundation::settings::Settings store(DefaultAllocator());
    EditorShortcutSettings& section = store.Section<EditorShortcutSettings>();

    ShortcutEdits edits;
    CHECK(edits.IsEmpty());
    // A chord for a bare action; a swap of undo and redo; a reset of one with an override;
    // a collision with an action nobody staged.
    REQUIRE(actions.Rebind(u8"sim.run", EditorShortcut::Of(u8"f7")).IsOk());
    edits.Set(u8"view.reset", EditorShortcut::Of(u8"f6"));
    edits.Set(u8"edit.undo", CtrlY);
    edits.Set(u8"edit.redo", CtrlZ);
    edits.Reset(u8"sim.run");
    edits.Set(u8"view.reset", EditorShortcut::Of(u8"f6")); // re-staged: still one entry
    CHECK(edits.Count() == 4u);
    REQUIRE(edits.Pending(u8"sim.run") != nullptr);
    CHECK(edits.Pending(u8"sim.run")->reset);
    edits.Set(u8"page.save", CtrlS);        // its own chord: fine
    edits.Discard(u8"page.save");           // changed my mind
    edits.Set(u8"page.save", F5);           // ... no: sim.run's DEFAULT comes back through the reset, so
                                            // F5 collides with it (sim.run is staged as a reset, not freed)
    CHECK(edits.Count() == 5u);

    Array<String> collisions;
    const usize applied = edits.Apply(actions, section, &collisions);
    CHECK(edits.IsEmpty());
    CHECK(actions.Shortcut(u8"view.reset") == EditorShortcut::Of(u8"f6"));
    CHECK(actions.Shortcut(u8"edit.undo") == CtrlY);
    CHECK(actions.Shortcut(u8"edit.redo") == CtrlZ);
    CHECK(actions.Shortcut(u8"sim.run") == F5);
    CHECK_FALSE(actions.HasOverride(u8"sim.run"));
    // The collision: page.save wanted F5, sim.run's default holds it; page.save is back to its own.
    CHECK(actions.Shortcut(u8"page.save") == CtrlS);
    CHECK_FALSE(actions.HasOverride(u8"page.save"));
    REQUIRE(collisions.Size() == 1u);
    CHECK(collisions[0].AsView().StartsWith(u8"F5: 'sim.run' holds it"));
    CHECK(applied == 4u);
    // The section holds the overrides that stand: three.
    CHECK(section.overrides.Size() == 3u);
    CHECK(section.Find(u8"edit.undo")->Chord() == CtrlY);
    CHECK(section.Find(u8"page.save") == nullptr);
    CHECK(section.Find(u8"sim.run") == nullptr);
    // An id nobody registered is dropped quietly.
    edits.Set(u8"nobody.home", F5);
    CHECK(edits.Apply(actions, section, &collisions) == 0u);
    CHECK(collisions.Size() == 1u);
}

TEST_CASE("actions: a shortcut override kept as a key code before chords were text reads as its keys")
{
    // foundation.ui's codes were USB HID usages with its modifier bits: T is 23, F5 is 62, Ctrl
    // the bits 0x00C0, Shift 0x0003.
    CHECK(editor::detail::LegacyChordKeys(23u, 0x0040u) == String(u8"ctrl-t"));
    CHECK(editor::detail::LegacyChordKeys(62u, 0u) == String(u8"f5"));
    CHECK(editor::detail::LegacyChordKeys(76u, 0x0001u) == String(u8"shift-delete"));
    CHECK(editor::detail::LegacyChordKeys(29u, 0x0042u) == String(u8"ctrl-shift-z"));
    CHECK(editor::detail::LegacyChordKeys(0u, 0u).IsEmpty());
    CHECK(EditorShortcut::Of(editor::detail::LegacyChordKeys(29u, 0x0042u).AsView()) == EditorShortcut::Of(u8"ctrl-shift-z"));
}
