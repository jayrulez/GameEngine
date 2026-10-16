// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - the lab driven from scripts (G1 of Specs/vg-and-gui-lab.md), with no UI-script layer: the
// widgets reach AngelScript and Luau through their reflection. Each language runs the same story
// against one native checker: build a small tree under `gui.root()`, set properties, add a class a
// sheet styles, listen to a click the dispatcher delivers; then the weak-handle rule: a destroyed
// element's handle answers isValid() false and any other use is the script's error, a destroy
// inside a click handler completes after it, a reused slot does not answer to the old handle, and
// a handle outliving its GUI reads as gone. Then bindings (G5): a script binds a label to a function
// of the GUI's data, a slider both ways to a data value, and a label to the slider, and each
// follows its source with no subscription.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.script;
import experimental.gui;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;
namespace gui = experimental::gui;

namespace
{
    struct Errors final : IScriptErrorHandler
    {
        int count = 0;
        String last;
        void OnError(const ScriptError& error) override
        {
            ++count;
            last = String(error.message);
        }
    };

    // A GUI for one script: its root, 400 x 300, installed as the script's `gui.root()`.
    struct GuiBed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator());
        gui::GuiScriptBinding binding;
        Errors errors;
        RefPtr<IScriptContext> context;

        explicit GuiBed(IScriptManager& manager)
        {
            root->SetSize(Float2{400.0f, 300.0f});
            binding.root = root.Get();
            context = manager.CreateContext();
            context->SetErrorHandler(&errors);
            gui::InstallGuiScriptService(*context, binding);
        }
        ~GuiBed()
        {
            if (context)
                context->SetErrorHandler(nullptr);
        }

        [[nodiscard]] f64 Number(const char8_t* function)
        {
            Result<Variant> result = context->Call(function, Span<Variant>{});
            REQUIRE(result.HasValue());
            const f64* value = result.Value().TryGet<f64>();
            REQUIRE(value != nullptr);
            return *value;
        }
        void Run(const char8_t* function) { REQUIRE(context->Call(function, Span<Variant>{}).HasValue()); }
        // `function` must fail with the gone-handle error.
        void Refused(const char8_t* function)
        {
            INFO(reinterpret_cast<const char*>(function));
            const int before = errors.count;
            CHECK_FALSE(context->Call(function, Span<Variant>{}).HasValue());
            CHECK(errors.count > before);
            CHECK(errors.last.AsView().ContainsIgnoreCase(StringView(u8"the object is gone")));
        }
        void Click(Float2 at)
        {
            gui::EventDispatcher* dispatcher = root->GetEventDispatcher();
            dispatcher->InjectMouseDown(at, gui::MouseButton::Left);
            dispatcher->InjectMouseUp(at, gui::MouseButton::Left);
        }
        void Update() { root->Update(Duration{}); }
    };

    [[nodiscard]] RefPtr<IScriptManager> Bound(RefPtr<IScriptManager> manager)
    {
        RegisterCoreTypes();
        gui::RegisterGuiScriptSurface();
        RegisterReflectedTypes(*manager);
        return manager;
    }

    // The native half of the story both languages tell. The script, once loaded, has built under
    // the root a FlexLayout "panel" holding a Label "title" (text "Hunt") and, at the root, a Button
    // "go" (80 x 30 at the origin, text "Go", class "primary") listening for clicks.
    void CheckStory(GuiBed& bed)
    {
        REQUIRE(bed.errors.count == 0);
        gui::Node* panel = bed.root->GetChildAt(0);
        REQUIRE(panel != nullptr);
        CHECK(Cast<gui::FlexLayout>(panel) != nullptr);
        gui::Label* title = Cast<gui::Label>(panel->GetChildAt(0));
        REQUIRE(title != nullptr);
        CHECK(title->GetText() == StringView(u8"Hunt"));
        CHECK(title->GetId() == StringView(u8"title"));
        gui::Button* go = Cast<gui::Button>(bed.root->GetChildAt(1));
        REQUIRE(go != nullptr);
        CHECK(go->GetText() == StringView(u8"Go"));
        CHECK(go->HasClass(u8"primary"));
        CHECK(go->GetSize().x == doctest::Approx(80.0f));
        CHECK(bed.Number(u8"foundTitle") == doctest::Approx(1.0)); // find by id, narrowed by Label.of

        // A sheet styles what the script classed; the script reads the styled value back.
        gui::StyleManager styles(gui::CSSParser::Parse(StringView(u8".primary { opacity: 0.5; }")));
        styles.ApplyTree(*bed.root);
        CHECK(go->GetAlpha() == doctest::Approx(0.5f));
        CHECK(bed.Number(u8"goOpacity") == doctest::Approx(0.5));

        // A click through the dispatcher reaches the script's handler.
        bed.Click(Float2{10.0f, 10.0f});
        CHECK(bed.Number(u8"clicks") == doctest::Approx(1.0));
    }

    // The weak-handle rule. The script holds `go` (above) and `bye`, a Button at (100, 0) whose
    // click handler destroys it and records whether it was still valid inside the handler.
    void CheckHandles(GuiBed& bed)
    {
        // A destroy inside a click handler completes after it: valid in the handler and until the
        // GUI's update ends.
        bed.Click(Float2{110.0f, 10.0f});
        CHECK(bed.Number(u8"byeValidInHandler") == doctest::Approx(1.0));
        CHECK(bed.Number(u8"byeValid") == doctest::Approx(1.0));
        bed.Update();
        CHECK(bed.Number(u8"byeValid") == doctest::Approx(0.0));
        bed.Refused(u8"readBye");
        bed.Refused(u8"writeBye");
        bed.Refused(u8"callBye");

        // A slot freed and taken again does not answer to the old handle.
        bed.Run(u8"makeAndDestroyTemp");
        bed.Update();
        bed.Run(u8"makeAnother");
        CHECK(bed.Number(u8"tempValid") == doctest::Approx(0.0));
        CHECK(bed.Number(u8"anotherValid") == doctest::Approx(1.0));

        // A handle outliving its GUI reads as gone, never as freed memory.
        CHECK(bed.Number(u8"goValid") == doctest::Approx(1.0));
        gui::ClearGuiScriptService(*bed.context);
        bed.binding.root = nullptr;
        bed.root = RefPtr<gui::SceneRoot>{};
        CHECK(bed.Number(u8"goValid") == doctest::Approx(0.0));
        bed.Refused(u8"readGo");
    }
}

namespace
{
    // The binding story (G5) both languages tell. Before the script loads, the GUI's data holds
    // score 3 and volume 0.5. The script builds, under the root: a Label bound to "Score: " and the
    // score text (bind, a function reading gui.getText), a Slider bound both ways to volume
    // (bindData), and a Label bound to the slider's value (a function reading slider.value), and
    // offers setScore(n), which writes the data through gui.set.
    void CheckBindings(GuiBed& bed)
    {
        REQUIRE(bed.errors.count == 0);
        gui::Label* score = Cast<gui::Label>(bed.root->GetChildAt(0));
        gui::Slider* slider = Cast<gui::Slider>(bed.root->GetChildAt(1));
        gui::Label* volume = Cast<gui::Label>(bed.root->GetChildAt(2));
        REQUIRE((score != nullptr && slider != nullptr && volume != nullptr));
        CHECK(score->GetText() == StringView(u8"Score: 3"));
        CHECK(slider->GetValue() == doctest::Approx(0.5f));
        CHECK(volume->GetText() == StringView(u8"0.5"));
        CHECK(bed.Number(u8"bound") == doctest::Approx(1.0)); // every bind call answered true

        // The game changes the data: the script's binding follows on the GUI's frame.
        bed.root->GetData().Set(StringView(u8"score"), Variant::From<i32>(4));
        bed.Update();
        CHECK(score->GetText() == StringView(u8"Score: 4"));

        // The user drags the slider: the data and the label reading the slider follow.
        slider->SetValue(0.75f);
        bed.Update();
        CHECK(volume->GetText() == StringView(u8"0.75"));
        const Variant written = bed.root->GetData().Get(StringView(u8"volume"));
        REQUIRE(written.TryGet<f32>() != nullptr);
        CHECK(*written.TryGet<f32>() == doctest::Approx(0.75f));

        // The script writes the data.
        bed.Run(u8"setNine");
        bed.Update();
        CHECK(score->GetText() == StringView(u8"Score: 9"));
        CHECK(bed.errors.count == 0);
    }

    void SeedBindingData(GuiBed& bed)
    {
        bed.root->GetData().Set(StringView(u8"score"), Variant::From<i32>(3));
        bed.root->GetData().Set(StringView(u8"volume"), Variant::From<f64>(0.5));
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("gui-script: AngelScript binds widgets to data and to each other")
{
    RefPtr<IScriptManager> manager = Bound(angelscript::CreateScriptManager(DefaultAllocator()));
    GuiBed bed(*manager);
    SeedBindingData(bed);
    const Status loaded = bed.context->Load(
        u8"funcdef string TextExpression();\n"
        u8"funcdef double NumberExpression();\n"
        u8"Slider@ slider;\n"
        u8"double allBound = 0;\n"
        u8"string scoreText() { return \"Score: \" + gui::getText(\"score\"); }\n"
        u8"double sliderValue() { return slider.value; }\n"
        u8"void main() {\n"
        u8"  Node@ root = gui::root();\n"
        u8"  Label@ score = Label::of(root.add(\"Label\"));\n"
        u8"  @slider = Slider::of(root.add(\"Slider\"));\n"
        u8"  Label@ volume = Label::of(root.add(\"Label\"));\n"
        u8"  bool a = score.bind(\"text\", TextExpression(scoreText));\n"
        u8"  bool b = slider.bindData(\"value\", \"volume\");\n"
        u8"  bool c = volume.bind(\"text\", NumberExpression(sliderValue));\n"
        u8"  allBound = (a && b && c) ? 1 : 0;\n"
        u8"}\n"
        u8"double bound() { return allBound; }\n"
        u8"void setNine() { gui::set(\"score\", 9); }\n",
        u8"main");
    REQUIRE(loaded.IsOk());
    CheckBindings(bed);
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("gui-script: Luau binds widgets to data and to each other")
{
    RefPtr<IScriptManager> manager = Bound(CreateLuauScriptManager(DefaultAllocator()));
    GuiBed bed(*manager);
    SeedBindingData(bed);
    const Status loaded = bed.context->Load(
        u8"local root = gui.root()\n"
        u8"local score = Label.of(root:add(\"Label\"))\n"
        u8"local slider = Slider.of(root:add(\"Slider\"))\n"
        u8"local volume = Label.of(root:add(\"Label\"))\n"
        u8"local a = score:bind(\"text\", function() return \"Score: \" .. gui.getText(\"score\") end)\n"
        u8"local b = slider:bindData(\"value\", \"volume\")\n"
        u8"local c = volume:bind(\"text\", function() return slider.value end)\n"
        u8"local allBound = (a and b and c) and 1 or 0\n"
        u8"function bound() return allBound end\n"
        u8"function setNine() gui.set(\"score\", 9) end\n",
        u8"main");
    REQUIRE(loaded.IsOk());
    CheckBindings(bed);
}
#endif // OPTION_HAS_LUAU

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("gui-script: AngelScript builds, styles and clicks the lab's widgets, and holds them weakly")
{
    RefPtr<IScriptManager> manager = Bound(angelscript::CreateScriptManager(DefaultAllocator()));
    GuiBed bed(*manager);
    const Status loaded = bed.context->Load(
        u8"double clickCount = 0;\n"
        u8"double byeInHandler = -1;\n"
        u8"Button@ go;\n"
        u8"Button@ bye;\n"
        u8"Node@ temp;\n"
        u8"Node@ another;\n"
        u8"void onGo() { clickCount += 1; }\n"
        u8"void onBye() { bye.destroy(); byeInHandler = bye.isValid() ? 1 : 0; }\n"
        u8"double clicks() { return clickCount; }\n"
        u8"double byeValidInHandler() { return byeInHandler; }\n"
        u8"void main() {\n"
        u8"  Node@ root = gui::root();\n"
        u8"  Node@ panel = root.add(\"FlexLayout\");\n"
        u8"  Label@ title = Label::of(panel.add(\"Label\"));\n"
        u8"  title.text = \"Hunt\";\n"
        u8"  title.id = \"title\";\n"
        u8"  @go = Button::of(root.add(\"Button\"));\n"
        u8"  go.text = \"Go\";\n"
        u8"  go.addClass(\"primary\");\n"
        u8"  go.width = 80; go.height = 30;\n"
        u8"  go.on(\"click\", Action(onGo));\n"
        u8"  @bye = Button::of(root.add(\"Button\"));\n"
        u8"  bye.x = 100; bye.width = 80; bye.height = 30;\n"
        u8"  bye.on(\"click\", Action(onBye));\n"
        u8"}\n"
        u8"double foundTitle() { Label@ t = Label::of(gui::root().find(\"title\")); return t !is null && t.text == \"Hunt\" ? 1 : 0; }\n"
        u8"double goOpacity() { return go.opacity; }\n"
        u8"double byeValid() { return bye.isValid() ? 1 : 0; }\n"
        u8"string readBye() { return bye.text; }\n"
        u8"void writeBye() { bye.text = \"x\"; }\n"
        u8"void callBye() { bye.addClass(\"x\"); }\n"
        u8"void makeAndDestroyTemp() { @temp = gui::root().add(\"Label\"); temp.destroy(); }\n"
        u8"void makeAnother() { @another = gui::root().add(\"Label\"); }\n"
        u8"double tempValid() { return temp.isValid() ? 1 : 0; }\n"
        u8"double anotherValid() { return another.isValid() ? 1 : 0; }\n"
        u8"double goValid() { return go.isValid() ? 1 : 0; }\n"
        u8"string readGo() { return go.text; }\n",
        u8"main");
    REQUIRE(loaded.IsOk());
    CheckStory(bed);
    CheckHandles(bed);
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("gui-script: Luau builds, styles and clicks the lab's widgets, and holds them weakly")
{
    RefPtr<IScriptManager> manager = Bound(CreateLuauScriptManager(DefaultAllocator()));
    GuiBed bed(*manager);
    const Status loaded = bed.context->Load(
        u8"local clickCount = 0\n"
        u8"local byeInHandler = -1\n"
        u8"local root = gui.root()\n"
        u8"local panel = root:add(\"FlexLayout\")\n"
        u8"local title = Label.of(panel:add(\"Label\"))\n"
        u8"title.text = \"Hunt\"\n"
        u8"title.id = \"title\"\n"
        u8"local go = Button.of(root:add(\"Button\"))\n"
        u8"go.text = \"Go\"\n"
        u8"go:addClass(\"primary\")\n"
        u8"go.width = 80\n"
        u8"go.height = 30\n"
        u8"go:on(\"click\", function() clickCount = clickCount + 1 end)\n"
        u8"local bye = Button.of(root:add(\"Button\"))\n"
        u8"bye.x = 100\n"
        u8"bye.width = 80\n"
        u8"bye.height = 30\n"
        u8"bye:on(\"click\", function() bye:destroy(); byeInHandler = bye:isValid() and 1 or 0 end)\n"
        u8"local temp, another\n"
        u8"function clicks() return clickCount end\n"
        u8"function byeValidInHandler() return byeInHandler end\n"
        u8"function foundTitle() local t = Label.of(gui.root():find(\"title\")); return (t ~= nil and t.text == \"Hunt\") and 1 or 0 end\n"
        u8"function goOpacity() return go.opacity end\n"
        u8"function byeValid() return bye:isValid() and 1 or 0 end\n"
        u8"function readBye() return bye.text end\n"
        u8"function writeBye() bye.text = \"x\" end\n"
        u8"function callBye() bye:addClass(\"x\") end\n"
        u8"function makeAndDestroyTemp() temp = gui.root():add(\"Label\"); temp:destroy() end\n"
        u8"function makeAnother() another = gui.root():add(\"Label\") end\n"
        u8"function tempValid() return temp:isValid() and 1 or 0 end\n"
        u8"function anotherValid() return another:isValid() and 1 or 0 end\n"
        u8"function goValid() return go:isValid() and 1 or 0 end\n"
        u8"function readGo() return go.text end\n",
        u8"main");
    REQUIRE(loaded.IsOk());
    CheckStory(bed);
    CheckHandles(bed);
}
#endif // OPTION_HAS_LUAU
