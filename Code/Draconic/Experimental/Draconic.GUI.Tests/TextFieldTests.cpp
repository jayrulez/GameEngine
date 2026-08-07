// Draconic GUI - TextField tests: the keyboard/text-input path end-to-end. Text arrives via
// the EventDispatcher's InjectText (routed to the focused node), editing keys via InjectKeyDown.
// A mock font (6px/byte advance) drives caret-placement measurement.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.fonts;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }

    // 6px advance per byte, 12px line height (mirrors the Text-test mock).
    class MockFont : public fonts::IFont
    {
    public:
        foundation::StringView FamilyName() const override { return foundation::StringView(u8"mock"); }
        fonts::FontMetrics Metrics() const override
        {
            fonts::FontMetrics m;
            m.ascent = 10.0f;
            m.descent = -2.0f;
            m.lineGap = 0.0f;
            m.lineHeight = 12.0f;
            m.pixelHeight = 10.0f;
            m.scale = 1.0f;
            return m;
        }
        foundation::f32 PixelHeight() const override { return 10.0f; }
        fonts::GlyphInfo GetGlyphInfo(foundation::i32 cp) const override
        {
            fonts::GlyphInfo g;
            g.codepoint = cp;
            g.advanceWidth = 6.0f;
            return g;
        }
        foundation::f32 GetKerning(foundation::i32, foundation::i32) const override { return 0.0f; }
        bool HasGlyph(foundation::i32) const override { return true; }
        foundation::f32 MeasureString(foundation::StringView text) const override
        {
            return static_cast<foundation::f32>(text.Size()) * 6.0f;
        }
        foundation::f32 MeasureString(foundation::StringView text,
                                foundation::Array<fonts::GlyphPosition>& out) const override
        {
            (void)out;
            return static_cast<foundation::f32>(text.Size()) * 6.0f;
        }
    };

    MockFont* NewMock() { return foundation::DefaultAllocator().New<MockFont>(); }

    // u32 view of a gui::KeyCode, since InjectKeyDown takes a raw code.
    foundation::u32 Key(KeyCode k) { return static_cast<foundation::u32>(k); }

    // In-memory clipboard so the cut/copy/paste path is testable without a shell.
    class MemClipboard : public IClipboard
    {
    public:
        bool HasText() const override { return m_text.Size() > 0; }
        foundation::String GetText() const override { return m_text; }
        void SetText(foundation::StringView text) override { m_text = foundation::String(text); }

    private:
        foundation::String m_text;
    };
}

TEST_CASE("textfield: typed text is inserted at the caret and the caret advances")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 100.0f});
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    root->AddChild(f.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Focus the field (text only routes to the focused node).
    f->RequestFocus();
    CHECK(f->IsFocused());

    d->InjectText(foundation::StringView(u8"Hi"));
    CHECK(f->GetText() == foundation::StringView(u8"Hi"));
    CHECK(f->GetCaret() == 2);

    // Move the caret to the start and insert there.
    d->InjectKeyDown(Key(KeyCode::Home));
    CHECK(f->GetCaret() == 0);
    d->InjectText(foundation::StringView(u8"Oh "));
    CHECK(f->GetText() == foundation::StringView(u8"Oh Hi"));
    CHECK(f->GetCaret() == 3);
}

TEST_CASE("textfield: text only reaches a focused field")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 100.0f});
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    root->AddChild(f.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectText(foundation::StringView(u8"nope")); // not focused -> dropped
    CHECK(f->GetText().Size() == 0);
}

TEST_CASE("textfield: backspace and delete remove whole codepoints (UTF-8)")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 100.0f});
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    root->AddChild(f.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    f->RequestFocus();

    f->SetText(foundation::StringView(u8"aé")); // 'a'(1) + 'é'(2 bytes), caret at end (3)
    CHECK(f->GetCaret() == 3);

    d->InjectKeyDown(Key(KeyCode::Backspace)); // removes the whole 'é'
    CHECK(f->GetText() == foundation::StringView(u8"a"));
    CHECK(f->GetCaret() == 1);

    d->InjectKeyDown(Key(KeyCode::Home));
    d->InjectKeyDown(Key(KeyCode::Delete)); // removes 'a' at the front
    CHECK(f->GetText().Size() == 0);
    CHECK(f->GetCaret() == 0);

    // Backspace at the start and delete at the end are no-ops.
    d->InjectKeyDown(Key(KeyCode::Backspace));
    d->InjectKeyDown(Key(KeyCode::Delete));
    CHECK(f->GetText().Size() == 0);
}

TEST_CASE("textfield: caret navigation steps by codepoint and clamps")
{
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    auto root = Make<SceneNode>();
    root->AddChild(f.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    f->RequestFocus();

    f->SetText(foundation::StringView(u8"é€")); // 2 bytes + 3 bytes = 5 bytes, caret at 5
    d->InjectKeyDown(Key(KeyCode::Home));
    CHECK(f->GetCaret() == 0);
    d->InjectKeyDown(Key(KeyCode::Right)); // over 'é'
    CHECK(f->GetCaret() == 2);
    d->InjectKeyDown(Key(KeyCode::Right)); // over '€'
    CHECK(f->GetCaret() == 5);
    d->InjectKeyDown(Key(KeyCode::Right)); // clamp at end
    CHECK(f->GetCaret() == 5);
    d->InjectKeyDown(Key(KeyCode::Left)); // back over '€'
    CHECK(f->GetCaret() == 2);
    d->InjectKeyDown(Key(KeyCode::End));
    CHECK(f->GetCaret() == 5);
}

TEST_CASE("textfield: change callback fires on edits")
{
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    auto root = Make<SceneNode>();
    root->AddChild(f.Get());
    EventDispatcher* d = root->GetEventDispatcher();
    f->RequestFocus();

    int changes = 0;
    foundation::String last;
    f->SetOnTextChanged(
        [&](foundation::StringView v)
        {
            ++changes;
            last = foundation::String(v);
        });

    d->InjectText(foundation::StringView(u8"x"));
    CHECK(changes == 1);
    CHECK(last.AsView() == foundation::StringView(u8"x"));
    d->InjectKeyDown(Key(KeyCode::Backspace));
    CHECK(changes == 2);
    CHECK(last.Size() == 0);

    // A navigation key does not fire the change callback.
    const int before = changes;
    d->InjectKeyDown(Key(KeyCode::Home));
    CHECK(changes == before);
}

TEST_CASE("textfield: mouse press focuses and places the caret at the nearest boundary")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr); // 6px/byte
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 100.0f});
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    f->SetFont(&cf);
    f->SetText(foundation::StringView(u8"abcd")); // widths at boundaries: 0,6,12,18,24
    root->AddChild(f.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Click near x=13 (content x, no padding) -> closest boundary is offset 2 (width 12).
    d->InjectMouseDown(foundation::Float2{13.0f, 12.0f}, MouseButton::Left);
    CHECK(f->IsFocused());
    CHECK(f->GetCaret() == 2);

    // Click past the end -> caret at end.
    d->InjectMouseDown(foundation::Float2{90.0f, 12.0f}, MouseButton::Left);
    CHECK(f->GetCaret() == 4);

    // Click at the far left -> caret at start.
    d->InjectMouseDown(foundation::Float2{0.0f, 12.0f}, MouseButton::Left);
    CHECK(f->GetCaret() == 0);
}

TEST_CASE("textfield: caret draws only while focused")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr);
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    f->SetFont(&cf);

    // Not focused: no caret geometry (empty text, no font-service atlas -> no glyph geometry).
    {
        vg::VGContext ctx;
        DrawContext dc{ctx};
        f->Draw(dc);
        CHECK(ctx.GetBatch().vertices.Size() == 0);
    }

    // Focused (via a coordinator root so RequestFocus routes): caret rect is drawn.
    auto root = Make<SceneNode>();
    root->AddChild(f.Get());
    f->RequestFocus();
    {
        vg::VGContext ctx;
        DrawContext dc{ctx};
        f->Draw(dc);
        CHECK(ctx.GetBatch().vertices.Size() > 0); // the caret quad
    }
}

TEST_CASE("textfield: WantsTextInput drives the dispatcher's text-input wish by focus")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 100.0f});
    auto f = Make<TextField>();
    f->SetSize(foundation::Float2{100.0f, 24.0f});
    auto plain = Make<Button>(); // a non-editing widget
    plain->SetSize(foundation::Float2{100.0f, 24.0f});
    root->AddChild(f.Get());
    root->AddChild(plain.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    CHECK(f->WantsTextInput());           // an editable field wants text input
    CHECK_FALSE(plain->WantsTextInput()); // a button does not

    CHECK_FALSE(d->WantsTextInput()); // nothing focused
    f->RequestFocus();
    CHECK(d->WantsTextInput()); // the field is focused
    plain->RequestFocus();
    CHECK_FALSE(d->WantsTextInput()); // focus moved to a non-editing widget

    // A disabled field does not want text input.
    f->RequestFocus();
    f->SetEnabled(false);
    CHECK_FALSE(f->WantsTextInput());
    CHECK_FALSE(d->WantsTextInput());
}

TEST_CASE("textfield: blink toggles the caret over time while focused")
{
    auto root = Make<SceneNode>();
    auto f = Make<TextField>();
    root->AddChild(f.Get());
    f->RequestFocus(); // ResetBlink -> visible

    // Half a period flips it off, another half flips it back on.
    f->Update(0.5);
    f->Update(0.5);
    // (No direct getter for visibility; exercised for coverage/no-crash and the focused guard.)

    f->ReleaseFocus();
    f->Update(1.0); // ignored while unfocused (no crash, no toggle)
    CHECK_FALSE(f->IsFocused());
}

// ===================== Selection =====================

namespace
{
    // A focused field holding `value`, wired under a scene so it has a dispatcher.
    struct Fixture
    {
        foundation::RefPtr<SceneNode> root = Make<SceneNode>();
        foundation::RefPtr<TextField> field = Make<TextField>();
        fonts::CachedFont font{NewMock(), nullptr, nullptr};

        explicit Fixture(foundation::StringView value = {})
        {
            root->SetSize(foundation::Float2{400.0f, 100.0f});
            field->SetSize(foundation::Float2{200.0f, 24.0f});
            field->SetFont(&font);
            root->AddChild(field.Get());
            if (value.Size() > 0)
                field->SetText(value);
            field->RequestFocus();
        }
        EventDispatcher* d() { return root->GetEventDispatcher(); }
    };
}

TEST_CASE("textfield: shift+arrows extend a selection; the selection reports its text")
{
    Fixture fx(u8"hello");
    fx.field->SetCaret(0);
    CHECK_FALSE(fx.field->HasSelection());

    const foundation::u32 shift = static_cast<foundation::u32>(KeyModShift);
    fx.d()->InjectKeyDown(Key(KeyCode::Right), shift);
    fx.d()->InjectKeyDown(Key(KeyCode::Right), shift);
    CHECK(fx.field->HasSelection());
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"he"));
    CHECK(fx.field->GetCaret() == 2);

    // A plain (no-shift) Right collapses the selection to its right edge.
    fx.d()->InjectKeyDown(Key(KeyCode::Right));
    CHECK_FALSE(fx.field->HasSelection());
    CHECK(fx.field->GetCaret() == 2);
}

TEST_CASE("textfield: shift+Home selects to the start; plain Left collapses to selection start")
{
    Fixture fx(u8"hello");
    fx.field->SetCaret(5);
    const foundation::u32 shift = static_cast<foundation::u32>(KeyModShift);
    fx.d()->InjectKeyDown(Key(KeyCode::Home), shift);
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"hello"));

    // Plain Left with a selection jumps the caret to the selection start (offset 0).
    fx.d()->InjectKeyDown(Key(KeyCode::Left));
    CHECK_FALSE(fx.field->HasSelection());
    CHECK(fx.field->GetCaret() == 0);
}

TEST_CASE("textfield: typing over a selection replaces it")
{
    Fixture fx(u8"hello");
    fx.field->SelectAll();
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"hello"));
    fx.d()->InjectText(foundation::StringView(u8"X"));
    CHECK(fx.field->GetText() == foundation::StringView(u8"X"));
    CHECK(fx.field->GetCaret() == 1);
    CHECK_FALSE(fx.field->HasSelection());
}

TEST_CASE("textfield: backspace and delete remove the whole selection")
{
    Fixture fx(u8"hello");
    fx.field->SetCaret(1);
    const foundation::u32 shift = static_cast<foundation::u32>(KeyModShift);
    fx.d()->InjectKeyDown(Key(KeyCode::Right), shift);
    fx.d()->InjectKeyDown(Key(KeyCode::Right), shift);
    fx.d()->InjectKeyDown(Key(KeyCode::Right), shift); // "ell" selected
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"ell"));
    fx.d()->InjectKeyDown(Key(KeyCode::Backspace));
    CHECK(fx.field->GetText() == foundation::StringView(u8"ho"));
    CHECK(fx.field->GetCaret() == 1);
}

// ===================== Clipboard =====================

TEST_CASE("textfield: Ctrl+C / Ctrl+V copy and paste a selection through the clipboard")
{
    MemClipboard clip;
    Fixture fx(u8"hello");
    fx.d()->SetClipboard(&clip);

    const foundation::u32 ctrl = static_cast<foundation::u32>(KeyModCtrl);
    const foundation::u32 shift = static_cast<foundation::u32>(KeyModShift);

    fx.field->SetCaret(0);
    fx.d()->InjectKeyDown(Key(KeyCode::Right), shift);
    fx.d()->InjectKeyDown(Key(KeyCode::Right), shift); // select "he"
    fx.d()->InjectKeyDown(Key(KeyCode::C), ctrl);
    CHECK(clip.GetText() == foundation::StringView(u8"he"));

    // Paste at the end.
    fx.d()->InjectKeyDown(Key(KeyCode::End));
    fx.d()->InjectKeyDown(Key(KeyCode::V), ctrl);
    CHECK(fx.field->GetText() == foundation::StringView(u8"hellohe"));
}

TEST_CASE("textfield: Ctrl+X cuts the selection to the clipboard")
{
    MemClipboard clip;
    Fixture fx(u8"hello");
    fx.d()->SetClipboard(&clip);

    fx.field->SelectAll();
    const foundation::u32 ctrl = static_cast<foundation::u32>(KeyModCtrl);
    fx.d()->InjectKeyDown(Key(KeyCode::X), ctrl);
    CHECK(clip.GetText() == foundation::StringView(u8"hello"));
    CHECK(fx.field->GetText().Size() == 0);
}

TEST_CASE("textfield: paste with no clipboard configured is a safe no-op")
{
    Fixture fx(u8"hi");
    const foundation::u32 ctrl = static_cast<foundation::u32>(KeyModCtrl);
    fx.d()->InjectKeyDown(Key(KeyCode::V), ctrl); // no clipboard set -> nothing happens
    CHECK(fx.field->GetText() == foundation::StringView(u8"hi"));
}

TEST_CASE("textfield: pasted newlines are stripped to keep the value single-line")
{
    MemClipboard clip;
    clip.SetText(foundation::StringView(u8"a\nb\r\nc"));
    Fixture fx;
    fx.d()->SetClipboard(&clip);
    const foundation::u32 ctrl = static_cast<foundation::u32>(KeyModCtrl);
    fx.d()->InjectKeyDown(Key(KeyCode::V), ctrl);
    CHECK(fx.field->GetText() == foundation::StringView(u8"abc"));
}

// ===================== Placeholder / max-length =====================

TEST_CASE("textfield: placeholder is stored and reported")
{
    auto f = Make<TextField>();
    f->SetPlaceholder(foundation::StringView(u8"Search..."));
    CHECK(f->GetPlaceholder() == foundation::StringView(u8"Search..."));
}

TEST_CASE("textfield: max-length caps typed input and truncates a paste")
{
    Fixture fx;
    fx.field->SetMaxLength(3);
    fx.d()->InjectText(foundation::StringView(u8"ab"));
    fx.d()->InjectText(foundation::StringView(u8"cd")); // only 'c' fits (cap of 3)
    CHECK(fx.field->GetText() == foundation::StringView(u8"abc"));

    // Further input past the cap is dropped.
    fx.d()->InjectText(foundation::StringView(u8"z"));
    CHECK(fx.field->GetText() == foundation::StringView(u8"abc"));

    // A paste is likewise truncated to the remaining room (here: none).
    MemClipboard clip;
    clip.SetText(foundation::StringView(u8"XYZ"));
    fx.d()->SetClipboard(&clip);
    const foundation::u32 ctrl = static_cast<foundation::u32>(KeyModCtrl);
    fx.d()->InjectKeyDown(Key(KeyCode::V), ctrl);
    CHECK(fx.field->GetText() == foundation::StringView(u8"abc"));
}

TEST_CASE("textfield: max-length counts codepoints, not bytes (UTF-8)")
{
    Fixture fx;
    fx.field->SetMaxLength(2);
    fx.d()->InjectText(foundation::StringView(u8"ééé")); // three 2-byte 'é'
    CHECK(fx.field->GetText() == foundation::StringView(u8"éé"));
}

// ===================== Mouse selection =====================

TEST_CASE("textfield: a double-click selects the word under the cursor")
{
    Fixture fx(u8"foo bar baz"); // words at 0-3, 4-7, 8-11 (6px/byte)
    // Two presses at the same spot with no time advance = a double-click. x within "bar".
    const foundation::Float2 pos{5.0f * 6.0f + 3.0f, 12.0f}; // ~ byte 5, inside "bar"
    fx.d()->InjectMouseDown(pos, MouseButton::Left);
    fx.d()->InjectMouseUp(pos, MouseButton::Left);
    fx.d()->InjectMouseDown(pos, MouseButton::Left);
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"bar"));
}

TEST_CASE("textfield: dragging the mouse extends the selection")
{
    Fixture fx(u8"hello");
    // Press at the start, move right to ~offset 3, release.
    fx.d()->InjectMouseDown(foundation::Float2{0.0f, 12.0f}, MouseButton::Left);
    CHECK_FALSE(fx.field->HasSelection());
    fx.d()->InjectMouseMove(foundation::Float2{3.0f * 6.0f, 12.0f}); // captured by the pressed field
    CHECK(fx.field->HasSelection());
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"hel"));
    fx.d()->InjectMouseUp(foundation::Float2{3.0f * 6.0f, 12.0f}, MouseButton::Left);
    // A move after release no longer extends.
    fx.d()->InjectMouseMove(foundation::Float2{5.0f * 6.0f, 12.0f});
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"hel"));
}

TEST_CASE("textfield: shift+click extends the selection from the caret")
{
    Fixture fx(u8"hello");
    fx.field->SetCaret(1); // after 'h'
    const foundation::u32 shift = static_cast<foundation::u32>(KeyModShift);
    fx.d()->InjectMouseDown(foundation::Float2{4.0f * 6.0f, 12.0f}, MouseButton::Left, shift);
    CHECK(fx.field->HasSelection());
    CHECK(fx.field->SelectedText() == foundation::StringView(u8"ell"));
}
