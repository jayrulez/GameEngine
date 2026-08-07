// PhysicsPlayground - the physics P1 consumer proof: a stack of falling crates, a
// kinematic sweeper, a trigger volume, and crosshair raycast shoving - all authored as
// RigidBody/Collider COMPONENTS on a scene, simulated by the Jolt-backed subsystem on the
// engine's fixed lane with render interpolation, and drawn via the physics DEBUG
// wireframes (green = awake dynamic, grey = sleeping, blue = static/kinematic, yellow =
// trigger). Fly with WASD/RMB-look; LEFT-CLICK shoves the body under the crosshair; R
// respawns the stack; the ImGui panel has gravity + time-scale sliders and live counts.

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Runtime.Client/AppMain.h"
#include "imgui.h"
#include <cmath>

import draconic.foundation;
import draconic.runtime;
import draconic.runtime.client;
import draconic.engine.defaultapp;
import draconic.shell;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.scene;
import draconic.engine.scene;
import draconic.engine.render;
import draconic.imgui;
import draconic.physics;
import draconic.physics.resource;
import draconic.engine.physics;
import draconic.ui;
import draconic.ui.resource;
import draconic.engine.ui;

#include "../Common/FlyCamera.h" // after the imports: uses draconic::foundation/runtime types

namespace foundation = draconic::foundation;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace shell = draconic::shell;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace physics = draconic::physics;
namespace imgui = draconic::imgui;

using foundation::f32;

namespace
{
    class PlaygroundApp final : public runtime::DefaultApplication
    {
    public:
        PlaygroundApp()
        {
#ifdef DRACONIC_PLAYGROUND_FONT
            SetUIFontPath(reinterpret_cast<const foundation::utf8char*>(DRACONIC_PLAYGROUND_FONT));
#endif
        }

    public:
        void Configure(runtime::IApplicationHost& host) override
        {
            // Physics/input/UI come from DefaultApplication (H2) - only the sample-local
            // extras register here (double-adding a subsystem = two instances ticking).
            runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight());
            }
        }

        void OnLaunch(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"playground");
            m_physics = m_scene->GetSystem<physics::PhysicsSceneSystem>();
            if (m_physics != nullptr)
            {
                m_physics->Settings().debugDraw = true;
            }

            // Camera.
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                cameras->Add(m_camera);
            }
            m_fly.position = foundation::Float3{8.0f, 6.0f, 14.0f};
            m_fly.yaw = 0.5f;
            m_fly.pitch = -0.3f;
            m_fly.moveSpeed = 10.0f;
            m_fly.fastSpeed = 30.0f;

            BuildWorld();
            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            foundation::ConsoleWrite(u8"PhysicsPlayground: WASD/RMB-look fly, LMB shove, R respawn.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override
        {
            runtime::DefaultApplication::OnUpdate(host, dt);
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
            if (auto* characters = m_scene->GetSystem<physics::CharacterComponentManager>())
            {
                if (physics::CharacterComponent* hero = characters->Get(m_hero))
                {
                    const f32 speed = 4.0f;
                    foundation::Float3 move{0.0f, 0.0f, 0.0f};
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
                if (auto* canvases = m_scene->GetSystem<draconic::ui::UICanvasComponentManager>())
                {
                    if (auto* canvas = canvases->Get(m_hudEntity);
                        canvas != nullptr && canvas->root.Get() != nullptr)
                    {
                        if (auto* button = foundation::Cast<draconic::ui::ViewGroup>(canvas->root.Get())
                                               ->FindByName<draconic::ui::Button>(u8"hud-btn"))
                        {
                            PlaygroundApp* self = this;
                            draconic::ui::Button* raw = button;
                            button->OnClick.Add(
                                [self, raw](draconic::ui::ButtonBase*)
                                {
                                    ++self->m_hudClicks;
                                    foundation::String text(u8"Clicks: ");
                                    const foundation::u32 n = self->m_hudClicks;
                                    if (n >= 10)
                                    {
                                        text.PushBack(
                                            static_cast<foundation::utf8char>('0' + n / 10 % 10));
                                    }
                                    text.PushBack(static_cast<foundation::utf8char>('0' + n % 10));
                                    raw->SetText(text.AsView());
                                    foundation::ConsoleWrite(u8"HUD button clicked\n");
                                });
                            m_hudBound = true;
                        }
                    }
                }
            }
            if (!m_kioskBound && m_scene != nullptr)
            {
                if (auto* panels = m_scene->GetSystem<draconic::ui::UIWorldPanelComponentManager>())
                {
                    if (auto* panel = panels->Get(m_kioskEntity);
                        panel != nullptr && panel->renderRoot.Get() != nullptr)
                    {
                        if (auto* button =
                                foundation::Cast<draconic::ui::ViewGroup>(panel->renderRoot.Get())
                                    ->FindByName<draconic::ui::Button>(u8"kiosk-btn"))
                        {
                            PlaygroundApp* self = this;
                            draconic::ui::Button* raw = button;
                            button->OnClick.Add(
                                [self, raw](draconic::ui::ButtonBase*)
                                {
                                    ++self->m_kioskTaps;
                                    foundation::String text(u8"Taps: ");
                                    const foundation::u32 n = self->m_kioskTaps;
                                    if (n >= 10)
                                    {
                                        text.PushBack(
                                            static_cast<foundation::utf8char>('0' + n / 10 % 10));
                                    }
                                    text.PushBack(static_cast<foundation::utf8char>('0' + n % 10));
                                    raw->SetText(text.AsView());
                                    foundation::ConsoleWrite(u8"Kiosk panel tapped\n");
                                });
                            m_kioskBound = true;
                        }
                    }
                }
            }

            // Crosshair shove: ray along the camera forward; impulse along the ray.
            // Gated on UI consumption: a click that lands ON the HUD never shoves.
            auto* gameUi = host.Ctx().GetSubsystem<draconic::ui::UISubsystem>();
            const bool uiAte = gameUi != nullptr && gameUi->PointerOverUI();
            if (!uiAte && input->Mouse()->IsButtonPressed(shell::MouseButton::Left) &&
                m_physics != nullptr && m_physics->World() != nullptr)
            {
                // Shove ray: through the CURSOR while it is free (click a crate to shove it);
                // along the camera forward (crosshair) while look-flying captures the mouse -
                // the same free-cursor/center-aim convention the kiosk world panel uses.
                foundation::Float3 rayDir = m_fly.Forward();
                const bool lookCaptured =
                    m_fly.mouseCaptured || input->Mouse()->IsButtonDown(shell::MouseButton::Right);
                shell::IWindow* win = host.Shell()->MainWindow();
                if (!lookCaptured && win != nullptr && win->Width() > 0 && win->Height() > 0)
                {
                    foundation::f32 fovY = 1.04719755f; // the camera component's authored fov
                    if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
                    {
                        if (render::CameraComponent* cam = cameras->Get(m_camera))
                        {
                            fovY = cam->fovYRadians;
                        }
                    }
                    const foundation::f32 w = static_cast<foundation::f32>(win->Width());
                    const foundation::f32 h = static_cast<foundation::f32>(win->Height());
                    const foundation::f32 ndcX = (input->Mouse()->X() / w) * 2.0f - 1.0f;
                    const foundation::f32 ndcY = 1.0f - (input->Mouse()->Y() / h) * 2.0f;
                    const foundation::f32 tanHalfY = foundation::Tan(fovY * 0.5f);
                    const foundation::f32 tanHalfX = tanHalfY * (w / h);
                    rayDir = foundation::Normalized(m_fly.Forward() + m_fly.Right() * (ndcX * tanHalfX) +
                                              m_fly.Up() * (ndcY * tanHalfY));
                }
                physics::RayHit hit;
                if (m_physics->World()->RayCast(m_fly.position, rayDir, 200.0f, hit))
                {
                    // A visible whack: crates are ~1000 kg (1 m^3 at Jolt's default density), so
                    // the impulse must be in the thousands - 400 only WOKE the body (delta-v
                    // 0.4 m/s reads as a highlight, not a shove).
                    m_physics->World()->AddImpulse(
                        hit.body, foundation::Float3{rayDir.x * 4000.0f, rayDir.y * 4000.0f + 1400.0f,
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
                        foundation::ConsoleWrite(u8"PhysicsPlayground: trigger entered!\n");
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
                if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
                {
                    if (render::CameraComponent* cam = cameras->Get(m_camera))
                    {
                        cam->aspect =
                            static_cast<f32>(frame.width) / static_cast<f32>(frame.height);
                    }
                }
            }
            runtime::DefaultApplication::OnRenderWindow(host, frame);
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
        }

    private:
        void BuildWorld()
        {
            auto* bodies = m_scene->GetSystem<physics::RigidBodyComponentManager>();

            // Ground: an infinite plane (P2) - crates land anywhere, not just on a slab.
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"ground");
                physics::RigidBodyComponent& body = bodies->Add(e);
                body.motion = physics::MotionKind::Static;
                body.layer = physics::PhysicsLayer::Static;
                body.shape = physics::ShapeKind::Plane;
                body.planeHalfExtent = 200.0f;
            }
            // A cooked TRIANGLE-MESH ramp (P2): two panels, two material slots - the
            // crosshair ray reports which slot it hit (HUD "surface").
            {
                const foundation::Float3 positions[] = {
                    {6.0f, 0.0f, -3.0f}, {6.0f, 0.0f, 3.0f},   // low edge
                    {12.0f, 3.0f, 3.0f}, {12.0f, 3.0f, -3.0f}, // high edge
                    {18.0f, 3.0f, 3.0f}, {18.0f, 3.0f, -3.0f}, // flat top end
                };
                const foundation::u32 indices[] = {0, 1, 2, 0, 2, 3,  // sloped panel
                                             3, 2, 4, 3, 4, 5}; // flat panel
                const foundation::u32 slots[] = {1, 1, 2, 2};
                foundation::Array<foundation::byte> blob;
                if (physics::CookTriangleMesh(foundation::Span<const foundation::Float3>(positions, 6),
                                              foundation::Span<const foundation::u32>(indices, 12),
                                              foundation::Span<const foundation::u32>(slots, 4), blob))
                {
                    m_rampShape = foundation::MakeRef<physics::CollisionShape>(foundation::DefaultAllocator());
                    m_rampShape->blob.Resize(blob.Size());
                    foundation::MemCopy(m_rampShape->blob.Data(), blob.Data(), blob.Size());
                    foundation::Array<foundation::Float3> outline;
                    if (physics::ExtractShapeTriangles(
                            foundation::Span<const foundation::byte>(blob.Data(), blob.Size()), outline))
                    {
                        m_rampShape->outline = static_cast<foundation::Array<foundation::Float3>&&>(outline);
                    }
                    scene::EntityHandle e = m_scene->CreateEntity(u8"ramp");
                    physics::RigidBodyComponent& body = bodies->Add(e);
                    body.motion = physics::MotionKind::Static;
                    body.layer = physics::PhysicsLayer::Static;
                    body.shape = physics::ShapeKind::Cooked;
                    body.collisionShape = m_rampShape;
                }
            }
            // A cooked CONVEX boulder (P2) dropped onto the ramp - hulls may be dynamic.
            {
                foundation::Array<foundation::Float3> points;
                const foundation::f32 axes[3][3] = {{0.9f, 0, 0}, {0, 0.7f, 0}, {0, 0, 0.8f}};
                for (const auto& a : axes)
                {
                    points.PushBack(foundation::Float3{a[0], a[1], a[2]});
                    points.PushBack(foundation::Float3{-a[0], -a[1], -a[2]});
                }
                points.PushBack(foundation::Float3{0.5f, 0.5f, 0.5f});
                points.PushBack(foundation::Float3{-0.5f, 0.5f, -0.5f});
                foundation::Array<foundation::byte> blob;
                if (physics::CookConvexHull(
                        foundation::Span<const foundation::Float3>(points.Data(), points.Size()), blob))
                {
                    m_boulderShape =
                        foundation::MakeRef<physics::CollisionShape>(foundation::DefaultAllocator());
                    m_boulderShape->blob.Resize(blob.Size());
                    foundation::MemCopy(m_boulderShape->blob.Data(), blob.Data(), blob.Size());
                    foundation::Array<foundation::Float3> outline;
                    if (physics::ExtractShapeTriangles(
                            foundation::Span<const foundation::byte>(blob.Data(), blob.Size()), outline))
                    {
                        m_boulderShape->outline = static_cast<foundation::Array<foundation::Float3>&&>(outline);
                    }
                    m_boulder = m_scene->CreateEntity(u8"boulder");
                    m_scene->SetLocalPosition(m_boulder, foundation::Float3{14.0f, 8.0f, 0.0f});
                    physics::RigidBodyComponent& body = bodies->Add(m_boulder);
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
                        e, foundation::Float3{(col - 2) * 1.05f, 0.5f + row * 1.05f, 0.0f});
                    physics::RigidBodyComponent& body = bodies->Add(e);
                    body.halfExtents = foundation::Float3{0.5f, 0.5f, 0.5f};
                    body.friction = 0.6f;
                    m_crates.PushBack(e);
                }
            }
            // The character (P3): arrow keys drive it, Space jumps; it climbs the ramp
            // (stairs + slopes) and shoves crates with its 500N of push.
            {
                m_hero = m_scene->CreateEntity(u8"hero");
                m_scene->SetLocalPosition(m_hero, foundation::Float3{-6.0f, 0.9f, 4.0f});
                m_scene->GetSystem<physics::CharacterComponentManager>()->Add(m_hero);

                // Billboard proof (UI P2): a nameplate riding the character, distance-scaled.
                m_nameplateDocument =
                    foundation::MakeRef<draconic::ui::UIDocument>(foundation::DefaultAllocator());
                m_nameplateDocument->markup = foundation::String(
                    u8"<Panel padding=\"4\""
                    u8"       style=\"background: rounded-rect(rgb(20, 24, 30), radius=4);\">"
                    u8"  <Label text=\"Hero\" font-size=\"13\"/>"
                    u8"</Panel>");
                auto* billboards = m_scene->GetSystem<draconic::ui::UIBillboardComponentManager>();
                if (billboards != nullptr)
                {
                    draconic::ui::UIBillboardComponent& plate = billboards->Add(m_hero);
                    plate.document = m_nameplateDocument;
                    plate.offset = foundation::Float3{0.0f, 1.4f, 0.0f}; // above the capsule
                    plate.scaleMode = draconic::ui::BillboardScale::Distance;
                    plate.referenceDistance = 12.0f;
                }
            }
            // World panel (UI world tier): an interactive kiosk standing in the arena -
            // a clickable counter ON A SURFACE. Aim at it and click; the pointer ray
            // routes into the panel (and is consumed - no crate shove through it).
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"kiosk");
                m_scene->SetLocalPosition(e, foundation::Float3{4.0f, 1.6f, -6.0f});
                m_kioskDocument = foundation::MakeRef<draconic::ui::UIDocument>(foundation::DefaultAllocator());
                m_kioskDocument->markup = foundation::String(
                    u8"<Panel padding=\"14\""
                    u8" style=\"background: rounded-rect(rgb(28, 32, 40), radius=10);\">"
                    u8"  <Flex direction=\"vertical\" spacing=\"10\">"
                    u8"    <Label text=\"KIOSK\" font-size=\"22\"/>"
                    u8"    <Button id=\"kiosk-btn\" text=\"Taps: 0\" width=\"260\" height=\"56\"/>"
                    u8"  </Flex>"
                    u8"</Panel>");
                auto* panels = m_scene->GetSystem<draconic::ui::UIWorldPanelComponentManager>();
                if (panels != nullptr)
                {
                    draconic::ui::UIWorldPanelComponent& panel = panels->Add(e);
                    panel.document = m_kioskDocument;
                    panel.sizeMeters = foundation::Float2{1.6f, 1.0f};
                    panel.pixelsPerMeter = 220.0f;
                    m_kioskEntity = e;
                }
            }

            // A motorized hinge spinner (P3 joints): a blade welded to the world pivot,
            // spinning at 2 rad/s - walk the character into it to get batted away.
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"spinner");
                m_scene->SetLocalPosition(e, foundation::Float3{-6.0f, 1.0f, -4.0f});
                physics::RigidBodyComponent& body = bodies->Add(e);
                body.halfExtents = foundation::Float3{2.0f, 0.1f, 0.1f};
                physics::JointComponent& joint =
                    m_scene->GetSystem<physics::JointComponentManager>()->Add(e);
                joint.kind = physics::JointKind::Hinge;
                joint.localAxis = foundation::Float3{0.0f, 1.0f, 0.0f};
                joint.motorEnabled = true;
                joint.motorTargetVelocity = 2.0f;
            }
            // Game-UI P1 proof: a screen-tier HUD canvas (runtime document - the cooked
            // asset path is exercised by the editor flow). The button proves CONSUMPTION:
            // clicking it must NOT fire the crosshair shove.
            {
                m_hudDocument = foundation::MakeRef<draconic::ui::UIDocument>(foundation::DefaultAllocator());
                // The proven UISandbox pause-menu vocabulary: kebab-case attributes,
                // EXPLICIT sizes (an unsized child in a root Flex stretches to a bar).
                m_hudDocument->markup = foundation::String(
                    u8"<Flex direction=\"vertical\" align=\"start\" padding=\"12\" spacing=\"8\">"
                    u8"  <Panel padding=\"12\" width=\"220\""
                    u8"         style=\"background: rounded-rect(rgb(28, 32, 40), radius=8);\">"
                    u8"    <Flex direction=\"vertical\" spacing=\"8\">"
                    u8"      <Label id=\"hud-title\" text=\"PhysicsPlayground\" font-size=\"18\"/>"
                    u8"      <Button id=\"hud-btn\" text=\"Clicks: 0\" width=\"180\" "
                    u8"height=\"36\"/>"
                    u8"    </Flex>"
                    u8"  </Panel>"
                    u8"</Flex>");
                scene::EntityHandle e = m_scene->CreateEntity(u8"hud");
                auto* canvases = m_scene->GetSystem<draconic::ui::UICanvasComponentManager>();
                if (canvases != nullptr)
                {
                    draconic::ui::UICanvasComponent& canvas = canvases->Add(e);
                    canvas.document = m_hudDocument;
                    m_hudEntity = e;
                }
            }
            // A kinematic sweeper the update drives in a circle (knocks crates around).
            {
                m_sweeper = m_scene->CreateEntity(u8"sweeper");
                m_scene->SetLocalPosition(m_sweeper, foundation::Float3{6.0f, 0.75f, 0.0f});
                physics::RigidBodyComponent& body = bodies->Add(m_sweeper);
                body.motion = physics::MotionKind::Kinematic;
                body.layer = physics::PhysicsLayer::Kinematic;
                body.halfExtents = foundation::Float3{0.4f, 0.75f, 0.4f};
            }
            // The trigger volume above the stack.
            {
                scene::EntityHandle e = m_scene->CreateEntity(u8"trigger");
                m_scene->SetLocalPosition(e, foundation::Float3{0.0f, 6.0f, 0.0f});
                physics::RigidBodyComponent& body = bodies->Add(e);
                body.motion = physics::MotionKind::Kinematic;
                body.isTrigger = true;
                body.halfExtents = foundation::Float3{2.0f, 1.0f, 2.0f};
            }
        }

        void RespawnStack()
        {
            // Teleport the crates back into the wall formation (velocities cleared).
            if (m_physics == nullptr || m_physics->World() == nullptr)
            {
                return;
            }
            auto* bodies = m_scene->GetSystem<physics::RigidBodyComponentManager>();
            int i = 0;
            for (scene::EntityHandle e : m_crates)
            {
                const int row = i / 5;
                const int col = i % 5;
                ++i;
                physics::RigidBodyComponent* body = bodies->Get(e);
                if (body == nullptr || !body->body.IsValid())
                {
                    continue;
                }
                const foundation::Float3 position{(col - 2) * 1.05f, 0.5f + row * 1.05f, 0.0f};
                m_physics->World()->SetBodyTransform(body->body, position,
                                                     foundation::Quaternion::Identity);
                m_physics->World()->SetLinearVelocity(body->body, foundation::Float3{0, 0, 0});
                body->prevPosition = body->currPosition = position;
                body->prevRotation = body->currRotation = foundation::Quaternion::Identity;
            }
        }

        void DrawHud(runtime::IApplicationHost& host)
        {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::Begin("Physics");
            if (m_physics != nullptr && m_physics->World() != nullptr)
            {
                ImGui::Text("bodies: %zu", m_physics->World()->BodyCount());
                foundation::Float3 gravity = m_physics->World()->Gravity();
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
            if (auto* characters = m_scene->GetSystem<physics::CharacterComponentManager>())
            {
                if (physics::CharacterComponent* hero = characters->Get(m_hero))
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
                foundation::Transform t = m_scene->GetLocalTransform(m_sweeper);
                t.position = foundation::Float3{6.0f * std::cos(m_sweepAngle), 0.75f,
                                          6.0f * std::sin(m_sweepAngle)};
                m_scene->SetLocalTransform(m_sweeper, t);
            }
        }

        void PushCameraToEntity()
        {
            foundation::Transform t = m_scene->GetLocalTransform(m_camera);
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);
        }

        scene::Scene* m_scene = nullptr;
        physics::PhysicsSceneSystem* m_physics = nullptr;
        scene::EntityHandle m_camera;
        scene::EntityHandle m_sweeper;
        foundation::Array<scene::EntityHandle> m_crates;
        scene::EntityHandle m_boulder;
        scene::EntityHandle m_hero;
        scene::EntityHandle m_hudEntity;
        foundation::RefPtr<draconic::ui::UIDocument> m_hudDocument;
        foundation::RefPtr<draconic::ui::UIDocument> m_nameplateDocument;
        bool m_hudBound = false;
        foundation::RefPtr<draconic::ui::UIDocument> m_kioskDocument;
        scene::EntityHandle m_kioskEntity{};
        bool m_kioskBound = false;
        foundation::u32 m_kioskTaps = 0;
        foundation::u32 m_hudClicks = 0;
        foundation::RefPtr<physics::CollisionShape> m_rampShape;
        foundation::RefPtr<physics::CollisionShape> m_boulderShape;
        foundation::u32 m_lastSurface = 0;
        bool m_haveSurface = false;
        draconic::samples::FlyCamera m_fly;
        f32 m_sweepAngle = 0.0f;
    };
}

DRACONIC_APP_MAIN(PlaygroundApp)
