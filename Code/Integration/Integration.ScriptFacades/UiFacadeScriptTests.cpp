// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.ui.script - the `ui` script facade exercised end-to-end on both backends.
//
// This drives a real AngelScript / Luau VM against a real screen tier: a UIContext + RootView + a
// gamekit ScreenStack, with an `instantiate` that returns a document tree carrying named controls
// (a Label "status", a ProgressBar "progress", a Button "cancel"). A script pushes the document,
// finds controls by name + type, writes + reads them back, checks a type-mismatch loud-null, and pops.
// We read the round-tripped values (proving writes reached the REAL views) and the live stack count.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;         // UIContext / RootView / Label / ProgressBar / Button / FrameLayout
import foundation.ui.gamekit; // ScreenStack
import engine.ui.script;      // the `ui` facade + UiScreenScriptBinding + InstallUiScreenScriptService
import foundation.script;
import foundation.script.facades; // ScriptRuntimeBinding (a context's run)
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;

namespace
{
    namespace ui = foundation::ui;
    namespace gamekit = foundation::ui::gamekit;

    // A real screen tier: context + root + stack, with an instantiate that returns a fresh document
    // tree of named controls (the facade wraps a plain root in a default UIScreen on push).
    struct UiBed
    {
        ui::UIContext context{DefaultAllocator()};
        RefPtr<ui::RootView> root;
        gamekit::ScreenStack stack;
        engine::uiscript::UiScreenScriptBinding binding;

        UiBed()
        {
            root = MakeRef<ui::RootView>(DefaultAllocator());
            context.AddRootView(root.Get());
            stack.Attach(root.Get());
            binding.stack = &stack; // the stack's attached root IS the root source
            binding.instantiate = Function<RefPtr<ui::View>(const Guid&)>{
                [](const Guid&) -> RefPtr<ui::View>
                {
                    auto group = MakeRef<ui::FrameLayout>(DefaultAllocator());
                    auto label = MakeRef<ui::Label>(DefaultAllocator());
                    label->Name = String(u8"status");
                    group->AddView(label.Get());
                    auto bar = MakeRef<ui::ProgressBar>(DefaultAllocator());
                    bar->Name = String(u8"progress");
                    group->AddView(bar.Get());
                    auto btn = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Cancel"));
                    btn->Name = String(u8"cancel");
                    group->AddView(btn.Get());
                    auto slider = MakeRef<ui::Slider>(DefaultAllocator());
                    slider->Name = String(u8"volume");
                    group->AddView(slider.Get());
                    auto image = MakeRef<ui::ImageView>(DefaultAllocator());
                    image->Name = String(u8"minimap");
                    group->AddView(image.Get());
                    auto story = MakeRef<ui::RichText>(DefaultAllocator());
                    story->Name = String(u8"story");
                    group->AddView(story.Get());
                    return group;
                }};
        }

        // The live controls, found through the screen root (null once the screen is popped).
        [[nodiscard]] ui::Label* StatusLabel() const
        {
            return root->FindByName<ui::Label>(u8"status");
        }
    };

    // Assert the outcome of any backend's run of the shared script. Kept to backend-NEUTRAL types
    // (String + bool globals; the numeric stack count is checked on the C++ side to dodge the
    // AngelScript-i32 / Luau-f64 number-type split).
    void CheckOutcome(const UiBed& bed, IScriptContext& ctx)
    {
        // Writes reached the REAL views: the in-script round-trip read "Loading" back off the label.
        CHECK(ctx.GetGlobal(u8"t").template Get<String>() == StringView(u8"Loading"));
        // Loud-null: "status" is a Label, so findButton("status") is a null-but-valid handle.
        CHECK(ctx.GetGlobal(u8"wrong").template Get<bool>() == false);
        // The pop happened (C++ side): the stack is empty and the popped screen's controls are gone.
        CHECK(bed.stack.Count() == 0);
        CHECK(bed.StatusLabel() == nullptr);
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("ui-facade: AngelScript pushes a screen, finds + drives typed controls, reads back, pops")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();

    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    // AngelScript: the `ui` facade's statics live in its namespace (ui::push); finders return handles
    // the call chains onto, exactly like the component `.of(...)` surface.
    const Status status =
        ctx->Load(u8"string t; bool wrong;\n"
                  u8"void main() {\n"
                  u8"  ui::push(Guid(17, 34));\n"
                  u8"  ui::findLabel(\"status\").setText(\"Loading\");\n"
                  u8"  ui::findProgressBar(\"progress\").setValue(0.5);\n"
                  u8"  t = ui::findLabel(\"status\").text;\n"
                  u8"  wrong = ui::findButton(\"status\").isValid();\n"
                  u8"  ui::pop();\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CheckOutcome(bed, *ctx);
}

TEST_CASE("ui-facade: a script's screens land on its own run's stack when the host gives each run one")
{
    // The editor's Game tabs: two runs' scripts push their screens, each onto its run's stack (the
    // binding's stackForRun, as UISubsystem::ScreensFor answers it), and find only their own.
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);

    UiBed bed; // its stack stands for the shared tier: nothing may land there
    RefPtr<ui::RootView> rootA = MakeRef<ui::RootView>(DefaultAllocator());
    RefPtr<ui::RootView> rootB = MakeRef<ui::RootView>(DefaultAllocator());
    bed.context.AddRootView(rootA.Get());
    bed.context.AddRootView(rootB.Get());
    gamekit::ScreenStack stackA;
    gamekit::ScreenStack stackB;
    stackA.Attach(rootA.Get());
    stackB.Attach(rootB.Get());
    int runA = 0;
    int runB = 0;
    bed.binding.stackForRun = Function<gamekit::ScreenStack*(const void*)>{
        [&](const void* run) -> gamekit::ScreenStack*
        { return run == &runA ? &stackA : run == &runB ? &stackB : &bed.stack; }};

    ScriptRuntimeBinding runtimeA;
    ScriptRuntimeBinding runtimeB;
    runtimeA.run = &runA;
    runtimeB.run = &runB;
    RefPtr<IScriptContext> ctxA = manager->CreateContext();
    RefPtr<IScriptContext> ctxB = manager->CreateContext();
    ctxA->SetService(kScriptRuntimeService, &runtimeA);
    ctxB->SetService(kScriptRuntimeService, &runtimeB);
    engine::uiscript::InstallUiScreenScriptService(*ctxA, bed.binding);
    engine::uiscript::InstallUiScreenScriptService(*ctxB, bed.binding);

    const StringView script = u8"bool found;\n"
                              u8"void main() {\n"
                              u8"  ui::push(Guid(17, 34));\n"
                              u8"  found = ui::findLabel(\"status\").isValid();\n"
                              u8"}\n";
    REQUIRE(ctxA->Load(script, u8"main").IsOk());
    CHECK(stackA.Count() == 1u);
    CHECK(stackB.Count() == 0u);
    REQUIRE(ctxB->Load(script, u8"main").IsOk());
    CHECK(stackB.Count() == 1u);
    CHECK(bed.stack.Count() == 0u);
    CHECK(ctxA->GetGlobal(u8"found").Get<bool>());
    CHECK(ctxB->GetGlobal(u8"found").Get<bool>());
    CHECK(rootA->FindByName<ui::Label>(u8"status") != nullptr);
    CHECK(rootB->FindByName<ui::Label>(u8"status") != nullptr);
}

TEST_CASE("ui-facade: findLabel resolves through the stack's root (the only root source)")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();

    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    UiBed bed;
    // The embedded-host (PaperKid HUD) bug was a separately-captured stale/null screenRoot while
    // the ScreenStack pointed at the live root; the binding no longer carries a raw root pointer
    // at all - ui::push lands on the stack's root and ui::findLabel searches the SAME root by
    // construction. This pins that a pushed screen is findable with nothing but the stack wired.
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    // A screen finds by name as ui does, with find (Sedulous 59680505).
    const Status status = ctx->Load(u8"string t; bool onTop; bool missing;\n"
                                    u8"void main() {\n"
                                    u8"  ui::push(Guid(17, 34));\n"
                                    u8"  ui::findLabel(\"status\").setText(\"Loading\");\n"
                                    u8"  t = ui::findLabel(\"status\").text;\n"
                                    u8"  onTop = ui::top().find(\"status\").isValid();\n"
                                    u8"  missing = ui::top().find(\"nope\").isValid();\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"t").template Get<String>() == StringView(u8"Loading"));
    CHECK(ctx->GetGlobal(u8"onTop").template Get<bool>());
    CHECK_FALSE(ctx->GetGlobal(u8"missing").template Get<bool>());
    CHECK(bed.stack.Count() == 1);
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("ui-facade: Luau pushes a screen, finds + drives typed controls, reads back, pops")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    // Luau: method calls use `:` and properties read with `.`, but the surface is otherwise identical
    // to AngelScript - the backend-neutrality proof.
    const Status status =
        ctx->Load(u8"ui.push(Guid.new(17, 34))\n"
                  u8"ui.findLabel(\"status\"):setText(\"Loading\")\n"
                  u8"ui.findProgressBar(\"progress\"):setValue(0.5)\n"
                  u8"t = ui.findLabel(\"status\").text\n"
                  u8"wrong = ui.findButton(\"status\"):isValid()\n"
                  u8"ui.pop()\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CheckOutcome(bed, *ctx);
}
#endif // OPTION_HAS_LUAU

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("ui-facade: AngelScript sets what an image shows by texture asset id, and reads it back")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    const Status status = ctx->Load(u8"bool readBack = false;\n"
                                    u8"bool wrong = true;\n"
                                    u8"void main() {\n"
                                    u8"  ui::push(Guid(17, 34));\n"
                                    u8"  Image map = ui::findImage(\"minimap\");\n"
                                    u8"  map.setSource(Guid(5, 6));\n"
                                    u8"  readBack = map.source.low == 6 && map.source.high == 5;\n"
                                    u8"  wrong = ui::findImage(\"cancel\").isValid();\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"readBack").template Get<bool>());
    CHECK_FALSE(ctx->GetGlobal(u8"wrong").template Get<bool>()); // a button is no image
    ui::ImageView* map = bed.root->FindByName<ui::ImageView>(u8"minimap");
    REQUIRE(map != nullptr);
    CHECK(map->Source.Value() == Format(u8"{}", Guid{5, 6})); // what the provider resolves
}

TEST_CASE("ui-facade: AngelScript moves and turns a view (a minimap marker), and reads it back")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    const Status status = ctx->Load(u8"bool readBack = false;\n"
                                    u8"void main() {\n"
                                    u8"  ui::push(Guid(17, 34));\n"
                                    u8"  Image marker = ui::findImage(\"minimap\");\n"
                                    u8"  marker.setTranslation(64.0f, 32.0f);\n"
                                    u8"  marker.setRotation(180.0f);\n"
                                    u8"  readBack = marker.translation.x == 64.0f && marker.rotation > 179.0f;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"readBack").template Get<bool>());
    ui::ImageView* marker = bed.root->FindByName<ui::ImageView>(u8"minimap");
    REQUIRE(marker != nullptr);
    CHECK(marker->Transform.Translation.y == doctest::Approx(32.0f));
    CHECK(marker->Transform.Rotation == doctest::Approx(DegreesToRadians(180.0f)));
}

TEST_CASE("ui-facade: AngelScript and Luau tween a view, with or without an Ease")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    const bool backends[] = {false, true};
    for (bool luau : backends)
    {
        RefPtr<IScriptManager> manager = luau ? CreateLuauScriptManager(DefaultAllocator())
                                              : angelscript::CreateScriptManager(DefaultAllocator());
        RegisterReflectedTypes(*manager);
        RefPtr<IScriptContext> ctx = manager->CreateContext();
        UiBed bed;
        engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

        // Zero seconds lands at once, so the script reads the ends back; the eased overloads
        // and the plain ones both bind, and a pulse starts without a frame to run it.
        const Status status =
            luau ? ctx->Load(u8"ui.push(Guid.new(17, 34))\n"
                             u8"local marker = ui.findImage(\"minimap\")\n"
                             u8"marker:moveTo(10, 20, 0, Ease.OutBack)\n"
                             u8"marker:scaleTo(2, 0)\n"
                             u8"marker:rotateTo(30, 0, Ease.Linear)\n"
                             u8"marker:fadeTo(0.5, 0, Ease.Out)\n"
                             u8"marker:pulse(1.2, 0.3)\n"
                             u8"tweened = marker.translation.y == 20 and marker.scale == 2 and marker.opacity == 0.5\n",
                             u8"main")
                 : ctx->Load(u8"bool tweened = false;\n"
                             u8"void main() {\n"
                             u8"  ui::push(Guid(17, 34));\n"
                             u8"  Image marker = ui::findImage(\"minimap\");\n"
                             u8"  marker.moveTo(10.0f, 20.0f, 0.0f, Ease::OutBack);\n"
                             u8"  marker.scaleTo(2.0f, 0.0f);\n"
                             u8"  marker.rotateTo(30.0f, 0.0f, Ease::Linear);\n"
                             u8"  marker.fadeTo(0.5f, 0.0f, Ease::Out);\n"
                             u8"  marker.pulse(1.2f, 0.3f);\n"
                             u8"  tweened = marker.translation.y == 20.0f && marker.scale == 2.0f && marker.opacity == 0.5f;\n"
                             u8"}\n",
                             u8"main");
        REQUIRE(status.IsOk());
        CHECK(ctx->GetGlobal(u8"tweened").template Get<bool>());
        ui::ImageView* marker = bed.root->FindByName<ui::ImageView>(u8"minimap");
        REQUIRE(marker != nullptr);
        CHECK(marker->Transform.Translation.x == doctest::Approx(10.0f));
        CHECK(marker->Transform.Rotation == doctest::Approx(DegreesToRadians(30.0f)));
    }
}

TEST_CASE("ui-facade: AngelScript binds a button click to a script delegate; firing runs the handler")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();

    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    // Bind the "cancel" button's click to a handler that does a STRUCTURAL mutation - it pops its own
    // screen - so we prove the handler both defers and can safely restructure. A void no-arg handler is
    // wrapped in the engine's `void Action()` funcdef (ScriptDelegate is double(double)).
    const Status status =
        ctx->Load(u8"bool clicked = false;\n"
                  u8"void onCancel() { clicked = true; ui::pop(); }\n"
                  u8"void main() {\n"
                  u8"  ui::push(Guid(17, 34));\n"
                  u8"  ui::findButton(\"cancel\").onClick(Action(onCancel));\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"clicked").template Get<bool>() == false);
    CHECK(bed.stack.Count() == 1); // screen is up

    // Fire the REAL button's OnClick from C++. The handler does NOT run inline (a click dispatches while
    // the tree is live) - it defers through the mutation queue, so nothing has changed yet.
    ui::Button* cancel = bed.root->FindByName<ui::Button>(u8"cancel");
    REQUIRE(cancel != nullptr);
    cancel->OnClick.Invoke(cancel);
    CHECK(ctx->GetGlobal(u8"clicked").template Get<bool>() == false); // deferred, not inline
    CHECK(bed.stack.Count() == 1);

    // Draining next frame runs the handler at a quiescent point, where its structural pop is safe.
    bed.context.MutationQueueRef().Drain();
    CHECK(ctx->GetGlobal(u8"clicked").template Get<bool>() == true);
    CHECK(bed.stack.Count() == 0); // the handler's ui::pop() took effect
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("ui-facade: Luau drives a slider - range, step, value, and a change handler")
{
    // Sedulous 8c8c0bd4: a settings menu's volume control from a script.
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    const Status status =
        ctx->Load(u8"changes = 0\n"
                  u8"ui.push(Guid.new(17, 34))\n"
                  u8"local volume = ui.findSlider(\"volume\")\n"
                  u8"volume:setRange(0, 10)\n"
                  u8"volume:setStep(1)\n"
                  u8"volume:setValue(4)\n"
                  u8"value = volume.value\n"
                  u8"top = volume.max\n"
                  u8"volume:onChanged(function() changes = changes + 1 end)\n"
                  u8"wrong = ui.findSlider(\"cancel\"):isValid()\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"value").template Get<f64>() == doctest::Approx(4.0));
    CHECK(ctx->GetGlobal(u8"top").template Get<f64>() == doctest::Approx(10.0));
    CHECK_FALSE(ctx->GetGlobal(u8"wrong").template Get<bool>()); // a button is no slider

    // A change (an arrow, a pad, a drag) runs the handler, deferred to the next drain.
    ui::Slider* volume = bed.root->FindByName<ui::Slider>(u8"volume");
    REQUIRE(volume != nullptr);
    volume->Value.SetValue(7.0f);
    CHECK(ctx->GetGlobal(u8"changes").template Get<f64>() == doctest::Approx(0.0));
    bed.context.MutationQueueRef().Drain();
    CHECK(ctx->GetGlobal(u8"changes").template Get<f64>() == doctest::Approx(1.0));
}

TEST_CASE("ui-facade: Luau sets what an image shows, and a nil id clears it")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    const Status status = ctx->Load(u8"ui.push(Guid.new(17, 34))\n"
                                    u8"local map = ui.findImage(\"minimap\")\n"
                                    u8"map:setSource(Guid.new(5, 6))\n"
                                    u8"set = not map.source:IsNil()\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"set").template Get<bool>());
    ui::ImageView* map = bed.root->FindByName<ui::ImageView>(u8"minimap");
    REQUIRE(map != nullptr);
    CHECK(map->Source.Value() == Format(u8"{}", Guid{5, 6}));

    REQUIRE(ctx->Load(u8"ui.findImage(\"minimap\"):setSource(Guid.new(0, 0))\n", u8"clear")
                .IsOk());
    CHECK(map->Source.Value().IsEmpty());
}

TEST_CASE("ui-facade: Luau moves and turns a view (a minimap marker)")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    const Status status = ctx->Load(u8"ui.push(Guid.new(17, 34))\n"
                                    u8"local marker = ui.findImage(\"minimap\")\n"
                                    u8"marker:setTranslation(-8, 20)\n"
                                    u8"marker:setRotation(-45)\n"
                                    u8"moved = marker.translation.y == 20 and marker.rotation < -44\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"moved").template Get<bool>());
    ui::ImageView* marker = bed.root->FindByName<ui::ImageView>(u8"minimap");
    REQUIRE(marker != nullptr);
    CHECK(marker->Transform.Translation.x == doctest::Approx(-8.0f));
    CHECK(marker->Transform.Rotation == doctest::Approx(DegreesToRadians(-45.0f)));
}

TEST_CASE("ui-facade: Luau binds a button click to a script function; firing runs the handler")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    // Luau passes the function value directly (no ScriptDelegate wrapper); method call uses `:`. The
    // handler pops its own screen - a structural mutation - to prove the deferred path is safe.
    const Status status =
        ctx->Load(u8"clicked = false\n"
                  u8"ui.push(Guid.new(17, 34))\n"
                  u8"ui.findButton(\"cancel\"):onClick(function() clicked = true; ui.pop() end)\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"clicked").template Get<bool>() == false);
    CHECK(bed.stack.Count() == 1);

    ui::Button* cancel = bed.root->FindByName<ui::Button>(u8"cancel");
    REQUIRE(cancel != nullptr);
    cancel->OnClick.Invoke(cancel);
    CHECK(ctx->GetGlobal(u8"clicked").template Get<bool>() == false); // deferred, not inline
    CHECK(bed.stack.Count() == 1);

    bed.context.MutationQueueRef().Drain();
    CHECK(ctx->GetGlobal(u8"clicked").template Get<bool>() == true);
    CHECK(bed.stack.Count() == 0); // the handler's ui.pop() took effect
}
#endif // OPTION_HAS_LUAU

// Rich text from a script (F2 of Specs/vg-and-gui-lab.md): the markup set and read back, its words
// without markup, a link's handler (deferred, reading the id it was given), and runs added one by
// one, the same from both languages.
namespace
{
    void CheckRichText(UiBed& bed, IScriptContext& ctx)
    {
        CHECK(ctx.GetGlobal(u8"words").template Get<String>() == StringView(u8"Your mark wears a rose gown"));
        CHECK(ctx.GetGlobal(u8"wrong").template Get<bool>() == false); // a label is no rich text
        ui::RichText* story = bed.root->FindByName<ui::RichText>(u8"story");
        REQUIRE(story != nullptr);
        // A click on the link runs the handler at the next drain, which reads the id it was for.
        story->ActivateLink(StringView(u8"hint"));
        CHECK(ctx.GetGlobal(u8"heard").template Get<String>().IsEmpty());
        bed.context.MutationQueueRef().Drain();
        CHECK(ctx.GetGlobal(u8"heard").template Get<String>() == StringView(u8"hint"));
        CHECK(ctx.GetGlobal(u8"links").template Get<String>() == StringView(u8"1"));
        // Runs added by the script after: the markup is gone, the runs are there.
        REQUIRE(story->Document().runs.Size() == 2u);
        CHECK(story->Document().runs[1].bold);
        CHECK(story->Markup.Value().IsEmpty());
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("ui-facade: AngelScript writes rich text by markup and by runs, and hears its links")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    const Status status =
        ctx->Load(u8"string words;\n"
                  u8"string heard;\n"
                  u8"string links;\n"
                  u8"bool wrong = true;\n"
                  u8"int count = 0;\n"
                  u8"void onHint() { count += 1; links = \"\" + count; heard = ui::findRichText(\"story\").clickedLink; }\n"
                  u8"void main() {\n"
                  u8"  ui::push(Guid(17, 34));\n"
                  u8"  ui::findRichText(\"story\").setMarkup(\"Your mark wears [color=#d04060]a rose gown[/color]\");\n"
                  u8"  words = ui::findRichText(\"story\").plainText;\n"
                  u8"  wrong = ui::findRichText(\"status\").isValid();\n"
                  u8"  ui::findRichText(\"story\").onLink(Action(onHint));\n"
                  u8"  ui::findRichText(\"story\").clearRuns();\n"
                  u8"  ui::findRichText(\"story\").addRun(\"Masks \");\n"
                  u8"  ui::findRichText(\"story\").addRun(\"off\", Color(1, 0, 0, 1), 0, true, false);\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CheckRichText(bed, *ctx);
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("ui-facade: Luau writes rich text by markup and by runs, and hears its links")
{
    RegisterCoreTypes();
    engine::uiscript::RegisterUiScriptSurface();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    UiBed bed;
    engine::uiscript::InstallUiScreenScriptService(*ctx, bed.binding);

    const Status status =
        ctx->Load(u8"heard = \"\"\n"
                  u8"links = \"\"\n"
                  u8"local count = 0\n"
                  u8"ui.push(Guid.new(17, 34))\n"
                  u8"local story = ui.findRichText(\"story\")\n"
                  u8"story:setMarkup(\"Your mark wears [color=#d04060]a rose gown[/color]\")\n"
                  u8"words = story.plainText\n"
                  u8"wrong = ui.findRichText(\"status\"):isValid()\n"
                  u8"story:onLink(function() count = count + 1; links = tostring(count); heard = story.clickedLink end)\n"
                  u8"story:clearRuns()\n"
                  u8"story:addRun(\"Masks \")\n"
                  u8"story:addRun(\"off\", Color.new(1, 0, 0, 1), 0, true, false)\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CheckRichText(bed, *ctx);
}
#endif // OPTION_HAS_LUAU
