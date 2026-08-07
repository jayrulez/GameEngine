// draconic.engine.ui - the Ui.* script facade PROVEN end-to-end on both backends.
//
// The Ui facade is owned by the UISubsystem (the out-of-tree pattern draconic.net's Net facade
// uses), NOT the neutral Foundation facade lib. This drives a real Wren / AngelScript VM: a fake
// UiScriptBinding (standing in for the live UISubsystem screen tier) is installed as the context's
// ui.runtime service, then a script pushes an overlay, drives its controls by id, binds a click
// handler, and pops it; we read back what each host call recorded and fire the click delegate.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.engine.ui; // Ui / UiScriptBinding / InstallUiScriptService / RegisterUiScriptFacade
import draconic.script;
import draconic.script.wren;
import draconic.script.angelscript;

using namespace draconic::foundation;
using namespace draconic::script;
namespace ui = draconic::ui;

namespace
{
    // Fake UISubsystem-screen-tier host pointers + what each call recorded.
    struct UiFake
    {
        ui::UiScriptBinding binding;
        int pushCalls = 0;
        Guid pushedDoc;
        i32 popped = -1;
        i32 textHandle = -1;
        String textId, textValue;
        i32 progHandle = -1;
        String progId;
        f64 progValue = -1.0;
        i32 visHandle = -1;
        String visId;
        bool visValue = false;
        i32 clickHandle = -1;
        String clickId;
        RefPtr<IScriptDelegate> clickFn;

        static constexpr i32 kHandle = 5;

        UiFake()
        {
            binding.pushOverlay = Function<i32(const Guid&)>{[this](const Guid& g) -> i32
                                                             {
                                                                 ++pushCalls;
                                                                 pushedDoc = g;
                                                                 return kHandle;
                                                             }};
            binding.popOverlay = Function<void(i32)>{[this](i32 h) { popped = h; }};
            binding.setText = Function<void(i32, StringView, StringView)>{
                [this](i32 h, StringView id, StringView t)
                {
                    textHandle = h;
                    textId = String(id);
                    textValue = String(t);
                }};
            binding.setProgress = Function<void(i32, StringView, f64)>{
                [this](i32 h, StringView id, f64 v)
                {
                    progHandle = h;
                    progId = String(id);
                    progValue = v;
                }};
            binding.setVisible = Function<void(i32, StringView, bool)>{
                [this](i32 h, StringView id, bool v)
                {
                    visHandle = h;
                    visId = String(id);
                    visValue = v;
                }};
            binding.onClick = Function<void(i32, StringView, RefPtr<IScriptDelegate>)>{
                [this](i32 h, StringView id, RefPtr<IScriptDelegate> fn)
                {
                    clickHandle = h;
                    clickId = String(id);
                    clickFn = Move(fn);
                }};
        }

        void CheckRecorded() const
        {
            CHECK(pushCalls == 1);
            CHECK(pushedDoc == Guid{17, 34});
            CHECK(textHandle == kHandle);
            CHECK(textId == StringView(u8"status"));
            CHECK(textValue == StringView(u8"Loading"));
            CHECK(progHandle == kHandle);
            CHECK(progId == StringView(u8"progress"));
            CHECK(progValue == doctest::Approx(0.5));
            CHECK(visId == StringView(u8"spinner"));
            CHECK(visValue == true);
            CHECK(clickHandle == kHandle);
            CHECK(clickId == StringView(u8"cancel"));
            CHECK(popped == kHandle);
        }

        // Fire the recorded click handler the way the host would (one ignored double arg today; a
        // void() delegate funcdef is a queued backend refinement).
        void FireClick()
        {
            REQUIRE(clickFn.Get() != nullptr);
            (void)clickFn->Invoke(Span<Variant>{}); // a click carries no payload
        }
    };
}

TEST_CASE("ui-facade: Wren pushes an overlay, drives controls by id, binds + fires a click handler")
{
    RegisterFoundationTypes();
    ui::RegisterUiScriptFacade();

    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    UiFake fake;
    ui::InstallUiScriptService(*ctx, fake.binding);

    // Top-level Wren reaches the reflected facades in "main" directly (RegisterReflectedTypes emitted
    // them). The click handler sets a module global we read back after firing the delegate.
    const Status status = ctx->Load(u8"var clicked = false\n"
                                    u8"var h = Ui.pushOverlay(Guid.new(17, 34))\n"
                                    u8"Ui.setText(h, \"status\", \"Loading\")\n"
                                    u8"Ui.setProgress(h, \"progress\", 0.5)\n"
                                    u8"Ui.setVisible(h, \"spinner\", true)\n"
                                    u8"Ui.onClick(h, \"cancel\", Fn.new { clicked = true })\n"
                                    u8"Ui.popOverlay(h)\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    fake.CheckRecorded();
    CHECK(ctx->GetGlobal(u8"clicked").Get<bool>() == false); // not fired yet
    fake.FireClick();
    CHECK(ctx->GetGlobal(u8"clicked").Get<bool>() == true); // the click handler ran
}

TEST_CASE("ui-facade: AngelScript pushes an overlay, drives controls by id, binds + fires a click")
{
    RegisterFoundationTypes();
    ui::RegisterUiScriptFacade();

    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    UiFake fake;
    ui::InstallUiScriptService(*ctx, fake.binding);

    // AngelScript: statics live in the type's namespace (Ui::pushOverlay); the click handler is a
    // natural void() wrapped in the engine-provided `Action` funcdef, and sets a global we read
    // after firing (the delegate param is `?&in`, so any funcdef shape is accepted).
    const Status status = ctx->Load(u8"bool clicked = false;\n"
                                    u8"int h;\n"
                                    u8"void onCancel() { clicked = true; }\n"
                                    u8"void main() {\n"
                                    u8"  h = Ui::pushOverlay(Guid(17, 34));\n"
                                    u8"  Ui::setText(h, \"status\", \"Loading\");\n"
                                    u8"  Ui::setProgress(h, \"progress\", 0.5);\n"
                                    u8"  Ui::setVisible(h, \"spinner\", true);\n"
                                    u8"  Ui::onClick(h, \"cancel\", Action(@onCancel));\n"
                                    u8"  Ui::popOverlay(h);\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    fake.CheckRecorded();
    CHECK(ctx->GetGlobal(u8"clicked").Get<bool>() == false);
    fake.FireClick();
    CHECK(ctx->GetGlobal(u8"clicked").Get<bool>() == true);
}
