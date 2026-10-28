// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PhysicsPlayground - the physics consumer proof: a stack of falling crates, a
// kinematic sweeper, a trigger volume, and crosshair raycast shoving - all authored as
// RigidBody/Collider COMPONENTS on a scene, simulated by the Jolt-backed subsystem on the
// engine's fixed lane with render interpolation, and drawn via the physics DEBUG
// wireframes (green = awake dynamic, grey = sleeping, blue = static/kinematic, yellow =
// trigger). Fly with WASD/RMB-look; LEFT-CLICK shoves the body under the crosshair; R
// respawns the stack; the ImGui panel has gravity + time-scale sliders and live counts.

#include "Core/Prelude.h"
#include "Runtime.Client/AppMain.h"
#include "imgui.h"
#include <cmath>

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import engine.defaultapp;
import foundation.shell;
import foundation.runtime.desktop;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.scene;
import engine.scene;
import engine.render;
import extensions.imgui;
import foundation.physics;
import foundation.physics.resource;
import engine.physics;
import foundation.gui;
import foundation.gui.resources; // GuiDocument (the documents made in code)
import engine.gui;

#include "../Common/FlyCamera.h" // after the imports: uses foundation::core/runtime types

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace shell = foundation::shell;
namespace scene = foundation::scene;
namespace physics = foundation::physics;
namespace imgui = extensions::imgui;
namespace gui = foundation::gui;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}




using core::f32;

namespace
{
    class PlaygroundApp final : public engine::runtime::DefaultApplication
    {
    public:
    public:
        void Configure(runtime::IApplicationHost& host) override
        {
            // Physics/input/UI come from DefaultApplication (H2) - only the sample-local
            // extras register here (double-adding a subsystem = two instances ticking).
            engine::runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight(),
                                                                    DataFileSystem());
            }
        }

        void OnLaunch(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"playground");
            m_physics = m_scene->GetSystem<engine::physics::PhysicsSceneSystem>();
            if (m_physics != nullptr)
            {
                m_physics->Settings().debugDraw = true;
            }

            // Camera.
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                cameras->Add(m_camera);
            }
            m_fly.position = core::Float3{8.0f, 6.0f, 14.0f};
            m_fly.yaw = 0.5f;
            m_fly.pitch = -0.3f;
            m_fly.moveSpeed = 10.0f;
            m_fly.fastSpeed = 30.0f;

            BuildWorld();
            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            core::ConsoleWrite(u8"PhysicsPlayground: WASD/RMB-look fly, LMB shove, R respawn.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, dt);
            m_fly.Update(host, dt);
            PushCameraToEntity();

            auto* input = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            if (input == nullptr)
            {
                return;
            }
            if (input->Keyboard()->IsKeyPressed(shell::KeyCode::Escape))
            {
                host.RequestExit(0);
            }
            if (input->Keyboard()->IsKeyPressed(shell::KeyCode::R))
            {
                RespawnStack();
            }

            // Arrow-key character drive (world axes) + Space jump.
            if (auto* characters = m_scene->GetSystem<engine::physics::CharacterComponentManager>())
            {
                if (engine::physics::CharacterComponent* hero = characters->Get(m_hero))
                {
                    const f32 speed = 4.0f;
                    core::Float3 move{0.0f, 0.0f, 0.0f};
                    if (input->Keyboard()->IsKeyDown(shell::KeyCode::Right))
                    {
                        move.x += speed;
                    }
                    if (input->Keyboard()->IsKeyDown(shell::KeyCode::Left))
                    {
                        move.x -= speed;
                    }
                    if (input->Keyboard()->IsKeyDown(shell::KeyCode::Up))
                    {
                        move.z -= speed;
                    }
                    if (input->Keyboard()->IsKeyDown(shell::KeyCode::Down))
                    {
                        move.z += speed;
                    }
                    hero->moveVelocity = move;
                    if (input->Keyboard()->IsKeyPressed(shell::KeyCode::Space))
                    {
                        hero->jumpSpeed = 6.0f;
                    }
                }
            }

            // HUD button binding (once the subsystem instantiated the tree).
            if (!m_hudBound && m_scene != nullptr)
            {
                if (auto* canvases = m_scene->GetSystem<engine::gui::GuiCanvasComponentManager>())
                {
                    if (auto* canvas = canvases->Get(m_hudEntity); canvas != nullptr && canvas->root)
                    {
                        if (auto* button = core::Cast<gui::Button>(FindById(*canvas->root, u8"hud-btn")))
                        {
                            PlaygroundApp* self = this;
                            button->SetOnClick(
                                [self, button]()
                                {
                                    ++self->m_hudClicks;
                                    button->SetText(core::Format(u8"Clicks: {}", self->m_hudClicks).AsView());
                                    core::ConsoleWrite(u8"HUD button clicked\n");
                                });
                            m_hudBound = true;
                        }
                    }
                }
            }
            if (!m_kioskBound && m_scene != nullptr)
            {
                if (auto* panels = m_scene->GetSystem<engine::gui::GuiWorldPanelComponentManager>())
                {
                    if (auto* panel = panels->Get(m_kioskEntity); panel != nullptr && panel->root)
                    {
                        if (auto* button = core::Cast<gui::Button>(FindById(*panel->root, u8"kiosk-btn")))
                        {
                            PlaygroundApp* self = this;
                            button->SetOnClick(
                                [self, button]()
                                {
                                    ++self->m_kioskTaps;
                                    button->SetText(core::Format(u8"Taps: {}", self->m_kioskTaps).AsView());
                                    core::ConsoleWrite(u8"Kiosk panel tapped\n");
                                });
                            m_kioskBound = true;
                        }
                    }
                }
            }

            // Crosshair shove: ray along the camera forward; impulse along the ray.
            // Gated on UI consumption: a click that lands ON the HUD never shoves.
            auto* gameGui = host.Ctx().GetSubsystem<engine::gui::GuiSubsystem>();
            const bool uiAte = gameGui != nullptr && gameGui->PointerOverGui();
            if (!uiAte && input->Mouse()->IsButtonPressed(shell::MouseButton::Left) &&
                m_physics != nullptr && m_physics->World() != nullptr)
            {
                // Shove ray: through the CURSOR while it is free (click a crate to shove it);
                // along the camera forward (crosshair) while look-flying captures the mouse -
                // the same free-cursor/center-aim convention the kiosk world panel uses.
                core::Float3 rayDir = m_fly.Forward();
                const bool lookCaptured =
                    m_fly.mouseCaptured || input->Mouse()->IsButtonDown(shell::MouseButton::Right);
                shell::IWindow* win = host.Shell()->MainWindow();
                if (!lookCaptured && win != nullptr && win->Width() > 0 && win->Height() > 0)
                {
                    core::f32 fovY = 1.04719755f; // the camera component's authored fov
                    if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
                    {
                        if (engine::render::CameraComponent* cam = cameras->Get(m_camera))
                        {
                            fovY = cam->fovYRadians;
                        }
                    }
                    const core::f32 w = static_cast<core::f32>(win->Width());
                    const core::f32 h = static_cast<core::f32>(win->Height());
                    const core::f32 ndcX = (input->Mouse()->X() / w) * 2.0f - 1.0f;
                    const core::f32 ndcY = 1.0f - (input->Mouse()->Y() / h) * 2.0f;
                    const core::f32 tanHalfY = core::Tan(fovY * 0.5f);
                    const core::f32 tanHalfX = tanHalfY * (w / h);
                    rayDir = core::Normalized(m_fly.Forward() + m_fly.Right() * (ndcX * tanHalfX) +
                                              m_fly.Up() * (ndcY * tanHalfY));
                }
                physics::RayHit hit;
                if (m_physics->World()->RayCast(m_fly.position, rayDir, 200.0f, hit))
                {
                    // A visible whack: crates are ~1000 kg (1 m^3 at Jolt's default density), so
                    // the impulse must be in the thousands - 400 only WOKE the body (delta-v
                    // 0.4 m/s reads as a highlight, not a shove).
                    m_physics->World()->AddImpulse(
                        hit.body, core::Float3{rayDir.x * 4000.0f, rayDir.y * 4000.0f + 1400.0f,
                                               rayDir.z * 4000.0f});
                    m_lastSurface = hit.surface; // ramp panels report 1 / 2
                    m_haveSurface = true;
                }
            }

            // Trigger events -> console (the volume floats over the stack).
            if (m_physics != nullptr)
            {
                for (const physics::ContactEvent& e : m_physics->Events())
                {
                    if (e.kind == physics::ContactKind::TriggerEnter)
                    {
                        core::ConsoleWrite(u8"PhysicsPlayground: trigger entered!\n");
                    }
                }
            }

            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(input, dt);
                DrawHud(host);
            }
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (m_scene != nullptr && frame.height > 0)
            {
                if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
                {
                    if (engine::render::CameraComponent* cam = cameras->Get(m_camera))
                    {
                        cam->aspect =
                            static_cast<f32>(frame.width) / static_cast<f32>(frame.height);
                    }
                }
            }
            RenderFrame(host, frame); // the scenes + window overlays; FinishFrame below closes the shot
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
            FinishFrame(host, frame); // the screenshot copy, with the overlay in it
        }

    private:
        // The node under `node` with `id` (a GUI tree from markup), or null.
        [[nodiscard]] static gui::Node* FindById(gui::Node& node, core::StringView id)
        {
            if (const auto* widget = core::Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (core::usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindById(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        void BuildWorld()
        {
            auto* bodies = m_scene->GetSystem<engine::physics::RigidBodyComponentManager>();

            // Ground: an infinite plane - crates land anywhere, not just on a slab.
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"ground");
                engine::physics::RigidBodyComponent& body = bodies->Add(e);
                body.motion = physics::MotionKind::Static;
                body.layer = physics::PhysicsLayer::Static;
                body.shape = physics::ShapeKind::Plane;
                body.planeHalfExtent = 200.0f;
            }
            // A cooked TRIANGLE-MESH ramp: two panels, two material slots - the
            // crosshair ray reports which slot it hit (HUD "surface").
            {
                const core::Float3 positions[] = {
                    {6.0f, 0.0f, -3.0f}, {6.0f, 0.0f, 3.0f},   // low edge
                    {12.0f, 3.0f, 3.0f}, {12.0f, 3.0f, -3.0f}, // high edge
                    {18.0f, 3.0f, 3.0f}, {18.0f, 3.0f, -3.0f}, // flat top end
                };
                const core::u32 indices[] = {0, 1, 2, 0, 2, 3,  // sloped panel
                                             3, 2, 4, 3, 4, 5}; // flat panel
                const core::u32 slots[] = {1, 1, 2, 2};
                core::Array<core::byte> blob;
                if (physics::CookTriangleMesh(core::Span<const core::Float3>(positions, 6),
                                              core::Span<const core::u32>(indices, 12),
                                              core::Span<const core::u32>(slots, 4), blob))
                {
                    m_rampShape = core::MakeRef<physics::CollisionShape>(AppRoot());
                    m_rampShape->blob.Resize(blob.Size());
                    core::MemCopy(m_rampShape->blob.Data(), blob.Data(), blob.Size());
                    core::Array<core::Float3> outline;
                    if (physics::ExtractShapeTriangles(
                            core::Span<const core::byte>(blob.Data(), blob.Size()), outline))
                    {
                        m_rampShape->outline = static_cast<core::Array<core::Float3>&&>(outline);
                    }
                    scene::EntityHandle e = m_scene->CreateEntity(u8"ramp");
                    engine::physics::RigidBodyComponent& body = bodies->Add(e);
                    body.motion = physics::MotionKind::Static;
                    body.layer = physics::PhysicsLayer::Static;
                    body.shape = physics::ShapeKind::Cooked;
                    body.collisionShape = m_rampShape;
                }
            }
            // A cooked CONVEX boulder dropped onto the ramp - hulls may be dynamic.
            {
                core::Array<core::Float3> points;
                const core::f32 axes[3][3] = {{0.9f, 0, 0}, {0, 0.7f, 0}, {0, 0, 0.8f}};
                for (const auto& a : axes)
                {
                    points.PushBack(core::Float3{a[0], a[1], a[2]});
                    points.PushBack(core::Float3{-a[0], -a[1], -a[2]});
                }
                points.PushBack(core::Float3{0.5f, 0.5f, 0.5f});
                points.PushBack(core::Float3{-0.5f, 0.5f, -0.5f});
                core::Array<core::byte> blob;
                if (physics::CookConvexHull(
                        core::Span<const core::Float3>(points.Data(), points.Size()), blob))
                {
                    m_boulderShape =
                        core::MakeRef<physics::CollisionShape>(AppRoot());
                    m_boulderShape->blob.Resize(blob.Size());
                    core::MemCopy(m_boulderShape->blob.Data(), blob.Data(), blob.Size());
                    core::Array<core::Float3> outline;
                    if (physics::ExtractShapeTriangles(
                            core::Span<const core::byte>(blob.Data(), blob.Size()), outline))
                    {
                        m_boulderShape->outline = static_cast<core::Array<core::Float3>&&>(outline);
                    }
                    m_boulder = m_scene->CreateEntity(u8"boulder");
                    m_scene->SetLocalPosition(m_boulder, core::Float3{14.0f, 8.0f, 0.0f});
                    engine::physics::RigidBodyComponent& body = bodies->Add(m_boulder);
                    body.shape = physics::ShapeKind::Cooked;
                    body.collisionShape = m_boulderShape;
                    body.friction = 0.4f;
                }
            }
            // The crate stack (5x4 wall).
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 5; ++col)
                {
                    scene::EntityHandle e = m_scene->CreateEntity(u8"crate");
                    m_scene->SetLocalPosition(
                        e, core::Float3{(col - 2) * 1.05f, 0.5f + row * 1.05f, 0.0f});
                    engine::physics::RigidBodyComponent& body = bodies->Add(e);
                    body.halfExtents = core::Float3{0.5f, 0.5f, 0.5f};
                    body.friction = 0.6f;
                    m_crates.PushBack(e);
                }
            }
            // The character: arrow keys drive it, Space jumps; it climbs the ramp
            // (stairs + slopes) and shoves crates. The crates are ~1 tonne (1 m^3 at Jolt's
            // default density), so the default 500N barely nudges them - give the hero real
            // strength so the shove reads on screen (maxStrength is applied live each step).
            {
                m_hero = m_scene->CreateEntity(u8"hero");
                m_scene->SetLocalPosition(m_hero, core::Float3{-6.0f, 0.9f, 4.0f});
                auto& hero =
                    m_scene->GetSystem<engine::physics::CharacterComponentManager>()->Add(
                        m_hero);
                hero.maxStrength = 6000.0f;

                // Billboard proof: a nameplate riding the character, distance-scaled.
                m_nameplateDocument = core::MakeRef<gui::GuiDocument>(AppRoot());
                m_nameplateDocument->markup = core::String(
                    u8"<Panel padding=\"4\" background-color=\"rgb(20, 24, 30)\" border-radius=\"4\">"
                    u8"  <Label text=\"Hero\" font-size=\"13\"/>"
                    u8"</Panel>");
                auto* billboards = m_scene->GetSystem<engine::gui::GuiBillboardComponentManager>();
                if (billboards != nullptr)
                {
                    engine::gui::GuiBillboardComponent& plate = billboards->Add(m_hero);
                    plate.document = m_nameplateDocument;
                    plate.offset = core::Float3{0.0f, 1.4f, 0.0f}; // above the capsule
                    plate.scaleMode = engine::gui::GuiBillboardScale::Distance;
                    plate.referenceDistance = 12.0f;
                }
            }
            // World panel (UI world tier): an interactive kiosk standing in the arena -
            // a clickable counter ON A SURFACE. Aim at it and click; the pointer ray
            // routes into the panel (and is consumed - no crate shove through it).
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"kiosk");
                m_scene->SetLocalPosition(e, core::Float3{4.0f, 1.6f, -6.0f});
                m_kioskDocument = core::MakeRef<gui::GuiDocument>(AppRoot());
                m_kioskDocument->markup = core::String(
                    u8"<Panel flex-direction=\"column\" gap=\"10\" padding=\"14\""
                    u8"       background-color=\"rgb(28, 32, 40)\" border-radius=\"10\">"
                    u8"  <Label text=\"KIOSK\" font-size=\"22\"/>"
                    u8"  <Button id=\"kiosk-btn\" text=\"Taps: 0\" width=\"260\" height=\"56\"/>"
                    u8"</Panel>");
                auto* panels = m_scene->GetSystem<engine::gui::GuiWorldPanelComponentManager>();
                if (panels != nullptr)
                {
                    engine::gui::GuiWorldPanelComponent& panel = panels->Add(e);
                    panel.document = m_kioskDocument;
                    panel.sizeMeters = core::Float2{1.6f, 1.0f};
                    panel.pixelsPerMeter = 220.0f;
                    m_kioskEntity = e;
                }
            }

            // A motorized hinge spinner (joints): a blade welded to the world pivot,
            // spinning at 2 rad/s - walk the character into it to get batted away.
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"spinner");
                m_scene->SetLocalPosition(e, core::Float3{-6.0f, 1.0f, -4.0f});
                engine::physics::RigidBodyComponent& body = bodies->Add(e);
                body.halfExtents = core::Float3{2.0f, 0.1f, 0.1f};
                engine::physics::JointComponent& joint =
                    m_scene->GetSystem<engine::physics::JointComponentManager>()->Add(e);
                joint.kind = physics::JointKind::Hinge;
                joint.localAxis = core::Float3{0.0f, 1.0f, 0.0f};
                joint.motorEnabled = true;
                joint.motorTargetVelocity = 2.0f;
            }
            // Game-UI proof: a screen-tier HUD canvas (runtime document - the cooked
            // asset path is exercised by the editor flow). The button proves CONSUMPTION:
            // clicking it must NOT fire the crosshair shove.
            {
                m_hudDocument = core::MakeRef<gui::GuiDocument>(AppRoot());
                // Styled through the markup's inline CSS, as WebScene's HUD is.
                m_hudDocument->markup = core::String(
                    u8"<Panel flex-direction=\"column\" align-items=\"end\" padding=\"12\">" // top right: the ImGui panel is top left
                    u8"  <Panel flex-direction=\"column\" gap=\"8\" padding=\"12\" width=\"220\""
                    u8"         background-color=\"rgb(28, 32, 40)\" border-radius=\"8\">"
                    u8"    <Label id=\"hud-title\" text=\"PhysicsPlayground\" font-size=\"18\"/>"
                    u8"    <Button id=\"hud-btn\" text=\"Clicks: 0\" width=\"180\" height=\"36\"/>"
                    u8"  </Panel>"
                    u8"</Panel>");
                scene::EntityHandle e = m_scene->CreateEntity(u8"hud");
                auto* canvases = m_scene->GetSystem<engine::gui::GuiCanvasComponentManager>();
                if (canvases != nullptr)
                {
                    engine::gui::GuiCanvasComponent& canvas = canvases->Add(e);
                    canvas.document = m_hudDocument;
                    m_hudEntity = e;
                }
            }
            // A kinematic sweeper the update drives in a circle (knocks crates around).
            {
                m_sweeper = m_scene->CreateEntity(u8"sweeper");
                m_scene->SetLocalPosition(m_sweeper, core::Float3{6.0f, 0.75f, 0.0f});
                engine::physics::RigidBodyComponent& body = bodies->Add(m_sweeper);
                body.motion = physics::MotionKind::Kinematic;
                body.layer = physics::PhysicsLayer::Kinematic;
                body.halfExtents = core::Float3{0.4f, 0.75f, 0.4f};
            }
            // The trigger volume above the stack.
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"trigger");
                m_scene->SetLocalPosition(e, core::Float3{0.0f, 6.0f, 0.0f});
                engine::physics::RigidBodyComponent& body = bodies->Add(e);
                body.motion = physics::MotionKind::Kinematic;
                body.isTrigger = true;
                body.halfExtents = core::Float3{2.0f, 1.0f, 2.0f};
            }
        }

        void RespawnStack()
        {
            // Teleport the crates back into the wall formation (velocities cleared).
            if (m_physics == nullptr || m_physics->World() == nullptr)
            {
                return;
            }
            auto* bodies = m_scene->GetSystem<engine::physics::RigidBodyComponentManager>();
            int i = 0;
            for (scene::EntityHandle e : m_crates)
            {
                const int row = i / 5;
                const int col = i % 5;
                ++i;
                engine::physics::RigidBodyComponent* body = bodies->Get(e);
                if (body == nullptr || !body->body.IsValid())
                {
                    continue;
                }
                const core::Float3 position{(col - 2) * 1.05f, 0.5f + row * 1.05f, 0.0f};
                m_physics->World()->SetBodyTransform(body->body, position,
                                                     core::Quaternion::Identity);
                m_physics->World()->SetLinearVelocity(body->body, core::Float3{0, 0, 0});
                body->prevPosition = body->currPosition = position;
                body->prevRotation = body->currRotation = core::Quaternion::Identity;
            }
        }

        void DrawHud(runtime::IApplicationHost& host)
        {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::Begin("Physics");
            if (m_physics != nullptr && m_physics->World() != nullptr)
            {
                ImGui::Text("bodies: %zu", m_physics->World()->BodyCount());
                core::Float3 gravity = m_physics->World()->Gravity();
                if (ImGui::SliderFloat("gravity y", &gravity.y, -30.0f, 10.0f))
                {
                    m_physics->World()->SetGravity(gravity);
                }
            }
            float scale = host.Ctx().TimeScale();
            if (ImGui::SliderFloat("time scale", &scale, 0.0f, 2.0f))
            {
                host.Ctx().SetTimeScale(scale);
            }
            if (m_haveSurface)
            {
                ImGui::Text("last hit surface slot: %u", m_lastSurface);
            }
            if (auto* characters = m_scene->GetSystem<engine::physics::CharacterComponentManager>())
            {
                if (engine::physics::CharacterComponent* hero = characters->Get(m_hero))
                {
                    ImGui::Text("character: %s", hero->ground == physics::CharacterGround::OnGround
                                                     ? "grounded"
                                                     : "airborne");
                }
            }
            ImGui::Text("LMB shove | R respawn | arrows+Space character | Esc quit");
            ImGui::End();

            // Drive the sweeper in a circle through SCENE transforms (kinematic follow).
            m_sweepAngle += 0.6f * (1.0f / 60.0f) * host.Ctx().TimeScale();
            if (m_sweeper.IsAssigned())
            {
                core::Transform t = m_scene->GetLocalTransform(m_sweeper);
                t.position = core::Float3{6.0f * std::cos(m_sweepAngle), 0.75f,
                                          6.0f * std::sin(m_sweepAngle)};
                m_scene->SetLocalTransform(m_sweeper, t);
            }
        }

        void PushCameraToEntity()
        {
            core::Transform t = m_scene->GetLocalTransform(m_camera);
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);
        }

        scene::Scene* m_scene = nullptr;
        engine::physics::PhysicsSceneSystem* m_physics = nullptr;
        scene::EntityHandle m_camera;
        scene::EntityHandle m_sweeper;
        core::Array<scene::EntityHandle> m_crates;
        scene::EntityHandle m_boulder;
        scene::EntityHandle m_hero;
        scene::EntityHandle m_hudEntity;
        core::RefPtr<gui::GuiDocument> m_hudDocument;
        core::RefPtr<gui::GuiDocument> m_nameplateDocument;
        bool m_hudBound = false;
        core::RefPtr<gui::GuiDocument> m_kioskDocument;
        scene::EntityHandle m_kioskEntity{};
        bool m_kioskBound = false;
        core::u32 m_kioskTaps = 0;
        core::u32 m_hudClicks = 0;
        core::RefPtr<physics::CollisionShape> m_rampShape;
        core::RefPtr<physics::CollisionShape> m_boulderShape;
        core::u32 m_lastSurface = 0;
        bool m_haveSurface = false;
        samples::FlyCamera m_fly;
        f32 m_sweepAngle = 0.0f;
    };
}

APP_MAIN(PlaygroundApp)
