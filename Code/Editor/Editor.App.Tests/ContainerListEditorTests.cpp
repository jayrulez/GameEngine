// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The one list widget and the one asset row (editor-lists-and-asset-slots.md P0), headless,
// ported from Sedulous d6018d3f: a drop on a slot assigns it, a drop on the list appends, a wrong
// type is refused and reported, the add icon appends or defers to its menu, a section list is its
// header alone with element icons for the sections, and a ResourceRefEditor's pick, drop and clear
// are one assignment.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit; // TreeDragData: a hierarchy row
import editor.core;
import editor.app;
import foundation.settings; // the Preferences store
import engine.project; // the reflected asset settings the dialog builds rows from

using namespace foundation::core;
using namespace editor;
namespace ui = foundation::ui;

namespace
{
    const Guid kMaterial{0x1111, 0x1};
    const Guid kTexture{0x2222, 0x2};

    RefPtr<app::AssetDragData> Drag(const Guid& id, StringView typeName)
    {
        return MakeRef<app::AssetDragData>(DefaultAllocator(), id, typeName, StringView(u8"dragged"));
    }

    // The slot of element `index`: the column's rows follow the header.
    app::AssetPickerSlot* SlotAt(ui::View* column, usize index)
    {
        auto* row = Cast<ui::ViewGroup>(Cast<ui::ViewGroup>(column)->GetChildAt(1 + index));
        return Cast<app::AssetPickerSlot>(row->GetChildAt(0));
    }

    ui::IconButton* AddIcon(ui::View* column)
    {
        auto* header = Cast<ui::ViewGroup>(Cast<ui::ViewGroup>(column)->GetChildAt(0));
        return Cast<ui::IconButton>(header->GetChildAt(1));
    }

    Array<String> Types(StringView type)
    {
        Array<String> types;
        types.PushBack(String(type));
        return types;
    }
}

TEST_CASE("container-list: a drop on a slot assigns it, a drop on the list appends, a wrong type "
          "is refused on both and reported")
{
    auto list = MakeRef<app::ContainerListEditor>(DefaultAllocator(), StringView(u8"Materials"),
                                                  StringView(u8"Mesh"));
    list->slotNames.PushBack(String(u8"Red"));
    list->slotNames.PushBack(String(u8"(none)"));
    list->SetAcceptedTypes(Types(u8"MaterialAsset"));
    usize assignedIndex = 99;
    Guid assigned;
    Guid appended;
    String rejected;
    list->OnAssignSlot = [&](usize i, const Guid& id)
    {
        assignedIndex = i;
        assigned = id;
    };
    list->OnAppendDropped = [&](const Guid& id) { appended = id; };
    list->OnRejectedDrop = [&](StringView, StringView typeName) { rejected = String(typeName); };
    ui::View* column = list->EditorView();

    // On slot 1: that element.
    app::AssetPickerSlot* slot = SlotAt(column, 1);
    REQUIRE(slot->AsDropTarget() != nullptr); // a typed list's slots are drop targets
    auto material = Drag(kMaterial, u8"MaterialAsset");
    CHECK(slot->OnDrop(material.Get(), 0, 0) == ui::DragDropEffects::Link);
    CHECK(assignedIndex == 1u);
    CHECK(assigned == kMaterial);

    // On the list itself (the header, a gap, an empty list): appended.
    ui::IDropTarget* zone = column->AsDropTarget();
    REQUIRE(zone != nullptr);
    CHECK(zone->OnDrop(material.Get(), 0, 0) == ui::DragDropEffects::Link);
    CHECK(appended == kMaterial);

    // A texture is refused on both, and reported.
    auto texture = Drag(kTexture, u8"TextureAsset");
    assignedIndex = 99;
    CHECK(slot->OnDrop(texture.Get(), 0, 0) == ui::DragDropEffects::None);
    CHECK(assignedIndex == 99u);
    CHECK(rejected == u8"TextureAsset");
    rejected = String();
    appended = Guid{};
    CHECK(zone->OnDrop(texture.Get(), 0, 0) == ui::DragDropEffects::None);
    CHECK(appended.IsNil());
    CHECK(rejected == u8"TextureAsset");
}

TEST_CASE("container-list: an untyped list takes no drop, and the wildcard takes any asset")
{
    auto untyped = MakeRef<app::ContainerListEditor>(DefaultAllocator(), StringView(u8"Things"),
                                                     StringView(u8"Cat"));
    untyped->slotNames.PushBack(String(u8"a"));
    untyped->OnAppendDropped = [](const Guid&) {};
    CHECK(SlotAt(untyped->EditorView(), 0)->AsDropTarget() == nullptr);
    CHECK(untyped->EditorView()->AsDropTarget() == nullptr);

    auto any = MakeRef<app::ContainerListEditor>(DefaultAllocator(), StringView(u8"Anything"),
                                                 StringView(u8"Cat"));
    any->slotNames.PushBack(String(u8"a"));
    any->SetAcceptedTypes(Types(app::AssetPickerSlot::AnyAssetType()));
    Guid assigned;
    any->OnAssignSlot = [&](usize, const Guid& id) { assigned = id; };
    auto texture = Drag(kTexture, u8"TextureAsset");
    CHECK(SlotAt(any->EditorView(), 0)->OnDrop(texture.Get(), 0, 0) == ui::DragDropEffects::Link);
    CHECK(assigned == kTexture);
}

TEST_CASE("container-list: the add icon appends, or defers to its menu")
{
    auto list = MakeRef<app::ContainerListEditor>(DefaultAllocator(), StringView(u8"Things"),
                                                  StringView(u8"Cat"));
    i32 adds = 0;
    list->OnAdd = [&]() { ++adds; };
    AddIcon(list->EditorView())->FireClick();
    CHECK(adds == 1);
    CHECK(AddIcon(list->EditorView())->TooltipText == u8"Add");

    // With a menu, the icon opens it (headless: no context to open in) and does not append.
    auto menued = MakeRef<app::ContainerListEditor>(DefaultAllocator(), StringView(u8"Behaviors"),
                                                    StringView(u8"Cat"));
    i32 menuAdds = 0;
    menued->OnAdd = [&]() { ++menuAdds; };
    menued->OnAddMenu = [](ui::ContextMenu& menu) { menu.AddItem(u8"Kind", []() {}); };
    AddIcon(menued->EditorView())->FireClick();
    CHECK(menuAdds == 0);
}

TEST_CASE("container-list: a section list is its header, and its elements carry their own icons")
{
    auto list = MakeRef<app::ContainerListEditor>(DefaultAllocator(), StringView(u8"Behaviors"),
                                                  StringView(u8"Script"));
    list->ElementsAsSections = true;
    list->slotNames.PushBack(String(u8"Mover"));
    list->slotNames.PushBack(String(u8"Spinner"));
    auto* column = Cast<ui::ViewGroup>(list->EditorView());
    CHECK(column->ChildCount() == 1u); // the header alone: the elements are sections
    auto* count = Cast<ui::Label>(Cast<ui::ViewGroup>(column->GetChildAt(0))->GetChildAt(0));
    REQUIRE(count != nullptr);
    CHECK(count->Text.Value() == StringView(u8"2 items"));

    // A section's header icons: up, down, remove, each reporting its element.
    usize moved = 99;
    bool movedUp = true;
    usize removed = 99;
    RefPtr<ui::View> first = app::ContainerListEditor::ElementActions(
        DefaultAllocator(), 0, 2,
        [&](usize i, bool up)
        {
            moved = i;
            movedUp = up;
        },
        [&](usize i) { removed = i; });
    auto* firstGroup = Cast<ui::ViewGroup>(first.Get());
    CHECK_FALSE(Cast<ui::IconButton>(firstGroup->GetChildAt(0))->IsEnabled); // first: no up
    CHECK(Cast<ui::IconButton>(firstGroup->GetChildAt(1))->IsEnabled);
    Cast<ui::IconButton>(firstGroup->GetChildAt(1))->FireClick();
    CHECK(moved == 0u);
    CHECK_FALSE(movedUp);
    Cast<ui::IconButton>(firstGroup->GetChildAt(2))->FireClick();
    CHECK(removed == 0u);
    RefPtr<ui::View> last = app::ContainerListEditor::ElementActions(
        DefaultAllocator(), 1, 2, [](usize, bool) {}, [](usize) {});
    CHECK_FALSE(Cast<ui::IconButton>(Cast<ui::ViewGroup>(last.Get())->GetChildAt(1))->IsEnabled);

    // No move callback: remove alone (an order that means nothing).
    RefPtr<ui::View> removeOnly = app::ContainerListEditor::ElementActions(
        DefaultAllocator(), 0, 3, Function<void(usize, bool)>{}, [&](usize i) { removed = i + 10; });
    auto* removeGroup = Cast<ui::ViewGroup>(removeOnly.Get());
    CHECK(removeGroup->ChildCount() == 1u);
    Cast<ui::IconButton>(removeGroup->GetChildAt(0))->FireClick();
    CHECK(removed == 10u);
}

TEST_CASE("resource-row: pick, drop and clear are one assignment; a dangling id reads (missing)")
{
    EditorContext context{DefaultAllocator()};
    Guid current;
    Array<Guid> writes;
    const StringView types[] = {u8"MaterialAsset"};
    auto row = MakeRef<app::ResourceRefEditor>(DefaultAllocator(), StringView(u8"Material"),
                                               StringView(u8"(none)"), StringView(u8"Mesh"),
                                               Span<const StringView>{types, 1});
    row->BindAsset(context, [&]() { return current; },
                   [&](const Guid& id)
                   {
                       current = id;
                       writes.PushBack(id);
                   });
    auto* slot = Cast<app::AssetPickerSlot>(row->EditorView());
    REQUIRE(slot != nullptr);
    CHECK(slot->AsDropTarget() != nullptr); // a typed row is a drop target from construction

    // A drop assigns through the bound write; a wrong type does not.
    auto material = Drag(kMaterial, u8"MaterialAsset");
    CHECK(slot->OnDrop(material.Get(), 0, 0) == ui::DragDropEffects::Link);
    CHECK(writes.Size() == 1u);
    CHECK(current == kMaterial);
    auto texture = Drag(kTexture, u8"TextureAsset");
    CHECK(slot->OnDrop(texture.Get(), 0, 0) == ui::DragDropEffects::None);
    CHECK(writes.Size() == 1u);

    // Clear is the same write, with the nil id; the row names what it now holds.
    slot->ClearButton()->FireClick();
    CHECK(writes.Size() == 2u);
    CHECK(current.IsNil());
    CHECK(row->ValueText() == StringView(u8"(none)"));

    // A dangling id reads "(missing)" and stays clearable.
    current = kTexture;
    row->Refresh();
    CHECK(row->ValueText() == StringView(u8"(missing)"));
    CHECK(slot->ClearButton()->IsEnabled);

    // A row that is no asset (an entity reference) takes no asset drop.
    auto entityRow = MakeRef<app::ResourceRefEditor>(DefaultAllocator(), StringView(u8"Target"),
                                                     StringView(u8"(none)"), StringView(u8"Joint"),
                                                     Span<const StringView>{});
    CHECK(Cast<app::AssetPickerSlot>(entityRow->EditorView())->AsDropTarget() == nullptr);
}

TEST_CASE("resource-row: a bound row shows its empty text for nil and drops edit and reveal on "
          "request; a refusal names every accepted type")
{
    EditorContext context{DefaultAllocator()};
    Guid current;
    const StringView types[] = {u8"StaticMeshAsset", u8"SkinnedMeshAsset"};
    auto row = MakeRef<app::ResourceRefEditor>(DefaultAllocator(), StringView(u8"Mesh"),
                                               StringView(u8"(primitive)"), StringView(u8"Preview"),
                                               Span<const StringView>{types, 2});
    row->SetEmptyText(u8"(primitive)");
    row->BindAsset(context, [&]() { return current; }, [&](const Guid& id) { current = id; },
                   app::ResourceRefEditor::BindOptions{.edit = false, .reveal = false});
    CHECK(row->ValueText() == StringView(u8"(primitive)"));
    CHECK_FALSE(row->OnEdit);
    CHECK_FALSE(row->OnReveal);
    auto* slot = Cast<app::AssetPickerSlot>(row->EditorView());
    REQUIRE(slot != nullptr);
    CHECK_FALSE(slot->ClearButton()->IsEnabled); // nil: nothing to clear, whatever the text says

    auto texture = Drag(kTexture, u8"TextureAsset");
    CHECK(slot->OnDrop(texture.Get(), 0, 0) == ui::DragDropEffects::None);
    CHECK(app::AssetPickerSlot::RejectionText(u8"brick", u8"TextureAsset", row->AcceptedTypes(),
                                              u8"field") ==
          StringView(u8"brick is a TextureAsset - this field takes StaticMeshAsset or "
                     u8"SkinnedMeshAsset"));
}

// Sedulous 4cfff157: an asset row placed outside a grid (a dialog row, a preview bar) shares its
// slot with the layout; building and releasing both must neither leak nor double release (ASAN).
TEST_CASE("resource-row: the settings dialog and a compact slot build and release cleanly")
{
    EditorContext context{DefaultAllocator()};
    {
        auto dialog = MakeRef<app::ProjectSettingsDialog>(DefaultAllocator(), context);
        CHECK(dialog.Get() != nullptr);
    }
    {
        Guid current;
        const StringView types[] = {u8"SkeletonAsset"};
        auto compact = MakeRef<app::CompactAssetSlot>(DefaultAllocator(), StringView(u8"Skeleton"),
                                                      Span<const StringView>{types, 1});
        compact->Editor().BindAsset(context, [&]() { return current; },
                                    [&](const Guid& id) { current = id; });
        compact->Build();
        REQUIRE(compact->ChildCount() == 2u); // the caption, then the row's slot
        auto* slot = Cast<app::AssetPickerSlot>(compact->GetChildAt(1));
        REQUIRE(slot != nullptr);
        auto skeleton = Drag(kMaterial, u8"SkeletonAsset");
        CHECK(slot->OnDrop(skeleton.Get(), 0, 0) == ui::DragDropEffects::Link);
        CHECK(current == kMaterial);
    }
}

namespace
{
    usize CountSlots(ui::View& view)
    {
        // Through the visual children: a dialog's layout and a scroll view's content are those.
        usize count = Cast<app::AssetPickerSlot>(&view) != nullptr ? 1u : 0u;
        if (auto* group = Cast<ui::ViewGroup>(&view))
        {
            for (usize i = 0; i < group->VisualChildCount(); ++i)
            {
                if (ui::View* child = group->GetVisualChild(i))
                {
                    count += CountSlots(*child);
                }
            }
        }
        return count;
    }
}

namespace
{
    // Every view of type T under `view`, in tree order (through the visual children).
    template <typename T>
    void CollectViews(ui::View& view, Array<T*>& out)
    {
        if (T* typed = Cast<T>(&view))
        {
            out.PushBack(typed);
        }
        if (auto* group = Cast<ui::ViewGroup>(&view))
        {
            for (usize i = 0; i < group->VisualChildCount(); ++i)
            {
                if (ui::View* child = group->GetVisualChild(i))
                {
                    CollectViews(*child, out);
                }
            }
        }
    }
}

// Sedulous 7d6e4460: the display settings are rows the dialog builds from their reflection (a
// field per size, a combo per enum, a check box per flag), and Save writes them back.
TEST_CASE("resource-row: the settings dialog edits number, choice and flag settings")
{
    const StringView dir = u8"scratch_settings_dialog_values";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    {
        auto dialog = MakeRef<app::ProjectSettingsDialog>(DefaultAllocator(), context);
        Array<ui::NumericField*> numbers;
        Array<ui::ComboBox*> choices;
        Array<ui::CheckBox*> flags;
        Array<ui::Button*> buttons;
        CollectViews(*dialog, numbers);
        CollectViews(*dialog, choices);
        CollectViews(*dialog, flags);
        CollectViews(*dialog, buttons);
        // Render width, height; window width, height. Render fit, window mode, then MSAA's own.
        REQUIRE(numbers.Size() == 4u);
        REQUIRE(choices.Size() == 3u);
        REQUIRE(flags.Size() == 1u);
        CHECK(numbers[2]->Value() == doctest::Approx(1280.0)); // seeded from the manifest
        CHECK(choices[0]->SelectedIndex() == static_cast<i32>(FitMode::Letterbox));
        CHECK(flags[0]->IsChecked.Value());

        numbers[0]->SetValue(640.0);
        numbers[1]->SetValue(360.0);
        numbers[2]->SetValue(0.0); // a window takes at least one pixel: the range clamps
        choices[0]->SetSelectedIndex(static_cast<i32>(FitMode::IntegerScale));
        choices[1]->SetSelectedIndex(static_cast<i32>(engine::project::WindowMode::Fullscreen));
        flags[0]->IsChecked.SetValue(false);
        ui::Button* save = nullptr;
        for (ui::Button* button : buttons)
        {
            save = button->Text.Value() == StringView(u8"Save") ? button : save;
        }
        REQUIRE(save != nullptr);
        save->FireClick();
    }
    const engine::project::ProjectSettings& settings = project->Settings();
    CHECK(settings.renderWidth == 640u);
    CHECK(settings.renderHeight == 360u);
    CHECK(settings.windowWidth == 1u);
    CHECK(settings.renderFit == FitMode::IntegerScale);
    CHECK(settings.windowMode == engine::project::WindowMode::Fullscreen);
    CHECK_FALSE(settings.windowResizable);
    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

// Sedulous 39147576: an asset list setting (the other UI fonts) is a list row of slots, one per
// entry the manifest holds, beside a slot per single asset setting.
TEST_CASE("resource-row: the settings dialog lists an asset list setting as slots")
{
    const StringView dir = u8"scratch_settings_dialog_lists";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());

    usize single = 0;
    for (const PropertyInfo& property : Properties(engine::project::ProjectSettings::StaticType()))
    {
        single += engine::project::IsAssetSetting(property) ? 1u : 0u;
    }
    {
        auto dialog = MakeRef<app::ProjectSettingsDialog>(DefaultAllocator(), context);
        CHECK(CountSlots(*dialog) == single); // the list is empty
    }
    project->Settings().uiFontIds.PushBack(kMaterial);
    project->Settings().uiFontIds.PushBack(kTexture);
    {
        auto dialog = MakeRef<app::ProjectSettingsDialog>(DefaultAllocator(), context);
        CHECK(CountSlots(*dialog) == single + 2u); // a slot per listed font
    }
    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

// editor-lists-and-asset-slots P3 (Sedulous 0fac1640): an entity reference takes a hierarchy row
// through the same drop as an asset, and no asset; an asset slot, the any-asset one included,
// takes no entity.
TEST_CASE("resource-row: an entity slot takes a hierarchy row and no asset")
{
    Guid current;
    const StringView entityType[] = {app::AssetPickerSlot::EntityType()};
    auto row = MakeRef<app::ResourceRefEditor>(DefaultAllocator(), StringView(u8"Target"),
                                               StringView(u8"(none)"), StringView(u8"Follow"),
                                               Span<const StringView>{entityType, 1});
    row->BindEntity([&]() { return current; }, [&](const Guid& id) { current = id; },
                    [](const Guid&) { return String(u8"Cart"); }, []() {});
    auto* slot = Cast<app::AssetPickerSlot>(row->EditorView());
    REQUIRE(slot != nullptr);
    CHECK(slot->AsDropTarget() != nullptr); // an entity row is a drop target

    // A hierarchy row that names its entity is assigned.
    auto entity = MakeRef<ui::toolkit::TreeDragData>(DefaultAllocator(), 0);
    entity->ItemKind = String(app::AssetPickerSlot::EntityItemKind());
    entity->ItemId = kMaterial;
    entity->ItemName = String(u8"Cart");
    CHECK(slot->OnDrop(entity.Get(), 0, 0) == ui::DragDropEffects::Link);
    CHECK(current == kMaterial);
    CHECK(row->ValueText() == StringView(u8"Cart"));

    // An asset is refused, and so is a tree row that names nothing.
    auto material = Drag(kTexture, u8"MaterialAsset");
    CHECK(slot->OnDrop(material.Get(), 0, 0) == ui::DragDropEffects::None);
    auto bare = MakeRef<ui::toolkit::TreeDragData>(DefaultAllocator(), 0);
    CHECK(slot->CanAcceptDrop(bare.Get(), 0, 0) == ui::DragDropEffects::None);
    CHECK(current == kMaterial);

    // Clear is the same write, with the nil id.
    slot->ClearButton()->FireClick();
    CHECK(current.IsNil());
    CHECK(row->ValueText() == StringView(u8"(none)"));

    // An asset slot, even the any-asset one, refuses an entity, and says what it wanted.
    const StringView anyType[] = {app::AssetPickerSlot::AnyAssetType()};
    auto any = MakeRef<app::ResourceRefEditor>(DefaultAllocator(), StringView(u8"Thing"),
                                               StringView(u8"(none)"), StringView(u8"Cat"),
                                               Span<const StringView>{anyType, 1});
    CHECK(Cast<app::AssetPickerSlot>(any->EditorView())->OnDrop(entity.Get(), 0, 0) ==
          ui::DragDropEffects::None);
    CHECK(app::AssetPickerSlot::RejectionText(u8"Cart", app::AssetPickerSlot::EntityType(),
                                              any->AcceptedTypes(), u8"field") ==
          StringView(u8"Cart is an entity - this field takes any asset"));

    // An entity list takes one on a slot and appends one; an asset is refused.
    auto list = MakeRef<app::ContainerListEditor>(DefaultAllocator(), StringView(u8"Targets"),
                                                  StringView(u8"Follow"));
    list->slotNames.PushBack(String(u8"(none)"));
    list->SetAcceptedTypes(Types(app::AssetPickerSlot::EntityType()));
    Guid assigned;
    Guid appended;
    list->OnAssignSlot = [&](usize, const Guid& id) { assigned = id; };
    list->OnAppendDropped = [&](const Guid& id) { appended = id; };
    CHECK(SlotAt(list->EditorView(), 0)->OnDrop(entity.Get(), 0, 0) == ui::DragDropEffects::Link);
    CHECK(list->EditorView()->AsDropTarget()->OnDrop(entity.Get(), 0, 0) ==
          ui::DragDropEffects::Link);
    CHECK(assigned == kMaterial);
    CHECK(appended == kMaterial);
    CHECK(list->EditorView()->AsDropTarget()->OnDrop(material.Get(), 0, 0) ==
          ui::DragDropEffects::None);
}

// Sedulous c5100e94: Preferences lists the user's preview resolutions as rows, one per entry of
// the store's section; Add appends one, Remove takes one out (applied on Save).
TEST_CASE("preferences: the preview resolutions are rows that add and remove")
{
    RegisterEditorSettingsTypes();
    EditorContext context{DefaultAllocator()};
    foundation::settings::Settings store(DefaultAllocator());
    GamePreviewSettings& previews = store.Section<GamePreviewSettings>();
    previews.seeded = true; // the user's own list, not the seeded one
    previews.presets.PushBack(GamePreviewResolution{String(u8"Deck"), 1280, 800});
    previews.presets.PushBack(GamePreviewResolution{String(u8"Phone"), 1080, 2400});

    auto dialog = MakeRef<app::EditorPreferencesDialog>(DefaultAllocator(), context, store);
    CHECK(dialog->PreviewRowCount() == 2u);
    const auto button = [&dialog](StringView text)
    {
        Array<ui::Button*> buttons;
        CollectViews(*dialog, buttons);
        for (ui::Button* candidate : buttons)
        {
            if (candidate->Text.Value() == text)
            {
                return candidate;
            }
        }
        return static_cast<ui::Button*>(nullptr);
    };
    REQUIRE(button(u8"Add resolution") != nullptr);
    button(u8"Add resolution")->FireClick();
    CHECK(dialog->PreviewRowCount() == 3u);
    REQUIRE(button(u8"Remove") != nullptr);
    button(u8"Remove")->FireClick();
    CHECK(dialog->PreviewRowCount() == 2u);
}

// Preferences shows a tab per category, not one long column: the editor's own, then one per
// domain-contributed category, contributions that name the same category sharing its tab.
TEST_CASE("preferences: a tab per category, contributions sharing their category's")
{
    RegisterEditorSettingsTypes();
    EditorContext context{DefaultAllocator()};
    bool navigationFlag = false;
    const auto contribute = [&context, &navigationFlag](StringView category, StringView label)
    {
        EditorContext::EditorSettingsContribution contribution;
        contribution.category = String(category);
        EditorContext::EditorSettingsBoolField field;
        field.label = String(label);
        field.get = [&navigationFlag]() { return navigationFlag; };
        field.set = [&navigationFlag](bool value) { navigationFlag = value; };
        contribution.bools.PushBack(Move(field));
        context.RegisterEditorSettingsContribution(Move(contribution));
    };
    contribute(u8"Navigation", u8"Show the navmesh");
    contribute(u8"Audio", u8"Mute in the background");
    contribute(u8"Navigation", u8"Show the bake bounds");
    foundation::settings::Settings store(DefaultAllocator());

    auto dialog = MakeRef<app::EditorPreferencesDialog>(DefaultAllocator(), context, store);
    ui::TabView& tabs = dialog->Tabs();
    REQUIRE(tabs.TabCount() == 7u);
    CHECK(dialog->TabIndexOf(u8"Appearance") == 0);
    CHECK(dialog->TabIndexOf(u8"Export") == 1);
    CHECK(dialog->TabIndexOf(u8"Agent access") == 2);
    CHECK(dialog->TabIndexOf(u8"Game preview") == 3);
    CHECK(dialog->TabIndexOf(u8"Shortcuts") == 4);
    CHECK(dialog->TabIndexOf(u8"Navigation") == 5);
    CHECK(dialog->TabIndexOf(u8"Audio") == 6);
    CHECK(tabs.SelectedIndex() == 0);

    // Both Navigation fields are on its tab, and they still write through their closures.
    Array<ui::CheckBox*> boxes;
    CollectViews(*tabs.GetChildAt(5), boxes);
    REQUIRE(boxes.Size() == 2u);
    CHECK(boxes[0]->Text.Value() == StringView(u8"Show the navmesh"));
    CHECK(boxes[1]->Text.Value() == StringView(u8"Show the bake bounds"));
    boxes[0]->IsChecked.SetValue(true);
    CHECK(navigationFlag);
}

// Project Settings shows a tab per category its settings name (as Preferences does), in the order
// they first name them; every labelled setting is on its category's tab, and MSAA, its own row,
// on the display settings' tab.
TEST_CASE("project-settings: a tab per category the settings name")
{
    const StringView dir = u8"scratch_settings_dialog_tabs";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    {
        auto dialog = MakeRef<app::ProjectSettingsDialog>(DefaultAllocator(), context);
        ui::TabView& tabs = dialog->Tabs();
        CHECK(tabs.SelectedIndex() == 0);
        CHECK(dialog->TabIndexOf(engine::project::kSettingDefaultCategory) == 0);

        // The categories, as the settings name them, in first-named order: one tab each.
        Array<String> categories;
        for (const PropertyInfo& property : Properties(engine::project::ProjectSettings::StaticType()))
        {
            if (engine::project::SettingAttribute(property, engine::project::kSettingLabelAttribute) ==
                nullptr)
            {
                continue;
            }
            const StringView category = engine::project::SettingCategory(property);
            bool seen = false;
            for (const String& known : categories)
            {
                seen = seen || known.AsView() == category;
            }
            if (!seen)
            {
                categories.PushBack(String(category));
            }
        }
        REQUIRE(categories.Size() > 1u);
        CHECK(tabs.TabCount() == categories.Size());
        for (usize i = 0; i < categories.Size(); ++i)
        {
            CHECK(dialog->TabIndexOf(categories[i].AsView()) == static_cast<i32>(i));
        }

        // Each labelled setting's label is on its category's tab.
        const auto labelsOn = [&tabs](i32 tab)
        {
            Array<ui::Label*> labels;
            CollectViews(*tabs.GetChildAt(static_cast<usize>(tab)), labels);
            return labels;
        };
        const auto hasLabel = [&labelsOn](i32 tab, StringView text)
        {
            for (ui::Label* label : labelsOn(tab))
            {
                if (label->Text.Value() == text)
                {
                    return true;
                }
            }
            return false;
        };
        for (const PropertyInfo& property : Properties(engine::project::ProjectSettings::StaticType()))
        {
            const String* label =
                engine::project::SettingAttribute(property, engine::project::kSettingLabelAttribute);
            if (label != nullptr)
            {
                CHECK(hasLabel(dialog->TabIndexOf(engine::project::SettingCategory(property)),
                               label->AsView()));
            }
        }
        CHECK(hasLabel(0, u8"Engine version"));
        CHECK(hasLabel(dialog->TabIndexOf(u8"Display"), u8"MSAA"));
        CHECK_FALSE(hasLabel(0, u8"MSAA"));
    }
    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}
