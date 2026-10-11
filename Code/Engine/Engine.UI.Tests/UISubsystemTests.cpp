// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Headless UISubsystem: canvas instantiation from documents (direct Ref override, no db),
// hot-reload rebuild, visibility/interactivity sync, serialization round-trip. No GPU -
// RenderOverlay untested here (the sample + editor smoke cover it on-screen).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs; // the data mount the subsystem reads engine data through
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.ui;
import foundation.ui.resource;
import foundation.ui.gamekit; // UIScreen + ScreenStack (the run screens test)
import engine.ui;
import foundation.script; // IScriptDelegate (the Ui host onClick test)
import foundation.render.api;
import engine.render;
import foundation.shell;
import foundation.shell.null;
import foundation.rhi;
import foundation.rhi.null;
import foundation.input;
import engine.input;
import foundation.fonts;          // IFontService (the extra font families test)
import foundation.fonts.resource; // Font products
import foundation.content;          // the content database a texture image binds from
import foundation.resource;
import foundation.image;
import foundation.texture.resource; // TextureFactory + the render texture record

using namespace foundation::core;
using namespace engine::ui;
using namespace engine::scene;
using namespace engine::render;
using namespace engine::input;
using namespace foundation::ui;
namespace scene = foundation::scene;
namespace runtime = foundation::runtime;

namespace
{
    RefPtr<UIDocument> MakeDocument(StringView markup)
    {
        RefPtr<UIDocument> document = MakeRef<UIDocument>(DefaultAllocator());
        document->markup = String(markup);
        return document;
    }
}

namespace
{
    // The data mount the subsystem reads its shaders and built-in font through (the repo's
    // Data/, found by the same discovery walk every executable uses).
    foundation::vfs::IFileSystem& DataFs()
    {
        static foundation::vfs::NativeFileSystem fs(foundation::vfs::FindDataRoot(),
                                                    DefaultAllocator());
        return fs;
    }
}

TEST_CASE("ui.subsystem: canvases instantiate, hot-reload, and sync visibility")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    scene::Scene* scene = sm.CreateScene(u8"menu");
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();
    REQUIRE(canvases != nullptr); // installed by the scene composition

    scene::EntityHandle e = scene->CreateEntity(u8"pause");
    UICanvasComponent& canvas = canvases->Add(e);
    RefPtr<UIDocument> document =
        MakeDocument(u8"<FlexLayout><Label id=\"title\" text=\"Paused\" /><Button "
                     u8"id=\"resume-btn\" text=\"Resume\" /></FlexLayout>");
    canvas.document = document; // Ref direct override (no content db)

    ctx.BeginFrame(1.0f / 60.0f); // subsystem builds the tree

    REQUIRE(canvas.root.Get() != nullptr);
    auto* group = Cast<ViewGroup>(canvas.root.Get());
    REQUIRE(group != nullptr);
    CHECK(group->FindByName(u8"resume-btn") != nullptr);
    // The tier split: the canvas parents into ITS SCENE's root (above that scene's
    // billboard layer); the screen root holds only the global overlay layer.
    RootView* sceneRoot = ui->SceneRoot(*scene);
    REQUIRE(sceneRoot != nullptr);
    CHECK(sceneRoot->ChildCount() == 2u);        // billboard layer + canvas
    CHECK(ui->ScreenRoot()->ChildCount() == 1u); // overlay layer only
    CHECK(Cast<ViewGroup>(sceneRoot)->FindByName(u8"resume-btn") != nullptr);

    // Hot reload: a NEW document product rebuilds the tree (structure proves it - a
    // pointer compare can false-negative on allocator address reuse).
    canvas.document = MakeDocument(u8"<FlexLayout><Label id=\"only\" text=\"v2\" /></FlexLayout>");
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(canvas.root.Get() != nullptr);
    CHECK(Cast<ViewGroup>(canvas.root.Get())->FindByName(u8"only") != nullptr);
    CHECK(Cast<ViewGroup>(canvas.root.Get())->FindByName(u8"resume-btn") == nullptr);

    // Visibility/interactivity flow into the live tree each frame.
    canvas.visible = false;
    canvas.interactive = false;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(canvas.root->Visibility == VisibilityValue::Gone);
    CHECK_FALSE(canvas.root->IsHitTestVisible);

    // Scene teardown detaches cleanly.
    sm.DestroyScene(scene);
    ctx.BeginFrame(1.0f / 60.0f);
    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: an effectively-inactive entity's canvas goes Gone (and back)")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    scene::Scene* scene = sm.CreateScene(u8"menu");
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();
    REQUIRE(canvases != nullptr);

    // The canvas rides a CHILD of the toggled entity (the hierarchy path).
    scene::EntityHandle parent = scene->CreateEntity(u8"holder");
    scene::EntityHandle e = scene->CreateEntity(u8"hud");
    scene->SetParent(e, parent);
    UICanvasComponent& canvas = canvases->Add(e);
    RefPtr<UIDocument> document =
        MakeDocument(u8"<FlexLayout><Label id=\"hp\" text=\"100\" /></FlexLayout>");
    canvas.document = document;

    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(canvas.root.Get() != nullptr);
    CHECK(canvas.root->Visibility == VisibilityValue::Visible);

    scene->SetActive(parent, false); // inactive entity: the game UI does not show
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(canvas.root->Visibility == VisibilityValue::Gone);

    scene->SetActive(parent, true);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(canvas.root->Visibility == VisibilityValue::Visible);

    sm.DestroyScene(scene);
    ctx.BeginFrame(1.0f / 60.0f);
    ctx.Shutdown();
    (void)ui;
}

TEST_CASE("ui.subsystem: canvas component serialization round-trips")
{
    RegisterUIComponentReflection(); // versioned payloads read the type's data version
    UICanvasComponent a;
    a.document.SetId(Guid{1, 2});
    a.theme.SetId(Guid{3, 4});
    a.order = 7;
    a.visible = false;
    a.interactive = false;
    a.scalerMode = CanvasScalerMode::ReferenceResolution;
    a.referenceResolution = Float2{1280.0f, 800.0f};
    a.renderMode = CanvasRenderMode::RenderTexture;
    a.renderTextureWidth = 640;
    a.renderTextureHeight = 360;

    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        BeginVersionedPayload(writer, TypeOf<UICanvasComponent>());
        Serialize(writer, a);
        EndVersionedPayload(writer);
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    UICanvasComponent b;
    {
        BinarySerializer reader(buffer, SerializeMode::Read);
        BeginVersionedPayload(reader, TypeOf<UICanvasComponent>());
        Serialize(reader, b);
        EndVersionedPayload(reader);
    }
    CHECK(b.document.id == a.document.id);
    CHECK(b.theme.id == a.theme.id);
    CHECK(b.order == 7);
    CHECK_FALSE(b.visible);
    CHECK_FALSE(b.interactive);
    CHECK(b.scalerMode == CanvasScalerMode::ReferenceResolution);
    CHECK(b.referenceResolution.x == doctest::Approx(1280.0f));
    CHECK(b.renderMode == CanvasRenderMode::RenderTexture); // v2 fields
    CHECK(b.renderTextureWidth == 640u);
    CHECK(b.renderTextureHeight == 360u);
}

TEST_CASE("ui.subsystem: billboards project through the scene camera and park behind it")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    (void)ui;
    // The camera manager comes from the render subsystem normally; add it directly here.
    ctx.Startup();
    scene::Scene* scene = sm.CreateScene(u8"world");
    scene->AddSystem<engine::render::CameraComponentManager>();

    // A camera at origin looking down -Z (identity rotation), and two anchors.
    scene::EntityHandle cam = scene->CreateEntity(u8"cam");
    scene->GetSystem<engine::render::CameraComponentManager>()->Add(cam);
    scene::EntityHandle front = scene->CreateEntity(u8"front");
    scene->SetLocalPosition(front, Float3{0.0f, 0.0f, -10.0f});
    scene::EntityHandle behind = scene->CreateEntity(u8"behind");
    scene->SetLocalPosition(behind, Float3{0.0f, 0.0f, 10.0f});
    scene->UpdateTransforms();

    auto* billboards = scene->GetSystem<UIBillboardComponentManager>();
    REQUIRE(billboards != nullptr);
    UIBillboardComponent& a = billboards->Add(front);
    a.document = MakeDocument(u8"<Label id=\"name-a\" text=\"A\"/>");
    UIBillboardComponent& b = billboards->Add(behind);
    b.document = MakeDocument(u8"<Label id=\"name-b\" text=\"B\"/>");

    ctx.BeginFrame(1.0f / 60.0f); // instantiate
    REQUIRE(a.root.Get() != nullptr);
    REQUIRE(b.root.Get() != nullptr);

    // The overlay-role split made the per-view sync directly testable: drive it with a
    // synthetic view whose VP has clip.w = -z_view (camera at origin looking down -Z),
    // the shape every real perspective produces. front (z=-10) is on-axis -> centered;
    // behind (z=+10) gets clip.w < 0 -> parks off-screen.
    foundation::render::SceneOverlayView view;
    view.sceneKey = scene;
    view.viewProjection = Float4x4::Identity();
    view.viewProjection(2, 3) = -1.0f; // clip.w = -z (row-vector convention)
    view.viewProjection(3, 3) = 0.0f;
    view.targetWidth = 800;
    view.targetHeight = 600;
    view.viewportWidth = 800;
    view.viewportHeight = 600;
    ui->UpdateSceneView(*scene, view);

    const foundation::ui::LayoutStyle& lpFront = a.root->Layout();
    const foundation::ui::LayoutStyle& lpBehind = b.root->Layout();
    CHECK(lpFront.Left.Value() == doctest::Approx(400.0f)); // on-axis -> target center
    CHECK(lpFront.Top.Value() == doctest::Approx(300.0f));
    CHECK(lpBehind.Left.Value() == doctest::Approx(-10000.0f)); // behind the camera -> parked
    CHECK(lpBehind.Top.Value() == doctest::Approx(-10000.0f));

    // Sub-rect view (split-screen half): billboards land in VIEWPORT pixels - the VG
    // viewport seam places the whole root at the view's rect, so the on-axis anchor
    // centers within the HALF, not the full target.
    view.viewportX = 400;
    view.viewportWidth = 400;
    view.viewportHeight = 300;
    ui->UpdateSceneView(*scene, view);
    CHECK(a.root->Layout().Left.Value() == doctest::Approx(200.0f)); // center of the 400x300 half
    CHECK(a.root->Layout().Top.Value() == doctest::Approx(150.0f));

    // Scene isolation is structural now: another scene's canvas parents into ITS root.
    scene::Scene* other = sm.CreateScene(u8"other");
    auto* otherCanvases = other->GetSystem<UICanvasComponentManager>();
    scene::EntityHandle e = other->CreateEntity(u8"hud");
    UICanvasComponent& canvas = otherCanvases->Add(e);
    canvas.document = MakeDocument(u8"<Label id=\"x\" text=\"other\"/>");
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(canvas.root.Get() != nullptr);
    CHECK(Cast<ViewGroup>(ui->SceneRoot(*other))->FindByName(u8"x") != nullptr);
    CHECK(Cast<ViewGroup>(ui->SceneRoot(*scene))->FindByName(u8"x") == nullptr);

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: the scene-less screen tier survives scene swaps and stays topmost")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    scene::Scene* scene = sm.CreateScene(u8"level");
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();
    scene::EntityHandle e = scene->CreateEntity(u8"hud");
    UICanvasComponent& canvas = canvases->Add(e);
    canvas.document = MakeDocument(u8"<Label id=\"hud\" text=\"HUD\"/>");

    RefPtr<UIDocument> loading = MakeRef<UIDocument>(DefaultAllocator());
    loading->markup = String(u8"<Panel><Label id=\"loading\" text=\"Loading...\"/></Panel>");
    RefPtr<View> overlay = ui->PushScreenOverlay(*loading);
    REQUIRE(overlay.Get() != nullptr);
    CHECK(ui->ScreenOverlayCount() == 1);

    ctx.BeginFrame(1.0f / 60.0f);

    // The tier split keeps the screen root scene-free: the canvas lives in ITS scene's
    // root; the overlay rides the screen root (drawn per window target, above scenes).
    CHECK(Cast<ViewGroup>(ui->ScreenRoot())->FindByName(u8"loading") != nullptr);
    CHECK(Cast<ViewGroup>(ui->ScreenRoot())->FindByName(u8"hud") == nullptr);
    CHECK(Cast<ViewGroup>(ui->SceneRoot(*scene))->FindByName(u8"hud") != nullptr);

    // Destroying the scene kills its root+canvas - the GLOBAL overlay survives.
    sm.DestroyScene(scene);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->ScreenOverlayCount() == 1);
    CHECK(Cast<ViewGroup>(ui->ScreenRoot())->FindByName(u8"loading") != nullptr);

    ui->RemoveScreenOverlay(overlay.Get());
    CHECK(ui->ScreenOverlayCount() == 0);
    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: an EMPTY overlay layer never blocks canvas hit-testing")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    scene::Scene* scene = sm.CreateScene(u8"level");
    scene::EntityHandle e = scene->CreateEntity(u8"hud");
    UICanvasComponent& canvas = scene->GetSystem<UICanvasComponentManager>()->Add(e);
    canvas.document = MakeDocument(u8"<Flex direction=\"vertical\"><Button id=\"btn\" text=\"hit "
                                   u8"me\" width=\"200\" height=\"40\"/></Flex>");
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(canvas.root.Get() != nullptr);

    // Lay out both tiers at a known size: the scene root holds the button; the EMPTY
    // screen tier must not intercept anything above it.
    RootView* sceneRoot = ui->SceneRoot(*scene);
    RootView* screenRoot = ui->ScreenRoot();
    REQUIRE(sceneRoot != nullptr);
    sceneRoot->ViewportSize = Float2{800.0f, 600.0f};
    screenRoot->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(sceneRoot);
    ui->Context().UpdateRootView(screenRoot);
    View* screenHit = screenRoot->HitTest(Float2{20.0f, 20.0f});
    CHECK((screenHit == nullptr || screenHit == screenRoot)); // empty tier: transparent
    View* hit = sceneRoot->HitTest(Float2{20.0f, 20.0f});
    REQUIRE(hit != nullptr);
    CHECK(hit->Name.AsView() == u8"btn");

    // With a pushed overlay the screen tier DOES block (a modal loading screen must).
    RefPtr<UIDocument> loading = MakeRef<UIDocument>(DefaultAllocator());
    loading->markup =
        String(u8"<Panel width=\"800\" height=\"600\"><Label text=\"Loading\"/></Panel>");
    RefPtr<View> overlay = ui->PushScreenOverlay(*loading);
    REQUIRE(overlay.Get() != nullptr);
    ctx.BeginFrame(1.0f / 60.0f);
    ui->Context().UpdateRootView(screenRoot);
    View* blocked = screenRoot->HitTest(Float2{20.0f, 20.0f});
    REQUIRE(blocked != nullptr);
    CHECK(blocked != screenRoot); // the occupied overlay layer eats the point

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: a PASSIVE screen overlay (badge) never turns the layer modal")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    // A watermark/badge pushed with IsHitTestVisible = false must not flip the
    // full-window overlay layer into a click shield (the WebScene HUD regression);
    // only a hit-testable overlay (modal menu) claims input.
    RefPtr<UIDocument> badgeDoc = MakeRef<UIDocument>(DefaultAllocator());
    badgeDoc->markup = String(u8"<Panel width=\"80\" height=\"24\"><Label "
                              u8"text=\"badge\"/></Panel>");
    RefPtr<View> badge = ui->PushScreenOverlay(*badgeDoc);
    REQUIRE(badge.Get() != nullptr);
    badge->IsHitTestVisible = false;
    CHECK(!ui->OverlayLayerWantsInput());
    ctx.BeginFrame(1.0f / 60.0f); // PumpInput applies the gate to the layer

    RootView* screenRoot = ui->ScreenRoot();
    screenRoot->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(screenRoot);
    // Probe OUTSIDE the 80x24 badge: the layer itself must not answer. (Hit-test
    // visibility is self-only by contract - ToastHost/tool-float layers rely on children
    // staying hittable - so the badge's own content still answers inside its box.)
    View* hit = screenRoot->HitTest(Float2{400.0f, 300.0f});
    CHECK((hit == nullptr || hit == screenRoot)); // transparent despite the badge

    // A modal overlay alongside it flips the layer back to input-claiming.
    RefPtr<UIDocument> menuDoc = MakeRef<UIDocument>(DefaultAllocator());
    menuDoc->markup =
        String(u8"<Panel width=\"800\" height=\"600\"><Label text=\"menu\"/></Panel>");
    RefPtr<View> menu = ui->PushScreenOverlay(*menuDoc);
    REQUIRE(menu.Get() != nullptr);
    CHECK(ui->OverlayLayerWantsInput());

    ctx.Shutdown();
}

namespace
{
    // Minimal pad/provider fakes for the nav pump (the input tests' pattern).
    struct NavFakePad final : foundation::shell::IGamepad
    {
        bool down[static_cast<u32>(foundation::shell::GamepadButton::Count)] = {};
        bool pressed[static_cast<u32>(foundation::shell::GamepadButton::Count)] = {};
        bool released[static_cast<u32>(foundation::shell::GamepadButton::Count)] = {};
        f32 axes[static_cast<u32>(foundation::shell::GamepadAxis::Count)] = {};
        [[nodiscard]] i32 Index() const override { return 0; }
        [[nodiscard]] StringView Name() const override { return u8"fake"; }
        [[nodiscard]] bool Connected() const override { return true; }
        [[nodiscard]] bool IsButtonDown(foundation::shell::GamepadButton b) const override
        {
            return down[static_cast<u32>(b)];
        }
        [[nodiscard]] bool IsButtonPressed(foundation::shell::GamepadButton b) const override
        {
            return pressed[static_cast<u32>(b)];
        }
        [[nodiscard]] bool IsButtonReleased(foundation::shell::GamepadButton b) const override
        {
            return released[static_cast<u32>(b)];
        }
        [[nodiscard]] f32 Axis(foundation::shell::GamepadAxis a) const override
        {
            return axes[static_cast<u32>(a)];
        }
        void SetRumble(f32, f32, u32) override {}
    };

    struct NavFakeDevices final : foundation::input::IInputSourceProvider
    {
        NavFakePad pad;
        [[nodiscard]] foundation::shell::IMouse* Mouse() override { return nullptr; }
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() override { return nullptr; }
        [[nodiscard]] i32 GamepadCount() const override { return 1; }
        [[nodiscard]] foundation::shell::IGamepad* Gamepad(i32 index) override
        {
            return index == 0 ? &pad : nullptr;
        }
        Array<foundation::shell::InputEvent> events; // this frame's key stream
        [[nodiscard]] Span<const foundation::shell::InputEvent> Events() override
        {
            return Span<const foundation::shell::InputEvent>(events.Data(), events.Size());
        }
        void PressKey(foundation::shell::KeyCode key)
        {
            events.Clear();
            foundation::shell::InputEvent event{};
            event.kind = foundation::shell::InputEventKind::KeyDown;
            event.key = key;
            events.PushBack(event);
        }
    };
}

TEST_CASE("ui.subsystem: an arrow with nothing focused lands on the menu, and does not also move on")
{
    // A click on empty space clears the UI's focus, and an arrow key with nothing focused went
    // nowhere, so a menu could not be reached from the keyboard again (Sedulous fb39977b).
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    NavFakeDevices devices;
    input->SetSourceProvider(&devices);

    scene::Scene* scene = sm.CreateScene(u8"menu");
    scene::EntityHandle e = scene->CreateEntity(u8"pause");
    UICanvasComponent& canvas = scene->GetSystem<UICanvasComponentManager>()->Add(e);
    canvas.document =
        MakeDocument(u8"<Flex direction=\"vertical\" spacing=\"4\">"
                     u8"<Button id=\"top\" text=\"Top\" width=\"200\" height=\"36\"/>"
                     u8"<Button id=\"bottom\" text=\"Bottom\" width=\"200\" height=\"36\"/>"
                     u8"</Flex>");
    ctx.BeginFrame(1.0f / 60.0f);
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    root->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(root);
    FocusManager* focus = ui->Context().GetFocusManager();
    REQUIRE(focus != nullptr);
    CHECK(focus->FocusedView() == nullptr);

    // Down with nothing focused lands on the first item; that press does not also move on.
    devices.PressKey(foundation::shell::KeyCode::Down);
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(focus->FocusedView() != nullptr);
    CHECK(focus->FocusedView()->Name.AsView() == u8"top");
    CHECK(focus->Source() == FocusSource::Keyboard);

    // The next Down moves on as before.
    devices.PressKey(foundation::shell::KeyCode::Down);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(focus->FocusedView()->Name.AsView() == u8"bottom");

    // Focus cleared (a click on empty space): an arrow finds the menu again.
    focus->ClearFocus();
    devices.PressKey(foundation::shell::KeyCode::Up);
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(focus->FocusedView() != nullptr);
    CHECK(focus->FocusedView()->Name.AsView() == u8"top");

    devices.events.Clear();
    ctx.Shutdown();
}

namespace
{
    // A mouse a case moves: MoveTo sets the motion as a real mouse reports it; a position set
    // directly (the platform's first report of where the cursor rests) has none.
    struct MovingMouse final : foundation::shell::IMouse
    {
        f32 x = 0.0f, y = 0.0f, dx = 0.0f, dy = 0.0f;
        void MoveTo(f32 nx, f32 ny)
        {
            dx = nx - x;
            dy = ny - y;
            x = nx;
            y = ny;
        }
        [[nodiscard]] f32 X() const override { return x; }
        [[nodiscard]] f32 Y() const override { return y; }
        [[nodiscard]] f32 GlobalX() const override { return x; }
        [[nodiscard]] f32 GlobalY() const override { return y; }
        [[nodiscard]] f32 DeltaX() const override { return dx; }
        [[nodiscard]] f32 DeltaY() const override { return dy; }
        [[nodiscard]] f32 ScrollX() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollY() const override { return 0.0f; }
        [[nodiscard]] bool IsButtonDown(foundation::shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonPressed(foundation::shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonReleased(foundation::shell::MouseButton) const override { return false; }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return false; }
        void SetCursorVisible(bool) override {}
        void SetCursor(foundation::shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };

    // A pad and a mouse behind one provider: a handheld, whose cursor is hidden but still has a
    // position.
    struct PadAndMouseDevices final : foundation::input::IInputSourceProvider
    {
        NavFakePad pad;
        MovingMouse mouse;
        [[nodiscard]] foundation::shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() override { return nullptr; }
        [[nodiscard]] i32 GamepadCount() const override { return 1; }
        [[nodiscard]] foundation::shell::IGamepad* Gamepad(i32 index) override { return index == 0 ? &pad : nullptr; }
    };
}

TEST_CASE("ui.subsystem: a pad steps a focused slider, and moves past it along the other axis")
{
    // Sedulous 8c8c0bd4: a pad's directions reach the focused control as the arrow keys do, so
    // right steps a slider instead of moving off it, and down still moves to the next control: a
    // settings menu of volume sliders works from a pad.
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    NavFakeDevices devices;
    input->SetSourceProvider(&devices);

    scene::Scene* scene = sm.CreateScene(u8"settings");
    scene::EntityHandle e = scene->CreateEntity(u8"menu");
    UICanvasComponent& canvas = scene->GetSystem<UICanvasComponentManager>()->Add(e);
    canvas.document =
        MakeDocument(u8"<Flex direction=\"vertical\" spacing=\"4\">"
                     u8"<Slider id=\"volume\" min=\"0\" max=\"100\" value=\"50\" step=\"10\" "
                     u8"width=\"200\" height=\"24\"/>"
                     u8"<Button id=\"back\" text=\"Back\" width=\"200\" height=\"36\"/>"
                     u8"</Flex>");
    ctx.BeginFrame(1.0f / 60.0f);
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    root->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(root);
    Slider* volume = Cast<ViewGroup>(canvas.root.Get())->FindByName<Slider>(u8"volume");
    REQUIRE(volume != nullptr);
    FocusManager* focus = ui->Context().GetFocusManager();
    focus->SetFocus(volume);

    const auto press = [&](foundation::shell::GamepadButton button)
    {
        devices.pad.down[static_cast<u32>(button)] = true;
        ctx.BeginFrame(1.0f / 60.0f);
        devices.pad.down[static_cast<u32>(button)] = false;
        ctx.BeginFrame(1.0f / 60.0f);
    };
    press(foundation::shell::GamepadButton::DPadRight);
    CHECK(volume->Value.Value() == doctest::Approx(60.0f)); // right steps the slider
    CHECK(focus->FocusedView() == volume);                  // and stays on it
    press(foundation::shell::GamepadButton::DPadDown);
    REQUIRE(focus->FocusedView() != nullptr);
    CHECK(focus->FocusedView()->Name.AsView() == u8"back"); // down moves on
    CHECK(volume->Value.Value() == doctest::Approx(60.0f));

    input->SetSourceProvider(nullptr);
    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: only a pointer in use hovers, and a pad's focus shows")
{
    // Sedulous 51756d2a and 773b2379: on the Steam Deck the hidden cursor rested over a menu's
    // last button and lit it like focus, while the real focus, set by the screen, drew no ring.
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    PadAndMouseDevices devices;
    input->SetSourceProvider(&devices);

    scene::Scene* scene = sm.CreateScene(u8"menu");
    scene::EntityHandle e = scene->CreateEntity(u8"pause");
    UICanvasComponent& canvas = scene->GetSystem<UICanvasComponentManager>()->Add(e);
    canvas.document =
        MakeDocument(u8"<Flex direction=\"vertical\" spacing=\"4\">"
                     u8"<Button id=\"top\" text=\"Top\" width=\"200\" height=\"36\"/>"
                     u8"<Button id=\"bottom\" text=\"Bottom\" width=\"200\" height=\"36\"/>"
                     u8"</Flex>");
    // The cursor rests over the bottom button from the very first frame: a position with no
    // motion, as the platform first reports one.
    devices.mouse.x = 100.0f;
    devices.mouse.y = 58.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    root->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(root);
    Button* top = Cast<ViewGroup>(canvas.root.Get())->FindByName<Button>(u8"top");
    Button* bottom = Cast<ViewGroup>(canvas.root.Get())->FindByName<Button>(u8"bottom");
    REQUIRE(top != nullptr);
    REQUIRE(bottom != nullptr);

    // The screen's default focus, set by itself.
    FocusManager* focus = ui->Context().GetFocusManager();
    focus->SetFocus(top);
    ctx.BeginFrame(1.0f / 60.0f);
    ctx.BeginFrame(1.0f / 60.0f);
    InputManager* inputManager = ui->Context().GetInputManager();
    CHECK_FALSE(inputManager->HoveredId() == bottom->Id); // a pointer that never moved hovers nothing
    CHECK(top->IsFocusVisible());                        // with a pad connected, the default focus shows

    // The pointer moves: it is in use, and hovers.
    devices.mouse.MoveTo(100.0f, 60.0f);
    ctx.BeginFrame(1.0f / 60.0f);
    devices.mouse.dx = devices.mouse.dy = 0.0f;
    CHECK(inputManager->HoveredId() == bottom->Id); // a moved pointer hovers

    // The pad takes over: the hover goes, and stays gone while the pointer rests.
    devices.pad.down[static_cast<u32>(foundation::shell::GamepadButton::DPadDown)] = true;
    ctx.BeginFrame(1.0f / 60.0f);
    devices.pad.down[static_cast<u32>(foundation::shell::GamepadButton::DPadDown)] = false;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(inputManager->HoveredId() == bottom->Id); // pad input drops the hover

    input->SetSourceProvider(nullptr);
    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: gamepad dpad moves focus with hold-repeat; South activates")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    NavFakeDevices devices;
    input->SetSourceProvider(&devices);

    scene::Scene* scene = sm.CreateScene(u8"menu");
    scene::EntityHandle e = scene->CreateEntity(u8"pause");
    UICanvasComponent& canvas = scene->GetSystem<UICanvasComponentManager>()->Add(e);
    canvas.document =
        MakeDocument(u8"<Flex direction=\"vertical\" spacing=\"4\">"
                     u8"<Button id=\"top\" text=\"Top\" width=\"200\" height=\"36\"/>"
                     u8"<Button id=\"bottom\" text=\"Bottom\" width=\"200\" height=\"36\"/>"
                     u8"</Flex>");
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(canvas.root.Get() != nullptr);
    // The menu lives in the SCENE root; with no pointer and no overlay, the pump routes
    // input there (pad-only nav must reach a pause menu no pointer ever hovered).
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    root->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(root);
    CHECK(ui->Context().ActiveInputRoot() == root);

    FocusManager* focus = ui->Context().GetFocusManager();
    REQUIRE(focus != nullptr);
    CHECK(focus->FocusedView() == nullptr);

    // First Down press bootstraps focus to the first focusable...
    devices.pad.down[static_cast<u32>(foundation::shell::GamepadButton::DPadDown)] = true;
    ctx.BeginFrame(1.0f / 60.0f);
    ui->Context().UpdateRootView(root);
    REQUIRE(focus->FocusedView() != nullptr);
    CHECK(focus->FocusedView()->Name.AsView() == u8"top");

    // ...held: no move until the initial repeat delay elapses, then it advances.
    ctx.BeginFrame(0.1f);
    CHECK(focus->FocusedView()->Name.AsView() == u8"top");
    ctx.BeginFrame(0.35f); // crosses the 0.4s initial delay
    CHECK(focus->FocusedView()->Name.AsView() == u8"bottom");
    devices.pad.down[static_cast<u32>(foundation::shell::GamepadButton::DPadDown)] = false;
    ctx.BeginFrame(1.0f / 60.0f);

    // South = Submit: the focused button activates through the Return path, on the RELEASE, so a
    // button that resumes play does not leave its press for gameplay (Sedulous 7af2c51f).
    const u32 south = static_cast<u32>(foundation::shell::GamepadButton::South);
    bool clicked = false;
    Cast<ViewGroup>(canvas.root.Get())
        ->FindByName<Button>(u8"bottom")
        ->OnClick.Add([&clicked](ButtonBase*) { clicked = true; });
    devices.pad.pressed[south] = true;
    devices.pad.down[south] = true;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(clicked); // the press alone does not activate
    devices.pad.pressed[south] = false;
    devices.pad.down[south] = false;
    devices.pad.released[south] = true;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(clicked); // the release does
    devices.pad.released[south] = false;

    // A press that went down on another view does not click where it lets go: a jump held into
    // a menu must not activate the button focus lands on.
    bool topClicked = false;
    Cast<ViewGroup>(canvas.root.Get())
        ->FindByName<Button>(u8"top")
        ->OnClick.Add([&topClicked](ButtonBase*) { topClicked = true; });
    clicked = false;
    devices.pad.pressed[south] = true;
    ctx.BeginFrame(1.0f / 60.0f); // down on "bottom"
    devices.pad.pressed[south] = false;
    devices.pad.down[static_cast<u32>(foundation::shell::GamepadButton::DPadUp)] = true;
    ctx.BeginFrame(1.0f / 60.0f); // focus moves to "top" while held
    devices.pad.down[static_cast<u32>(foundation::shell::GamepadButton::DPadUp)] = false;
    CHECK(focus->FocusedView()->Name.AsView() == u8"top");
    devices.pad.released[south] = true;
    ctx.BeginFrame(1.0f / 60.0f);
    devices.pad.released[south] = false;
    CHECK_FALSE(topClicked); // let go on another view, nothing activates
    CHECK_FALSE(clicked);

    // An OCCUPIED screen tier is modal: input routing flips to the screen root.
    RefPtr<UIDocument> modal = MakeRef<UIDocument>(DefaultAllocator());
    modal->markup = String(u8"<Panel><Label text=\"Loading\"/></Panel>");
    RefPtr<View> overlay = ui->PushScreenOverlay(*modal);
    REQUIRE(overlay.Get() != nullptr);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == ui->ScreenRoot());
    ui->RemoveScreenOverlay(overlay.Get());
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == root); // back to the scene tier

    ctx.Shutdown();
}

namespace
{
    // A cooked font product of one family with one (empty) size: enough for the service to
    // resolve (family, size) to an entry, which is all the binding test asks.
    RefPtr<foundation::fonts::Font> MakeFontProduct(StringView family)
    {
        RefPtr<foundation::fonts::Font> font = MakeRef<foundation::fonts::Font>(DefaultAllocator());
        font->SetFamily(family);
        foundation::fonts::Font::Entry entry;
        entry.pixelHeight = 24.0f;
        font->AddEntry(Move(entry));
        return font;
    }
}

TEST_CASE("ui.subsystem: other fonts beside the default, each a family a label picks")
{
    // Sedulous 39147576: the project's uiFontIds load beside the default font, each its own
    // family, and the default family stays the default font's whatever order they bind in.
    runtime::Context ctx(foundation::core::DefaultAllocator());
    ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    foundation::fonts::IFontService* fallback = ui->Context().FontService();

    RefPtr<foundation::fonts::Font> body = MakeFontProduct(u8"Body");
    RefPtr<foundation::fonts::Font> title = MakeFontProduct(u8"Title");
    const foundation::fonts::Font* extras[] = {title.Get()};

    // Extras before a default wait for it: the fallback service stays.
    ui->SetExtraFonts(Span<const foundation::fonts::Font* const>(extras, 1));
    CHECK(ui->Context().FontService() == fallback);

    ui->SetDefaultFont(body.Get());
    foundation::fonts::IFontService* service = ui->Context().FontService();
    REQUIRE(service != fallback);
    CHECK(service->DefaultFontFamily() == StringView(u8"Body"));
    foundation::fonts::CachedFont* bodyFont = service->GetFont(u8"Body", 24.0f);
    foundation::fonts::CachedFont* titleFont = service->GetFont(u8"Title", 24.0f);
    REQUIRE(bodyFont != nullptr);
    REQUIRE(titleFont != nullptr);
    CHECK(titleFont != bodyFont);       // a label naming the title family gets the title face
    CHECK(service->GetFont(24.0f) == bodyFont); // no family named: the default's

    // None left: the default alone serves every family.
    ui->SetExtraFonts({});
    CHECK(ui->Context().FontService()->GetFont(u8"Title", 24.0f) ==
          ui->Context().FontService()->GetFont(u8"Body", 24.0f));

    // No default: the fallback again.
    ui->SetDefaultFont(nullptr);
    CHECK(ui->Context().FontService() == fallback);

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: the screen tier lays out at the game's render resolution")
{
    // Sedulous f9b1feb7: with a render resolution, the screen tier lays out at it and draws at
    // the target's resolution (a scale on the root, set as it draws); nought goes back to the
    // target's own size, scale one.
    runtime::Context ctx(foundation::core::DefaultAllocator());
    ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    CHECK_FALSE(ui->HasScreenResolution());
    ui->SetScreenResolution(320, 180, FitMode::Letterbox);
    CHECK(ui->HasScreenResolution());
    ui->ScreenRoot()->DpiScale = 4.0f; // as a fitted draw into a 1280x720 window sets it
    ui->SetScreenResolution(0, 180, FitMode::Letterbox);
    CHECK_FALSE(ui->HasScreenResolution());
    CHECK(ui->ScreenRoot()->DpiScale == doctest::Approx(1.0f));
    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: preview roots live in the context but never on the screen root")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    // Parse failure -> null (the page keeps its last good preview).
    UIDocument bad;
    bad.markup = String(u8"<NoSuchControl>");
    CHECK(ui->CreatePreview(bad).Get() == nullptr);

    UIDocument good;
    good.markup = String(u8"<FlexLayout><Label id=\"pv\" text=\"preview\" /></FlexLayout>");
    RefPtr<RootView> preview = ui->CreatePreview(good);
    REQUIRE(preview.Get() != nullptr);

    // The document instantiated under the preview root...
    REQUIRE(preview->ChildCount() == 1u);
    CHECK(Cast<ViewGroup>(preview.Get())->FindByName(u8"pv") != nullptr);
    // ...which is NOT parented to the screen root (RenderOverlay draws only the screen
    // root, so a preview can never leak into game targets)...
    const u32 screenChildren = static_cast<u32>(ui->ScreenRoot()->ChildCount());
    CHECK(Cast<ViewGroup>(ui->ScreenRoot())->FindByName(u8"pv") == nullptr);
    // ...and frames tick without disturbing the screen tier.
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->ScreenRoot()->ChildCount() == screenChildren);

    ui->DestroyPreview(preview.Get());
    ctx.BeginFrame(1.0f / 60.0f);
    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: canvases stack by order; billboard layer stays below; despawn sweeps")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    scene::Scene* scene = sm.CreateScene(u8"hud");
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();

    // Components added in order a (order 5), b (order 0), c (order 5 - a TIE with a).
    // Component references are transient (the pool compacts) - set fields right after
    // each Add and re-resolve later via Get.
    scene::EntityHandle ea = scene->CreateEntity(u8"a");
    {
        UICanvasComponent& a = canvases->Add(ea);
        a.document = MakeDocument(u8"<Label id=\"canvas-a\" text=\"a\"/>");
        a.order = 5;
    }
    scene::EntityHandle eb = scene->CreateEntity(u8"b");
    {
        UICanvasComponent& b = canvases->Add(eb);
        b.document = MakeDocument(u8"<Label id=\"canvas-b\" text=\"b\"/>");
        b.order = 0;
    }
    scene::EntityHandle ec = scene->CreateEntity(u8"c");
    {
        UICanvasComponent& c = canvases->Add(ec);
        c.document = MakeDocument(u8"<Label id=\"canvas-c\" text=\"c\"/>");
        c.order = 5;
    }

    ctx.BeginFrame(1.0f / 60.0f);
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    REQUIRE(root->ChildCount() == 4u); // billboard layer + 3 canvas hosts

    // Which canvas lives at which child index (child order = draw order, later = on top)?
    auto canvasAt = [root](usize index) -> StringView
    {
        auto* group = Cast<ViewGroup>(root->GetChildAt(index));
        if (group == nullptr)
        {
            return u8"";
        }
        if (group->FindByName(u8"canvas-a") != nullptr)
        {
            return u8"a";
        }
        if (group->FindByName(u8"canvas-b") != nullptr)
        {
            return u8"b";
        }
        if (group->FindByName(u8"canvas-c") != nullptr)
        {
            return u8"c";
        }
        return u8"";
    };
    // The billboard layer is child 0 (below every canvas) and holds no canvas.
    CHECK(canvasAt(0) == u8"");
    // Sorted by order, STABLE for the a/c tie (component order): b(0), a(5), c(5).
    CHECK(canvasAt(1) == u8"b");
    CHECK(canvasAt(2) == u8"a");
    CHECK(canvasAt(3) == u8"c");

    // An order change re-sorts on the next sync: push b on top.
    canvases->Get(eb)->order = 10;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(canvasAt(1) == u8"a");
    CHECK(canvasAt(2) == u8"c");
    CHECK(canvasAt(3) == u8"b");

    // Despawning the entity sweeps its host out of the scene root (menus close on
    // despawn even though component managers have no destroy hook).
    scene->DestroyEntity(ec);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(root->ChildCount() == 3u);
    CHECK(Cast<ViewGroup>(root)->FindByName(u8"canvas-c") == nullptr);
    CHECK(canvasAt(1) == u8"a");
    CHECK(canvasAt(2) == u8"b");

    ctx.Shutdown();
}

TEST_CASE(
    "ui.subsystem: ReferenceResolution scaler lays out at the reference size and scales to fit")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    scene::Scene* scene = sm.CreateScene(u8"menu");
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();
    scene::EntityHandle e = scene->CreateEntity(u8"hud");
    {
        UICanvasComponent& c = canvases->Add(e);
        c.document = MakeDocument(u8"<Flex direction=\"vertical\"><Button id=\"btn\" text=\"go\" "
                                  u8"width=\"200\" height=\"40\"/></Flex>");
        c.scalerMode = CanvasScalerMode::ReferenceResolution;
        c.referenceResolution = Float2{1600.0f, 900.0f};
    }
    ctx.BeginFrame(1.0f / 60.0f);
    UICanvasComponent* c = canvases->Get(e);
    REQUIRE(c != nullptr);
    REQUIRE(c->root.Get() != nullptr);

    // Lay the scene root out at a smaller, differently-proportioned viewport.
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    root->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(root);

    // The document laid out at the REFERENCE size, uniformly scaled by
    // min(800/1600, 600/900) = 0.5 and centered (letterbox: 75px top/bottom).
    CHECK(c->root->Width() == doctest::Approx(1600.0f));
    CHECK(c->root->Height() == doctest::Approx(900.0f));
    CHECK(c->root->Transform.Scale.x == doctest::Approx(0.5f));
    CHECK(c->root->Transform.Scale.y == doctest::Approx(0.5f));
    CHECK(c->root->Bounds.x == doctest::Approx(0.0f));
    CHECK(c->root->Bounds.y == doctest::Approx(75.0f));

    // Hit-testing follows the transform: reference-space (100, 20) draws at
    // (50, 75 + 10) - the button is hit there, and the letterbox bar is empty.
    View* hit = root->HitTest(Float2{50.0f, 85.0f});
    REQUIRE(hit != nullptr);
    CHECK(hit->Name.AsView() == u8"btn");
    View* bar = root->HitTest(Float2{50.0f, 30.0f});
    CHECK((bar == nullptr || bar == root));

    // Switching back to ConstantPixel restores 1:1 layout on the next sync.
    c->scalerMode = CanvasScalerMode::ConstantPixel;
    ctx.BeginFrame(1.0f / 60.0f);
    ui->Context().UpdateRootView(root);
    c = canvases->Get(e);
    CHECK(c->root->Width() == doctest::Approx(800.0f));
    CHECK(c->root->Transform.Scale.x == doctest::Approx(1.0f));
    View* direct = root->HitTest(Float2{100.0f, 20.0f});
    REQUIRE(direct != nullptr);
    CHECK(direct->Name.AsView() == u8"btn");

    ctx.Shutdown();
}

namespace
{
    // An event-first provider fake: no polled devices, just this frame's tagged stream
    // (the shape ShellInputSource/GameViewportInputSource produce for key/text).
    struct EventFakeDevices final : foundation::input::IInputSourceProvider
    {
        Array<foundation::shell::InputEvent> events;
        [[nodiscard]] foundation::shell::IMouse* Mouse() override { return nullptr; }
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() override { return nullptr; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] foundation::shell::IGamepad* Gamepad(i32) override { return nullptr; }
        [[nodiscard]] Span<const foundation::shell::InputEvent> Events() override
        {
            return {events.Data(), events.Size()};
        }

        void PushKey(foundation::shell::InputEventKind kind, foundation::shell::KeyCode key)
        {
            foundation::shell::InputEvent e;
            e.kind = kind;
            e.key = key;
            events.PushBack(e);
        }
        void PushText(StringView text)
        {
            foundation::shell::InputEvent e;
            e.kind = foundation::shell::InputEventKind::TextInput;
            usize i = 0;
            for (; i < text.Size() && i < 31; ++i)
            {
                e.text[i] = text[i];
            }
            e.text[i] = 0;
            events.PushBack(e);
        }
    };
}

TEST_CASE("ui.subsystem: key/text events reach a focused game EditText; IME follows focus")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    EventFakeDevices devices;
    input->SetSourceProvider(&devices);

    // The player-window IME target (headless stand-in).
    foundation::shell::NullWindow window(1, foundation::shell::WindowSettings{});
    ui->SetTextInputTarget(&window);

    scene::Scene* scene = sm.CreateScene(u8"menu");
    scene::EntityHandle e = scene->CreateEntity(u8"form");
    {
        UICanvasComponent& c = scene->GetSystem<UICanvasComponentManager>()->Add(e);
        c.document = MakeDocument(u8"<Flex direction=\"vertical\">"
                                  u8"<EditText id=\"name-field\" width=\"200\" height=\"30\"/>"
                                  u8"</Flex>");
    }
    ctx.BeginFrame(1.0f / 60.0f);
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    root->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(root);

    auto* edit = Cast<ViewGroup>(root)->FindByName<EditText>(u8"name-field");
    REQUIRE(edit != nullptr);

    // Nothing focused: no IME, keyboard not consumed.
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(window.IsTextInputActive());
    CHECK_FALSE(input->Runtime().GetConsumptionMask().keyboard);

    // Focus the field: the next pump starts platform text input (WantsTextInput went
    // on) and publishes the keyboard consumption class.
    ui->Context().GetFocusManager()->SetFocus(edit);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(window.IsTextInputActive());
    CHECK(input->Runtime().GetConsumptionMask().keyboard);

    // Text events flow through the provider's event stream into the editor...
    devices.PushText(u8"hi");
    ctx.BeginFrame(1.0f / 60.0f);
    devices.events.Clear();
    CHECK(edit->Text() == u8"hi");

    // ...as do ordered key events (Backspace erases the last character).
    devices.PushKey(foundation::shell::InputEventKind::KeyDown, foundation::shell::KeyCode::Backspace);
    devices.PushKey(foundation::shell::InputEventKind::KeyUp, foundation::shell::KeyCode::Backspace);
    ctx.BeginFrame(1.0f / 60.0f);
    devices.events.Clear();
    CHECK(edit->Text() == u8"h");

    // Dropping focus stops text input and releases the keyboard class.
    ui->Context().GetFocusManager()->ClearFocus();
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(window.IsTextInputActive());
    CHECK_FALSE(input->Runtime().GetConsumptionMask().keyboard);

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: RenderTexture canvases own an offscreen target and stay out of the tiers")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    // Headless GPU: the Null RHI device (texture lifecycle without a real GPU).
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    ui->EnsureRenderReady(device, 2);
    foundation::rhi::null::NullCommandEncoder encoder;

    scene::Scene* scene = sm.CreateScene(u8"world");
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();
    scene::EntityHandle e = scene->CreateEntity(u8"screen");
    {
        UICanvasComponent& c = canvases->Add(e);
        c.document = MakeDocument(u8"<Label id=\"rt-label\" text=\"scoreboard\"/>");
        c.renderMode = CanvasRenderMode::RenderTexture;
        c.renderTextureWidth = 256;
        c.renderTextureHeight = 128;
    }
    const usize rootsBefore = ui->Context().RootViewCount(); // screen + scene roots
    ctx.BeginFrame(1.0f / 60.0f);
    UICanvasComponent* c = canvases->Get(e);
    REQUIRE(c != nullptr);
    REQUIRE(c->root.Get() != nullptr);
    REQUIRE(c->renderRoot.Get() != nullptr);
    CHECK(ui->Context().RootViewCount() == rootsBefore + 1); // the standalone RT root

    // NOT in the overlay tiers: the scene root holds only the billboard layer, and the
    // document is unreachable from it (RT canvases never draw in the overlay pass and
    // never take pointer input - the input pump only probes tier roots).
    RootView* sceneRoot = ui->SceneRoot(*scene);
    REQUIRE(sceneRoot != nullptr);
    CHECK(sceneRoot->ChildCount() == 1u);
    CHECK(Cast<ViewGroup>(sceneRoot)->FindByName(u8"rt-label") == nullptr);
    CHECK(Cast<ViewGroup>(ui->ScreenRoot())->FindByName(u8"rt-label") == nullptr);
    CHECK(Cast<ViewGroup>(c->renderRoot.Get())->FindByName(u8"rt-label") != nullptr);

    // No texture until the host seam runs; then create at the authored size.
    CHECK(c->renderTexture == nullptr);
    ui->RenderCanvasTextures(encoder, 0);
    c = canvases->Get(e);
    REQUIRE(c->renderTexture != nullptr);
    REQUIRE(c->renderTextureView != nullptr);
    CHECK(c->renderTexture->desc.width == 256u);
    CHECK(c->renderTexture->desc.height == 128u);
    CHECK(ui->CanvasRenderTextureView(*scene, e) == c->renderTextureView);
    // The root laid out at the texture size.
    CHECK(c->renderRoot->ViewportSize.x == doctest::Approx(256.0f));
    CHECK(c->renderRoot->ViewportSize.y == doctest::Approx(128.0f));

    // Resize: the target recreates at the new size. (A fresh UI frame first - the
    // seam runs at most once per frame so co-hosted editor pages share one draw.)
    c->renderTextureWidth = 512;
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderCanvasTextures(encoder, 1);
    c = canvases->Get(e);
    REQUIRE(c->renderTexture != nullptr);
    CHECK(c->renderTexture->desc.width == 512u);
    CHECK(c->renderTexture->desc.height == 128u);

    // Mode flip back to ScreenOverlay: the standalone root goes away, the document
    // re-parents into the scene root, and the GPU target is swept.
    c->renderMode = CanvasRenderMode::ScreenOverlay;
    ctx.BeginFrame(1.0f / 60.0f);
    c = canvases->Get(e);
    CHECK(c->renderRoot.Get() == nullptr);
    CHECK(c->renderTexture == nullptr);
    CHECK(c->renderTextureView == nullptr);
    CHECK(Cast<ViewGroup>(ui->SceneRoot(*scene))->FindByName(u8"rt-label") != nullptr);
    ui->RenderCanvasTextures(encoder, 0);
    CHECK(ui->CanvasRenderTextureView(*scene, e) == nullptr);

    // And back to RenderTexture, then DESPAWN: the sweep destroys the orphaned target
    // AND unregisters the standalone root from the context (which stores roots
    // non-owning - a stale registration would dangle).
    c->renderMode = CanvasRenderMode::RenderTexture;
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderCanvasTextures(encoder, 0);
    REQUIRE(canvases->Get(e)->renderTexture != nullptr);
    CHECK(ui->Context().RootViewCount() == rootsBefore + 1);
    scene->DestroyEntity(e);
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderCanvasTextures(encoder, 0); // sweeps; must not crash or leak
    CHECK(canvases->Get(e) == nullptr);
    CHECK(ui->Context().RootViewCount() == rootsBefore); // RT root swept with it

    ctx.Shutdown();
}

// A cooked theme carries the vector images its @icon directives name; the game's parse reads them
// from the theme, so svg(name) draws with nothing else loaded.
TEST_CASE("ui.subsystem: a theme's embedded icons draw through svg() in the game UI")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    const StringView reference = u8"{0b9f3c2e-4a7d-4e21-9c55-6a1d2b3c4d5e}";
    UITheme theme;
    theme.stylesheet = Format(u8"@icon heart \"{}\";\n.heart {{ background: svg(heart, tint=#E53935); }}\n"
                              u8".plain {{ background: svg(nothing); }}\n",
                              reference);
    theme.icons.PushBack(UIThemeIcon{String(reference),
                                     String(u8"<svg viewBox=\"0 0 24 24\"><path d=\"M12 21 L3 12 L12 3 L21 12 Z\" "
                                            u8"fill=\"#ffffff\"/></svg>")});
    ui->SetDefaultTheme(&theme);

    auto root = MakeRef<RootView>(DefaultAllocator());
    ui->Context().AddRootView(root.Get());
    auto heart = MakeRef<Panel>(DefaultAllocator());
    heart->AddClass(u8"heart");
    root->AddView(heart.Get());
    auto plain = MakeRef<Panel>(DefaultAllocator());
    plain->AddClass(u8"plain");
    root->AddView(plain.Get());

    Drawable* drawn = heart->ResolveStyleDrawable(StyleProperty::Background);
    REQUIRE(drawn != nullptr);
    CHECK(Cast<SVGDrawable>(drawn) != nullptr);
    CHECK(plain->ResolveStyleDrawable(StyleProperty::Background) == nullptr); // no such icon

    ui->Context().RemoveRootView(root.Get());
    ctx.Shutdown();
}

TEST_CASE(
    "ui.subsystem: the project-default theme swaps the context stylesheet (GameTheme fallback)")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    StyleSheet* builtin = ui->Context().GetStyleSheet();
    REQUIRE(builtin != nullptr); // the built-in GameTheme ships by default

    // A cooked UITheme (validated .sss text) replaces the context stylesheet.
    UITheme theme;
    theme.stylesheet = String(u8"Button { text-color: #ff0000; }");
    ui->SetDefaultTheme(&theme);
    StyleSheet* custom = ui->Context().GetStyleSheet();
    REQUIRE(custom != nullptr);
    CHECK(custom != builtin);

    // Null (nil manifest reference / cleared) restores the built-in GameTheme.
    ui->SetDefaultTheme(nullptr);
    StyleSheet* restored = ui->Context().GetStyleSheet();
    REQUIRE(restored != nullptr);
    CHECK(restored != custom);

    // The built-in LIGHT variant exists alongside GameTheme and differs in palette.
    RefPtr<StyleSheet> light = GameLightTheme::Create(DefaultAllocator());
    CHECK(light.Get() != nullptr);
    CHECK(GameLightTheme::Palette().Background.r !=
          doctest::Approx(GameTheme::Palette().Background.r));

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: removing a canvas or billboard COMPONENT sweeps its tree (user repro)")
{
    // The user's exact repro: add a ui.Canvas in the editor, see it render, REMOVE the
    // component (entity stays) - the UI must disappear. Same for billboards, whose
    // layer needed its own sweep.
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    scene::Scene* scene = sm.CreateScene(u8"level");

    scene::EntityHandle e = scene->CreateEntity(u8"hud");
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();
    canvases->Add(e).document = MakeDocument(u8"<Label id=\"hud-label\" text=\"HUD\"/>");
    auto* billboards = scene->GetSystem<UIBillboardComponentManager>();
    billboards->Add(e).document = MakeDocument(u8"<Label id=\"plate\" text=\"name\"/>");
    ctx.BeginFrame(1.0f / 60.0f);
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    REQUIRE(Cast<ViewGroup>(root)->FindByName(u8"hud-label") != nullptr);
    REQUIRE(Cast<ViewGroup>(root)->FindByName(u8"plate") != nullptr);

    // Remove ONLY the components; the entity survives.
    canvases->Remove(e);
    billboards->Remove(e);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(Cast<ViewGroup>(root)->FindByName(u8"hud-label") == nullptr);
    CHECK(Cast<ViewGroup>(root)->FindByName(u8"plate") == nullptr);

    ctx.Shutdown();
}

namespace
{
    // A pointer-capable provider fake: settable position + button state for the polled
    // pump, plus the tagged event stream - the full shape a real source presents.
    struct PointerFakeMouse final : foundation::shell::IMouse
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        bool buttons[8] = {};
        [[nodiscard]] f32 X() const override { return x; }
        [[nodiscard]] f32 Y() const override { return y; }
        [[nodiscard]] f32 GlobalX() const override { return x; }
        [[nodiscard]] f32 GlobalY() const override { return y; }
        [[nodiscard]] f32 DeltaX() const override { return 0.0f; }
        [[nodiscard]] f32 DeltaY() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollX() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollY() const override { return 0.0f; }
        [[nodiscard]] bool IsButtonDown(foundation::shell::MouseButton b) const override
        {
            return buttons[static_cast<u32>(b) & 7];
        }
        [[nodiscard]] bool IsButtonPressed(foundation::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool IsButtonReleased(foundation::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(foundation::shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };

    struct PointerFakeDevices final : foundation::input::IInputSourceProvider
    {
        PointerFakeMouse mouse;
        bool mousePresent = true; // false = pointer-less frame (pad/keyboard-only path)
        Array<foundation::shell::InputEvent> events;
        [[nodiscard]] foundation::shell::IMouse* Mouse() override
        {
            return mousePresent ? &mouse : nullptr;
        }
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() override { return nullptr; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] foundation::shell::IGamepad* Gamepad(i32) override { return nullptr; }
        [[nodiscard]] Span<const foundation::shell::InputEvent> Events() override
        {
            return {events.Data(), events.Size()};
        }
        void PushText(StringView text)
        {
            foundation::shell::InputEvent e;
            e.kind = foundation::shell::InputEventKind::TextInput;
            usize i = 0;
            for (; i < text.Size() && i < 31; ++i)
            {
                e.text[i] = text[i];
            }
            e.text[i] = 0;
            events.PushBack(e);
        }
        // One full click at the CURRENT position across two pump frames.
        void PressLeft()
        {
            mouse.buttons[static_cast<u32>(foundation::shell::MouseButton::Left)] = true;
        }
        void ReleaseLeft()
        {
            mouse.buttons[static_cast<u32>(foundation::shell::MouseButton::Left)] = false;
        }
    };
}

TEST_CASE("ui.subsystem: a bound source confines routing + consumption to ITS scene")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    PointerFakeDevices devices;
    input->SetSourceProvider(&devices); // un-bound: the AllScenes default applies

    // Two scenes, each with an interactive button at the SAME coordinates - the
    // historical known edge (probing in creation order can hit the wrong scene).
    scene::Scene* sceneA = sm.CreateScene(u8"a");
    scene::EntityHandle ea = sceneA->CreateEntity(u8"hud-a");
    sceneA->GetSystem<UICanvasComponentManager>()->Add(ea).document =
        MakeDocument(u8"<Flex direction=\"vertical\"><Button id=\"btn-a\" text=\"A\" width=\"200\" "
                     u8"height=\"40\"/></Flex>");
    scene::Scene* sceneB = sm.CreateScene(u8"b");
    scene::EntityHandle eb = sceneB->CreateEntity(u8"hud-b");
    sceneB->GetSystem<UICanvasComponentManager>()->Add(eb).document =
        MakeDocument(u8"<Flex direction=\"vertical\">"
                     u8"<Button id=\"btn-b\" text=\"B\" width=\"200\" height=\"40\"/>"
                     u8"<EditText id=\"field-b\" width=\"200\" height=\"30\"/>"
                     u8"</Flex>");

    ctx.BeginFrame(1.0f / 60.0f); // instantiate trees
    RootView* rootA = ui->SceneRoot(*sceneA);
    RootView* rootB = ui->SceneRoot(*sceneB);
    REQUIRE(rootA != nullptr);
    REQUIRE(rootB != nullptr);
    rootA->ViewportSize = Float2{800.0f, 600.0f};
    rootB->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(rootA);
    ui->Context().UpdateRootView(rootB);

    bool clickedA = false;
    bool clickedB = false;
    Cast<ViewGroup>(rootA)->FindByName<Button>(u8"btn-a")->OnClick.Add([&clickedA](ButtonBase*)
                                                                       { clickedA = true; });
    Cast<ViewGroup>(rootB)->FindByName<Button>(u8"btn-b")->OnClick.Add([&clickedB](ButtonBase*)
                                                                       { clickedB = true; });

    // UN-BOUND (AllScenes default): the pointer probe walks scene roots in creation
    // order - scene A wins the overlapping point. (The documented historical edge.)
    devices.mouse.x = 50.0f;
    devices.mouse.y = 20.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == rootA);
    CHECK(input->Runtime().GetConsumptionMask().pointer);

    // BOUND to scene B: the same coordinates now route to B - and ONLY B. The click
    // fires B's button; A's identical button at the identical point never hears it.
    input->SetSourceProvider(&devices, sceneB);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == rootB);
    CHECK(input->Runtime().GetConsumptionMask().pointer);
    devices.PressLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    devices.ReleaseLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(clickedB);
    CHECK_FALSE(clickedA);

    // Pointer-less frames (pad/keyboard-only): the fallback root is the BOUND scene,
    // not the first scene with content.
    devices.mousePresent = false;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == rootB);

    // Keyboard/text follow the binding: focus B's field, stream text, and the
    // keyboard consumption class publishes for the bound scene.
    auto* field = Cast<ViewGroup>(rootB)->FindByName<EditText>(u8"field-b");
    REQUIRE(field != nullptr);
    ui->Context().GetFocusManager()->SetFocus(field);
    devices.PushText(u8"go");
    ctx.BeginFrame(1.0f / 60.0f);
    devices.events.Clear();
    CHECK(field->Text() == u8"go");
    CHECK(input->Runtime().GetConsumptionMask().keyboard);
    ui->Context().GetFocusManager()->ClearFocus();

    // Un-binding (provider kept) returns to the un-bound default: creation order.
    devices.mousePresent = true;
    input->SetSourceProvider(&devices, nullptr);
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == rootA);

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: ScreenTierOnly keeps un-bound input out of scene UI (editor policy)")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    PointerFakeDevices devices;
    input->SetSourceProvider(&devices); // un-bound...
    input->SetUnboundScenePolicy(engine::input::UnboundInputScenePolicy::ScreenTierOnly);

    scene::Scene* scene = sm.CreateScene(u8"editing");
    scene::EntityHandle e = scene->CreateEntity(u8"hud");
    scene->GetSystem<UICanvasComponentManager>()->Add(e).document =
        MakeDocument(u8"<Flex direction=\"vertical\"><Button id=\"btn\" text=\"hud\" width=\"200\" "
                     u8"height=\"40\"/></Flex>");
    ctx.BeginFrame(1.0f / 60.0f);
    RootView* root = ui->SceneRoot(*scene);
    REQUIRE(root != nullptr);
    root->ViewportSize = Float2{800.0f, 600.0f};
    ui->ScreenRoot()->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(root);
    ui->Context().UpdateRootView(ui->ScreenRoot());

    bool clicked = false;
    Cast<ViewGroup>(root)->FindByName<Button>(u8"btn")->OnClick.Add([&clicked](ButtonBase*)
                                                                    { clicked = true; });

    // The HUD renders (tree exists, visible) but is NOT interactive: the pointer over
    // its button neither routes to the scene root nor publishes consumption - an
    // editor pane click at these coordinates stays an editor click.
    devices.mouse.x = 50.0f;
    devices.mouse.y = 20.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == ui->ScreenRoot());
    CHECK_FALSE(input->Runtime().GetConsumptionMask().pointer);
    CHECK_FALSE(ui->PointerOverUI());
    devices.PressLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    devices.ReleaseLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(clicked);

    // The scene-less screen tier is NOT confined: an occupied overlay is modal and
    // fully interactive under the same policy (loading screens/system menus work).
    RefPtr<UIDocument> modal = MakeRef<UIDocument>(DefaultAllocator());
    modal->markup = String(u8"<Flex direction=\"vertical\"><Button id=\"ok\" text=\"OK\" "
                           u8"width=\"200\" height=\"40\"/></Flex>");
    RefPtr<View> overlay = ui->PushScreenOverlay(*modal);
    REQUIRE(overlay.Get() != nullptr);
    ctx.BeginFrame(1.0f / 60.0f);
    ui->Context().UpdateRootView(ui->ScreenRoot());
    bool okClicked = false;
    Cast<ViewGroup>(ui->ScreenRoot())
        ->FindByName<Button>(u8"ok")
        ->OnClick.Add([&okClicked](ButtonBase*) { okClicked = true; });
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == ui->ScreenRoot());
    CHECK(input->Runtime().GetConsumptionMask().pointer);
    devices.PressLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    devices.ReleaseLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(okClicked);
    CHECK_FALSE(clicked); // the scene button under the overlay still never fires
    ui->RemoveScreenOverlay(overlay.Get());

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: RT canvases auto-bind the entity's sprite/decal texture override")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    ui->EnsureRenderReady(device, 2);
    foundation::rhi::null::NullCommandEncoder encoder;

    // The render managers normally come from the RenderSubsystem; add them directly
    // (the camera-manager test's pattern).
    scene::Scene* scene = sm.CreateScene(u8"world");
    scene->AddSystem<engine::render::SpriteComponentManager>();
    scene->AddSystem<engine::render::DecalComponentManager>();
    auto* canvases = scene->GetSystem<UICanvasComponentManager>();
    auto* sprites = scene->GetSystem<engine::render::SpriteComponentManager>();
    auto* decals = scene->GetSystem<engine::render::DecalComponentManager>();

    // One entity carries the RT canvas AND the material components that show it -
    // the declarative contract: same entity = auto-bound, no scripting needed.
    scene::EntityHandle e = scene->CreateEntity(u8"scoreboard");
    {
        UICanvasComponent& c = canvases->Add(e);
        c.document = MakeDocument(u8"<Label id=\"score\" text=\"0 : 0\"/>");
        c.renderMode = CanvasRenderMode::RenderTexture;
        c.renderTextureWidth = 256;
        c.renderTextureHeight = 128;
    }
    sprites->Add(e);
    decals->Add(e);

    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderCanvasTextures(encoder, 0);
    UICanvasComponent* c = canvases->Get(e);
    REQUIRE(c != nullptr);
    REQUIRE(c->renderTextureView != nullptr);
    CHECK(sprites->Get(e)->texture == c->renderTextureView);
    CHECK(decals->Get(e)->texture == c->renderTextureView);

    // Resize recreates the target - the view pointer changes (the reason manual
    // assignment breaks) - and the binder refreshes the overrides the same call.
    // (No pointer-inequality check: an allocator may legitimately reuse the address;
    // the CONTRACT is override == current view, whatever that is.)
    c->renderTextureWidth = 512;
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderCanvasTextures(encoder, 1);
    c = canvases->Get(e);
    REQUIRE(c->renderTextureView != nullptr);
    CHECK(c->renderTexture->desc.width == 512u);
    CHECK(sprites->Get(e)->texture == c->renderTextureView);
    CHECK(decals->Get(e)->texture == c->renderTextureView);

    // Removing the CANVAS un-binds (the target is destroyed - a stale override would
    // dangle); the sprite/decal components themselves survive.
    canvases->Remove(e);
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderCanvasTextures(encoder, 0);
    CHECK(sprites->Get(e)->texture == nullptr);
    CHECK(decals->Get(e)->texture == nullptr);

    ctx.Shutdown();
}

TEST_CASE("ui.worldpanel: ray/uv math - front and back hits, edges, parallel misses")
{
    // Identity-oriented panel at the origin: right = +X, up = +Y, normal = +Z.
    const Float4x4 panel = Float4x4::Identity();
    const Float2 size{2.0f, 1.0f};

    // Straight-on from +Z at the exact center.
    WorldPanelHit hit = RayHitWorldPanel(Float3{0, 0, 5}, Float3{0, 0, -1}, panel, size);
    REQUIRE(hit.hit);
    CHECK(hit.distance == doctest::Approx(5.0f));
    CHECK(hit.uv.x == doctest::Approx(0.5f));
    CHECK(hit.uv.y == doctest::Approx(0.5f));

    // Off-center: +0.5 right, +0.25 up -> u 0.75, v 0.25 (v runs DOWN like UI pixels).
    hit = RayHitWorldPanel(Float3{0.5f, 0.25f, 5}, Float3{0, 0, -1}, panel, size);
    REQUIRE(hit.hit);
    CHECK(hit.uv.x == doctest::Approx(0.75f));
    CHECK(hit.uv.y == doctest::Approx(0.25f));

    // The BACK face hits too (double-sided panels).
    hit = RayHitWorldPanel(Float3{0, 0, -5}, Float3{0, 0, 1}, panel, size);
    CHECK(hit.hit);

    // Beyond the half extent: miss. Parallel to the plane: miss. Behind pointer: miss.
    CHECK_FALSE(RayHitWorldPanel(Float3{1.5f, 0, 5}, Float3{0, 0, -1}, panel, size).hit);
    CHECK_FALSE(RayHitWorldPanel(Float3{0, 0, 5}, Float3{1, 0, 0}, panel, size).hit);
    CHECK_FALSE(RayHitWorldPanel(Float3{0, 0, 5}, Float3{0, 0, 1}, panel, size).hit);

    // Pointer-ray unprojection: the view center looks straight down the camera axis.
    foundation::render::ViewCamera camera;
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 16.0f / 9.0f, 0.1f, 100.0f);
    Float3 origin, direction;
    PointerRayFromCamera(camera, Float2{640.0f, 360.0f}, Float2{1280.0f, 720.0f}, origin,
                         direction);
    CHECK(direction.z < -0.99f); // identity view: forward is -Z
    CHECK(Abs(direction.x) < 0.01f);
    CHECK(Abs(direction.y) < 0.01f);
}

TEST_CASE("ui.worldpanel: instantiates, renders to its target, drives the sprite, and "
          "takes a ray-routed click")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    ui->EnsureRenderReady(device, 2);
    foundation::rhi::null::NullCommandEncoder encoder;

    PointerFakeDevices devices;
    input->SetSourceProvider(&devices);

    scene::Scene* scene = sm.CreateScene(u8"world");
    scene->AddSystem<engine::render::CameraComponentManager>();
    scene->AddSystem<engine::render::SpriteComponentManager>();
    scene::EntityHandle cam = scene->CreateEntity(u8"cam");
    scene->SetLocalPosition(cam, Float3{0.0f, 2.0f, 10.0f});
    scene->GetSystem<engine::render::CameraComponentManager>()->Add(cam);

    scene::EntityHandle e = scene->CreateEntity(u8"kiosk");
    scene->SetLocalPosition(e, Float3{0.0f, 2.0f, 0.0f});
    auto* panels = scene->GetSystem<UIWorldPanelComponentManager>();
    REQUIRE(panels != nullptr);
    UIWorldPanelComponent& panel = panels->Add(e);
    panel.document = MakeDocument(u8"<FrameLayout><Button id=\"kiosk-btn\" text=\"Press\" "
                                  u8"width=\"200\" height=\"100\"/></FrameLayout>");
    panel.sizeMeters = Float2{2.0f, 1.0f};
    panel.pixelsPerMeter = 100.0f; // -> 200x100 target
    scene->UpdateTransforms();

    ctx.BeginFrame(1.0f / 60.0f); // instantiate
    REQUIRE(panel.renderRoot.Get() != nullptr);
    ui->RenderCanvasTextures(encoder, 0);
    CHECK(panel.renderTexture != nullptr);
    CHECK(panel.renderTextureView != nullptr);
    CHECK(panel.renderRoot->ViewportSize.x == doctest::Approx(200.0f));
    // The target carries the 2px transparent edge border on each side (silhouette
    // antialiasing): content 200x100 in a 204x104 texture.
    CHECK(panel.renderTexture->desc.width == 204);
    CHECK(panel.renderTexture->desc.height == 104);

    // The sprite is DRIVEN: auto-added, entity-oriented, texture-bound, and inflated
    // by the border ratio so the CONTENT keeps the authored world size.
    auto* sprite = scene->GetSystem<engine::render::SpriteComponentManager>()->Get(e);
    REQUIRE(sprite != nullptr);
    CHECK(sprite->orientation == engine::render::SpriteOrientation::EntityOriented);
    CHECK(sprite->size.x == doctest::Approx(2.0f * 204.0f / 200.0f));
    CHECK(sprite->size.y == doctest::Approx(1.0f * 104.0f / 100.0f));
    CHECK(sprite->texture == panel.renderTextureView);

    // Route a click: lay the scene root out (the surface size the ray math reads),
    // point at the view center - the camera looks straight at the panel center.
    RootView* sceneRoot = ui->SceneRoot(*scene);
    REQUIRE(sceneRoot != nullptr);
    sceneRoot->ViewportSize = Float2{800.0f, 600.0f};
    bool clicked = false;
    Cast<ViewGroup>(panel.renderRoot.Get())
        ->FindByName<Button>(u8"kiosk-btn")
        ->OnClick.Add([&clicked](ButtonBase*) { clicked = true; });

    devices.mouse.x = 400.0f;
    devices.mouse.y = 300.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == panel.renderRoot.Get());
    CHECK(ui->PointerOverUI());

    devices.mouse.buttons[static_cast<u32>(foundation::shell::MouseButton::Left)] = true;
    ctx.BeginFrame(1.0f / 60.0f);
    devices.mouse.buttons[static_cast<u32>(foundation::shell::MouseButton::Left)] = false;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(clicked);

    // Non-interactive: the ray ignores it; the pointer no longer routes to the panel.
    panel.interactive = false;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() != panel.renderRoot.Get());

    ctx.Shutdown();
}

TEST_CASE("ui.worldpanel: the ray hits under an OBLIQUE camera (the playground pose)")
{
    // Reproduces PhysicsPlayground exactly: fly camera at (8,6,14) yaw 0.5 pitch -0.3,
    // kiosk at (4,1.6,-6) identity-oriented, 1.6x1.0 m. The pointer is placed at the
    // kiosk center's PROJECTED screen position - the ray must come back to uv ~center.
    foundation::render::ViewCamera camera;
    const Quaternion rotation = FromYawPitchRoll(0.5f, -0.3f, 0.0f);
    Float4x4 world = RotationMatrix(rotation);
    world.m[3][0] = 8.0f;
    world.m[3][1] = 6.0f;
    world.m[3][2] = 14.0f;
    camera.view = Inverse(world);
    camera.projection = Float4x4::PerspectiveFovRH(1.04719755f, 1280.0f / 720.0f, 0.1f, 1000.0f);
    camera.position = Float3{8.0f, 6.0f, 14.0f};

    Float4x4 panel = Float4x4::Identity();
    panel.m[3][0] = 4.0f;
    panel.m[3][1] = 1.6f;
    panel.m[3][2] = -6.0f;
    const Float2 sizeMeters{1.6f, 1.0f};

    // Project the panel center to screen pixels.
    const Float4 clip = Float4{4.0f, 1.6f, -6.0f, 1.0f} * camera.ViewProjection();
    REQUIRE(clip.w > 0.0f); // in front of the camera
    const Float2 viewSize{1280.0f, 720.0f};
    const Float2 pointer{(clip.x / clip.w * 0.5f + 0.5f) * viewSize.x,
                         (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * viewSize.y};

    Float3 origin, direction;
    PointerRayFromCamera(camera, pointer, viewSize, origin, direction);
    const WorldPanelHit hit = RayHitWorldPanel(origin, direction, panel, sizeMeters);
    REQUIRE(hit.hit);
    CHECK(hit.uv.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(hit.uv.y == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("ui.subsystem: a stretched full-screen canvas does NOT swallow the pointer "
          "(world panels stay reachable - the kiosk regression)")
{
    // CanvasHostView stretches every document to the viewport; the doc root must stay
    // hit-TRANSPARENT or one HUD eats the pointer everywhere: consumption reads true on
    // empty space (crate clicks die) and the scene root outbids every world panel.
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    ui->EnsureRenderReady(device, 2);
    foundation::rhi::null::NullCommandEncoder encoder;
    PointerFakeDevices devices;
    input->SetSourceProvider(&devices);

    scene::Scene* scene = sm.CreateScene(u8"world");
    scene->AddSystem<engine::render::CameraComponentManager>();
    scene->AddSystem<engine::render::SpriteComponentManager>();
    scene::EntityHandle cam = scene->CreateEntity(u8"cam");
    scene->SetLocalPosition(cam, Float3{0.0f, 2.0f, 10.0f});
    scene->GetSystem<engine::render::CameraComponentManager>()->Add(cam);

    // A PhysicsPlayground-style HUD: stretched root Flex, content in one corner.
    scene::EntityHandle hud = scene->CreateEntity(u8"hud");
    scene->GetSystem<UICanvasComponentManager>()->Add(hud).document =
        MakeDocument(u8"<Flex direction=\"vertical\" align=\"start\" padding=\"12\">"
                     u8"<Button id=\"hud-btn\" text=\"HUD\" width=\"180\" height=\"36\"/></Flex>");

    // And a world panel mid-view.
    scene::EntityHandle kiosk = scene->CreateEntity(u8"kiosk");
    scene->SetLocalPosition(kiosk, Float3{0.0f, 2.0f, 0.0f});
    UIWorldPanelComponent& panel = scene->GetSystem<UIWorldPanelComponentManager>()->Add(kiosk);
    panel.document = MakeDocument(u8"<FrameLayout><Button id=\"kiosk-btn\" text=\"Tap\" "
                                  u8"width=\"200\" height=\"100\"/></FrameLayout>");
    panel.sizeMeters = Float2{2.0f, 1.0f};
    panel.pixelsPerMeter = 100.0f;
    scene->UpdateTransforms();

    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderCanvasTextures(encoder, 0);
    RootView* sceneRoot = ui->SceneRoot(*scene);
    sceneRoot->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(sceneRoot);

    // Pointer over the HUD button: the scene root wins, consumption reads true.
    devices.mouse.x = 30.0f;
    devices.mouse.y = 30.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == sceneRoot);
    CHECK(ui->PointerOverUI());

    // Pointer at the view CENTER (empty HUD space, kiosk dead ahead): the panel is
    // reachable THROUGH the stretched HUD, and the hit consumes the pointer.
    devices.mouse.x = 400.0f;
    devices.mouse.y = 300.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->Context().ActiveInputRoot() == panel.renderRoot.Get());
    CHECK(ui->PointerOverUI());

    // Pointer over truly empty space (no panel behind): NOT consumed - gameplay
    // clicks (crate shoves) pass through.
    panel.visible = false;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(ui->PointerOverUI());

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: a press over EMPTY space never consumes the pointer "
          "(the crosshair-shove regression)")
{
    // A press parks on the RootView so mouse-up still routes - but a root press is NOT
    // UI interaction. The consumption mask must ignore it: before the fix, ANY held
    // click published a consumed mask for the press duration, gating gameplay input
    // (PhysicsPlayground's LMB crate shove polls IsButtonPressed on exactly the press
    // frame, so it was gated 100% of the time while the HUD kept working).
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    PointerFakeDevices devices;
    input->SetSourceProvider(&devices);

    scene::Scene* scene = sm.CreateScene(u8"world");
    scene::EntityHandle hud = scene->CreateEntity(u8"hud");
    scene->GetSystem<UICanvasComponentManager>()->Add(hud).document =
        MakeDocument(u8"<Flex direction=\"vertical\" align=\"start\" padding=\"12\">"
                     u8"<Button id=\"hud-btn\" text=\"HUD\" width=\"180\" height=\"36\"/></Flex>");

    ctx.BeginFrame(1.0f / 60.0f);
    RootView* sceneRoot = ui->SceneRoot(*scene);
    REQUIRE(sceneRoot != nullptr);
    sceneRoot->ViewportSize = Float2{800.0f, 600.0f};
    ui->Context().UpdateRootView(sceneRoot);

    // Press + hold over empty space: never consumed - on the press frame or held.
    devices.mouse.x = 400.0f;
    devices.mouse.y = 300.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(ui->PointerOverUI());
    devices.PressLeft();
    ctx.BeginFrame(1.0f / 60.0f); // the press frame (where the shove polls)
    CHECK_FALSE(ui->PointerOverUI());
    ctx.BeginFrame(1.0f / 60.0f); // still held
    CHECK_FALSE(ui->PointerOverUI());
    devices.ReleaseLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK_FALSE(ui->PointerOverUI());

    // Press ON the HUD button: consumed while held (the intended consumption).
    devices.mouse.x = 30.0f;
    devices.mouse.y = 30.0f;
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->PointerOverUI());
    devices.PressLeft();
    ctx.BeginFrame(1.0f / 60.0f);
    CHECK(ui->PointerOverUI());
    devices.ReleaseLeft();
    ctx.BeginFrame(1.0f / 60.0f);

    ctx.Shutdown();
}

TEST_CASE("ui.subsystem: an ImageView's source names a texture asset, drawn by every UI renderer")
{
    runtime::Context ctx(foundation::core::DefaultAllocator());
    ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    ui->EnsureRenderReady(device, 2);
    foundation::rhi::null::NullCommandEncoder encoder;

    // A cooked render texture in a content database, bound through a resource manager.
    foundation::texture::RegisterTextureResource();
    FileDelete(u8"scratch_ui_texture_images/minimap.rasset");
    RemoveDirectory(u8"scratch_ui_texture_images");
    foundation::vfs::NativeFileSystem mount(u8"scratch_ui_texture_images", DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");
    auto* instance = db.RootGroup()->CreateInstance(
        u8"minimap", foundation::texture::RenderTextureResource::StaticType());
    foundation::texture::RenderTextureResource record;
    record.width = 128;
    record.height = 64;
    REQUIRE(instance->WriteObject(record).IsOk());
    foundation::texture::TextureFactory factory(DefaultAllocator(), device);
    foundation::resource::ResourceManager resources(DefaultAllocator(), db);
    resources.AddFactory(&factory);

    IResourceProvider* provider = ui->Context().ResourceProvider();
    REQUIRE(provider != nullptr);
    String id = Format(u8"{}", instance->Id());
    CHECK(provider->LoadImage(id.AsView()) == nullptr); // no resource manager yet

    ui->SetResourceManager(&resources);
    const foundation::image::ImageData* image = provider->LoadImage(id.AsView());
    REQUIRE(image != nullptr);
    CHECK(image->Width() == 128u); // the texture's size: the view's natural size
    CHECK(image->Height() == 64u);
    String braced(u8"{");
    braced.Append(id.AsView());
    braced.Append(u8"}");
    CHECK(provider->LoadImage(braced.AsView()) == image); // one key per texture
    CHECK(provider->LoadImage(u8"not-a-guid") == nullptr);

    // An ImageView naming it in a drawn root: the key is registered on the renderer that drew,
    // and on a renderer of another format once that one draws too.
    RefPtr<UIDocument> document = MakeDocument(u8"<ImageView id=\"map\"/>");
    RefPtr<RootView> preview = ui->CreatePreview(*document);
    REQUIRE(preview.Get() != nullptr);
    ImageView* view = Cast<ImageView>(Cast<ViewGroup>(preview.Get())->FindByName(u8"map"));
    REQUIRE(view != nullptr);
    view->Source.SetValue(id);
    foundation::rhi::TextureView* target = nullptr;
    foundation::rhi::Texture* targetTexture = nullptr;
    REQUIRE(device
                .CreateTexture(foundation::rhi::TextureDesc::RenderTarget(foundation::rhi::TextureFormat::RGBA8UnormSrgb,
                                                              256, 256),
                               targetTexture)
                .IsOk());
    REQUIRE(device.CreateTextureView(targetTexture, foundation::rhi::TextureViewDesc{}, target).IsOk());
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderPreview(*preview, encoder, target, foundation::rhi::TextureFormat::RGBA8UnormSrgb, 256, 256, 0);
    CHECK(view->GetImage() == image);
    CHECK(ui->RenderersShowing(image) == 1u);
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderPreview(*preview, encoder, target, foundation::rhi::TextureFormat::RGBA16Float, 256, 256, 1);
    CHECK(ui->RenderersShowing(image) == 2u);

    // Another project (or none): the key stays valid for the view that holds it, but nothing
    // backs it, and the renderers drop it as they next draw.
    ui->SetResourceManager(nullptr);
    CHECK(provider->LoadImage(id.AsView()) == nullptr);
    ctx.BeginFrame(1.0f / 60.0f);
    ui->RenderPreview(*preview, encoder, target, foundation::rhi::TextureFormat::RGBA8UnormSrgb, 256, 256, 0);
    CHECK(ui->RenderersShowing(image) == 1u); // dropped by the one that drew, not yet the other

    ui->DestroyPreview(preview.Get());
    device.DestroyTextureView(target);
    device.DestroyTexture(targetTexture);
    ctx.Shutdown();
    FileDelete(u8"scratch_ui_texture_images/minimap.rasset");
    RemoveDirectory(u8"scratch_ui_texture_images");
}

namespace
{
    // A menu screen of two buttons named for its run, as a game's title screen.
    RefPtr<foundation::ui::gamekit::UIScreen> MenuFor(UISubsystem& ui, StringView prefix)
    {
        RefPtr<UIDocument> document = MakeDocument(
            Format(u8"<Flex direction=\"vertical\" spacing=\"4\">"
                   u8"<Button id=\"{}-top\" text=\"Top\" width=\"200\" height=\"36\"/>"
                   u8"<Button id=\"{}-bottom\" text=\"Bottom\" width=\"200\" height=\"36\"/>"
                   u8"</Flex>",
                   prefix, prefix)
                .AsView());
        RefPtr<View> tree = ui.InstantiateScreenOverlay(*document);
        RefPtr<foundation::ui::gamekit::UIScreen> screen =
            MakeRef<foundation::ui::gamekit::UIScreen>(DefaultAllocator());
        screen->AddView(tree.Get());
        return screen;
    }
}

TEST_CASE("ui.subsystem: with run screens on, each run has its own screens and keys reach the bound run's")
{
    // Two Game tabs in one editor run two games: each run's menus sit on its own screen tier, and
    // the keys reach the menu of the run whose scene the input is bound to, not the other's.
    runtime::Context ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule uiModule{u8"ui", &AddUISceneManagers, nullptr};
        const scene::SceneModule* modules[] = {&uiModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    auto* input = ctx.AddSubsystem<engine::input::InputSubsystem>(nullptr);
    auto* ui = ctx.AddSubsystem<UISubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    // Off (the player): every run shares the one screen tier.
    int runA = 0;
    int runB = 0;
    CHECK(&ui->ScreensFor(&runA) == &ui->Screens());
    CHECK(ui->RunScreenCount() == 0u);

    ui->SetRunScreens(true);
    foundation::ui::gamekit::ScreenStack& stackA = ui->ScreensFor(&runA);
    foundation::ui::gamekit::ScreenStack& stackB = ui->ScreensFor(&runB);
    CHECK(&stackA != &stackB);
    CHECK(&stackA != &ui->Screens());
    CHECK(&ui->ScreensFor(&runA) == &stackA); // the same run, the same tier
    CHECK(&ui->ScreensFor(nullptr) == &ui->Screens());
    CHECK(ui->RunScreenCount() == 2u);
    CHECK(ui->ScreenRootFor(&runA) != ui->ScreenRootFor(&runB));

    scene::Scene* sceneA = sm.CreateScene(u8"a");
    scene::Scene* sceneB = sm.CreateScene(u8"b");
    sceneA->SetRun(&runA);
    sceneB->SetRun(&runB);
    (void)stackA.Push(MenuFor(*ui, u8"a"));
    (void)stackB.Push(MenuFor(*ui, u8"b"));
    RootView* runRoots[2] = {ui->ScreenRootFor(&runA), ui->ScreenRootFor(&runB)};
    for (RootView* root : runRoots)
    {
        root->ViewportSize = Float2{800.0f, 600.0f};
        ui->Context().UpdateRootView(root);
    }
    CHECK(ui->ScreenRootFor(&runA)->FindByName<Button>(u8"a-top") != nullptr);
    CHECK(ui->ScreenRootFor(&runA)->FindByName<Button>(u8"b-top") == nullptr);

    // Input bound to run B's scene (its Game tab has the keyboard): Down lands on B's menu.
    NavFakeDevices devices;
    input->SetSourceProvider(&devices, sceneB);
    FocusManager* focus = ui->Context().GetFocusManager();
    focus->ClearFocus(); // a push focuses its screen; start from nothing focused
    devices.PressKey(foundation::shell::KeyCode::Down);
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(focus->FocusedView() != nullptr);
    CHECK(focus->FocusedView()->Name.AsView() == u8"b-top");

    // Bound to run A's: the keys move to A's menu.
    focus->ClearFocus();
    input->SetSourceProvider(&devices, sceneA);
    devices.PressKey(foundation::shell::KeyCode::Down);
    ctx.BeginFrame(1.0f / 60.0f);
    REQUIRE(focus->FocusedView() != nullptr);
    CHECK(focus->FocusedView()->Name.AsView() == u8"a-top");

    // A run's end takes its tier and its screens; the other run's stay.
    focus->ClearFocus();
    ui->EndRunScreens(&runA);
    CHECK(ui->RunScreenCount() == 1u);
    CHECK(&ui->ScreensFor(&runB) == &stackB);
    CHECK(stackB.Count() == 1u);

    devices.events.Clear();
    input->SetSourceProvider(nullptr);
    ctx.Shutdown();
}

TEST_CASE("default loading screen: the engine's logo over the bar the player drives")
{
    // A stand-in logo: the player passes the AssiduousEngine logo's SVG text.
    const StringView svg = u8"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 931 200\">"
                           u8"<rect width=\"931\" height=\"200\" fill=\"#F58F42\"/></svg>";
    RefPtr<foundation::ui::View> screen = engine::ui::BuildDefaultLoadingScreen(DefaultAllocator(), svg);
    auto* root = Cast<foundation::ui::ViewGroup>(screen.Get());
    REQUIRE(root != nullptr);
    // The controls an authored loading document names, so the player drives either alike.
    CHECK(root->FindByName<foundation::ui::ProgressBar>(u8"progress") != nullptr);
    CHECK(root->FindByName<foundation::ui::Label>(u8"status") != nullptr);
    auto* logo = root->FindByName<foundation::ui::DrawableView>(u8"logo");
    REQUIRE(logo != nullptr);
    CHECK(logo->Drawable.Get() != nullptr);

    // No logo to draw: the bar alone.
    RefPtr<foundation::ui::View> bare = engine::ui::BuildDefaultLoadingScreen(DefaultAllocator(), StringView());
    auto* bareRoot = Cast<foundation::ui::ViewGroup>(bare.Get());
    REQUIRE(bareRoot != nullptr);
    CHECK(bareRoot->FindByName(u8"logo") == nullptr);
    CHECK(bareRoot->FindByName<foundation::ui::ProgressBar>(u8"progress") != nullptr);
}
