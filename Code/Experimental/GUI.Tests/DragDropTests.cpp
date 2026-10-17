// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - drag and drop (E7): a press past the threshold offers the drag up from the pressed node,
// a node owning its press ending the walk; every move, modifier change and requery asks the node
// under the pointer and up its ancestors what a drop would do (an effect, a reason, a place); the
// cursor, the preview and a hint show it; a release drops, Escape, a right press or the source
// leaving cancels; the source is told how it ended; a finger drags after a long press.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    // A box at a place of its own in the root.
    core::RefPtr<Panel> Box(SceneRoot& root, float x, float y, float w, float h)
    {
        auto box = core::MakeRef<Panel>(core::DefaultAllocator());
        box->EditLayoutStyle([=](LayoutStyle& s)
        {
            s.Placement = Position::Absolute;
            s.Left = LayoutLength::Px(x);
            s.Top = LayoutLength::Px(y);
            s.Width = LayoutLength::Px(w);
            s.Height = LayoutLength::Px(h);
        });
        root.AddChild(box.Get());
        return box;
    }

    struct Scene
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<Panel> source;
        core::RefPtr<Panel> target;
        DragResult ended;
        int ends = 0;

        Scene()
        {
            root->SetSize(core::Float2{400.0f, 300.0f});
            source = Box(*root, 10.0f, 10.0f, 80.0f, 40.0f);
            target = Box(*root, 200.0f, 10.0f, 100.0f, 100.0f);
            source->SetDragSource([](DragRequest& r)
            {
                r.Payload.Type = core::String(u8"card");
                r.Payload.Value = core::Variant::From<core::String>(core::String(u8"ace"));
                r.Payload.Label = core::String(u8"Ace of spades");
                return true;
            });
            source->SetOnDragEnd([this](const DragResult& r)
            {
                ended = r;
                ++ends;
            });
            (void)root->UpdateLayout();
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        // Presses the source and moves `to`, in steps.
        void DragTo(core::Float2 to, core::u32 modifiers = 0)
        {
            D().InjectMouseDown(core::Float2{30.0f, 30.0f}, MouseButton::Left);
            D().InjectMouseMove(core::Float2{40.0f, 30.0f}, modifiers);
            D().InjectMouseMove(to, modifiers);
        }
    };
}

TEST_CASE("dnd: a press starts a drag only past the threshold, and the press is cancelled (no click)")
{
    Scene s;
    int clicks = 0;
    (void)s.source->AddEventListener(EventType::MouseClick, [&](const Event&) { ++clicks; });
    bool started = false;
    (void)s.source->AddEventListener(EventType::DragStart, [&](const Event&) { started = true; });
    s.D().InjectMouseDown(core::Float2{30.0f, 30.0f}, MouseButton::Left);
    s.D().InjectMouseMove(core::Float2{33.0f, 31.0f}); // 4 px: short of the 6
    CHECK_FALSE(s.D().IsDragging());
    s.D().InjectMouseMove(core::Float2{38.0f, 31.0f});
    CHECK(s.D().IsDragging());
    CHECK(started);
    CHECK(s.D().GetDragSource() == s.source.Get());
    CHECK(s.source->IsDragSource()); // `:drag-source`
    CHECK(s.D().GetDragPayload().Type == core::StringView(u8"card"));
    s.D().InjectMouseUp(core::Float2{38.0f, 31.0f}, MouseButton::Left);
    CHECK_FALSE(s.D().IsDragging());
    CHECK(clicks == 0);
    CHECK(s.ends == 1);
    CHECK(s.ended.Effect == DropEffect::None); // dropped where nothing takes it
    CHECK_FALSE(s.source->IsDragSource());

    // A press too short to drag still clicks.
    s.D().InjectMouseDown(core::Float2{30.0f, 30.0f}, MouseButton::Left);
    s.D().InjectMouseUp(core::Float2{31.0f, 30.0f}, MouseButton::Left);
    CHECK(clicks == 1);
}

TEST_CASE("dnd: the drag is offered up from the pressed node; one owning its press ends the walk")
{
    Scene s;
    // A plain child of the source: the source above it gives the drag.
    auto child = core::MakeRef<Panel>(core::DefaultAllocator());
    child->EditLayoutStyle([](LayoutStyle& st)
    {
        st.Width = LayoutLength::Px(20.0f);
        st.Height = LayoutLength::Px(20.0f);
    });
    s.source->AddChild(child.Get());
    (void)s.root->UpdateLayout();
    const Rect c = child->GetScreenBounds();
    const core::Float2 at{c.x + 5.0f, c.y + 5.0f};
    s.D().InjectMouseDown(at, MouseButton::Left);
    s.D().InjectMouseMove(core::Float2{at.x + 20.0f, at.y});
    CHECK(s.D().IsDragging());
    CHECK(s.D().GetDragSource() == s.source.Get());
    s.D().CancelDrag();
    s.D().InjectMouseUp(at, MouseButton::Left);

    // A slider inside the source owns its press: no drag, the slider moves instead.
    child->RemoveFromParent();
    auto slider = core::MakeRef<Slider>(core::DefaultAllocator());
    slider->EditLayoutStyle([](LayoutStyle& st)
    {
        st.Width = LayoutLength::Px(70.0f);
        st.Height = LayoutLength::Px(20.0f);
    });
    s.source->AddChild(slider.Get());
    (void)s.root->UpdateLayout();
    const Rect sl = slider->GetScreenBounds();
    s.D().InjectMouseDown(core::Float2{sl.x + 10.0f, sl.y + 10.0f}, MouseButton::Left);
    s.D().InjectMouseMove(core::Float2{sl.x + 50.0f, sl.y + 10.0f});
    CHECK_FALSE(s.D().IsDragging());
    CHECK(slider->GetValue() > 0.0);
    s.D().InjectMouseUp(core::Float2{sl.x + 50.0f, sl.y + 10.0f}, MouseButton::Left);
}

TEST_CASE("dnd: each move asks the node under the pointer and up its ancestors; the first that takes it is the target")
{
    Scene s;
    auto inner = Box(*s.root, 220.0f, 30.0f, 40.0f, 40.0f); // over the target, not its child
    s.root->RemoveChild(inner.Get());
    s.target->AddChild(inner.Get());
    (void)s.root->UpdateLayout();
    s.target->SetDropTypes(core::String(u8"card chip"));
    int enters = 0, leaves = 0, overs = 0;
    (void)s.target->AddEventListener(EventType::DragEnter, [&](const Event&) { ++enters; });
    (void)s.target->AddEventListener(EventType::DragLeave, [&](const Event&) { ++leaves; });
    (void)s.target->AddEventListener(EventType::DragOver, [&](const Event&) { ++overs; });

    s.DragTo(core::Float2{100.0f, 200.0f}); // over nothing that takes it
    CHECK(s.D().GetDropTarget() == nullptr);
    CHECK(s.D().GetDropEffect() == DropEffect::None);
    CHECK(s.D().GetCursor() == Cursor::NoDrop);

    // Over the inner box, inside the target: the target (its ancestor) takes it.
    const Rect in = inner->GetScreenBounds();
    s.D().InjectMouseMove(core::Float2{in.x + 10.0f, in.y + 10.0f});
    CHECK(s.D().GetDropTarget() == s.target.Get());
    CHECK(s.target->IsDropTarget()); // `:drop-target`
    CHECK(enters == 1);
    CHECK(overs == 1);
    CHECK(s.D().GetDropEffect() == DropEffect::Move);
    CHECK(s.D().GetCursor() == Cursor::Move);
    s.D().InjectMouseMove(core::Float2{in.x + 12.0f, in.y + 10.0f});
    CHECK(overs == 2);
    CHECK(enters == 1);

    // Off it: left.
    s.D().InjectMouseMove(core::Float2{100.0f, 200.0f});
    CHECK(leaves == 1);
    CHECK_FALSE(s.target->IsDropTarget());
    s.D().InjectMouseUp(core::Float2{100.0f, 200.0f}, MouseButton::Left);
}

TEST_CASE("dnd: the modifiers choose the effect among those the source allows, asked again as they change")
{
    Scene s;
    s.target->SetDropTypes(core::String(u8"card"));
    s.DragTo(core::Float2{250.0f, 50.0f});
    CHECK(s.D().GetDropEffect() == DropEffect::Move);
    s.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Unknown), KeyModCtrl); // Ctrl held
    CHECK(s.D().GetDropEffect() == DropEffect::Copy);
    CHECK(s.D().GetCursor() == Cursor::Copy);
    s.D().InjectMouseMove(core::Float2{252.0f, 50.0f}, KeyModCtrl | KeyModShift);
    CHECK(s.D().GetDropEffect() == DropEffect::Link);
    CHECK(s.D().GetCursor() == Cursor::Alias);
    s.D().InjectKeyUp(static_cast<core::u32>(KeyCode::Unknown), 0);
    CHECK(s.D().GetDropEffect() == DropEffect::Move);
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{252.0f, 50.0f}, MouseButton::Left);

    // A source allowing copy alone: Shift asks for a move, gets a copy.
    s.source->SetDragSource([](DragRequest& r)
    {
        r.Payload.Type = core::String(u8"card");
        r.Payload.Allowed = DropEffectsCopy;
        return true;
    });
    s.DragTo(core::Float2{250.0f, 50.0f}, KeyModShift);
    CHECK(s.D().GetDropEffect() == DropEffect::Copy);
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{250.0f, 50.0f}, MouseButton::Left);
}

TEST_CASE("dnd: a target says what a drop would do, or refuses with a reason that ends the walk; the hint shows it")
{
    Scene s;
    s.target->SetDropTypes(core::String(u8"card")); // would take it, but a child refuses first
    auto picky = Box(*s.root, 0.0f, 0.0f, 50.0f, 50.0f);
    s.root->RemoveChild(picky.Get());
    s.target->AddChild(picky.Get());
    picky->SetOnDragQuery([](DropQuery& q)
    {
        if (q.Modifiers & KeyModShift)
            q.Refuse(u8"The discard pile takes no aces");
        else
        {
            q.Accept(DropEffect::Copy);
            q.Reason = core::String(u8"Copy to the discard pile");
            q.Place = 3;
        }
    });
    (void)s.root->UpdateLayout();
    const Rect p = picky->GetScreenBounds();
    s.DragTo(core::Float2{p.x + 10.0f, p.y + 10.0f});
    CHECK(s.D().GetDropTarget() == picky.Get());
    CHECK(s.D().GetDropEffect() == DropEffect::Copy);
    CHECK(s.D().GetDropReason() == core::StringView(u8"Copy to the discard pile"));
    REQUIRE(s.D().GetDropHint() != nullptr);
    CHECK_FALSE(core::Cast<UIWidget>(s.D().GetDropHint())->HasClass(core::StringView(u8"refused")));

    s.D().InjectMouseMove(core::Float2{p.x + 11.0f, p.y + 10.0f}, KeyModShift);
    CHECK(s.D().GetDropTarget() == nullptr); // refused: not passed on to the target above
    CHECK(s.D().GetDropEffect() == DropEffect::None);
    CHECK(s.D().GetDropReason() == core::StringView(u8"The discard pile takes no aces"));
    REQUIRE(s.D().GetDropHint() != nullptr);
    CHECK(core::Cast<UIWidget>(s.D().GetDropHint())->HasClass(core::StringView(u8"refused")));

    // Dropped with the place the target gave.
    core::i32 place = -1;
    DropEffect effect = DropEffect::None;
    (void)picky->AddEventListener(EventType::Drop, [&](const Event& e)
    {
        place = static_cast<const DragEvent&>(e).Place;
        effect = static_cast<const DragEvent&>(e).Effect;
    });
    s.D().InjectMouseMove(core::Float2{p.x + 12.0f, p.y + 10.0f}, 0);
    s.D().InjectMouseUp(core::Float2{p.x + 12.0f, p.y + 10.0f}, MouseButton::Left);
    CHECK(place == 3);
    CHECK(effect == DropEffect::Copy);
    CHECK(s.ended.Effect == DropEffect::Copy); // the source told
    CHECK(s.ended.Target == picky.Get());
    CHECK_FALSE(s.ended.Cancelled);
    CHECK(s.D().GetDropHint() == nullptr);
}

TEST_CASE("dnd: the preview follows the pointer, held where the press was, and shows the effect")
{
    Scene s;
    s.target->SetDropTypes(core::String(u8"card"));
    s.DragTo(core::Float2{120.0f, 150.0f});
    auto* preview = core::Cast<DragPreview>(s.D().GetDragPreview());
    REQUIRE(preview != nullptr);
    CHECK(preview->GetText() == core::StringView(u8"Ace of spades"));
    CHECK(preview->GetEffect() == DropEffect::None);
    (void)s.root->UpdateLayout();
    CHECK(preview->GetScreenBounds().x == doctest::Approx(120.0f + 14.0f)); // beside the pointer
    s.D().InjectMouseMove(core::Float2{250.0f, 50.0f});
    CHECK(preview->GetEffect() == DropEffect::Move);
    // It takes no hits: the target under it is still what is asked.
    CHECK(s.D().GetDropTarget() == s.target.Get());
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{250.0f, 50.0f}, MouseButton::Left);
    CHECK(s.D().GetDragPreview() == nullptr);

    // A preview of the source's own keeps the grab point.
    s.source->SetDragSource([](DragRequest& r)
    {
        r.Payload.Type = core::String(u8"card");
        auto own = core::MakeRef<Panel>(core::DefaultAllocator());
        own->EditLayoutStyle([](LayoutStyle& st)
        {
            st.Width = LayoutLength::Px(80.0f);
            st.Height = LayoutLength::Px(40.0f);
        });
        r.Preview = own;
        return true;
    });
    s.DragTo(core::Float2{150.0f, 150.0f});
    (void)s.root->UpdateLayout();
    // Pressed at (30, 30), 20 in from the source's corner (10, 10): the preview's corner 20 back.
    CHECK(s.D().GetDragPreview()->GetScreenBounds().x == doctest::Approx(130.0f));
    CHECK(s.D().GetDragPreview()->GetScreenBounds().y == doctest::Approx(130.0f));
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{150.0f, 150.0f}, MouseButton::Left);
}

TEST_CASE("dnd: Escape, a right press and the source leaving the tree cancel; the source hears it cancelled")
{
    Scene s;
    s.target->SetDropTypes(core::String(u8"card"));
    s.DragTo(core::Float2{250.0f, 50.0f});
    s.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK_FALSE(s.D().IsDragging());
    CHECK(s.ended.Cancelled);
    CHECK(s.ended.Effect == DropEffect::None);
    CHECK_FALSE(s.target->IsDropTarget());
    int drops = 0;
    (void)s.target->AddEventListener(EventType::Drop, [&](const Event&) { ++drops; });
    s.D().InjectMouseUp(core::Float2{250.0f, 50.0f}, MouseButton::Left);
    CHECK(drops == 0); // the release after a cancel drops nothing

    s.DragTo(core::Float2{250.0f, 50.0f});
    s.D().InjectMouseDown(core::Float2{250.0f, 50.0f}, MouseButton::Right);
    CHECK_FALSE(s.D().IsDragging());
    CHECK(s.ends == 2);
    s.D().InjectMouseUp(core::Float2{250.0f, 50.0f}, MouseButton::Right);
    s.D().InjectMouseUp(core::Float2{250.0f, 50.0f}, MouseButton::Left);
    CHECK(drops == 0);

    s.DragTo(core::Float2{250.0f, 50.0f});
    s.source->RemoveFromParent();
    CHECK_FALSE(s.D().IsDragging());
    CHECK(s.ends == 2); // a source gone hears nothing
}

TEST_CASE("dnd: a finger drags after a long press over a source, and opens no context menu")
{
    Scene s;
    s.target->SetDropTypes(core::String(u8"card"));
    int menus = 0;
    (void)s.source->AddEventListener(EventType::ContextMenu, [&](const Event&) { ++menus; });
    // A quick move is no drag (a finger's move pans first).
    s.D().InjectTouchDown(1, core::Float2{30.0f, 30.0f});
    s.D().InjectTouchMove(1, core::Float2{50.0f, 30.0f});
    CHECK_FALSE(s.D().IsDragging());
    s.D().InjectTouchUp(1, core::Float2{50.0f, 30.0f});

    s.D().InjectTouchDown(1, core::Float2{30.0f, 30.0f});
    s.D().Tick(0.6f); // held still
    CHECK(menus == 0);
    s.D().InjectTouchMove(1, core::Float2{60.0f, 30.0f});
    CHECK(s.D().IsDragging());
    s.D().InjectTouchMove(1, core::Float2{250.0f, 50.0f});
    CHECK(s.D().GetDropTarget() == s.target.Get());
    s.D().InjectTouchUp(1, core::Float2{250.0f, 50.0f});
    CHECK(s.ended.Effect == DropEffect::Move);
}

TEST_CASE("dnd: code can start a drag (no press of its own), and RequeryDrag asks again")
{
    Scene s;
    bool open = false;
    s.target->SetOnDragQuery([&](DropQuery& q)
    {
        if (open)
            q.Accept();
    });
    s.D().InjectMouseMove(core::Float2{250.0f, 50.0f});
    DragRequest request;
    request.Payload.Type = core::String(u8"card");
    s.D().StartDrag(*s.source, core::Move(request));
    CHECK(s.D().IsDragging());
    CHECK(s.D().GetDropTarget() == nullptr);
    open = true; // what is under the pointer changed with the pointer still
    s.D().RequeryDrag();
    CHECK(s.D().GetDropTarget() == s.target.Get());
    s.D().InjectMouseUp(core::Float2{250.0f, 50.0f}, MouseButton::Left);
    CHECK(s.ended.Effect == DropEffect::Move);
}

TEST_CASE("dnd: a drag near a scroller's edge scrolls it, faster deeper in, and asks the drop again")
{
    Scene s;
    auto list = Box(*s.root, 200.0f, 120.0f, 150.0f, 100.0f);
    list->EditLayoutStyle([](LayoutStyle& st) { st.OverflowY = Overflow::Auto; });
    for (int i = 0; i < 20; ++i)
    {
        auto row = core::MakeRef<Panel>(core::DefaultAllocator());
        row->EditLayoutStyle([](LayoutStyle& st)
        {
            st.Height = LayoutLength::Px(30.0f);
            st.Shrink = 0.0f;
        });
        list->AddChild(row.Get());
    }
    int queries = 0;
    list->SetOnDragQuery([&](DropQuery& q)
    {
        ++queries;
        q.Accept();
    });
    (void)s.root->UpdateLayout();
    s.DragTo(core::Float2{270.0f, 170.0f}); // the middle: no scroll
    s.D().Tick(0.1f);
    CHECK(list->GetScrollOffset().y == doctest::Approx(0.0f));
    const int before = queries;
    s.D().InjectMouseMove(core::Float2{270.0f, 210.0f}); // 10 into the bottom band of 24
    s.D().Tick(0.1f);
    const float slow = list->GetScrollOffset().y;
    CHECK(slow > 0.0f);
    CHECK(queries > before + 1); // asked again as the content moved
    s.D().InjectMouseMove(core::Float2{270.0f, 219.0f}); // at the edge
    s.D().Tick(0.1f);
    CHECK(list->GetScrollOffset().y - slow > slow);
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{270.0f, 219.0f}, MouseButton::Left);
}

TEST_CASE("dnd: a drag resting a moment opens a closed tree node and selects a tab")
{
    TreeModel model; // outlives the scene, whose tree is its client
    Scene s;
    const core::i32 a = model.AddNode(TreeModel::kRoot, core::StringView(u8"A"));
    (void)model.AddNode(a, core::StringView(u8"A1"));
    auto tree = core::MakeRef<TreeView>(core::DefaultAllocator());
    tree->EditLayoutStyle([](LayoutStyle& st)
    {
        st.Placement = Position::Absolute;
        st.Left = LayoutLength::Px(200.0f);
        st.Top = LayoutLength::Px(150.0f);
        st.Width = LayoutLength::Px(150.0f);
        st.Height = LayoutLength::Px(100.0f);
    });
    tree->SetRowHeight(20.0f);
    s.root->AddChild(tree.Get());
    tree->SetModel(&model);
    (void)s.root->UpdateLayout();

    s.DragTo(core::Float2{260.0f, 160.0f}); // over row 0, A (closed)
    s.D().Tick(0.3f);
    CHECK_FALSE(tree->IsItemExpanded(0));
    s.D().Tick(0.4f);
    CHECK(tree->IsItemExpanded(0));
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{260.0f, 160.0f}, MouseButton::Left);

    auto bar = core::MakeRef<TabBar>(core::DefaultAllocator());
    bar->EditLayoutStyle([](LayoutStyle& st)
    {
        st.Placement = Position::Absolute;
        st.Left = LayoutLength::Px(10.0f);
        st.Top = LayoutLength::Px(250.0f);
    });
    s.root->AddChild(bar.Get());
    const char8_t* const names[] = {u8"One", u8"Two"};
    for (const char8_t* name : names)
        bar->AddTab(core::StringView(name))->EditLayoutStyle([](LayoutStyle& st)
        {
            st.Width = LayoutLength::Px(60.0f);
            st.Height = LayoutLength::Px(24.0f);
        });
    (void)s.root->UpdateLayout();
    const Rect two = bar->GetTab(1)->GetScreenBounds();
    s.DragTo(core::Float2{two.x + 20.0f, two.y + 10.0f});
    s.D().Tick(0.7f);
    CHECK(bar->GetSelectedIndex() == 1);
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{two.x + 20.0f, two.y + 10.0f}, MouseButton::Left);
}

TEST_CASE("dnd: a drag resting on a closed expander's header opens it")
{
    Scene s;
    auto expander = core::MakeRef<Expander>(core::DefaultAllocator());
    expander->SetTitle(core::StringView(u8"More"));
    expander->EditLayoutStyle([](LayoutStyle& st)
    {
        st.Placement = Position::Absolute;
        st.Left = LayoutLength::Px(10.0f);
        st.Top = LayoutLength::Px(150.0f);
        st.Width = LayoutLength::Px(150.0f);
    });
    s.root->AddChild(expander.Get());
    (void)s.root->UpdateLayout();
    const Rect header = expander->GetHeader()->GetScreenBounds();
    s.DragTo(core::Float2{header.x + 40.0f, header.y + header.height * 0.5f});
    s.D().Tick(0.7f);
    CHECK(expander->IsOpen());
    s.D().CancelDrag();
    s.D().InjectMouseUp(core::Float2{header.x + 40.0f, header.y + 5.0f}, MouseButton::Left);
}
