// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Particles - :page partition (implementation).
//
// The three-pane ParticleEffect authoring tool (see ParticleEffectPage.cppm for the overview).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.particles;

import foundation.core;
import foundation.fonts; // VerticalAlignment (the labels)
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.particles;
import foundation.particles.resource; // ParticleEffectResource + ResolveParticleEffectResources (preview)
import foundation.resource;           // ResourceManager (resolve the preview's mesh/material refs)
import particles.pipeline;
import engine.particles;
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;
namespace particles = foundation::particles;
namespace resource = foundation::resource;
namespace render = foundation::render;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace ui = foundation::ui;
namespace vg = foundation::vg;
namespace fonts = foundation::fonts;

namespace editor
{
    namespace
    {

        // The owning page; captured (as a copyable pointer) into stored editor callbacks. An edit
        // mutates the target field in place, then CommitEdit(key) records a coalesced undo step.
        using Page = ParticleEffectEditorPage*;

        // A tree row IS an EditableLabel that remembers the node it is currently bound to (set in
        // Bind, refreshed on every recycle) - the same pattern the scene HierarchyView uses, so
        // in-place rename works on virtualized rows: the commit handler reads the row's live
        // identity. Only System rows are editable (double/slow-click); other kinds are read-only.
        class ParticleTreeRow final : public ui::EditableLabel
        {
        public:
            void Bind(StringView label, f32 textInset, ParticleNodeKind kind, i32 systemIndex)
            {
                m_kind = kind;
                m_systemIndex = systemIndex;
                SetText(label);
                // Indent past the expander-chevron column; the caller derives textInset from
                // TreeView::ContentInset(depth) so it can never drift from the tree's IndentWidth.
                TextOffsetX.SetValue(textInset);
                const bool renamable = (kind == ParticleNodeKind::System);
                DoubleClickToEdit.SetValue(renamable);
                SlowClickToEdit.SetValue(renamable);
            }
            [[nodiscard]] ParticleNodeKind Kind() const noexcept { return m_kind; }
            [[nodiscard]] i32 SystemIndex() const noexcept { return m_systemIndex; }

        private:
            ParticleNodeKind m_kind = ParticleNodeKind::Effect;
            i32 m_systemIndex = -1;
        };

        void Add(ui::toolkit::PropertyGrid& g, RefPtr<ui::toolkit::PropertyEditor> e)
        {
            g.AddProperty(Move(e));
        }

        // ---- scalar rows (each mutates in place + commits an undo step keyed by category+name) ----

        void RowFloat(ui::toolkit::PropertyGrid& g, StringView name, f32* field, StringView cat,
                      Page page, f64 mn = -1e9, f64 mx = 1e9, f64 step = 0.05)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::FloatEditor>(
                           g.MemoryAllocator(), name, static_cast<f64>(*field), mn, mx, step, 3,
                           Function<void(f64)>{[field, page, key](f64 v)
                                               {
                                                   *field = static_cast<f32>(v);
                                                   page->CommitEdit(key.AsView());
                                               }},
                           cat)
                           .Get()));
        }
        void RowInt(ui::toolkit::PropertyGrid& g, StringView name, i32* field, StringView cat,
                    Page page, i64 mn = 0, i64 mx = 1000000)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::IntEditor>(
                           g.MemoryAllocator(), name, static_cast<i64>(*field), mn, mx,
                           Function<void(i64)>{[field, page, key](i64 v)
                                               {
                                                   *field = static_cast<i32>(v);
                                                   page->CommitEdit(key.AsView());
                                               }},
                           cat)
                           .Get()));
        }
        void RowBool(ui::toolkit::PropertyGrid& g, StringView name, bool* field, StringView cat,
                     Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::BoolEditor>(
                           g.MemoryAllocator(), name, *field,
                           Function<void(bool)>{[field, page, key](bool v)
                                                {
                                                    *field = v;
                                                    page->CommitEdit(key.AsView());
                                                }},
                           cat)
                           .Get()));
        }
        void RowFloat2(ui::toolkit::PropertyGrid& g, StringView name, Float2* field, StringView cat,
                       Page page, f32 mn = -100000.0f, f32 mx = 100000.0f, f32 step = 0.01f)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::Float2Editor>(
                           g.MemoryAllocator(), name, *field, mn, mx, step,
                           Function<void(Float2)>{[field, page, key](Float2 v)
                                                  {
                                                      *field = v;
                                                      page->CommitEdit(key.AsView());
                                                  }},
                           cat)
                           .Get()));
        }
        void RowFloat3(ui::toolkit::PropertyGrid& g, StringView name, Float3* field, StringView cat,
                       Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::Float3Editor>(
                           g.MemoryAllocator(), name, *field, -1000.0f, 1000.0f, 0.05f,
                           Function<void(Float3)>{[field, page, key](Float3 v)
                                                  {
                                                      *field = v;
                                                      page->CommitEdit(key.AsView());
                                                  }},
                           cat)
                           .Get()));
        }
        void RowColor(ui::toolkit::PropertyGrid& g, StringView name, Float4* field, StringView cat,
                      Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::ColorEditor>(
                           g.MemoryAllocator(), name, Color{field->x, field->y, field->z, field->w},
                           Function<void(Color)>{[field, page, key](Color c)
                                                 {
                                                     *field = Float4{c.r, c.g, c.b, c.a};
                                                     page->CommitEdit(key.AsView());
                                                 }},
                           cat)
                           .Get()));
        }
        void RowEnum(ui::toolkit::PropertyGrid& g, StringView name, i32 value,
                     Span<const StringView> items, Function<void(i32)> setter, StringView cat)
        {
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::EnumEditor>(g.MemoryAllocator(), name, value, items,
                                                        Move(setter), cat)
                           .Get()));
        }

        // ---- composite value-type rows ----------------------------------------------------------

        void RowRangeFloat(ui::toolkit::PropertyGrid& g, StringView label, particles::RangeFloat* r,
                           StringView cat, Page page, f64 mn = -1e6, f64 mx = 1e6, f64 step = 0.02)
        {
            String lo(label), hi(label);
            lo.Append(u8" min");
            hi.Append(u8" max");
            RowFloat(g, lo.AsView(), &r->min, cat, page, mn, mx, step);
            RowFloat(g, hi.AsView(), &r->max, cat, page, mn, mx, step);
        }
        // Full 4-component RangeFloat2 (min.x/min.y and max.x/max.y - NOT a square approximation).
        void RowRangeFloat2(ui::toolkit::PropertyGrid& g, StringView label,
                            particles::RangeFloat2* r, StringView cat, Page page)
        {
            String lo(label), hi(label);
            lo.Append(u8" min");
            hi.Append(u8" max");
            RowFloat2(g, lo.AsView(), &r->min, cat, page, 0.0f, 1000.0f, 0.01f);
            RowFloat2(g, hi.AsView(), &r->max, cat, page, 0.0f, 1000.0f, 0.01f);
        }
        void RowRangeColor(ui::toolkit::PropertyGrid& g, StringView label, particles::RangeColor* r,
                           StringView cat, Page page)
        {
            String lo(label), hi(label);
            lo.Append(u8" start");
            hi.Append(u8" end");
            RowColor(g, lo.AsView(), &r->min, cat, page);
            RowColor(g, hi.AsView(), &r->max, cat, page);
        }

        // Emission shape sub-form: type + the params it uses (radius/extents/angle/arc/from-shell).
        void RowEmissionShape(ui::toolkit::PropertyGrid& g, StringView label,
                              particles::EmissionShape* sh, StringView cat, Page page)
        {
            static constexpr StringView kShapes[] = {u8"Point",  u8"Sphere", u8"Hemisphere",
                                                     u8"Box",    u8"Cone",   u8"Ring",
                                                     u8"Circle", u8"Edge"};
            String key(cat);
            key.Append(label);
            String shapeLabel(label);
            shapeLabel.Append(u8" shape");
            RowEnum(g, shapeLabel.AsView(), static_cast<i32>(sh->type),
                    Span<const StringView>{kShapes, 8},
                    Function<void(i32)>{[sh, page, key](i32 v)
                                        {
                                            sh->type = static_cast<particles::EmissionShapeType>(v);
                                            page->CommitEdit(key.AsView());
                                        }},
                    cat);
            String r(label), ex(label), an(label), ar(label), fs(label);
            r.Append(u8" radius");
            ex.Append(u8" box extents");
            an.Append(u8" cone angle");
            ar.Append(u8" arc");
            fs.Append(u8" from shell");
            RowFloat(g, r.AsView(), &sh->radius, cat, page, 0.0, 100.0, 0.05);
            RowFloat3(g, ex.AsView(), &sh->extents, cat, page);
            RowFloat(g, an.AsView(), &sh->angle, cat, page, 0.0, 3.1416, 0.01);
            RowFloat(g, ar.AsView(), &sh->arc, cat, page, 0.0, 1.0, 0.01);
            RowBool(g, fs.AsView(), &sh->emitFromShell, cat, page);
        }

        // ---- CurveCanvas-backed float curve editor (1 or 2 channels) ----------------------------
        // Hosts an interactive CurveCanvas as its editor view; edits write every key (value +
        // Hermite tangents) back into the ParticleCurveFloat / ParticleCurveFloat2 and commit undo.

        class CurveFieldEditor final : public ui::toolkit::PropertyEditor
        {
        public:
            // Single-channel (ParticleCurveFloat: Alpha / Rotation / Speed).
            CurveFieldEditor(StringView name, particles::ParticleCurveFloat* curve, Page page,
                             StringView key, StringView cat)
                : ui::toolkit::PropertyEditor(name, cat), m_curve1(curve), m_page(page), m_key(key)
            {
            }
            // Two-channel (ParticleCurveFloat2: Size X/Y, shared time).
            CurveFieldEditor(StringView name, particles::ParticleCurveFloat2* curve, Page page,
                             StringView key, StringView cat)
                : ui::toolkit::PropertyEditor(name, cat), m_curve2(curve), m_page(page), m_key(key)
            {
            }

            void RefreshView() override {}

        protected:
            RefPtr<ui::View> CreateEditorView() override
            {
                auto canvas = MakeRef<ui::toolkit::CurveCanvas>(MemoryAllocator());
                canvas->MaxKeys = particles::kMaxCurveKeys;
                canvas->AutoFitValueRange = true;
                m_canvas = canvas.Get();

                if (m_curve1 != nullptr)
                {
                    ui::toolkit::ChannelDescriptor ch;
                    ch.Name = String(u8"V");
                    ch.StrokeColor = Color{0.45f, 0.75f, 1.0f, 1.0f};
                    canvas->SetChannels(Span<const ui::toolkit::ChannelDescriptor>{&ch, 1});
                    PushFloat1(*canvas);
                }
                else if (m_curve2 != nullptr)
                {
                    canvas->LinkedTime = true;
                    ui::toolkit::ChannelDescriptor chs[2];
                    chs[0].Name = String(u8"X");
                    chs[0].StrokeColor = Color{0.9f, 0.4f, 0.4f, 1.0f};
                    chs[1].Name = String(u8"Y");
                    chs[1].StrokeColor = Color{0.4f, 0.9f, 0.5f, 1.0f};
                    canvas->SetChannels(Span<const ui::toolkit::ChannelDescriptor>{chs, 2});
                    PushFloat2(*canvas);
                }

                CurveFieldEditor* self = this;
                canvas->OnEditEnd.Add([self]() { self->WriteBack(); });
                canvas->OnKeyChanged.Add([self](i32, i32) { self->WriteBack(); });
                canvas->OnKeyAdded.Add([self](i32, i32) { self->WriteBack(); });
                canvas->OnKeyRemoved.Add([self](i32, i32) { self->WriteBack(); });

                // Give the canvas a fixed height (property rows otherwise collapse to text height),
                // via a wrapper with a Px LayoutParams - the same idiom InputMapPage uses.
                auto wrap = MakeRef<ui::FlexLayout>(MemoryAllocator());
                wrap->Direction = ui::Orientation::Vertical;
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(120.0f));
                wrap->AddView(canvas.Get(), lp);
                return wrap;
            }

        private:
            void PushFloat1(ui::toolkit::CurveCanvas& c)
            {
                Array<ui::toolkit::CurveCanvas::Key> keys;
                for (i32 i = 0; i < m_curve1->keyCount; ++i)
                {
                    const particles::CurveKeyFloat& k = m_curve1->keys[i];
                    keys.PushBack(
                        ui::toolkit::CurveCanvas::Key{k.time, k.value, k.tangentIn, k.tangentOut});
                }
                if (keys.IsEmpty())
                {
                    keys.PushBack(ui::toolkit::CurveCanvas::Key{0.0f, 0.0f});
                    keys.PushBack(ui::toolkit::CurveCanvas::Key{1.0f, 1.0f});
                }
                c.SetKeys(0, Span<const ui::toolkit::CurveCanvas::Key>{keys.Data(), keys.Size()});
            }
            void PushFloat2(ui::toolkit::CurveCanvas& c)
            {
                Array<ui::toolkit::CurveCanvas::Key> kx, ky;
                for (i32 i = 0; i < m_curve2->keyCount; ++i)
                {
                    kx.PushBack(ui::toolkit::CurveCanvas::Key{
                        m_curve2->times[i], m_curve2->values[i].x, m_curve2->tangentsIn[i].x,
                        m_curve2->tangentsOut[i].x});
                    ky.PushBack(ui::toolkit::CurveCanvas::Key{
                        m_curve2->times[i], m_curve2->values[i].y, m_curve2->tangentsIn[i].y,
                        m_curve2->tangentsOut[i].y});
                }
                if (kx.IsEmpty())
                {
                    kx.PushBack(ui::toolkit::CurveCanvas::Key{0.0f, 0.1f});
                    kx.PushBack(ui::toolkit::CurveCanvas::Key{1.0f, 0.1f});
                    ky.PushBack(ui::toolkit::CurveCanvas::Key{0.0f, 0.1f});
                    ky.PushBack(ui::toolkit::CurveCanvas::Key{1.0f, 0.1f});
                }
                c.SetKeys(0, Span<const ui::toolkit::CurveCanvas::Key>{kx.Data(), kx.Size()});
                c.SetKeys(1, Span<const ui::toolkit::CurveCanvas::Key>{ky.Data(), ky.Size()});
            }
            void WriteBack()
            {
                if (m_canvas == nullptr)
                {
                    return;
                }
                if (m_curve1 != nullptr)
                {
                    const i32 n = Min(m_canvas->GetKeyCount(0), particles::kMaxCurveKeys);
                    m_curve1->keyCount = n;
                    for (i32 i = 0; i < n; ++i)
                    {
                        const ui::toolkit::CurveCanvas::Key k = m_canvas->GetKey(0, i);
                        m_curve1->keys[i] =
                            particles::CurveKeyFloat{k.Time, k.Value, k.TangentIn, k.TangentOut};
                    }
                }
                else if (m_curve2 != nullptr)
                {
                    const i32 n = Min(Min(m_canvas->GetKeyCount(0), m_canvas->GetKeyCount(1)),
                                      particles::kMaxCurveKeys);
                    m_curve2->keyCount = n;
                    for (i32 i = 0; i < n; ++i)
                    {
                        const ui::toolkit::CurveCanvas::Key kx = m_canvas->GetKey(0, i);
                        const ui::toolkit::CurveCanvas::Key ky = m_canvas->GetKey(1, i);
                        m_curve2->times[i] = kx.Time;
                        m_curve2->values[i] = Float2{kx.Value, ky.Value};
                        m_curve2->tangentsIn[i] = Float2{kx.TangentIn, ky.TangentIn};
                        m_curve2->tangentsOut[i] = Float2{kx.TangentOut, ky.TangentOut};
                    }
                }
                m_page->CommitEdit(m_key.AsView());
            }

            particles::ParticleCurveFloat* m_curve1 = nullptr;
            particles::ParticleCurveFloat2* m_curve2 = nullptr;
            Page m_page;
            String m_key;
            ui::toolkit::CurveCanvas* m_canvas = nullptr;
        };

        // ---- GradientEditor-backed color-over-lifetime editor -----------------------------------

        class GradientFieldEditor final : public ui::toolkit::PropertyEditor
        {
        public:
            GradientFieldEditor(StringView name, particles::ParticleCurveColor* curve, Page page,
                                StringView key, StringView cat)
                : ui::toolkit::PropertyEditor(name, cat), m_curve(curve), m_page(page), m_key(key)
            {
            }
            void RefreshView() override {}

        protected:
            RefPtr<ui::View> CreateEditorView() override
            {
                auto grad = MakeRef<ui::toolkit::GradientEditor>(MemoryAllocator());
                grad->MaxStops = particles::kMaxCurveKeys;
                m_grad = grad.Get();

                Array<ui::toolkit::GradientEditor::Stop> stops;
                for (i32 i = 0; i < m_curve->keyCount; ++i)
                {
                    stops.PushBack(ui::toolkit::GradientEditor::Stop{m_curve->keys[i].time,
                                                                     m_curve->keys[i].color});
                }
                if (stops.IsEmpty())
                {
                    stops.PushBack(
                        ui::toolkit::GradientEditor::Stop{0.0f, Float4{1.0f, 1.0f, 1.0f, 1.0f}});
                    stops.PushBack(
                        ui::toolkit::GradientEditor::Stop{1.0f, Float4{1.0f, 1.0f, 1.0f, 0.0f}});
                }
                grad->SetStops(
                    Span<const ui::toolkit::GradientEditor::Stop>{stops.Data(), stops.Size()});

                GradientFieldEditor* self = this;
                grad->OnEditEnd.Add([self]() { self->WriteBack(); });
                grad->OnStopChanged.Add([self](i32) { self->WriteBack(); });
                grad->OnStopAdded.Add([self](i32) { self->WriteBack(); });
                grad->OnStopRemoved.Add([self](i32) { self->WriteBack(); });

                auto wrap = MakeRef<ui::FlexLayout>(MemoryAllocator());
                wrap->Direction = ui::Orientation::Vertical;
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(40.0f));
                wrap->AddView(grad.Get(), lp);
                return wrap;
            }

        private:
            void WriteBack()
            {
                if (m_grad == nullptr)
                {
                    return;
                }
                const i32 n = Min(m_grad->StopCount(), particles::kMaxCurveKeys);
                m_curve->keyCount = n;
                for (i32 i = 0; i < n; ++i)
                {
                    const ui::toolkit::GradientEditor::Stop s = m_grad->GetStop(i);
                    m_curve->keys[i] = particles::CurveKeyColor{s.Time, s.Color};
                }
                m_page->CommitEdit(m_key.AsView());
            }

            particles::ParticleCurveColor* m_curve;
            Page m_page;
            String m_key;
            ui::toolkit::GradientEditor* m_grad = nullptr;
        };

        void RowCurveFloat(ui::toolkit::PropertyGrid& g, StringView name,
                           particles::ParticleCurveFloat* c, StringView cat, Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g,
                RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<CurveFieldEditor>(g.MemoryAllocator(), name, c, page, key.AsView(), cat)
                        .Get()));
        }
        void RowCurveFloat2(ui::toolkit::PropertyGrid& g, StringView name,
                            particles::ParticleCurveFloat2* c, StringView cat, Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g,
                RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<CurveFieldEditor>(g.MemoryAllocator(), name, c, page, key.AsView(), cat)
                        .Get()));
        }
        void RowCurveColor(ui::toolkit::PropertyGrid& g, StringView name,
                           particles::ParticleCurveColor* c, StringView cat, Page page)
        {
            String key(cat);
            key.Append(name);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<GradientFieldEditor>(g.MemoryAllocator(), name, c, page, key.AsView(),
                                                    cat)
                           .Get()));
        }
    } // namespace

    // ============================ Tree adapter ================================================

    i32 ParticleTreeAdapter::RootCount() const { return static_cast<i32>(m_owner->m_roots.Size()); }
    i32 ParticleTreeAdapter::GetChildCount(i32 nodeId) const
    {
        if (nodeId == -1)
        {
            return RootCount();
        }
        if (nodeId < 0 || nodeId >= static_cast<i32>(m_owner->m_nodes.Size()))
        {
            return 0;
        }
        return static_cast<i32>(m_owner->m_nodes[static_cast<usize>(nodeId)].children.Size());
    }
    i32 ParticleTreeAdapter::GetChildId(i32 parentId, i32 childIndex) const
    {
        if (parentId == -1)
        {
            return (childIndex >= 0 && childIndex < RootCount())
                       ? m_owner->m_roots[static_cast<usize>(childIndex)]
                       : -1;
        }
        if (parentId < 0 || parentId >= static_cast<i32>(m_owner->m_nodes.Size()))
        {
            return -1;
        }
        const Array<i32>& kids = m_owner->m_nodes[static_cast<usize>(parentId)].children;
        return (childIndex >= 0 && childIndex < static_cast<i32>(kids.Size()))
                   ? kids[static_cast<usize>(childIndex)]
                   : -1;
    }
    i32 ParticleTreeAdapter::GetDepth(i32 nodeId) const
    {
        return (nodeId >= 0 && nodeId < static_cast<i32>(m_owner->m_nodes.Size()))
                   ? m_owner->m_nodes[static_cast<usize>(nodeId)].depth
                   : 0;
    }
    bool ParticleTreeAdapter::HasChildren(i32 nodeId) const { return GetChildCount(nodeId) > 0; }
    RefPtr<ui::View> ParticleTreeAdapter::CreateView(i32)
    {
        // The row is a ParticleTreeRow (EditableLabel that remembers its bound node). System rows
        // rename in place: the commit handler reads the row's LIVE identity, so recycling is safe.
        auto row = MakeRef<ParticleTreeRow>(m_owner->Allocator());
        row->FontSize.SetValue(Optional<f32>{12.0f});
        row->Ellipsis.SetValue(true);
        ParticleEffectEditorPage* owner = m_owner;
        ParticleTreeRow* raw = row.Get();
        row->OnRenameCommitted.Add(
            [owner, raw](ui::EditableLabel*, StringView newName)
            {
                if (raw->Kind() != ParticleNodeKind::System)
                {
                    return;
                }
                if (particles::ParticleSystem* s =
                        owner->m_asset->Effect().GetSystem(raw->SystemIndex()))
                {
                    s->name = String(newName);
                    owner->CommitEdit(u8"rename-system");
                    owner->RebuildTree();
                }
            });
        return RefPtr<ui::View>(row.Get());
    }
    void ParticleTreeAdapter::BindView(ui::View* view, i32 nodeId, i32 depth, bool)
    {
        if (nodeId < 0 || nodeId >= static_cast<i32>(m_owner->m_nodes.Size()))
        {
            return;
        }
        const ParticleTreeNode& node = m_owner->m_nodes[static_cast<usize>(nodeId)];
        static_cast<ParticleTreeRow*>(view)->Bind(
            node.label.AsView(), m_owner->m_tree->ContentInset(depth), node.kind, node.systemIndex);
    }
    bool ParticleTreeAdapter::CanMove(i32 fromPosition, i32 toPosition)
    {
        // Reorder is only meaningful within the same folder (initializers among initializers,
        // behaviors among behaviors). Positions are flat list positions -> resolve to node refs.
        ui::FlattenedTreeAdapter* flat = m_owner->m_tree->InternalTreeView()->FlatAdapter();
        if (flat == nullptr)
        {
            return false;
        }
        const i32 fromNode = flat->GetNodeId(fromPosition);
        const i32 toNode = flat->GetNodeId(toPosition);
        if (fromNode < 0 || toNode < 0)
        {
            return false;
        }
        const ParticleTreeNode& a = m_owner->m_nodes[static_cast<usize>(fromNode)];
        const ParticleTreeNode& b = m_owner->m_nodes[static_cast<usize>(toNode)];
        return a.kind == b.kind && a.systemIndex == b.systemIndex &&
               (a.kind == ParticleNodeKind::Initializer || a.kind == ParticleNodeKind::Behavior);
    }
    void ParticleTreeAdapter::MoveItem(i32 fromPosition, i32 toPosition)
    {
        ui::FlattenedTreeAdapter* flat = m_owner->m_tree->InternalTreeView()->FlatAdapter();
        if (flat == nullptr)
        {
            return;
        }
        const i32 fromNode = flat->GetNodeId(fromPosition);
        const i32 toNode = flat->GetNodeId(toPosition);
        if (fromNode < 0 || toNode < 0)
        {
            return;
        }
        const ParticleTreeNode a = m_owner->m_nodes[static_cast<usize>(fromNode)];
        const ParticleTreeNode b = m_owner->m_nodes[static_cast<usize>(toNode)];
        if (a.kind != b.kind || a.systemIndex != b.systemIndex)
        {
            return;
        }
        const i32 sysIndex = a.systemIndex;
        const i32 from = a.moduleIndex;
        const i32 to = b.moduleIndex;
        const bool init = (a.kind == ParticleNodeKind::Initializer);
        ParticleEffectEditorPage* self = m_owner;
        m_owner->QueueStructural(
            u8"reorder",
            Function<void()>{[self, sysIndex, from, to, init]()
                             {
                                 if (particles::ParticleSystem* s =
                                         self->m_asset->Effect().GetSystem(sysIndex))
                                 {
                                     if (init)
                                     {
                                         s->MoveInitializer(from, to);
                                     }
                                     else
                                     {
                                         s->MoveBehavior(from, to);
                                     }
                                 }
                             }},
            ParticleNodeRef{a.kind, sysIndex, to});
    }

    // ============================ Page: construction ==========================================

    ParticleEffectEditorPage::ParticleEffectEditorPage(EditorContext& context,
                                                       runtime::IApplicationHost& host,
                                                       ui::runtime::UIHost& uiHost,
                                                       foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        // Shared preview substrate (viewport + preview scene + orbit camera + render loop).
        m_preview =
            MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"particle.preview");
        m_preview->SetClearColor(Color{0.272f, 0.272f, 0.313f, 1.0f}); // darker field shows particles
        m_preview->Camera().position = Float3{0.0f, 2.0f, 6.0f};
        m_preview->Camera().LookAt(Float3{0.0f, 1.0f, 0.0f});

        SetInstanceId(instance.Id());
        Provide<IPlaybackPage>(*this);

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<pipeline::ParticleEffectAsset>(
            Cast<pipeline::ParticleEffectAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            LOG_ERROR(u8"Editor",
                               u8"particle effect '{}' failed to read - page opens empty", m_title);
        }

        BuildPreviewScene();
        m_undoBaseline = SnapshotEffect();

        auto transport = MakeRef<ui::FlexLayout>(Allocator());
        transport->Direction = ui::Orientation::Horizontal;
        transport->Spacing = 6.0f;
        transport->Padding = ui::Thickness{6, 4};
        {
            ParticleEffectEditorPage* self = this;
            // Simulation-speed slider (scales the preview scene's time) - beyond Sedulous.
            auto speedLabel = MakeRef<ui::Label>(Allocator());
            speedLabel->FontSize.SetValue(Optional<f32>{12.0f});
            speedLabel->VAlign.SetValue(fonts::VerticalAlignment::Middle);
            speedLabel->SetText(u8"Speed");
            transport->AddView(speedLabel.Get());
            auto speed = MakeRef<ui::Slider>(Allocator());
            speed->Min.SetValue(0.0f);
            speed->Max.SetValue(3.0f);
            speed->Value.SetValue(1.0f);
            speed->OnValueChanged.Add(
                [self](ui::Slider*, f32 v)
                {
                    self->m_simSpeed = v;
                    self->m_preview->SetTimeScale(v);
                });
            ui::LayoutStyle slp;
            slp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(90.0f));
            transport->AddView(speed.Get(), slp);

            m_statsLabel = MakeRef<ui::Label>(Allocator());
            m_statsLabel->FontSize.SetValue(12.0f);
            m_statsLabel->VAlign.SetValue(fonts::VerticalAlignment::Middle);
            m_statsLabel->SetText(u8"");
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            transport->AddView(m_statsLabel.Get(), grow);
        }

        auto centerColumn = MakeRef<ui::FlexLayout>(Allocator());
        centerColumn->Direction = ui::Orientation::Vertical;
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            centerColumn->AddView(transport.Get(), lp);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            centerColumn->AddView(m_preview->View(), grow);
        }

        // ---- left: authoring tree ----
        m_adapter = MakeUnique<ParticleTreeAdapter>(Allocator(), *this);
        m_tree = MakeRef<ui::toolkit::DraggableTreeView>(Allocator());
        m_tree->SetItemHeight(22.0f);
        m_tree->SetAdapter(m_adapter.Get());
        {
            ParticleEffectEditorPage* self = this;
            m_tree->InternalTreeView()->OnItemClick.Add(
                [self](ui::TreeView::ItemClickInfo info)
                {
                    if (info.NodeId >= 0 && info.NodeId < static_cast<i32>(self->m_nodes.Size()))
                    {
                        const ParticleTreeNode& n = self->m_nodes[static_cast<usize>(info.NodeId)];
                        self->SelectNode(ParticleNodeRef{n.kind, n.systemIndex, n.moduleIndex});
                    }
                });
            m_tree->InternalTreeView()->OnItemRightClick.Add(
                [self](i32 nodeId, f32 x, f32 y)
                {
                    const Float2 s =
                        self->m_tree->InternalTreeView()->InternalListView()->LocalToScreen(
                            Float2{x, y});
                    self->ShowNodeContextMenu(nodeId, s.x, s.y);
                });
        }

        // ---- right: inspector ----
        m_grid = MakeRef<ui::toolkit::PropertyGrid>(Allocator());
        m_titleLabel = MakeRef<ui::Label>(Allocator());
        m_titleLabel->FontSize.SetValue(12.0f);
        auto inspectorColumn = MakeRef<ui::FlexLayout>(Allocator());
        inspectorColumn->Direction = ui::Orientation::Vertical;
        inspectorColumn->Spacing = 4.0f;
        inspectorColumn->Padding = ui::Thickness{6, 4};
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            inspectorColumn->AddView(m_titleLabel.Get(), lp);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            inspectorColumn->AddView(m_grid.Get(), grow);
        }

        // Assemble the three panes: [tree | center] then that | inspector.
        auto leftSplit = MakeRef<ui::toolkit::SplitView>(Allocator());
        leftSplit->SetSplitRatio(0.22f);
        // The systems tree under its list header (title + add icon).
        auto treeColumn = MakeRef<ui::FlexLayout>(Allocator());
        treeColumn->Direction = ui::Orientation::Vertical;
        treeColumn->Spacing = 4.0f;
        treeColumn->Padding = ui::Thickness{6, 4};
        {
            ParticleEffectEditorPage* self = this;
            auto header = MakeRef<app::ListHeader>(Allocator(), StringView(u8"Systems"),
                                                   StringView(u8"Add system"),
                                                   [self]() { self->AddSystem(); });
            treeColumn->AddView(header.Get(), app::ListHeader::RowStyle());
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            treeColumn->AddView(m_tree.Get(), grow);
        }
        leftSplit->SetPanes(treeColumn.Get(), centerColumn.Get());
        auto rightSplit = MakeRef<ui::toolkit::SplitView>(Allocator());
        rightSplit->SetSplitRatio(0.72f);
        rightSplit->SetPanes(leftSplit.Get(), inspectorColumn.Get());
        // The page toolbar: the standard set, then the playback transport over the preview.
        m_toolbar = MakeRef<app::PageToolbar>(Allocator(), *this, m_context->Actions());
        m_toolbar->AddPlayback();
        m_content = app::PageToolbar::Frame(Allocator(), *m_toolbar, *rightSplit);

        RebuildTree();
        SelectNode(ParticleNodeRef{ParticleNodeKind::Effect, -1, -1});
    }

    // ============================ Page: preview scene ========================================

    void ParticleEffectEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr == nullptr || m_asset.Get() == nullptr)
        {
            return;
        }
        m_preview->SetSimulationEnabled(true); // particles must simulate to animate the preview

        m_emitter = scenePtr->CreateEntity(u8"Emitter");
        if (auto* mgr = scenePtr->GetSystem<engine::particles::ParticleEffectComponentManager>())
        {
            mgr->Add(m_emitter).SetEffect(m_asset->Effect());
        }
        RebuildPreviewResources(); // resolve the effect's mesh/material refs so mesh systems render

        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.castsShadows = false;
        }
    }

    void ParticleEffectEditorPage::RebuildPreviewResources()
    {
        engine::particles::ParticleEffectComponent* c = PreviewComponent();
        if (c == nullptr || m_asset.Get() == nullptr)
        {
            return;
        }
        resource::ResourceManager* mgr = m_context != nullptr ? m_context->Resources() : nullptr;
        if (mgr == nullptr)
        {
            m_previewResource = nullptr; // headless / no project: nothing to resolve against
            c->SetRenderResources(nullptr);
            return;
        }
        // Build an in-memory resource from the authored effect + resolve its per-system mesh/texture/
        // material refs (the same pass the cook factory runs), then attach it for RENDER-lookup only -
        // the sim keeps borrowing the live effect (BuildPreviewScene's SetEffect), so scalar edits stay
        // live while every mesh system now renders its resolved mesh + materials.
        auto res = MakeRef<particles::ParticleEffectResource>(Allocator());
        particles::CloneEffect(m_asset->Effect(), res->Effect());
        particles::ResolveParticleEffectResources(*res, *mgr);
        c->SetRenderResources(res.Get());
        m_previewResource = res; // hold it alive (the component borrows it for the frame)
    }

    engine::particles::ParticleEffectComponent* ParticleEffectEditorPage::PreviewComponent() const
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr == nullptr)
        {
            return nullptr;
        }
        auto* mgr = scenePtr->GetSystem<engine::particles::ParticleEffectComponentManager>();
        return mgr != nullptr ? mgr->Get(m_emitter) : nullptr;
    }

    // ============================ Page: transport ============================================

    bool ParticleEffectEditorPage::CanPlay() const
    {
        engine::particles::ParticleEffectComponent* c = PreviewComponent();
        return c != nullptr && c->instance;
    }

    void ParticleEffectEditorPage::Play()
    {
        m_paused = false;
        m_stopped = false;
        if (m_asset.Get() != nullptr)
        {
            for (i32 i = 0; i < m_asset->Effect().SystemCount(); ++i)
            {
                m_asset->Effect().GetSystem(i)->emitter.isEmitting = true;
            }
        }
        if (engine::particles::ParticleEffectComponent* c = PreviewComponent())
        {
            if (c->instance)
            {
                c->instance->isActive = true;
            }
        }
    }
    void ParticleEffectEditorPage::Stop()
    {
        // Clears the particles and stops emitting: the rewind.
        SetPaused(false);
        m_stopped = true;
        if (engine::particles::ParticleEffectComponent* c = PreviewComponent())
        {
            if (c->instance)
            {
                c->instance->Reset();
                c->instance->Stop();
            }
        }
    }
    void ParticleEffectEditorPage::Restart()
    {
        if (engine::particles::ParticleEffectComponent* c = PreviewComponent())
        {
            if (c->instance)
            {
                c->instance->Reset();
            }
        }
        Play();
    }
    void ParticleEffectEditorPage::SetPaused(bool paused)
    {
        m_paused = paused;
        if (engine::particles::ParticleEffectComponent* c = PreviewComponent())
        {
            if (c->instance)
            {
                c->instance->isActive = !paused;
            }
        }
    }

    // ============================ Page: tree =================================================

    void ParticleEffectEditorPage::RebuildTree()
    {
        m_nodes.Clear();
        m_roots.Clear();
        if (m_asset.Get() == nullptr)
        {
            m_tree->SetAdapter(m_adapter.Get());
            return;
        }
        particles::ParticleEffect& fx = m_asset->Effect();

        auto addNode = [this](ParticleNodeKind kind, i32 sys, i32 mod, i32 depth,
                              String label) -> i32
        {
            ParticleTreeNode n;
            n.kind = kind;
            n.systemIndex = sys;
            n.moduleIndex = mod;
            n.depth = depth;
            n.label = Move(label);
            m_nodes.PushBack(Move(n));
            return static_cast<i32>(m_nodes.Size()) - 1;
        };

        const i32 rootId = addNode(ParticleNodeKind::Effect, -1, -1, 0, String(u8"Effect"));
        m_roots.PushBack(rootId);

        for (i32 s = 0; s < fx.SystemCount(); ++s)
        {
            particles::ParticleSystem* sys = fx.GetSystem(s);
            if (sys == nullptr)
            {
                continue;
            }
            String sysLabel =
                sys->name.IsEmpty() ? Format(u8"System {}", s) : String(sys->name.AsView());
            const i32 sysId = addNode(ParticleNodeKind::System, s, -1, 1, Move(sysLabel));
            m_nodes[static_cast<usize>(rootId)].children.PushBack(sysId);

            const i32 emitterId = addNode(ParticleNodeKind::Emitter, s, -1, 2, String(u8"Emitter"));
            m_nodes[static_cast<usize>(sysId)].children.PushBack(emitterId);

            const i32 initFolder = addNode(ParticleNodeKind::InitializersFolder, s, -1, 2,
                                           Format(u8"Initializers ({})", sys->InitializerCount()));
            m_nodes[static_cast<usize>(sysId)].children.PushBack(initFolder);
            for (i32 i = 0; i < sys->InitializerCount(); ++i)
            {
                const char* tn = sys->GetInitializer(i)->GetType()->name;
                const i32 id = addNode(ParticleNodeKind::Initializer, s, i, 3,
                                       String(reinterpret_cast<const utf8char*>(tn)));
                m_nodes[static_cast<usize>(initFolder)].children.PushBack(id);
            }

            const i32 behFolder = addNode(ParticleNodeKind::BehaviorsFolder, s, -1, 2,
                                          Format(u8"Behaviors ({})", sys->BehaviorCount()));
            m_nodes[static_cast<usize>(sysId)].children.PushBack(behFolder);
            for (i32 i = 0; i < sys->BehaviorCount(); ++i)
            {
                const char* tn = sys->GetBehavior(i)->GetType()->name;
                const i32 id = addNode(ParticleNodeKind::Behavior, s, i, 3,
                                       String(reinterpret_cast<const utf8char*>(tn)));
                m_nodes[static_cast<usize>(behFolder)].children.PushBack(id);
            }
        }

        // Re-flatten the view (SetAdapter recreates the flattened tree from the new node table),
        // expanding everything so the whole authoring graph is visible.
        m_tree->SetAdapter(m_adapter.Get());
        if (ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter())
        {
            HashSet<i32> expanded;
            for (i32 i = 0; i < static_cast<i32>(m_nodes.Size()); ++i)
            {
                expanded.Insert(i);
            }
            flat->SetExpandedNodes(expanded);
        }
        m_tree->InternalTreeView()->InternalListView()->NotifyDataChanged();
    }

    i32 ParticleEffectEditorPage::NodeIdForRef(const ParticleNodeRef& ref) const
    {
        for (i32 i = 0; i < static_cast<i32>(m_nodes.Size()); ++i)
        {
            const ParticleTreeNode& n = m_nodes[static_cast<usize>(i)];
            if (n.kind == ref.kind && n.systemIndex == ref.systemIndex &&
                n.moduleIndex == ref.moduleIndex)
            {
                return i;
            }
        }
        return -1;
    }

    particles::ParticleSystem* ParticleEffectEditorPage::SelectedSystem() const
    {
        if (m_asset.Get() == nullptr || m_selected.systemIndex < 0)
        {
            return nullptr;
        }
        return m_asset->Effect().GetSystem(m_selected.systemIndex);
    }

    void ParticleEffectEditorPage::SelectNode(const ParticleNodeRef& ref)
    {
        m_selected = ref;
        // Reflect the selection in the tree highlight (best-effort: scan flat positions for the id).
        const i32 nodeId = NodeIdForRef(ref);
        if (nodeId >= 0)
        {
            if (ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter())
            {
                for (i32 p = 0; p < flat->ItemCount(); ++p)
                {
                    if (flat->GetNodeId(p) == nodeId)
                    {
                        m_tree->Selection().ClearSelection();
                        m_tree->Selection().Select(p);
                        break;
                    }
                }
            }
        }
        RebuildInspector();
    }

    void ParticleEffectEditorPage::QueueStructural(StringView undoKey, Function<void()> mutate,
                                                   ParticleNodeRef reselect)
    {
        ParticleEffectEditorPage* self = this;
        String key(undoKey);
        auto run = [self, mutate = Move(mutate), reselect, key = Move(key)]() mutable
        {
            mutate();
            self->m_selected = reselect;
            // Reattach the preview (modules recreated) so the live sim uses the new module set.
            if (engine::particles::ParticleEffectComponent* c = self->PreviewComponent())
            {
                if (self->m_asset.Get() != nullptr)
                {
                    c->SetEffect(self->m_asset->Effect());
                    self->RebuildPreviewResources(); // the system set changed - re-resolve per system
                }
            }
            Array<byte> after = self->SnapshotEffect();
            (void)self->Commands().Execute(
                UniquePtr<IEditorCommand>(self->Allocator().New<EditParticleCommand>(
                                              *self, key.AsView(), self->m_undoBaseline, after),
                                          self->Allocator()));
            self->m_undoBaseline = Move(after);
            self->RebuildTree();
            self->RebuildInspector();
            self->MarkDirty();
        };
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(Function<void()>{Move(run)});
        }
        else
        {
            run();
        }
    }

    void ParticleEffectEditorPage::ShowNodeContextMenu(i32 nodeId, f32 screenX, f32 screenY)
    {
        if (nodeId < 0 || nodeId >= static_cast<i32>(m_nodes.Size()) || m_asset.Get() == nullptr)
        {
            return;
        }
        const ParticleTreeNode node = m_nodes[static_cast<usize>(nodeId)];
        ui::UIContext* ctx = Ctx();
        if (ctx == nullptr)
        {
            return;
        }
        ParticleEffectEditorPage* self = this;
        auto menu = MakeRef<ui::ContextMenu>(Allocator());

        auto addInitMenu = [self, &menu](i32 sysIndex)
        {
            ui::MenuItem* item = menu->AddSubmenu(u8"Add Initializer");
            ui::ContextMenu* sub = Cast<ui::ContextMenu>(item->Submenu.Get());
            static constexpr StringView kNames[] = {u8"Position",        u8"Velocity", u8"Lifetime",
                                                    u8"Color",           u8"Size",     u8"Rotation",
                                                    u8"Mesh Orientation"};
            for (i32 k = 0; k < 7; ++k)
            {
                const i32 kind = k;
                sub->AddItem(
                    kNames[k],
                    [self, sysIndex, kind]()
                    {
                        self->QueueStructural(
                            u8"add-init",
                            Function<void()>{
                                [self, sysIndex, kind]()
                                {
                                    particles::ParticleSystem* s =
                                        self->m_asset->Effect().GetSystem(sysIndex);
                                    if (s == nullptr)
                                    {
                                        return;
                                    }
                                    switch (kind)
                                    {
                                    case 0:
                                        s->AddInitializer<particles::PositionInitializer>();
                                        break;
                                    case 1:
                                        s->AddInitializer<particles::VelocityInitializer>();
                                        break;
                                    case 2:
                                        s->AddInitializer<particles::LifetimeInitializer>();
                                        break;
                                    case 3:
                                        s->AddInitializer<particles::ColorInitializer>();
                                        break;
                                    case 4:
                                        s->AddInitializer<particles::SizeInitializer>();
                                        break;
                                    case 5:
                                        s->AddInitializer<particles::RotationInitializer>();
                                        break;
                                    case 6:
                                        s->AddInitializer<particles::MeshOrientationInitializer>();
                                        break;
                                    default:
                                        break;
                                    }
                                }},
                            ParticleNodeRef{ParticleNodeKind::InitializersFolder, sysIndex, -1});
                    });
            }
        };
        auto addBehMenu = [self, &menu](i32 sysIndex)
        {
            ui::MenuItem* item = menu->AddSubmenu(u8"Add Behavior");
            ui::ContextMenu* sub = Cast<ui::ContextMenu>(item->Submenu.Get());
            static constexpr StringView kNames[] = {
                u8"Gravity",   u8"Drag",          u8"Wind",      u8"Turbulence", u8"Vortex",
                u8"Attractor", u8"Radial Force",  u8"Collision", u8"Color/Life", u8"Alpha/Life",
                u8"Size/Life", u8"Rotation/Life", u8"Speed/Life"};
            for (i32 k = 0; k < 13; ++k)
            {
                const i32 kind = k;
                sub->AddItem(
                    kNames[k],
                    [self, sysIndex, kind]()
                    {
                        self->QueueStructural(
                            u8"add-beh",
                            Function<void()>{
                                [self, sysIndex, kind]()
                                {
                                    particles::ParticleSystem* s =
                                        self->m_asset->Effect().GetSystem(sysIndex);
                                    if (s == nullptr)
                                    {
                                        return;
                                    }
                                    switch (kind)
                                    {
                                    case 0:
                                        s->AddBehavior<particles::GravityBehavior>();
                                        break;
                                    case 1:
                                        s->AddBehavior<particles::DragBehavior>();
                                        break;
                                    case 2:
                                        s->AddBehavior<particles::WindBehavior>();
                                        break;
                                    case 3:
                                        s->AddBehavior<particles::TurbulenceBehavior>();
                                        break;
                                    case 4:
                                        s->AddBehavior<particles::VortexBehavior>();
                                        break;
                                    case 5:
                                        s->AddBehavior<particles::AttractorBehavior>();
                                        break;
                                    case 6:
                                        s->AddBehavior<particles::RadialForceBehavior>();
                                        break;
                                    case 7:
                                        s->AddBehavior<particles::CollisionBehavior>();
                                        break;
                                    case 8:
                                        s->AddBehavior<particles::ColorOverLifetimeBehavior>();
                                        break;
                                    case 9:
                                        s->AddBehavior<particles::AlphaOverLifetimeBehavior>();
                                        break;
                                    case 10:
                                        s->AddBehavior<particles::SizeOverLifetimeBehavior>();
                                        break;
                                    case 11:
                                        s->AddBehavior<particles::RotationOverLifetimeBehavior>();
                                        break;
                                    case 12:
                                        s->AddBehavior<particles::SpeedOverLifetimeBehavior>();
                                        break;
                                    default:
                                        break;
                                    }
                                }},
                            ParticleNodeRef{ParticleNodeKind::BehaviorsFolder, sysIndex, -1});
                    });
            }
        };

        switch (node.kind)
        {
        case ParticleNodeKind::Effect:
            menu->AddItem(u8"Add System", [self]() { self->AddSystem(); });
            break;
        case ParticleNodeKind::System:
        {
            const i32 sysIndex = node.systemIndex;
            addInitMenu(sysIndex);
            addBehMenu(sysIndex);
            menu->AddSeparator();
            if (self->m_asset->Effect().SystemCount() > 1)
            {
                menu->AddItem(u8"Delete System",
                              [self, sysIndex]() { self->DeleteSystem(sysIndex); });
            }
            break;
        }
        case ParticleNodeKind::InitializersFolder:
            addInitMenu(node.systemIndex);
            break;
        case ParticleNodeKind::BehaviorsFolder:
            addBehMenu(node.systemIndex);
            break;
        case ParticleNodeKind::Initializer:
        {
            const i32 sysIndex = node.systemIndex;
            const i32 mod = node.moduleIndex;
            menu->AddItem(
                u8"Move Up",
                [self, sysIndex, mod]()
                {
                    self->QueueStructural(
                        u8"reorder",
                        Function<void()>{[self, sysIndex, mod]()
                                         {
                                             if (auto* s =
                                                     self->m_asset->Effect().GetSystem(sysIndex))
                                                 s->MoveInitializer(mod, mod - 1);
                                         }},
                        ParticleNodeRef{ParticleNodeKind::Initializer, sysIndex, Max(mod - 1, 0)});
                },
                mod > 0);
            menu->AddItem(
                u8"Move Down",
                [self, sysIndex, mod]()
                {
                    self->QueueStructural(
                        u8"reorder",
                        Function<void()>{[self, sysIndex, mod]()
                                         {
                                             if (auto* s =
                                                     self->m_asset->Effect().GetSystem(sysIndex))
                                                 s->MoveInitializer(mod, mod + 1);
                                         }},
                        ParticleNodeRef{ParticleNodeKind::Initializer, sysIndex, mod + 1});
                });
            menu->AddSeparator();
            menu->AddItem(
                u8"Delete",
                [self, sysIndex, mod]()
                {
                    self->QueueStructural(
                        u8"del-init",
                        Function<void()>{[self, sysIndex, mod]()
                                         {
                                             if (auto* s =
                                                     self->m_asset->Effect().GetSystem(sysIndex))
                                                 s->RemoveInitializer(mod);
                                         }},
                        ParticleNodeRef{ParticleNodeKind::InitializersFolder, sysIndex, -1});
                });
            break;
        }
        case ParticleNodeKind::Behavior:
        {
            const i32 sysIndex = node.systemIndex;
            const i32 mod = node.moduleIndex;
            menu->AddItem(
                u8"Move Up",
                [self, sysIndex, mod]()
                {
                    self->QueueStructural(
                        u8"reorder",
                        Function<void()>{[self, sysIndex, mod]()
                                         {
                                             if (auto* s =
                                                     self->m_asset->Effect().GetSystem(sysIndex))
                                                 s->MoveBehavior(mod, mod - 1);
                                         }},
                        ParticleNodeRef{ParticleNodeKind::Behavior, sysIndex, Max(mod - 1, 0)});
                },
                mod > 0);
            menu->AddItem(u8"Move Down",
                          [self, sysIndex, mod]()
                          {
                              self->QueueStructural(
                                  u8"reorder",
                                  Function<void()>{
                                      [self, sysIndex, mod]()
                                      {
                                          if (auto* s = self->m_asset->Effect().GetSystem(sysIndex))
                                              s->MoveBehavior(mod, mod + 1);
                                      }},
                                  ParticleNodeRef{ParticleNodeKind::Behavior, sysIndex, mod + 1});
                          });
            menu->AddSeparator();
            menu->AddItem(u8"Delete",
                          [self, sysIndex, mod]()
                          {
                              self->QueueStructural(
                                  u8"del-beh",
                                  Function<void()>{
                                      [self, sysIndex, mod]()
                                      {
                                          if (auto* s = self->m_asset->Effect().GetSystem(sysIndex))
                                              s->RemoveBehavior(mod);
                                      }},
                                  ParticleNodeRef{ParticleNodeKind::BehaviorsFolder, sysIndex, -1});
                          });
            break;
        }
        default:
            break;
        }
        menu->Show(ctx, screenX, screenY);
    }

    // ============================ Page: inspector ===========================================

    void ParticleEffectEditorPage::RebuildInspector()
    {
        m_grid->Clear();
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        m_titleLabel->SetText(Format(u8"Systems: {}", m_asset->Effect().SystemCount()).AsView());

        switch (m_selected.kind)
        {
        case ParticleNodeKind::Effect:
            BuildEffectInspector();
            break;
        case ParticleNodeKind::System:
        case ParticleNodeKind::InitializersFolder:
        case ParticleNodeKind::BehaviorsFolder:
            if (particles::ParticleSystem* sys = SelectedSystem())
            {
                BuildSystemInspector(*sys);
            }
            break;
        case ParticleNodeKind::Emitter:
            if (particles::ParticleSystem* sys = SelectedSystem())
            {
                BuildEmitterInspector(*sys);
            }
            break;
        case ParticleNodeKind::Initializer:
            if (particles::ParticleSystem* sys = SelectedSystem())
            {
                BuildModuleInspector(sys->GetInitializer(m_selected.moduleIndex));
            }
            break;
        case ParticleNodeKind::Behavior:
            if (particles::ParticleSystem* sys = SelectedSystem())
            {
                BuildModuleInspector(sys->GetBehavior(m_selected.moduleIndex));
            }
            break;
        }
    }

    void ParticleEffectEditorPage::AssignMaterial(i32 sysIndex, usize slot, const Guid& picked)
    {
        particles::ParticleSystem* s = m_asset->Effect().GetSystem(sysIndex);
        if (s != nullptr && slot < s->materialRefs.Size())
        {
            s->materialRefs[slot] = picked;
            CommitEdit(u8"material");
            RebuildPreviewResources(); // re-resolve the new material
            QueueInspectorRebuild();
        }
    }

    void ParticleEffectEditorPage::QueueInspectorRebuild()
    {
        // Rebuilding clears the property grid (destroys the row views). When the trigger is an enum row
        // whose own callback is mid-dispatch, tearing the grid down underneath it is unsafe - defer to
        // the mutation queue so it runs after dispatch completes (the UI mutation-queue rule).
        ParticleEffectEditorPage* self = this;
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(
                Function<void()>{[self]() { self->RebuildInspector(); }});
        }
        else
        {
            RebuildInspector();
        }
    }

    void ParticleEffectEditorPage::BuildEffectInspector()
    {
        // Adding a system is the tree header's add icon and the effect's context menu.
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
            MakeRef<ui::toolkit::StringEditor>(
                Allocator(), u8"Systems", Format(u8"{}", m_asset->Effect().SystemCount()).AsView(),
                Function<void(StringView)>{}, StringView(u8"Effect"))
                .Get()));
    }

    void ParticleEffectEditorPage::AddSystem()
    {
        ParticleEffectEditorPage* self = this;
        QueueStructural(u8"add-system",
                        Function<void()>{[self]() { self->m_asset->Effect().AddSystem(2000); }},
                        ParticleNodeRef{ParticleNodeKind::System, m_asset->Effect().SystemCount(),
                                        -1});
    }

    void ParticleEffectEditorPage::DeleteSystem(i32 sysIndex)
    {
        ParticleEffectEditorPage* self = this;
        QueueStructural(u8"del-system",
                        Function<void()>{[self, sysIndex]()
                                         { self->m_asset->Effect().RemoveSystem(sysIndex); }},
                        ParticleNodeRef{ParticleNodeKind::Effect, -1, -1});
    }

    void ParticleEffectEditorPage::BuildSystemInspector(particles::ParticleSystem& sys)
    {
        Page page = this;
        ui::toolkit::PropertyGrid& g = *m_grid;
        const i32 sysIndex = m_selected.systemIndex;

        // Render-mode-conditional sections (the visibleWhen ask): show only the fields the current
        // render mode actually uses. CommitEdit re-runs this inspector after a render-mode change, so
        // the sections update live. Billboard-family modes are textured + flipbook-capable; Trail is
        // textured (ribbon) but not flipbook; Mesh + Light use neither texture nor flipbook.
        const particles::ParticleRenderMode rm = sys.renderMode;
        const bool billboardFamily = rm == particles::ParticleRenderMode::Billboard ||
                                     rm == particles::ParticleRenderMode::StretchedBillboard ||
                                     rm == particles::ParticleRenderMode::HorizontalBillboard ||
                                     rm == particles::ParticleRenderMode::VerticalBillboard;
        const bool textured = billboardFamily || rm == particles::ParticleRenderMode::Trail;
        const bool meshMode = rm == particles::ParticleRenderMode::Mesh;

        // --- General --- (its header carries the system's delete icon while another remains)
        {
            const StringView cat = u8"General";
            if (m_asset->Effect().SystemCount() > 1)
            {
                ParticleEffectEditorPage* self = this;
                auto remove =
                    MakeRef<ui::IconButton>(Allocator(), app::EditorIcons::Get().remove.Get());
                remove->TooltipText = String(u8"Delete system");
                remove->OnClick.Add([self, sysIndex](ui::ButtonBase*)
                                    { self->DeleteSystem(sysIndex); });
                g.SetCategoryHeaderActions(cat, RefPtr<ui::View>(remove.Get()));
            }
            // System name (String field -> StringEditor).
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::StringEditor>(
                           Allocator(), u8"Name", sys.name.AsView(),
                           Function<void(StringView)>{[&sys, page](StringView v)
                                                      {
                                                          sys.name = String(v);
                                                          page->CommitEdit(u8"sys-name");
                                                      }},
                           cat)
                           .Get()));
            static constexpr StringView kSim[] = {u8"CPU", u8"GPU", u8"Auto"};
            RowEnum(g, u8"Simulation", static_cast<i32>(sys.desiredMode),
                    Span<const StringView>{kSim, 3},
                    Function<void(i32)>{[&sys, page](i32 v)
                                        {
                                            sys.desiredMode =
                                                static_cast<particles::SimulationMode>(v);
                                            page->CommitEdit(u8"sim-mode");
                                        }},
                    cat);
            static constexpr StringView kSpace[] = {u8"World", u8"Local"};
            RowEnum(g, u8"Sim Space", static_cast<i32>(sys.simulationSpace),
                    Span<const StringView>{kSpace, 2},
                    Function<void(i32)>{[&sys, page](i32 v)
                                        {
                                            sys.simulationSpace =
                                                static_cast<particles::ParticleSpace>(v);
                                            page->CommitEdit(u8"sim-space");
                                        }},
                    cat);
            static constexpr StringView kBlend[] = {u8"Alpha", u8"Additive", u8"Premultiplied",
                                                    u8"Multiply"};
            RowEnum(g, u8"Blend Mode", static_cast<i32>(sys.blendMode),
                    Span<const StringView>{kBlend, 4},
                    Function<void(i32)>{[&sys, page](i32 v)
                                        {
                                            sys.blendMode =
                                                static_cast<particles::ParticleBlendMode>(v);
                                            page->CommitEdit(u8"blend");
                                        }},
                    cat);
            static constexpr StringView kRender[] = {u8"Billboard", u8"Stretched", u8"Horizontal",
                                                     u8"Vertical",  u8"Mesh",      u8"Trail",
                                                     u8"Light"};
            RowEnum(g, u8"Render Mode", static_cast<i32>(sys.renderMode),
                    Span<const StringView>{kRender, 7},
                    Function<void(i32)>{[&sys, page](i32 v)
                                        {
                                            sys.renderMode =
                                                static_cast<particles::ParticleRenderMode>(v);
                                            page->CommitEdit(u8"render");
                                            // Render mode drives which sections show (Texture/Mesh/
                                            // Flipbook/Trail) - refresh so they update live.
                                            page->QueueInspectorRebuild();
                                        }},
                    cat);
            // Max particles (accessor-gated: reallocates the stream budget).
            i32 maxParticles = sys.MaxParticles();
            String key = Format(u8"maxp-{}", sysIndex);
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(
                       MakeRef<ui::toolkit::IntEditor>(
                           Allocator(), u8"Max Particles", static_cast<i64>(maxParticles), 1,
                           1000000,
                           Function<void(i64)>{[&sys, page, key](i64 v)
                                               {
                                                   sys.SetMaxParticles(static_cast<i32>(v));
                                                   page->CommitEdit(key.AsView());
                                               }},
                           cat)
                           .Get()));
            RowBool(g, u8"Sort Particles", &sys.sortParticles, cat, page);
            RowBool(g, u8"Soft Particles", &sys.softParticles, cat, page);
            RowFloat(g, u8"Soft Distance", &sys.softDistance, cat, page, 0.0, 10.0, 0.01);
            RowFloat(g, u8"Prewarm Time", &sys.prewarmTime, cat, page, 0.0, 60.0, 0.1);
        }

        // --- Texture --- (only the textured render modes: billboards + trail)
        if (textured)
        {
            const StringView cat = u8"Texture";
            ParticleEffectEditorPage* self = this;
            const StringView types[] = {u8"TextureAsset"};
            auto texture = MakeRef<app::ResourceRefEditor>(Allocator(), StringView(u8"Texture"),
                                                           StringView(u8"(none)"), cat,
                                                           Span<const StringView>{types, 1});
            texture->BindAsset(
                *m_context,
                [self, sysIndex]()
                {
                    particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex);
                    return s != nullptr ? s->textureRef : Guid{};
                },
                [self, sysIndex](const Guid& picked)
                {
                    if (particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex))
                    {
                        s->textureRef = picked;
                        self->CommitEdit(u8"texture");
                        self->RebuildPreviewResources();
                        self->QueueInspectorRebuild();
                    }
                });
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(texture.Get()));
        }

        // --- Mesh --- (Mesh render mode only: the per-particle mesh + its scale)
        if (meshMode)
        {
            const StringView cat = u8"Mesh";
            ParticleEffectEditorPage* self = this;
            const StringView meshTypes[] = {u8"StaticMeshAsset", u8"SkinnedMeshAsset"};
            auto mesh = MakeRef<app::ResourceRefEditor>(Allocator(), StringView(u8"Mesh"),
                                                        StringView(u8"(none)"), cat,
                                                        Span<const StringView>{meshTypes, 2});
            mesh->BindAsset(
                *m_context,
                [self, sysIndex]()
                {
                    particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex);
                    return s != nullptr ? s->meshRef : Guid{};
                },
                [self, sysIndex](const Guid& picked)
                {
                    if (particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex))
                    {
                        s->meshRef = picked;
                        self->CommitEdit(u8"mesh");
                        self->RebuildPreviewResources(); // re-resolve the new mesh
                        self->QueueInspectorRebuild();
                    }
                });
            Add(g, RefPtr<ui::toolkit::PropertyEditor>(mesh.Get()));
            RowFloat(g, u8"Mesh Scale", &sys.meshScale, cat, page, 0.001, 1000.0, 0.01);

            // Effect-level per-submesh materials (materialRefs; slot 0 = whole-mesh material, indexed by
            // SubMesh::materialIndex). Empty -> the runtime component's material is the fallback. Reuses
            // the shared editor.app ContainerListEditor - the identical add/move/remove slot widget the
            // scene inspector uses for MeshComponent materials.
            {
                auto slots = MakeRef<app::ContainerListEditor>(Allocator(),
                                                               StringView(u8"Materials"), cat);
                for (usize mi = 0; mi < sys.materialRefs.Size(); ++mi)
                {
                    String name(m_context->AssetNameFor(sys.materialRefs[mi]));
                    if (mi == 0)
                    {
                        name.Append(u8" (whole mesh)");
                    }
                    slots->slotNames.PushBack(Move(name));
                }
                slots->OnAdd = [self, sysIndex]()
                {
                    if (particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex))
                    {
                        s->materialRefs.PushBack(Guid{});
                        self->CommitEdit(u8"material-add");
                        self->RebuildPreviewResources();
                        self->QueueInspectorRebuild();
                    }
                };
                slots->OnRemoveSlot = [self, sysIndex](usize slot)
                {
                    particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex);
                    if (s != nullptr && slot < s->materialRefs.Size())
                    {
                        s->materialRefs.RemoveAt(slot);
                        self->CommitEdit(u8"material-remove");
                        self->RebuildPreviewResources();
                        self->QueueInspectorRebuild();
                    }
                };
                slots->OnMoveSlot = [self, sysIndex](usize slot, bool up)
                {
                    particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex);
                    if (s == nullptr || (up && slot == 0))
                    {
                        return;
                    }
                    const usize other = up ? (slot - 1) : (slot + 1);
                    if (slot < s->materialRefs.Size() && other < s->materialRefs.Size())
                    {
                        const Guid tmp = s->materialRefs[slot];
                        s->materialRefs[slot] = s->materialRefs[other];
                        s->materialRefs[other] = tmp;
                        self->CommitEdit(u8"material-move");
                        self->RebuildPreviewResources();
                        self->QueueInspectorRebuild();
                    }
                };
                slots->OnPickSlot = [self, sysIndex](usize slot)
                {
                    ui::UIContext* ctx = self->Ctx();
                    if (ctx == nullptr || self->m_context->Project() == nullptr)
                    {
                        return;
                    }
                    Array<String> types;
                    types.PushBack(String(u8"MaterialAsset"));
                    auto dialog = MakeRef<app::AssetPickerDialog>(self->Allocator(),
                                                                  *self->m_context, Move(types));
                    dialog->OnPicked = [self, sysIndex, slot](const Guid& picked)
                    { self->AssignMaterial(sysIndex, slot, picked); };
                    dialog->Show(ctx);
                };
                // A drop on a slot is the same write as a pick; a drop on the list appends.
                Array<String> accepted;
                accepted.PushBack(String(u8"MaterialAsset"));
                slots->SetAcceptedTypes(Move(accepted));
                slots->OnAssignSlot = [self, sysIndex](usize slot, const Guid& picked)
                { self->AssignMaterial(sysIndex, slot, picked); };
                slots->OnAppendDropped = [self, sysIndex](const Guid& picked)
                {
                    if (particles::ParticleSystem* s = self->m_asset->Effect().GetSystem(sysIndex))
                    {
                        s->materialRefs.PushBack(picked);
                        self->CommitEdit(u8"material-add");
                        self->RebuildPreviewResources();
                        self->QueueInspectorRebuild();
                    }
                };
                app::ContainerListEditor* rawSlots = slots.Get();
                slots->OnRejectedDrop = [self, rawSlots](StringView assetName, StringView typeName)
                {
                    self->m_context->Notify(
                        editor::NoticeKind::Warning,
                        app::AssetPickerSlot::RejectionText(assetName, typeName,
                                                            rawSlots->AcceptedTypes(), u8"list")
                            .AsView());
                };
                Add(g, RefPtr<ui::toolkit::PropertyEditor>(slots.Get()));
            }
        }

        // --- LOD ---
        {
            const StringView cat = u8"LOD";
            RowFloat(g, u8"Start Distance", &sys.lodStartDistance, cat, page, 0.0, 10000.0, 0.5);
            RowFloat(g, u8"Cull Distance", &sys.lodCullDistance, cat, page, 0.0, 10000.0, 0.5);
            RowFloat(g, u8"Min Rate", &sys.lodMinRate, cat, page, 0.0, 1.0, 0.01);
        }

        // --- Flipbook --- (billboard-family only: animates the billboard UV sheet)
        if (billboardFamily)
        {
            const StringView cat = u8"Flipbook";
            RowBool(g, u8"Enabled", &sys.flipbook.enabled, cat, page);
            RowInt(g, u8"Columns", &sys.flipbook.columns, cat, page, 1, 64);
            RowInt(g, u8"Rows", &sys.flipbook.rows, cat, page, 1, 64);
            RowFloat(g, u8"FPS", &sys.flipbook.fps, cat, page, 0.0, 120.0, 0.5);
            RowBool(g, u8"Over Lifetime", &sys.flipbook.overLifetime, cat, page);
            RowInt(g, u8"Start Frame", &sys.flipbook.startFrame, cat, page, 0, 4096);
        }

        // --- Trail --- (Trail render mode only)
        if (rm == particles::ParticleRenderMode::Trail)
        {
            const StringView cat = u8"Trail";
            RowBool(g, u8"Enabled", &sys.trail.enabled, cat, page);
            RowInt(g, u8"Max Points", &sys.trail.maxPoints, cat, page, 2, 256);
            RowFloat(g, u8"Record Interval", &sys.trail.recordInterval, cat, page, 0.0, 1.0, 0.001);
            RowFloat(g, u8"Lifetime", &sys.trail.lifetime, cat, page, 0.0, 10.0, 0.05);
            RowFloat(g, u8"Width Start", &sys.trail.widthStart, cat, page, 0.0, 10.0, 0.01);
            RowFloat(g, u8"Width End", &sys.trail.widthEnd, cat, page, 0.0, 10.0, 0.01);
            RowFloat(g, u8"Min Vertex Dist", &sys.trail.minVertexDistance, cat, page, 0.0, 10.0,
                     0.01);
            RowBool(g, u8"Use Particle Color", &sys.trail.useParticleColor, cat, page);
            RowColor(g, u8"Trail Color", &sys.trail.trailColor, cat, page);
        }
    }

    void ParticleEffectEditorPage::BuildEmitterInspector(particles::ParticleSystem& sys)
    {
        Page page = this;
        ui::toolkit::PropertyGrid& g = *m_grid;
        const StringView cat = u8"Emitter";

        // Emitter-config-driven field visibility (the visibleWhen ask, extended past render mode): show
        // only the spawn controls the current emission mode actually uses. Continuous modes use Spawn
        // Rate; burst modes use the Burst Count/Interval/Cycles trio; the duration window applies to both.
        const particles::EmissionMode em = sys.emitter.mode;
        const bool continuous = em == particles::EmissionMode::Continuous ||
                                em == particles::EmissionMode::ContinuousAndBurst;
        const bool burst = em == particles::EmissionMode::Burst ||
                           em == particles::EmissionMode::ContinuousAndBurst;

        static constexpr StringView kModes[] = {u8"Continuous", u8"Burst", u8"Continuous + Burst"};
        RowEnum(g, u8"Mode", static_cast<i32>(sys.emitter.mode), Span<const StringView>{kModes, 3},
                Function<void(i32)>{[&sys, page](i32 v)
                                    {
                                        sys.emitter.mode = static_cast<particles::EmissionMode>(v);
                                        page->CommitEdit(u8"emit-mode");
                                        // Mode drives which spawn fields show - refresh so they
                                        // update live as you switch modes.
                                        page->QueueInspectorRebuild();
                                    }},
                cat);
        if (continuous)
        {
            RowFloat(g, u8"Spawn Rate", &sys.emitter.spawnRate, cat, page, 0.0, 100000.0, 1.0);
        }
        RowFloat(g, u8"Duration (s)", &sys.emitter.duration, cat, page, 0.0, 600.0, 0.1);
        RowBool(g, u8"Looping", &sys.emitter.looping, cat, page);
        if (burst)
        {
            RowInt(g, u8"Burst Count", &sys.emitter.burstCount, cat, page, 0, 100000);
            RowFloat(g, u8"Burst Interval", &sys.emitter.burstInterval, cat, page, 0.0, 600.0, 0.05);
            RowInt(g, u8"Burst Cycles (0=inf)", &sys.emitter.burstCycles, cat, page, 0, 100000);
        }
    }

    void ParticleEffectEditorPage::BuildModuleInspector(ISerializable* module)
    {
        if (module == nullptr)
        {
            return;
        }
        Page page = this;
        ui::toolkit::PropertyGrid& g = *m_grid;
        const StringView cat = reinterpret_cast<const utf8char*>(module->GetType()->name);

        // Initializers.
        if (auto* posInit = Cast<particles::PositionInitializer>(module))
        {
            RowEmissionShape(g, u8"Shape", &posInit->shape, cat, page);
            RowBool(g, u8"Local Space", &posInit->localSpace, cat, page);
        }
        else if (auto* velInit = Cast<particles::VelocityInitializer>(module))
        {
            RowFloat3(g, u8"Base Velocity", &velInit->baseVelocity, cat, page);
            RowFloat3(g, u8"Randomness", &velInit->randomness, cat, page);
            RowFloat(g, u8"Shape Dir Speed", &velInit->shapeDirectionSpeed, cat, page);
            RowFloat(g, u8"Velocity Inherit", &velInit->velocityInheritance, cat, page, 0.0, 1.0,
                     0.01);
            RowEmissionShape(g, u8"Shape", &velInit->shape, cat, page);
        }
        else if (auto* lifeInit = Cast<particles::LifetimeInitializer>(module))
        {
            RowRangeFloat(g, u8"Lifetime", &lifeInit->lifetime, cat, page, 0.0, 100.0, 0.05);
        }
        else if (auto* colorInit = Cast<particles::ColorInitializer>(module))
        {
            RowRangeColor(g, u8"Color", &colorInit->color, cat, page);
        }
        else if (auto* sizeInit = Cast<particles::SizeInitializer>(module))
        {
            RowRangeFloat2(g, u8"Size", &sizeInit->size, cat, page);
        }
        else if (auto* rotInit = Cast<particles::RotationInitializer>(module))
        {
            RowRangeFloat(g, u8"Rotation", &rotInit->rotation, cat, page);
            RowRangeFloat(g, u8"Rotation Speed", &rotInit->rotationSpeed, cat, page);
        }
        else if (auto* orientInit = Cast<particles::MeshOrientationInitializer>(module))
        {
            RowBool(g, u8"Random Axis", &orientInit->randomAxis, cat, page);
            RowFloat3(g, u8"Fixed Axis", &orientInit->fixedAxis, cat, page);
        }
        // Behaviors.
        else if (auto* gravityMod = Cast<particles::GravityBehavior>(module))
        {
            RowFloat(g, u8"Multiplier", &gravityMod->multiplier, cat, page);
            RowFloat3(g, u8"Direction", &gravityMod->direction, cat, page);
        }
        else if (auto* dragMod = Cast<particles::DragBehavior>(module))
        {
            RowFloat(g, u8"Drag", &dragMod->drag, cat, page);
        }
        else if (auto* windMod = Cast<particles::WindBehavior>(module))
        {
            RowFloat3(g, u8"Force", &windMod->force, cat, page);
            RowFloat(g, u8"Turbulence", &windMod->turbulence, cat, page);
        }
        else if (auto* turbulenceMod = Cast<particles::TurbulenceBehavior>(module))
        {
            RowFloat(g, u8"Strength", &turbulenceMod->strength, cat, page);
            RowFloat(g, u8"Frequency", &turbulenceMod->frequency, cat, page);
            RowFloat(g, u8"Speed", &turbulenceMod->speed, cat, page);
        }
        else if (auto* vortexMod = Cast<particles::VortexBehavior>(module))
        {
            RowFloat(g, u8"Strength", &vortexMod->strength, cat, page);
            RowFloat3(g, u8"Center", &vortexMod->center, cat, page);
            RowFloat3(g, u8"Axis", &vortexMod->axis, cat, page);
        }
        else if (auto* attractorMod = Cast<particles::AttractorBehavior>(module))
        {
            RowFloat(g, u8"Strength", &attractorMod->strength, cat, page);
            RowFloat3(g, u8"Position", &attractorMod->position, cat, page);
            RowFloat(g, u8"Radius", &attractorMod->radius, cat, page, 0.0, 1000.0, 0.05);
        }
        else if (auto* radialMod = Cast<particles::RadialForceBehavior>(module))
        {
            RowFloat(g, u8"Strength", &radialMod->strength, cat, page);
        }
        else if (auto* collisionMod = Cast<particles::CollisionBehavior>(module))
        {
            RowFloat(g, u8"Radius", &collisionMod->radius, cat, page, 0.0, 10.0, 0.01);
            RowFloat(g, u8"Bounce", &collisionMod->bounce, cat, page, 0.0, 1.0, 0.01);
            RowFloat(g, u8"Friction", &collisionMod->friction, cat, page, 0.0, 1.0, 0.01);
            RowFloat(g, u8"Lifetime Loss", &collisionMod->lifetimeLoss, cat, page, 0.0, 1.0, 0.01);
            // Plane / sphere / box counts + per-primitive fields.
            RowInt(g, u8"Plane Count", &collisionMod->planeCount, u8"Collision Planes", page, 0,
                   particles::CollisionBehavior::kMaxPlanes);
            for (i32 i = 0;
                 i < collisionMod->planeCount && i < particles::CollisionBehavior::kMaxPlanes; ++i)
            {
                String c = Format(u8"Plane {}", i);
                RowFloat3(g, u8"Normal", &collisionMod->planes[i].normal, c.AsView(), page);
                RowFloat(g, u8"Distance", &collisionMod->planes[i].distance, c.AsView(), page,
                         -1000.0, 1000.0, 0.05);
            }
            RowInt(g, u8"Sphere Count", &collisionMod->sphereCount, u8"Collision Spheres", page, 0,
                   particles::CollisionBehavior::kMaxSpheres);
            for (i32 i = 0;
                 i < collisionMod->sphereCount && i < particles::CollisionBehavior::kMaxSpheres;
                 ++i)
            {
                String c = Format(u8"Sphere {}", i);
                RowFloat3(g, u8"Center", &collisionMod->spheres[i].center, c.AsView(), page);
                RowFloat(g, u8"Radius", &collisionMod->spheres[i].radius, c.AsView(), page, 0.0,
                         1000.0, 0.05);
            }
            RowInt(g, u8"Box Count", &collisionMod->boxCount, u8"Collision Boxes", page, 0,
                   particles::CollisionBehavior::kMaxBoxes);
            for (i32 i = 0;
                 i < collisionMod->boxCount && i < particles::CollisionBehavior::kMaxBoxes; ++i)
            {
                String c = Format(u8"Box {}", i);
                RowFloat3(g, u8"Center", &collisionMod->boxes[i].center, c.AsView(), page);
                RowFloat3(g, u8"Half Extents", &collisionMod->boxes[i].halfExtents, c.AsView(),
                          page);
            }
        }
        else if (auto* colorCurveMod = Cast<particles::ColorOverLifetimeBehavior>(module))
        {
            RowCurveColor(g, u8"Color", &colorCurveMod->curve, cat, page);
        }
        else if (auto* alphaCurveMod = Cast<particles::AlphaOverLifetimeBehavior>(module))
        {
            RowCurveFloat(g, u8"Alpha", &alphaCurveMod->curve, cat, page);
        }
        else if (auto* sizeCurveMod = Cast<particles::SizeOverLifetimeBehavior>(module))
        {
            RowCurveFloat2(g, u8"Size", &sizeCurveMod->curve, cat, page);
        }
        else if (auto* rotCurveMod = Cast<particles::RotationOverLifetimeBehavior>(module))
        {
            RowCurveFloat(g, u8"Rotation", &rotCurveMod->curve, cat, page);
        }
        else if (auto* speedCurveMod = Cast<particles::SpeedOverLifetimeBehavior>(module))
        {
            RowCurveFloat(g, u8"Speed", &speedCurveMod->curve, cat, page);
        }
    }

    // ============================ Page: undo =================================================

    Array<byte> ParticleEffectEditorPage::SnapshotEffect() const
    {
        Array<byte> blob;
        if (m_asset.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        particles::SerializeEffect(ar, const_cast<particles::ParticleEffect&>(m_asset->Effect()));
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void ParticleEffectEditorPage::ApplyEffectBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        // If-changed guard: at command push time the live effect already equals the target (the
        // edit ran in place), so Execute() is a no-op and never rebuilds the grid mid-drag. Only a
        // real undo/redo (live != target) re-deserializes + rebuilds.
        Array<byte> cur = SnapshotEffect();
        if (cur.Size() == blob.Size())
        {
            bool same = true;
            for (usize i = 0; i < blob.Size(); ++i)
            {
                if (cur[i] != blob[i])
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return;
            }
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        particles::SerializeEffect(ar, m_asset->Effect());
        m_undoBaseline = blob;
        if (engine::particles::ParticleEffectComponent* c = PreviewComponent())
        {
            c->SetEffect(m_asset->Effect());
        }
        RebuildPreviewResources(); // undo/redo may have changed refs or the system set - re-resolve
        ParticleEffectEditorPage* self = this;
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(Function<void()>{[self]()
                                                                 {
                                                                     self->RebuildTree();
                                                                     self->RebuildInspector();
                                                                 }});
        }
        else
        {
            RebuildTree();
            RebuildInspector();
        }
        MarkDirty();
    }

    void ParticleEffectEditorPage::CommitEdit(StringView mergeKey)
    {
        Array<byte> after = SnapshotEffect();
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            Allocator().New<EditParticleCommand>(*this, mergeKey, m_undoBaseline, after),
            Allocator()));
        m_undoBaseline = Move(after);
        MarkDirty();
    }

    // ============================ Page: frame / render ======================================

    ui::UIContext* ParticleEffectEditorPage::Ctx() const
    {
        if (m_tree.Get() != nullptr && m_tree->InternalTreeView() != nullptr)
        {
            return m_tree->InternalTreeView()->Context;
        }
        return nullptr;
    }

    void ParticleEffectEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        if (m_toolbar.Get() != nullptr)
        {
            m_toolbar->Refresh(); // the page's actions and the transport follow it each frame
        }
        // Live stats overlay: alive count per system + totals.
        if (m_statsLabel.Get() != nullptr && m_asset.Get() != nullptr)
        {
            i32 total = 0;
            i32 cap = 0;
            const i32 count = m_asset->Effect().SystemCount();
            for (i32 i = 0; i < count; ++i)
            {
                particles::ParticleSystem* s = m_asset->Effect().GetSystem(i);
                total += s->AliveCount();
                cap += s->MaxParticles();
            }
            m_statsLabel->SetText(Format(u8"{} systems | {}/{} particles{}", count, total, cap,
                                         m_paused ? StringView(u8" | PAUSED") : StringView(u8""))
                                      .AsView());
        }

        DrawEmissionGizmo();

        if (m_preview)
        {
            m_preview->Update(dt);
        }
    }

    void ParticleEffectEditorPage::DrawEmissionGizmo()
    {
        if (m_preview.Get() == nullptr || !m_preview->IsValid())
        {
            return;
        }
        particles::ParticleSystem* sys = SelectedSystem();
        if (sys == nullptr)
        {
            return;
        }
        // Find the system's position-initializer shape (the emission volume).
        const particles::EmissionShape* shape = nullptr;
        for (i32 i = 0; i < sys->InitializerCount(); ++i)
        {
            if (auto* pos = Cast<particles::PositionInitializer>(sys->GetInitializer(i)))
            {
                shape = &pos->shape;
                break;
            }
        }
        if (shape == nullptr)
        {
            return;
        }
        auto& dd = m_preview->SceneDebugDraw();
        const Color c{0.30f, 0.85f, 1.0f, 1.0f};
        const Float3 o = sys->position;
        const Float3 up{0.0f, 1.0f, 0.0f};
        switch (shape->type)
        {
        case particles::EmissionShapeType::Sphere:
        case particles::EmissionShapeType::Hemisphere:
            dd.DrawWireSphere(o, Max(shape->radius, 0.01f), c);
            break;
        case particles::EmissionShapeType::Box:
            dd.DrawWireBoxCenter(o, shape->extents, c);
            break;
        case particles::EmissionShapeType::Cone:
            dd.DrawCone(o, up, Max(shape->radius, 0.5f), Max(shape->angle, 0.01f), c);
            break;
        case particles::EmissionShapeType::Ring:
        case particles::EmissionShapeType::Circle:
            dd.DrawCircleNormal(o, Max(shape->radius, 0.01f), up, c);
            break;
        case particles::EmissionShapeType::Edge:
            dd.DrawLine(o - Float3{Max(shape->radius, 0.01f), 0.0f, 0.0f},
                        o + Float3{Max(shape->radius, 0.01f), 0.0f, 0.0f}, c);
            break;
        case particles::EmissionShapeType::Point:
        default:
            dd.DrawCross(o, 0.15f, c);
            break;
        }
    }

    void ParticleEffectEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                                  foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    Status ParticleEffectEditorPage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        foundation::content::Instance* instance =
            m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status saved = instance->WriteObject(*m_asset);
        if (saved.IsOk())
        {
            ClearDirty();
            m_context->RequestCook(false);
            LOG_INFO(u8"Editor", u8"saved particle effect '{}'", m_title);
        }
        return saved;
    }

    void ParticleEffectEditorPage::OnClose()
    {
        if (m_tree.Get() != nullptr)
        {
            m_tree->SetAdapter(nullptr);
        }
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    // ============================ Factory / creator =========================================

    const TypeInfo* ParticleEffectPageFactory::PrimaryType() const
    {
        return &pipeline::ParticleEffectAsset::StaticType();
    }

    UniquePtr<EditorPage>
    ParticleEffectPageFactory::CreatePage(EditorContext& context,
                                          foundation::content::Instance& instance)
    {
        auto* page = context.Allocator().New<ParticleEffectEditorPage>(context, *m_host, *m_uiHost,
                                                                       instance);
        return UniquePtr<EditorPage>(page, context.Allocator());
    }

    void RegisterParticleEditor(EditorContext& context, runtime::IApplicationHost& host,
                                ui::runtime::UIHost& uiHost)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            context.Allocator().New<ParticleEffectPageFactory>(host, uiHost),
            context.Allocator()));

    }
}
