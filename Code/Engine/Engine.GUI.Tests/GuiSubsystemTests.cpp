// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine.GUI (E5 of Specs/gui-lab-evolution-3.md): the resource provider answering asset guids
// (documents, sheets, vector images, textures) and following re-cooks; the subsystem's screen tier
// (the screen stack under the overlay layer) and scene tier (a root per scene); the game's sheets
// in order over the theme, restyled when one is cooked again; a document pushed by its guid; both
// tiers drawn into the engine's overlay passes (the Null device, the shaders from the data mount);
// the domain.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.content;
import foundation.resource;
import foundation.rhi;
import foundation.rhi.null;
import foundation.image;
import foundation.vg.resource;
import foundation.vg.svg;
import foundation.texture.resource;
import foundation.render.api;
import foundation.shell;
import foundation.input;
import engine.input;
import engine.domain;
import experimental.gui;
import experimental.gui.gamekit;
import experimental.gui.resources;
import engine.gui;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace runtime = foundation::runtime;
namespace rhi = foundation::rhi;
namespace lab = experimental::gui;
using engine::gui::GuiResourceProvider;
using engine::gui::GuiSubsystem;

namespace
{
    // The data mount the subsystem reads its shaders and built-in font through.
    foundation::vfs::IFileSystem& DataFs()
    {
        static foundation::vfs::NativeFileSystem fs(foundation::vfs::FindDataRoot(), DefaultAllocator());
        return fs;
    }

    lab::Node* FindById(lab::Node& node, StringView id)
    {
        if (const lab::UIWidget* widget = Cast<lab::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return &node;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (lab::Node* found = FindById(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    String Braced(const Guid& id)
    {
        String braced(u8"{");
        braced.Append(Format(u8"{}", id).AsView());
        braced.Append(StringView(u8"}"));
        return braced;
    }

    // A content database of cooked GUI assets in a scratch folder, and a resource manager over it
    // with their factories.
    struct Project
    {
        rhi::null::NullDevice device{DefaultAllocator()};
        String folder;
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<foundation::content::ContentDatabase> db;
        lab::GuiDocumentFactory documents{DefaultAllocator()};
        lab::GuiStyleSheetFactory sheets{DefaultAllocator()};
        foundation::vg::VectorImageFactory vectors{DefaultAllocator()};
        UniquePtr<foundation::texture::TextureFactory> textures;
        UniquePtr<foundation::resource::ResourceManager> resources;

        explicit Project(StringView name) : folder(name)
        {
            lab::RegisterGuiResource();
            foundation::vg::RegisterVGResource();
            foundation::texture::RegisterTextureResource();
            Clean();
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), folder.AsView(), DefaultAllocator());
            db = MakeUnique<foundation::content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount, BinarySerializerFactory(), u8".rasset");
            textures = MakeUnique<foundation::texture::TextureFactory>(DefaultAllocator(), DefaultAllocator(), device);
            resources = MakeUnique<foundation::resource::ResourceManager>(DefaultAllocator(), DefaultAllocator(), *db);
            resources->AddFactory(&documents);
            resources->AddFactory(&sheets);
            resources->AddFactory(&vectors);
            resources->AddFactory(textures.Get());
        }
        ~Project()
        {
            resources.Reset();
            textures.Reset();
            db.Reset();
            mount.Reset();
            Clean();
        }
        void Clean()
        {
            const StringView files[] = {u8"menu", u8"base", u8"game", u8"icon", u8"map"};
            for (const StringView file : files)
                FileDelete(PathJoin(folder.AsView(), Format(u8"{}.rasset", file).AsView()).AsView());
            RemoveDirectory(folder.AsView());
        }
        Guid Document(StringView name, StringView markup)
        {
            auto* instance = db->RootGroup()->CreateInstance(name, lab::GuiDocumentSource::StaticType());
            lab::GuiDocumentSource source;
            source.markup = String(markup);
            REQUIRE(instance->WriteObject(source).IsOk());
            return instance->Id();
        }
        Guid Sheet(StringView name, StringView css)
        {
            auto* instance = db->RootGroup()->CreateInstance(name, lab::GuiStyleSheetSource::StaticType());
            RewriteSheet(instance->Id(), css);
            return instance->Id();
        }
        // The sheet cooked again with `css`, and reloaded as the editor's re-cook does.
        void RewriteSheet(const Guid& id, StringView css)
        {
            lab::GuiStyleSheetSource source;
            source.css = String(css);
            REQUIRE(db->GetInstance(id)->WriteObject(source).IsOk());
            (void)resources->Reload(id);
        }
        Guid Vector(StringView name, StringView svg)
        {
            auto* instance = db->RootGroup()->CreateInstance(name, foundation::vg::VectorImageSource::StaticType());
            foundation::vg::VectorImageSource source;
            source.svg = String(svg);
            REQUIRE(instance->WriteObject(source).IsOk());
            return instance->Id();
        }
        Guid Texture(StringView name, u32 width, u32 height)
        {
            auto* instance = db->RootGroup()->CreateInstance(name, foundation::texture::RenderTextureResource::StaticType());
            foundation::texture::RenderTextureResource record;
            record.width = width;
            record.height = height;
            REQUIRE(instance->WriteObject(record).IsOk());
            return instance->Id();
        }
    };

    // A mouse the test moves and presses; a press or release shows as an edge for one frame.
    struct FakeMouse final : foundation::shell::IMouse
    {
        f32 x = -100.0f;
        f32 y = -100.0f;
        bool down = false;
        bool wasDown = false;
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
            return b == foundation::shell::MouseButton::Left && down;
        }
        [[nodiscard]] bool IsButtonPressed(foundation::shell::MouseButton b) const override
        {
            return b == foundation::shell::MouseButton::Left && down && !wasDown;
        }
        [[nodiscard]] bool IsButtonReleased(foundation::shell::MouseButton b) const override
        {
            return b == foundation::shell::MouseButton::Left && !down && wasDown;
        }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(foundation::shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };

    // The engine's active input source as a test drives it: the mouse, and this frame's events.
    struct FakeDevices final : foundation::input::IInputSourceProvider
    {
        FakeMouse mouse;
        Array<foundation::shell::InputEvent> events;
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() override { return nullptr; }
        [[nodiscard]] foundation::shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] foundation::shell::IGamepad* Gamepad(i32) override { return nullptr; }
        [[nodiscard]] Span<const foundation::shell::InputEvent> Events() override { return {events.Data(), events.Size()}; }
        void Key(foundation::shell::KeyCode key)
        {
            foundation::shell::InputEvent down;
            down.kind = foundation::shell::InputEventKind::KeyDown;
            down.key = key;
            events.PushBack(down);
            foundation::shell::InputEvent up = down;
            up.kind = foundation::shell::InputEventKind::KeyUp;
            events.PushBack(up);
        }
    };

    // What Engine.UI does in the same lane before the GUI: claims the pointer, when told to.
    class ClaimingSubsystem final : public runtime::Subsystem
    {
    public:
        explicit ClaimingSubsystem(engine::input::InputSubsystem& input) noexcept : m_input(input) {}
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -650; }
        void BeginFrame(f32) override
        {
            if (Claims)
                m_input.Runtime().SetConsumptionMask({true, false});
        }
        bool Claims = false;

    private:
        engine::input::InputSubsystem& m_input;
    };

    // A context with the scene subsystem and the GUI's (and the input subsystem's, on fake
    // devices), started.
    struct Engine
    {
        runtime::Context context{DefaultAllocator()};
        scene::SceneManager scenes{DefaultAllocator()};
        FakeDevices devices;
        engine::input::InputSubsystem* input = nullptr;
        ClaimingSubsystem* claiming = nullptr;
        GuiSubsystem* gui = nullptr;

        Engine()
        {
            auto* sceneSubsystem = context.AddSubsystem<engine::scene::SceneSubsystem>();
            sceneSubsystem->RegisterManager(&scenes);
            input = context.AddSubsystem<engine::input::InputSubsystem>(nullptr);
            claiming = context.AddSubsystem<ClaimingSubsystem>(*input);
            gui = context.AddSubsystem<GuiSubsystem>(DefaultAllocator(), DataFs());
            context.Startup();
            input->SetSourceProvider(&devices);
        }
        ~Engine()
        {
            input->SetSourceProvider(nullptr);
            context.Shutdown();
        }
        // A frame as the application runs one: the last one's end (EndFrame), then this one's
        // BeginFrame lane, where the GUI pumps; the test reads what it did before its end.
        void Frame()
        {
            context.EndFrame();
            context.BeginFrame(1.0f / 60.0f);
            devices.events.Clear();
            devices.mouse.wasDown = devices.mouse.down;
        }
        [[nodiscard]] bool PointerClaimed() const { return input->Runtime().GetConsumptionMask().pointer; }
    };

    RefPtr<lab::Button> MakeButton(StringView id, Float2 at)
    {
        RefPtr<lab::Button> button = MakeRef<lab::Button>(DefaultAllocator());
        button->SetId(id);
        button->SetPosition(at);
        button->SetSize(Float2{100.0f, 40.0f});
        return button;
    }
}

TEST_CASE("engine.gui: the provider answers documents, sheets, vector images and textures by guid, and only as what they are")
{
    Project project(u8"scratch_engine_gui_provider");
    const Guid menu = project.Document(u8"menu", u8"<Panel id=\"menu\"/>");
    const Guid sheet = project.Sheet(u8"game", u8"#menu { width: 10px; }");
    const Guid icon = project.Vector(u8"icon", u8"<svg width='8' height='8'><rect width='8' height='8'/></svg>");
    const Guid map = project.Texture(u8"map", 128, 64);

    GuiResourceProvider provider(DefaultAllocator());
    String text;
    CHECK_FALSE(provider.LoadText(Braced(menu).AsView(), text)); // no resource manager yet
    provider.SetResourceManager(project.resources.Get());
    Array<const foundation::image::ImageData*> told;
    provider.SetTextureSink([&](const foundation::image::ImageData* key, rhi::TextureView* view) { told.PushBack(view != nullptr ? key : nullptr); });

    REQUIRE(provider.LoadText(Braced(menu).AsView(), text));
    CHECK(text.AsView() == StringView(u8"<Panel id=\"menu\"/>"));
    REQUIRE(provider.LoadText(Format(u8"{}", sheet).AsView(), text)); // the bare guid too
    CHECK(text.AsView() == StringView(u8"#menu { width: 10px; }"));
    CHECK_FALSE(provider.LoadText(u8"menu.gml", text)); // a path is not a guid
    CHECK_FALSE(provider.LoadText(Braced(icon).AsView(), text)); // a vector image is no text

    const foundation::vg::svg::SVGDocument* picture = provider.LoadSvg(Braced(icon).AsView());
    REQUIRE(picture != nullptr);
    CHECK(provider.LoadSvg(Braced(icon).AsView()) == picture); // parsed once
    CHECK(provider.LoadSvg(Braced(sheet).AsView()) == nullptr);

    const foundation::image::ImageData* image = provider.LoadImage(Braced(map).AsView());
    REQUIRE(image != nullptr);
    CHECK(image->Width() == 128u);
    CHECK(image->Height() == 64u);
    CHECK(provider.LoadImage(Braced(map).AsView()) == image); // one key per texture
    REQUIRE(told.Size() == 1u);
    CHECK(told[0] == image); // the renderers learn it
    CHECK(provider.LoadImage(Braced(menu).AsView()) == nullptr);

    // Another project: what was bound goes, the renderers let the key go.
    provider.SetResourceManager(nullptr);
    REQUIRE(told.Size() == 2u);
    CHECK(told[1] == nullptr);
    CHECK_FALSE(provider.LoadText(Braced(menu).AsView(), text));
    CHECK(provider.LoadImage(Braced(map).AsView()) == nullptr);
}

TEST_CASE("engine.gui: a sheet cooked again is seen at the next poll, once")
{
    Project project(u8"scratch_engine_gui_poll");
    const Guid sheet = project.Sheet(u8"game", u8"#a { width: 1px; }");
    GuiResourceProvider provider(DefaultAllocator());
    provider.SetResourceManager(project.resources.Get());
    String text;
    REQUIRE(provider.LoadText(Braced(sheet).AsView(), text));
    CHECK_FALSE(provider.Poll());
    project.RewriteSheet(sheet, u8"#a { width: 2px; }");
    CHECK(provider.Poll());
    REQUIRE(provider.LoadText(Braced(sheet).AsView(), text));
    CHECK(text.AsView() == StringView(u8"#a { width: 2px; }"));
    CHECK_FALSE(provider.Poll());
}

TEST_CASE("engine.gui: the screen tier holds the screen stack under the overlay layer, and each scene a root of its own")
{
    Engine engine;
    lab::SceneRoot* screen = engine.gui->ScreenRoot();
    REQUIRE(screen != nullptr);
    REQUIRE(screen->ChildCount() == 2u);
    CHECK(screen->GetChildAt(0) == engine.gui->Screens());
    CHECK(screen->GetChildAt(1) == engine.gui->OverlayLayer());
    CHECK(engine.gui->Context().GetResourceProvider() == &engine.gui->Resources());
    CHECK(engine.gui->Fonts() != nullptr);

    scene::Scene* level = engine.scenes.CreateScene(u8"level");
    lab::SceneRoot* root = engine.gui->SceneRootFor(*level);
    REQUIRE(root != nullptr);
    CHECK(root != screen);
    engine.scenes.DestroyScene(level);
    CHECK(engine.gui->SceneRootFor(*level) == nullptr);
    CHECK(engine.gui->ScreenRoot() == screen); // the screen tier outlives a scene
}

TEST_CASE("engine.gui: the game's sheets apply in order over the theme, restyle when one is cooked again, and a document pushes by its guid")
{
    Project project(u8"scratch_engine_gui_sheets");
    const Guid base = project.Sheet(u8"base", u8"#box { width: 120px; height: 10px; }");
    const Guid game = project.Sheet(u8"game", u8"#box { width: 200px; }");
    const Guid menu = project.Document(u8"menu", u8"<Screen><Panel id=\"box\"/></Screen>");
    Engine engine;
    engine.gui->SetResourceManager(project.resources.Get());
    const Guid sheets[] = {base, game};
    engine.gui->SetStyleSheets(Span<const Guid>(sheets, 2));
    CHECK(engine.gui->StyleSheets().Size() == 2u);

    lab::Screen* pushed = engine.gui->Screens()->PushDocument(Braced(menu).AsView());
    REQUIRE(pushed != nullptr);
    engine.gui->ScreenRoot()->SetViewportSize(Float2{800.0f, 600.0f});
    engine.Frame();
    lab::Node* box = FindById(*engine.gui->ScreenRoot(), u8"box");
    REQUIRE(box != nullptr);
    CHECK(box->GetSize().x == doctest::Approx(200.0f)); // the game's over the shared one
    CHECK(box->GetSize().y == doctest::Approx(10.0f));  // what it leaves is the shared one's

    const u64 generation = engine.gui->SheetGeneration();
    engine.Frame();
    CHECK(engine.gui->SheetGeneration() == generation); // nothing changed: nothing re-applied
    project.RewriteSheet(game, u8"#box { width: 150px; }");
    engine.Frame();
    CHECK(engine.gui->SheetGeneration() == generation + 1);
    CHECK(box->GetSize().x == doctest::Approx(150.0f));

    engine.gui->SetResourceManager(nullptr); // before the project goes
}

TEST_CASE("engine.gui: both tiers draw into the engine's overlay passes, the screen's layers before its pass")
{
    rhi::null::NullDevice device{DefaultAllocator()}; // outlives the GUI that draws on it
    Engine engine;
    engine.gui->EnsureRenderReady(device, 2);
    REQUIRE(engine.gui->IsRenderReady());
    CHECK(engine.gui->Context().GetPictureBaker() != nullptr);

    // A faded group on a screen: its layer is drawn before the screen's pass.
    RefPtr<lab::Panel> group = MakeRef<lab::Panel>(DefaultAllocator());
    group->SetSize(Float2{60.0f, 40.0f});
    lab::NodeBox look;
    look.BackgroundColor = Color{1.0f, 0.0f, 0.0f, 1.0f};
    group->SetBox(Move(look));
    group->SetAlpha(0.5f);
    RefPtr<lab::Panel> inner = MakeRef<lab::Panel>(DefaultAllocator());
    inner->SetSize(Float2{20.0f, 20.0f});
    lab::NodeBox innerLook;
    innerLook.BackgroundColor = Color{0.0f, 0.0f, 1.0f, 1.0f};
    inner->SetBox(Move(innerLook));
    group->AddChild(inner.Get());
    REQUIRE(engine.gui->Screens()->Push(*group) != nullptr);
    engine.Frame();

    foundation::render::ScreenOverlayView view;
    view.width = 320;
    view.height = 180;
    view.targetFormat = rhi::TextureFormat::RGBA8UnormSrgb;
    rhi::null::NullCommandEncoder encoder;
    engine.gui->PrepareOverlay(encoder, view);
    const u64 layerPasses = encoder.counters->Snapshot().renderPasses;
    CHECK(layerPasses == 1u); // the faded group's layer, before the overlay pass opens
    CHECK(engine.gui->ScreenRoot()->GetViewportSize().x == doctest::Approx(320.0f));
    rhi::RenderPassEncoder* pass = encoder.BeginRenderPass(rhi::RenderPassDesc{});
    REQUIRE(pass != nullptr);
    const u64 drawsBefore = encoder.counters->Snapshot().draws;
    engine.gui->Render(*pass, view);
    CHECK(encoder.counters->Snapshot().draws > drawsBefore);
    CHECK(encoder.counters->Snapshot().renderPasses == layerPasses + 1); // Render opened none
    // Unprepared (another view): drawn in the pass all the same.
    const u64 drawsUnprepared = encoder.counters->Snapshot().draws;
    engine.gui->Render(*pass, view);
    CHECK(encoder.counters->Snapshot().draws > drawsUnprepared);

    // The scene tier: the view's scene root, laid out at its viewport.
    scene::Scene* level = engine.scenes.CreateScene(u8"level");
    lab::SceneRoot* root = engine.gui->SceneRootFor(*level);
    REQUIRE(root != nullptr);
    RefPtr<lab::Panel> badge = MakeRef<lab::Panel>(DefaultAllocator());
    badge->SetSize(Float2{30.0f, 30.0f});
    lab::NodeBox badgeLook;
    badgeLook.BackgroundColor = Color{0.0f, 1.0f, 0.0f, 1.0f};
    badge->SetBox(Move(badgeLook));
    root->AddChild(badge.Get());
    foundation::render::SceneOverlayView sceneView;
    sceneView.sceneKey = level;
    sceneView.viewportX = 160;
    sceneView.viewportWidth = 160;
    sceneView.viewportHeight = 180;
    sceneView.targetWidth = 320;
    sceneView.targetHeight = 180;
    sceneView.targetFormat = rhi::TextureFormat::RGBA8Unorm;
    const u64 drawsScene = encoder.counters->Snapshot().draws;
    engine.gui->Render(*pass, sceneView);
    CHECK(encoder.counters->Snapshot().draws > drawsScene);
    CHECK(root->GetViewportSize().x == doctest::Approx(160.0f));
    // Another scene's view draws nothing of this one.
    sceneView.sceneKey = &engine;
    const u64 drawsOther = encoder.counters->Snapshot().draws;
    engine.gui->Render(*pass, sceneView);
    CHECK(encoder.counters->Snapshot().draws == drawsOther);
    pass->End();
    engine.scenes.DestroyScene(level);
}

TEST_CASE("engine.gui: the domain brings the documents' and sheets' resource module")
{
    const engine::DomainModule& domain = engine::gui::GuiDomain();
    CHECK(domain.id == StringView(u8"gui"));
    bool documents = false;
    for (const foundation::resource::ResourceModule* module : domain.resources)
        documents = documents || module == &lab::kGuiResourceModule;
    CHECK(documents);
    CHECK_FALSE(domain.HasScene()); // its components come with E7
}

TEST_CASE("engine.gui: input goes to the root whose GUI is under the pointer, a click lands, and the action layer hears what the GUI took")
{
    Engine engine;
    scene::Scene* level = engine.scenes.CreateScene(u8"level");
    lab::SceneRoot* hud = engine.gui->SceneRootFor(*level);
    REQUIRE(hud != nullptr);
    hud->SetViewportSize(Float2{800.0f, 600.0f});
    RefPtr<lab::Button> button = MakeButton(u8"fire", Float2{20.0f, 20.0f});
    hud->AddChild(button.Get());
    i32 clicks = 0;
    (void)button->AddEventListener(lab::EventType::MouseClick, [&](const lab::Event&) { ++clicks; });
    engine.gui->ScreenRoot()->SetViewportSize(Float2{800.0f, 600.0f});
    engine.Frame();

    // Over the button: the scene's root takes the input, and the pointer is the GUI's.
    engine.devices.mouse.x = 50.0f;
    engine.devices.mouse.y = 30.0f;
    engine.Frame();
    CHECK(engine.gui->ActiveInputRoot() == hud);
    CHECK(engine.gui->PointerOverGui());
    CHECK(engine.PointerClaimed());
    engine.devices.mouse.down = true;
    engine.Frame();
    engine.devices.mouse.down = false;
    engine.Frame();
    CHECK(clicks == 1);

    // Over empty space: the game's pointer; and the frame's end clears the claim.
    engine.devices.mouse.x = 500.0f;
    engine.Frame();
    CHECK_FALSE(engine.gui->PointerOverGui());
    CHECK_FALSE(engine.PointerClaimed());
    engine.devices.mouse.x = 50.0f;
    engine.Frame();
    CHECK(engine.PointerClaimed());
    engine.context.EndFrame();
    CHECK_FALSE(engine.PointerClaimed());

    // Engine.UI's claim (made first in the lane) stands though the GUI takes nothing.
    engine.claiming->Claims = true;
    engine.devices.mouse.x = 500.0f;
    engine.Frame();
    CHECK_FALSE(engine.gui->PointerOverGui());
    CHECK(engine.PointerClaimed());
    engine.claiming->Claims = false;
    engine.context.EndFrame();

    // A press stays with the root it began in until it is let go.
    engine.devices.mouse.x = 50.0f;
    engine.devices.mouse.down = true;
    engine.Frame();
    RefPtr<lab::Button> menu = MakeButton(u8"menu", Float2{400.0f, 20.0f});
    RefPtr<lab::UIWidget> screen = MakeRef<lab::UIWidget>(DefaultAllocator());
    screen->AddChild(menu.Get());
    REQUIRE(engine.gui->Screens()->Push(*screen) != nullptr);
    engine.devices.mouse.x = 450.0f; // over the screen's button now, still held
    engine.Frame();
    CHECK(engine.gui->ActiveInputRoot() == hud);
    CHECK(engine.PointerClaimed()); // it began over the GUI
    engine.devices.mouse.down = false;
    engine.Frame();
    engine.Frame();
    CHECK(engine.gui->ActiveInputRoot() == engine.gui->ScreenRoot());
    engine.scenes.DestroyScene(level);
    CHECK(engine.gui->ActiveInputRoot() != hud);
}

TEST_CASE("engine.gui: with no GUI under the pointer the keys go to the screens a game pushed, and Back pops one")
{
    Engine engine;
    engine.gui->ScreenRoot()->SetViewportSize(Float2{800.0f, 600.0f});
    RefPtr<lab::UIWidget> title = MakeRef<lab::UIWidget>(DefaultAllocator());
    title->AddChild(MakeButton(u8"play", Float2{20.0f, 20.0f}).Get());
    RefPtr<lab::UIWidget> settings = MakeRef<lab::UIWidget>(DefaultAllocator());
    settings->AddChild(MakeButton(u8"back", Float2{20.0f, 20.0f}).Get());
    REQUIRE(engine.gui->Screens()->Push(*title) != nullptr);
    REQUIRE(engine.gui->Screens()->Push(*settings) != nullptr);
    engine.Frame();
    engine.Frame();
    CHECK(engine.gui->Screens()->Count() == 2u);
    engine.devices.Key(foundation::shell::KeyCode::Escape);
    engine.Frame();
    CHECK(engine.gui->ActiveInputRoot() == engine.gui->ScreenRoot());
    CHECK(engine.gui->Screens()->Count() == 1u);
}

TEST_CASE("engine.gui: a source bound to another scene never reaches this scene's GUI")
{
    Engine engine;
    scene::Scene* level = engine.scenes.CreateScene(u8"level");
    scene::Scene* other = engine.scenes.CreateScene(u8"other");
    lab::SceneRoot* hud = engine.gui->SceneRootFor(*level);
    hud->SetViewportSize(Float2{800.0f, 600.0f});
    RefPtr<lab::Button> button = MakeButton(u8"fire", Float2{20.0f, 20.0f});
    hud->AddChild(button.Get());
    engine.devices.mouse.x = 50.0f;
    engine.devices.mouse.y = 30.0f;
    engine.Frame();
    CHECK(engine.gui->ActiveInputRoot() == hud);
    engine.input->SetSourceProvider(&engine.devices, other); // an editor Game tab on the other scene
    engine.Frame();
    CHECK(engine.gui->ActiveInputRoot() != hud);
    CHECK_FALSE(engine.gui->PointerOverGui());
    engine.input->SetSourceProvider(&engine.devices, level);
    engine.Frame();
    CHECK(engine.gui->ActiveInputRoot() == hud);
    engine.scenes.DestroyScene(other);
    engine.scenes.DestroyScene(level);
}
