// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Particles - the `:page` partition (implementation; see ParticleEffectPage.cppm).
//
// The heavy engine imports (the particle component, the lights, the scene, the resources) live
// here, out of the interface, so importers of the page do not pay for them.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.particles;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.particles;
import foundation.particles.resource; // ParticleEffectResource, ResolveParticleEffectResources (the preview's drawing)
import foundation.resource;
import particles.pipeline;
import engine.particles;
import foundation.render; // the emission shape's lines
import engine.render;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace scene = foundation::scene;

    namespace
    {
        // The tree's model, which lets a system's row be renamed in place (F2, or Rename on its menu).
        class ParticleTreeModel final : public gui::TreeModel
        {
        public:
            ParticleTreeModel(Function<bool(i32 node)> editable, Function<bool(i32 node, StringView name)> rename)
                : m_editable(foundation::core::Move(editable)), m_rename(foundation::core::Move(rename))
            {
            }
            [[nodiscard]] bool IsEditable(const gui::ModelIndex& index) const override { return m_editable && m_editable(static_cast<i32>(index.InternalId)); }
            bool SetData(const gui::ModelIndex& index, const gui::Variant& value) override
            {
                const String name = value.ToString();
                return m_rename && !name.IsEmpty() && m_rename(static_cast<i32>(index.InternalId), name.AsView());
            }

        private:
            Function<bool(i32)> m_editable;
            Function<bool(i32, StringView)> m_rename;
        };

        // A module's name in the tree, which is its type's display name.
        [[nodiscard]] StringView ModuleName(const ISerializable* module)
        {
            if (module == nullptr || module->GetType() == nullptr)
                return StringView{};
            const TypeInfo& type = *module->GetType();
            return TypeAttrString(type, "displayName", StringView(reinterpret_cast<const char8_t*>(type.name)));
        }

        template <typename T>
        [[nodiscard]] bool Same(const Array<T>& a, const Array<T>& b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
                if (!(a[i] == b[i]))
                    return false;
            return true;
        }

        // The folder a list's row sits in (Initializers for an initializer).
        [[nodiscard]] ParticleRowKind FolderOf(ParticleRowKind kind)
        {
            switch (kind)
            {
            case ParticleRowKind::Initializer:
                return ParticleRowKind::Initializers;
            case ParticleRowKind::Behavior:
                return ParticleRowKind::Behaviors;
            case ParticleRowKind::SubEmitter:
                return ParticleRowKind::SubEmitters;
            default:
                return kind;
            }
        }

        // A sub-emitter as a line: what sets it off and where it spawns ("On death into Sparks").
        [[nodiscard]] String LinkName(particles::ParticleEffect& effect, const particles::SubEmitterLink& link);

        [[nodiscard]] String SystemName(const particles::ParticleSystem& system, usize index)
        {
            return system.name.IsEmpty() ? Format(u8"System {}", index + 1) : String(system.name.AsView());
        }

        String LinkName(particles::ParticleEffect& effect, const particles::SubEmitterLink& link)
        {
            const particles::ParticleSystem* child = link.childSystemIndex >= 0 ? effect.GetSystem(link.childSystemIndex) : nullptr;
            return Format(u8"{} into {}", link.trigger == particles::ParticleEventType::OnDeath ? StringView(u8"On death") : StringView(u8"On birth"),
                          child != nullptr ? SystemName(*child, static_cast<usize>(link.childSystemIndex)).AsView() : StringView(u8"nothing"));
        }

        // The place in the grid that a tree row names within its system (empty for the system itself).
        [[nodiscard]] String PlaceOf(const ParticleTreeRow& row)
        {
            switch (row.kind)
            {
            case ParticleRowKind::Emitter:
                return String(u8"emitter");
            case ParticleRowKind::Initializers:
                return String(u8"initializers");
            case ParticleRowKind::Behaviors:
                return String(u8"behaviors");
            case ParticleRowKind::Initializer:
                return Format(u8"initializers.{}", row.module);
            case ParticleRowKind::Behavior:
                return Format(u8"behaviors.{}", row.module);
            case ParticleRowKind::SubEmitters:
                return String(u8"subEmitters");
            case ParticleRowKind::SubEmitter:
                return Format(u8"subEmitters.{}", row.module);
            case ParticleRowKind::System:
                break;
            }
            return String{};
        }
    }

    ParticleEffectEditorPage::ParticleEffectEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::ParticleEffectAsset::StaticType(), kPath), m_format(context.Allocator())
    {
        particles::RegisterParticleModules(); // reflection for the modules and the systems
        HideAssetRows(); // the grid shows the chosen system's rows beside the tree, not the effect's
        Provide<IPlaybackPage>(*this);
        ParticleEffectEditorPage* self = this;

        m_model = MakeUnique<ParticleTreeModel>(
            Allocator(), [self](i32 node) { return node >= 0 && static_cast<usize>(node) < self->m_rows.Size() && self->m_rows[static_cast<usize>(node)].kind == ParticleRowKind::System; },
            [self](i32 node, StringView name)
            {
                return node >= 0 && static_cast<usize>(node) < self->m_rows.Size() && self->RenameSystem(self->m_rows[static_cast<usize>(node)].system, name);
            });
        m_tree = Find<gui::TreeView>(u8"particle-tree");
        if (auto* add = Find<gui::ButtonBase>(u8"particle-add-system"))
            add->SetOnClick([self]() { self->AddSystem(); });
        if (auto* remove = Find<gui::ButtonBase>(u8"particle-remove"))
            remove->SetOnClick([self]() { (void)self->RemoveChosen(); });
        if (m_tree != nullptr)
        {
            m_tree->SetModel(m_model.Get());
            m_tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                          {
                                              if (self->m_showing || !index.IsValid())
                                                  return;
                                              const usize node = static_cast<usize>(index.InternalId);
                                              if (node < self->m_rows.Size())
                                                  self->Choose(self->m_rows[node]);
                                          });
            // A row's context menu: what can be added, moved, renamed or removed there.
            (void)m_tree->AddEventListener(gui::EventType::ContextMenu, [self](const gui::Event& e)
                                           {
                                               const Float2 at = static_cast<const gui::MouseEvent&>(e).Position;
                                               const i32 item = self->m_tree->ItemAt(at);
                                               const usize node = item >= 0 ? static_cast<usize>(self->m_tree->GetItemIndex(item).InternalId) : self->m_rows.Size();
                                               if (node >= self->m_rows.Size())
                                                   return;
                                               self->ShowRowMenu(self->m_rows[node], *self->m_tree, at);
                                               e.Handled = true;
                                           });
        }

        // The chosen system's rows come from reflection, with the toolkit's editors for its curves
        // and gradients. Each edit is one undo step.
        m_values = MakeUnique<gui::ReflectedPropertySource>(Allocator(), Allocator());
        m_values->SetTextFormat(&m_format);
        m_values->RegisterCurveType(u8"ParticleCurveFloat", gui::ReflectedCurveParts{String(u8"keys")});
        m_values->RegisterGradientType(u8"ParticleCurveColor", gui::ReflectedGradientParts{String(u8"colors"), String(u8"alphas"), String(u8"interpolation")});
        m_values->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                {
                                    // A mesh's materials are named by slot; the first one also serves the
                                    // whole mesh.
                                    constexpr StringView kMaterials = u8"materialRefs.";
                                    if (path.StartsWith(kMaterials))
                                    {
                                        const Optional<i64> slot = ParseInt(path.SubStr(kMaterials.Size(), path.Size() - kMaterials.Size()));
                                        if (slot.HasValue())
                                            info.DisplayName = slot.Value() == 0 ? String(u8"Slot 1 (whole mesh)") : Format(u8"Slot {}", slot.Value() + 1);
                                        return;
                                    }
                                    // A sub-emitter is named by what sets it off and where it spawns. Its
                                    // target is a choice among the other systems by name, since the index
                                    // alone tells the user nothing.
                                    constexpr StringView kLinks = u8"subEmitters.";
                                    particles::ParticleEffect* effect = self->Effect();
                                    particles::ParticleSystem* system = effect != nullptr ? effect->GetSystem(self->m_system) : nullptr;
                                    if (system == nullptr || !path.StartsWith(kLinks))
                                        return;
                                    const StringView rest = path.SubStr(kLinks.Size(), path.Size() - kLinks.Size());
                                    usize dot = 0;
                                    while (dot < rest.Size() && rest[dot] != u8'.')
                                        ++dot;
                                    const Optional<i64> index = ParseInt(rest.SubStr(0, dot));
                                    if (!index.HasValue() || index.Value() < 0 || static_cast<usize>(index.Value()) >= system->subEmitters.Size())
                                        return;
                                    if (dot == rest.Size())
                                    {
                                        info.DisplayName = LinkName(*effect, system->subEmitters[static_cast<usize>(index.Value())]);
                                        return;
                                    }
                                    if (rest.SubStr(dot + 1, rest.Size() - dot - 1) != StringView(u8"childSystemIndex"))
                                        return;
                                    info.Kind = gui::PropertyKind::Enum;
                                    info.HasRange = false;
                                    info.Hint = String{};
                                    info.Choices.Clear();
                                    info.Choices.PushBack(gui::PropertyChoice{String(u8"Nothing"), -1});
                                    for (i32 s = 0; s < effect->SystemCount(); ++s)
                                        if (s != self->m_system) // never into itself
                                            info.Choices.PushBack(gui::PropertyChoice{SystemName(*effect->GetSystem(s), static_cast<usize>(s)), s});
                                });
        m_values->SetOnChange([self](const gui::ReflectedChange& change)
                              {
                                  const i32 system = self->m_system;
                                  (void)self->Commands().Execute(UniquePtr<IEditorCommand>(
                                      EditorRootAllocator().New<app::ReflectedEditCommand>(
                                          TypeOf<particles::ParticleSystem>(),
                                          [self, system]() -> void*
                                          {
                                              particles::ParticleEffect* effect = self->Effect();
                                              return effect != nullptr ? effect->GetSystem(system) : nullptr;
                                          },
                                          change, self->m_format, u8"edit_particle", [self]() { self->Changed(); }),
                                      EditorRootAllocator()));
                              });

        AddSection(kSystemSection, u8"System", *m_values);

        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"particle.preview");
            m_pane->OnFrame = [self]() { self->FramePreview(); };
            BindAction(Find<gui::ButtonBase>(u8"particle-restart"), u8"playback.restart");
            BindAction(Find<gui::ButtonBase>(u8"particle-play"), u8"playback.play");
            BindAction(Find<gui::ButtonBase>(u8"particle-stop"), u8"playback.stop");
            m_count = Find<gui::Label>(u8"particle-count");
            if (auto* speed = Find<gui::Slider>(u8"particle-speed"))
                speed->SetOnValueChanging([self](f64 value)
                                         {
                                             if (self->m_pane && self->m_pane->Viewport() != nullptr)
                                                 self->m_pane->Viewport()->SetTimeScale(static_cast<f32>(value));
                                         });
            BuildPreviewScene();
        }
        const particles::ParticleEffect* effect = Effect();
        Choose(ParticleTreeRow{ParticleRowKind::System, effect != nullptr && effect->SystemCount() > 0 ? 0 : -1, -1});
        FollowEffect();
        ShowFacts();
    }

    ParticleEffectEditorPage::~ParticleEffectEditorPage()
    {
        if (m_tree != nullptr)
            m_tree->SetModel(nullptr);
    }

    StringView ParticleEffectEditorPage::SystemPath(StringView gridPath)
    {
        const usize skip = kSystemSection.Size() + 1; // "system/"
        return gridPath.StartsWith(kSystemSection) && gridPath.Size() >= skip && gridPath[skip - 1] == u8'/' ? gridPath.SubStr(skip, gridPath.Size() - skip)
                                                                                                            : gridPath;
    }

    particles::ParticleEffect* ParticleEffectEditorPage::Effect() const
    {
        auto* asset = ObjectAs<pipeline::ParticleEffectAsset>();
        return asset != nullptr ? &asset->Effect() : nullptr;
    }

    gui::IPropertySource& ParticleEffectEditorPage::SystemRows() noexcept
    {
        return *m_values;
    }

    // === The tree and the rows ===

    void ParticleEffectEditorPage::BindSystem()
    {
        particles::ParticleEffect* effect = Effect();
        particles::ParticleSystem* system = effect != nullptr ? effect->GetSystem(m_system) : nullptr;
        if (system == nullptr)
        {
            m_values->SetObjects(Span<const Instance>{});
            return;
        }
        // The system's address is read again each time, because undoing a whole-asset edit makes the
        // systems anew.
        const Instance target(system, &TypeOf<particles::ParticleSystem>());
        m_values->SetObjects(Span<const Instance>(&target, 1u));
    }

    void ParticleEffectEditorPage::Choose(const ParticleTreeRow& row)
    {
        const particles::ParticleEffect* effect = Effect();
        m_system = effect != nullptr && row.system >= 0 && row.system < effect->SystemCount() ? row.system : -1;
        m_chosen = m_system >= 0 ? row : ParticleTreeRow{};
        BindSystem();
        // Open the place the row names (for a list's row, its list and then the row) and scroll it
        // into view.
        const String place = PlaceOf(m_chosen);
        if (gui::PropertyGrid* grid = Grid(); grid != nullptr && !place.IsEmpty())
        {
            if (m_chosen.module >= 0)
                grid->SetOpen(Sections().PathIn(kSystemSection, PlaceOf(ParticleTreeRow{FolderOf(m_chosen.kind), m_chosen.system, -1}).AsView()).AsView(), true);
            const String path = Sections().PathIn(kSystemSection, place.AsView());
            grid->SetOpen(path.AsView(), true);
            const i32 found = grid->FindRow(path.AsView());
            if (found >= 0)
                grid->ScrollToItem(found, gui::ScrollAlign::Start);
        }
        ShowTree();
    }

    String ParticleEffectEditorPage::Tree() const
    {
        String out;
        const particles::ParticleEffect* effect = Effect();
        for (const ParticleTreeRow& row : m_rows)
        {
            if (!out.IsEmpty())
                out += u8"\n";
            const particles::ParticleSystem* system = const_cast<particles::ParticleEffect*>(effect)->GetSystem(row.system);
            switch (row.kind)
            {
            case ParticleRowKind::System:
                out += SystemName(*system, static_cast<usize>(row.system)).AsView();
                break;
            case ParticleRowKind::Emitter:
                out += u8"  Emitter";
                break;
            case ParticleRowKind::Initializers:
                out += u8"  Initializers";
                break;
            case ParticleRowKind::Behaviors:
                out += u8"  Behaviors";
                break;
            case ParticleRowKind::Initializer:
                out += Format(u8"    {}", ModuleName(const_cast<particles::ParticleSystem*>(system)->GetInitializer(row.module))).AsView();
                break;
            case ParticleRowKind::Behavior:
                out += Format(u8"    {}", ModuleName(const_cast<particles::ParticleSystem*>(system)->GetBehavior(row.module))).AsView();
                break;
            case ParticleRowKind::SubEmitters:
                out += u8"  Sub-emitters";
                break;
            case ParticleRowKind::SubEmitter:
                out += Format(u8"    {}", LinkName(*const_cast<particles::ParticleEffect*>(effect), system->subEmitters[static_cast<usize>(row.module)])).AsView();
                break;
            }
        }
        return out;
    }

    void ParticleEffectEditorPage::ShowTree()
    {
        particles::ParticleEffect* effect = Effect();
        if (effect == nullptr)
            return;
        m_showing = true;
        // Rows the user folded stay folded when the tree is rebuilt after a choice or an edit; a row
        // new to the tree opens.
        Array<ParticleTreeRow> folded(Allocator());
        if (m_tree != nullptr)
            for (usize node = 0; node < m_rows.Size(); ++node)
                if (m_rows[node].module < 0 && !m_tree->IsIdExpanded(static_cast<gui::ItemId>(node)))
                    folded.PushBack(m_rows[node]);
        m_model->Clear();
        m_rows.Clear();
        i32 chosen = -1;
        const auto add = [&](i32 parent, StringView text, const ParticleTreeRow& row) -> i32
        {
            const i32 node = m_model->AddNode(parent, text);
            m_rows.PushBack(row);
            if (row == m_chosen)
                chosen = node;
            return node;
        };
        for (i32 s = 0; s < effect->SystemCount(); ++s)
        {
            particles::ParticleSystem& system = *effect->GetSystem(s);
            const i32 systemNode = add(gui::kTreeModelRoot, SystemName(system, static_cast<usize>(s)).AsView(), ParticleTreeRow{ParticleRowKind::System, s, -1});
            m_model->SetIcon(systemNode, u8"Editor/Icons/Types/ParticleEffectAsset.svg", u8"cat-rendering");
            const i32 emitter = add(systemNode, u8"Emitter", ParticleTreeRow{ParticleRowKind::Emitter, s, -1});
            m_model->SetIcon(emitter, u8"Editor/Icons/particles.svg");
            const i32 initializers = add(systemNode, u8"Initializers", ParticleTreeRow{ParticleRowKind::Initializers, s, -1});
            m_model->SetIcon(initializers, u8"Editor/Icons/folder.svg");
            for (i32 i = 0; i < system.InitializerCount(); ++i)
                (void)add(initializers, ModuleName(system.GetInitializer(i)), ParticleTreeRow{ParticleRowKind::Initializer, s, i});
            const i32 behaviors = add(systemNode, u8"Behaviors", ParticleTreeRow{ParticleRowKind::Behaviors, s, -1});
            m_model->SetIcon(behaviors, u8"Editor/Icons/folder.svg");
            for (i32 b = 0; b < system.BehaviorCount(); ++b)
                (void)add(behaviors, ModuleName(system.GetBehavior(b)), ParticleTreeRow{ParticleRowKind::Behavior, s, b});
            const i32 links = add(systemNode, u8"Sub-emitters", ParticleTreeRow{ParticleRowKind::SubEmitters, s, -1});
            m_model->SetIcon(links, u8"Editor/Icons/hierarchy.svg");
            for (usize l = 0; l < system.subEmitters.Size(); ++l)
                (void)add(links, LinkName(*effect, system.subEmitters[l]).AsView(), ParticleTreeRow{ParticleRowKind::SubEmitter, s, static_cast<i32>(l)});
        }
        if (m_tree != nullptr)
        {
            for (usize node = 0; node < m_rows.Size(); ++node)
                if (m_rows[node].module < 0)
                {
                    bool wasFolded = false;
                    for (const ParticleTreeRow& row : folded)
                        wasFolded = wasFolded || row == m_rows[node];
                    m_tree->SetIdExpanded(static_cast<gui::ItemId>(node), !wasFolded);
                }
            if (chosen >= 0)
            {
                const gui::ItemId id = static_cast<gui::ItemId>(chosen);
                m_tree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
            }
        }
        m_showing = false;
    }

    void ParticleEffectEditorPage::ShowFacts()
    {
        const particles::ParticleEffect* effect = Effect();
        if (effect == nullptr)
            return;
        i32 budget = 0;
        for (i32 s = 0; s < effect->SystemCount(); ++s)
            budget += const_cast<particles::ParticleEffect*>(effect)->GetSystem(s)->MaxParticles();
        SetFact(u8"Systems", Format(u8"{}", effect->SystemCount()).AsView());
        SetFact(u8"Particle Budget", Format(u8"{}", budget).AsView());
    }

    void ParticleEffectEditorPage::Changed()
    {
        // A rename, a module added or removed, a render mode or a link changed: refresh the tree, the
        // facts, the link names and the preview.
        m_values->Refresh();
        const particles::ParticleEffect* effect = Effect();
        if (effect != nullptr && m_chosen.module >= 0)
        {
            const particles::ParticleSystem* system = const_cast<particles::ParticleEffect*>(effect)->GetSystem(m_chosen.system);
            const i32 count = system == nullptr ? 0
                                                : (m_chosen.kind == ParticleRowKind::Initializer ? system->InitializerCount()
                                                   : m_chosen.kind == ParticleRowKind::Behavior  ? system->BehaviorCount()
                                                                                                  : static_cast<i32>(system->subEmitters.Size()));
            if (m_chosen.module >= count)
                m_chosen = ParticleTreeRow{FolderOf(m_chosen.kind), m_chosen.system, -1};
        }
        ShowTree();
        ShowFacts();
        FollowEffect();
    }

    void ParticleEffectEditorPage::OnObjectChanged()
    {
        // After a whole-asset edit or its undo, the chosen system may be gone or its address moved.
        // After the asset is read again (written outside the page), the effect itself is a new
        // object. The system's rows are bound first, because binding the effect's rows makes the grid
        // list every section, the system's included.
        particles::ParticleEffect* effect = Effect();
        const i32 count = effect != nullptr ? effect->SystemCount() : 0;
        if (m_system >= count)
            m_system = count - 1;
        if (m_chosen.system != m_system)
            m_chosen = ParticleTreeRow{ParticleRowKind::System, m_system, -1};
        BindSystem();
        Changed();
    }

    void ParticleEffectEditorPage::AddSystem()
    {
        if (Effect() == nullptr)
            return;
        EditWhole(u8"edit_particle", [](ISerializable& object) { (void)pipeline::AddDefaultParticleSystem(static_cast<pipeline::ParticleEffectAsset&>(object).Effect()); });
        Choose(ParticleTreeRow{ParticleRowKind::System, Effect()->SystemCount() - 1, -1});
    }

    bool ParticleEffectEditorPage::RemoveChosen()
    {
        particles::ParticleEffect* effect = Effect();
        if (effect == nullptr || m_chosen.system < 0 || m_chosen.system >= effect->SystemCount())
            return false;
        const ParticleTreeRow row = m_chosen;
        if (row.module >= 0)
        {
            // Remove through the rows, as the grid's own remove does, so the list records its own
            // undo step.
            gui::ListEdit edit;
            edit.Kind = gui::ListEdit::Op::Remove;
            edit.Index = static_cast<usize>(row.module);
            m_chosen = ParticleTreeRow{FolderOf(row.kind), row.system, -1};
            return m_values->EditList(PlaceOf(ParticleTreeRow{FolderOf(row.kind), row.system, -1}).AsView(), edit);
        }
        if (row.kind != ParticleRowKind::System || effect->SystemCount() < 2)
            return false; // an effect keeps at least one system; the emitter and lists are not removed here
        const i32 index = row.system;
        EditWhole(u8"edit_particle", [index](ISerializable& object) { static_cast<pipeline::ParticleEffectAsset&>(object).Effect().RemoveSystem(index); });
        Choose(ParticleTreeRow{ParticleRowKind::System, Min(index, Effect()->SystemCount() - 1), -1});
        return true;
    }

    bool ParticleEffectEditorPage::RenameSystem(i32 system, StringView name)
    {
        particles::ParticleEffect* effect = Effect();
        if (effect == nullptr || system < 0 || system >= effect->SystemCount() || name.IsEmpty())
            return false;
        Choose(ParticleTreeRow{ParticleRowKind::System, system, -1});
        m_values->BeginEdit(u8"name");
        m_values->CommitEdit(u8"name", gui::PropertyWrite{gui::Variant(name)}); // one undo step, like the Name row
        return true;
    }

    bool ParticleEffectEditorPage::BeginRename(i32 system)
    {
        if (m_tree == nullptr)
            return false;
        for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
        {
            const usize node = static_cast<usize>(m_tree->GetItemIndex(item).InternalId);
            if (node < m_rows.Size() && m_rows[node].kind == ParticleRowKind::System && m_rows[node].system == system)
                return m_tree->BeginEdit(item);
        }
        return false;
    }

    Array<String> ParticleEffectEditorPage::ModuleTypes(ParticleRowKind folder)
    {
        Array<String> types;
        if (folder != ParticleRowKind::Initializers && folder != ParticleRowKind::Behaviors)
            return types;
        const StringView list = folder == ParticleRowKind::Initializers ? StringView(u8"initializers") : StringView(u8"behaviors");
        Array<gui::PropertyInfo> rows(Allocator());
        m_values->GetProperties(StringView{}, rows);
        for (const gui::PropertyInfo& info : rows)
            if (info.Name.AsView() == list)
                types = info.ElementTypes;
        return types;
    }

    bool ParticleEffectEditorPage::AddModule(const ParticleTreeRow& folder, StringView type)
    {
        if (folder.kind != ParticleRowKind::Initializers && folder.kind != ParticleRowKind::Behaviors)
            return false;
        Choose(folder);
        const particles::ParticleSystem* system = Effect() != nullptr ? Effect()->GetSystem(folder.system) : nullptr;
        if (system == nullptr)
            return false;
        gui::ListEdit edit;
        edit.Kind = gui::ListEdit::Op::Insert;
        edit.Index = static_cast<usize>(folder.kind == ParticleRowKind::Initializers ? system->InitializerCount() : system->BehaviorCount());
        edit.ElementType = String(type);
        const ParticleRowKind kind = folder.kind == ParticleRowKind::Initializers ? ParticleRowKind::Initializer : ParticleRowKind::Behavior;
        const ParticleTreeRow made{kind, folder.system, static_cast<i32>(edit.Index)};
        if (!m_values->EditList(PlaceOf(folder).AsView(), edit))
            return false;
        Choose(made); // open the new module
        return true;
    }

    bool ParticleEffectEditorPage::MoveModule(const ParticleTreeRow& row, i32 by)
    {
        if (row.module < 0 || row.kind == ParticleRowKind::System)
            return false;
        const particles::ParticleSystem* system = Effect() != nullptr ? Effect()->GetSystem(row.system) : nullptr;
        if (system == nullptr)
            return false;
        const i32 count = row.kind == ParticleRowKind::Initializer ? system->InitializerCount()
                          : row.kind == ParticleRowKind::Behavior  ? system->BehaviorCount()
                                                                   : static_cast<i32>(system->subEmitters.Size());
        const i32 to = row.module + by;
        if (to < 0 || to >= count)
            return false;
        Choose(row);
        gui::ListEdit edit;
        edit.Kind = gui::ListEdit::Op::Move;
        edit.Index = static_cast<usize>(row.module);
        edit.To = static_cast<usize>(to);
        if (!m_values->EditList(PlaceOf(ParticleTreeRow{FolderOf(row.kind), row.system, -1}).AsView(), edit))
            return false;
        Choose(ParticleTreeRow{row.kind, row.system, to}); // it stays chosen at its new place
        return true;
    }

    void ParticleEffectEditorPage::ShowRowMenu(const ParticleTreeRow& row, gui::Node& owner, Float2 at)
    {
        ParticleEffectEditorPage* self = this;
        m_rowMenu = MakeRef<gui::Menu>(Allocator());
        const particles::ParticleEffect* effect = Effect();
        const i32 systems = effect != nullptr ? effect->SystemCount() : 0;
        switch (row.kind)
        {
        case ParticleRowKind::System:
            (void)m_rowMenu->AddItem(u8"Add System", [self]() { self->AddSystem(); });
            (void)m_rowMenu->AddItem(u8"Rename", [self, row]() { (void)self->BeginRename(row.system); })->SetShortcut(u8"F2");
            m_rowMenu->AddSeparator();
            m_rowMenu->AddItem(u8"Delete System", [self, row]()
                               {
                                   self->Choose(row);
                                   (void)self->RemoveChosen();
                               })
                ->SetEnabled(systems > 1); // an effect keeps at least one system
            break;
        case ParticleRowKind::Initializers:
        case ParticleRowKind::Behaviors:
            for (const String& type : ModuleTypes(row.kind))
                (void)m_rowMenu->AddItem(Format(u8"Add {}", type).AsView(), [self, row, type]() { (void)self->AddModule(row, type.AsView()); });
            break;
        case ParticleRowKind::Initializer:
        case ParticleRowKind::Behavior:
        case ParticleRowKind::SubEmitter:
        {
            (void)m_rowMenu->AddItem(u8"Move Up", [self, row]() { (void)self->MoveModule(row, -1); });
            (void)m_rowMenu->AddItem(u8"Move Down", [self, row]() { (void)self->MoveModule(row, 1); });
            m_rowMenu->AddSeparator();
            (void)m_rowMenu->AddItem(u8"Delete", [self, row]()
                                     {
                                         self->Choose(row);
                                         (void)self->RemoveChosen();
                                     });
            break;
        }
        case ParticleRowKind::Emitter:
        case ParticleRowKind::SubEmitters:
            return; // the emitter is not removable; links are added in the grid
        }
        m_rowMenu->Open(owner, at);
    }

    // === The preview ===

    void ParticleEffectEditorPage::BuildPreviewScene()
    {
        PreviewViewport* preview = m_pane ? m_pane->Viewport() : nullptr;
        scene::Scene* scenePtr = preview != nullptr ? preview->Scene() : nullptr;
        if (scenePtr == nullptr || Effect() == nullptr)
            return;
        preview->SetSimulationEnabled(true); // particles show only while simulating
        m_emitter = scenePtr->CreateEntity(u8"Emitter");
        if (auto* effects = scenePtr->GetSystem<engine::particles::ParticleEffectComponentManager>())
            (void)effects->Add(m_emitter); // FollowEffect gives it the effect
        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) * Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.type = engine::render::LightType::Directional;
            light.castsShadows = false;
        }
        FramePreview();
        m_pane->Show();
    }

    particles::ParticleEffectInstance* ParticleEffectEditorPage::PreviewInstance() const
    {
        PreviewViewport* preview = m_pane ? m_pane->Viewport() : nullptr;
        scene::Scene* scenePtr = preview != nullptr ? preview->Scene() : nullptr;
        auto* effects = scenePtr != nullptr ? scenePtr->GetSystem<engine::particles::ParticleEffectComponentManager>() : nullptr;
        engine::particles::ParticleEffectComponent* component = effects != nullptr ? effects->Get(m_emitter) : nullptr;
        return component != nullptr ? component->instance.Get() : nullptr;
    }

    void ParticleEffectEditorPage::FollowEffect()
    {
        particles::ParticleEffect* effect = Effect();
        if (effect == nullptr)
            return;
        // What the preview runs over (the systems and their modules) and what they draw with.
        Array<const void*> shape(Allocator());
        Array<Guid> bindings(Allocator());
        for (i32 s = 0; s < effect->SystemCount(); ++s)
        {
            particles::ParticleSystem* system = effect->GetSystem(s);
            shape.PushBack(system);
            for (i32 i = 0; i < system->InitializerCount(); ++i)
                shape.PushBack(system->GetInitializer(i));
            for (i32 b = 0; b < system->BehaviorCount(); ++b)
                shape.PushBack(system->GetBehavior(b));
            shape.PushBack(nullptr); // marks a system's end
            bindings.PushBack(system->textureRef);
            bindings.PushBack(system->meshRef);
            for (const Guid& material : system->materialRefs)
                bindings.PushBack(material);
            bindings.PushBack(Guid{0u, static_cast<u64>(system->materialRefs.Size())}); // marks a system's end
        }
        const bool reshaped = !Same(shape, m_shape);
        const bool rebound = reshaped || !Same(bindings, m_bindings);
        m_shape = Move(shape);
        m_bindings = Move(bindings);

        PreviewViewport* preview = m_pane ? m_pane->Viewport() : nullptr;
        scene::Scene* scenePtr = preview != nullptr ? preview->Scene() : nullptr;
        auto* effects = scenePtr != nullptr ? scenePtr->GetSystem<engine::particles::ParticleEffectComponentManager>() : nullptr;
        engine::particles::ParticleEffectComponent* component = effects != nullptr ? effects->Get(m_emitter) : nullptr;
        if (reshaped)
        {
            ++m_restarts;
            if (component != nullptr)
            {
                // Run over the authored effect itself, so an edited value shows at once. The
                // instance is new because the systems' modules are new; it keeps the transport's
                // state.
                component->SetEffect(*effect);
                if (component->instance)
                    component->instance->isActive = IsPlaying();
            }
        }
        if (rebound && component != nullptr)
            RebindPreviewResources();
    }

    void ParticleEffectEditorPage::RebindPreviewResources()
    {
        PreviewViewport* preview = m_pane ? m_pane->Viewport() : nullptr;
        scene::Scene* scenePtr = preview != nullptr ? preview->Scene() : nullptr;
        auto* effects = scenePtr != nullptr ? scenePtr->GetSystem<engine::particles::ParticleEffectComponentManager>() : nullptr;
        engine::particles::ParticleEffectComponent* component = effects != nullptr ? effects->Get(m_emitter) : nullptr;
        foundation::resource::ResourceManager* resources = Context().Resources();
        if (component == nullptr || Effect() == nullptr)
            return;
        if (resources == nullptr)
        {
            component->SetRenderResources(nullptr);
            m_previewResources = nullptr;
            return;
        }
        // Bind the effect's references as the cook's factory binds them, but only for drawing; the
        // simulation keeps running the authored effect.
        auto bound = MakeRef<particles::ParticleEffectResource>(Allocator());
        particles::CloneEffect(*Effect(), bound->Effect());
        particles::ResolveParticleEffectResources(*bound, *resources);
        component->SetRenderResources(bound.Get());
        m_previewResources = bound; // the component borrows it
    }

    void ParticleEffectEditorPage::FramePreview()
    {
        if (!m_pane || m_pane->Viewport() == nullptr)
            return;
        PreviewViewport* preview = m_pane->Viewport();
        preview->Camera().position = Float3{0.0f, 2.0f, 6.0f};
        preview->Camera().LookAt(Float3{0.0f, 1.0f, 0.0f});
    }

    void ParticleEffectEditorPage::DrawEmissionShape()
    {
        PreviewViewport* preview = m_pane ? m_pane->Viewport() : nullptr;
        particles::ParticleEffect* effect = Effect();
        particles::ParticleSystem* system = effect != nullptr ? effect->GetSystem(m_system) : nullptr;
        if (preview == nullptr || !preview->IsValid() || system == nullptr)
            return;
        // Where the chosen system spawns: its first position initializer's shape.
        const particles::EmissionShape* shape = nullptr;
        for (i32 i = 0; i < system->InitializerCount() && shape == nullptr; ++i)
            if (auto* position = Cast<particles::PositionInitializer>(system->GetInitializer(i)))
                shape = &position->shape;
        if (shape == nullptr)
            return;
        auto& lines = preview->SceneDebugDraw();
        const Color color{0.30f, 0.85f, 1.0f, 1.0f};
        const Float3 at = system->position;
        const Float3 up{0.0f, 1.0f, 0.0f};
        const f32 radius = Max(shape->radius, 0.01f);
        switch (shape->type)
        {
        case particles::EmissionShapeType::Sphere:
        case particles::EmissionShapeType::Hemisphere:
            lines.DrawWireSphere(at, radius, color);
            break;
        case particles::EmissionShapeType::Box:
            lines.DrawWireBoxCenter(at, shape->extents, color);
            break;
        case particles::EmissionShapeType::Cone:
            lines.DrawCone(at, up, Max(shape->radius, 0.5f), Max(shape->angle, 0.01f), color);
            break;
        case particles::EmissionShapeType::Ring:
        case particles::EmissionShapeType::Circle:
            lines.DrawCircleNormal(at, radius, up, color);
            break;
        case particles::EmissionShapeType::Edge:
            lines.DrawLine(at - Float3{radius, 0.0f, 0.0f}, at + Float3{radius, 0.0f, 0.0f}, color);
            break;
        case particles::EmissionShapeType::Point:
        default:
            lines.DrawCross(at, 0.15f, color);
            break;
        }
    }

    void ParticleEffectEditorPage::ShowTransport()
    {
        if (m_count == nullptr)
            return;
        particles::ParticleEffect* effect = Effect();
        i32 alive = 0;
        i32 most = 0;
        for (i32 s = 0; effect != nullptr && s < effect->SystemCount(); ++s)
        {
            alive += effect->GetSystem(s)->AliveCount();
            most += effect->GetSystem(s)->MaxParticles();
        }
        const StringView state = m_stopped ? StringView(u8"stopped") : (m_paused ? StringView(u8"paused") : StringView(u8"playing"));
        // The stylesheet fixes its width. Paused is shown by colour, as the Play button shows it by its
        // icon, because a running count beside a word would grow and shrink and move what follows it.
        const String text = m_stopped ? String(u8"Stopped") : Format(u8"{} of {}", alive, most);
        if (m_count->GetText() != text.AsView())
            m_count->SetText(text.AsView());
        constexpr StringView kStates[] = {u8"stopped", u8"paused", u8"playing"};
        for (const StringView name : kStates)
            if (name == state)
                m_count->AddClass(name);
            else
                m_count->RemoveClass(name);
    }

    // === The transport: it drives the preview only and never changes the effect ===

    bool ParticleEffectEditorPage::CanPlay() const
    {
        return PreviewInstance() != nullptr;
    }

    void ParticleEffectEditorPage::Play()
    {
        particles::ParticleEffectInstance* instance = PreviewInstance();
        if (instance == nullptr)
            return;
        if (m_stopped)
            instance->Reset(); // from the start
        m_paused = false;
        m_stopped = false;
        instance->isActive = true;
    }

    void ParticleEffectEditorPage::Pause()
    {
        particles::ParticleEffectInstance* instance = PreviewInstance();
        if (instance == nullptr || m_stopped)
            return;
        m_paused = true;
        instance->isActive = false;
    }

    void ParticleEffectEditorPage::Stop()
    {
        particles::ParticleEffectInstance* instance = PreviewInstance();
        if (instance == nullptr)
            return;
        instance->Reset(); // its particles gone
        instance->isActive = false;
        m_paused = false;
        m_stopped = true;
    }

    void ParticleEffectEditorPage::Restart()
    {
        particles::ParticleEffectInstance* instance = PreviewInstance();
        if (instance == nullptr)
            return;
        instance->Reset();
        m_paused = false;
        m_stopped = false;
        instance->isActive = true;
    }

    void ParticleEffectEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        ShowTransport();
        DrawEmissionShape();
        if (m_pane)
            m_pane->Update(dt);
    }

    void ParticleEffectEditorPage::OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void ParticleEffectEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    const TypeInfo* ParticleEffectPageFactory::PrimaryType() const
    {
        return &pipeline::ParticleEffectAsset::StaticType();
    }

    UniquePtr<EditorPage> ParticleEffectPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<ParticleEffectEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterParticleEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(editor::EditorRootAllocator().New<ParticleEffectPageFactory>(services), editor::EditorRootAllocator()));
    }
}
