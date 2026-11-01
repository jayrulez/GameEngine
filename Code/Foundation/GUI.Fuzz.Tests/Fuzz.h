// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Fuzz - the fuzz targets and what feeds them: seeds from the sandbox's own pages and string
// table, and a deterministic mutator. Shared by the ctest runner (FuzzTests.cpp: every seed, then a
// seeded run of mutations of each) and the libFuzzer entries (LibFuzzer.cpp, built with
// OPTION_GUI_LIBFUZZER under clang). A target takes any bytes and must neither crash nor hang; what
// it builds is thrown away.

#pragma once

namespace guifuzz
{
    namespace core = foundation::core;
    namespace gui = foundation::gui;
    using core::u32;
    using core::u64;
    using core::usize;
    using core::f32;

    /// A deterministic source of numbers (xorshift64*), so a failing mutation is found again from its seed.
    struct Random
    {
        u64 state;
        explicit Random(u64 seed) : state(seed != 0 ? seed : 0x9e3779b97f4a7c15ull) {}
        u64 Next()
        {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            return state * 0x2545f4914f6cdd1dull;
        }
        u32 Below(u32 bound) { return bound == 0 ? 0u : static_cast<u32>(Next() % bound); }
        f32 Unit() { return static_cast<f32>(Next() >> 40) / static_cast<f32>(1u << 24); }
    };

    /// Mutates `text` a few times: a byte flipped, a span cut or doubled, a piece of syntax or a
    /// stray byte put in, the end cut off. `tokens` is the target's syntax.
    inline core::String Mutate(core::String text, Random& random, const char* const* tokens, usize tokenCount)
    {
        const auto token = [](const char* piece) { return core::StringView(reinterpret_cast<const core::utf8char*>(piece)); };
        const u32 edits = 1 + random.Below(6);
        for (u32 e = 0; e < edits; ++e)
        {
            const usize size = text.Size();
            const usize at = size == 0 ? 0 : random.Below(static_cast<u32>(size));
            switch (random.Below(7))
            {
            case 0: // flip a byte
                if (size > 0)
                    text[at] = static_cast<core::utf8char>(text[at] ^ static_cast<core::utf8char>(1u << random.Below(8)));
                break;
            case 1: // cut a span
                if (size > 0)
                    text.Remove(at, 1 + random.Below(static_cast<u32>(core::Min<usize>(size - at, 64))));
                break;
            case 2: // double a span
                if (size > 0)
                {
                    const core::String span(text.AsView().SubStr(at, 1 + random.Below(static_cast<u32>(core::Min<usize>(size - at, 64)))));
                    text.Insert(at, span.AsView());
                }
                break;
            case 3: // a piece of syntax
                text.Insert(at, token(tokens[random.Below(static_cast<u32>(tokenCount))]));
                break;
            case 4: // a byte of anything, a broken UTF-8 lead among them
            {
                const core::utf8char stray = static_cast<core::utf8char>(random.Below(256));
                text.Insert(at, core::StringView(&stray, 1));
                break;
            }
            case 5: // the end cut off
                text.Remove(at, size - at);
                break;
            default: // a run nested deep
            {
                const u32 length = 1 + random.Below(200);
                const core::utf8char lead = static_cast<core::utf8char>(tokens[random.Below(static_cast<u32>(tokenCount))][0]);
                core::String run;
                for (u32 n = 0; n < length; ++n)
                    run.PushBack(lead);
                text.Insert(at, run.AsView());
                break;
            }
            }
        }
        return text;
    }

    inline constexpr const char* kCssTokens[] = {
        "{", "}", ";", ":", "(", ")", ",", "\"", "'", "/*", "*/", "@media ", "@keyframes x ", "@import ",
        "var(--a)", "var(--a, var(--b))", "calc(1px + 50%)", "::thumb", ":hover", ":not(", ">", "+", "~", "*",
        "#id", ".cls", "[attr=v]", "!important", "url(", "rgba(1,2,3", "#ff00ff", "1e999", "-0.5px", "100%",
        "transition: all 1s;", "animation: x 1s infinite;", "--a: ", "\\", "\n", "\t", "\xF0\x9F", "\xC3",
    };
    inline constexpr const char* kMarkupTokens[] = {
        "<", ">", "</", "/>", "=\"", "\"", "'", "&amp;", "&#x1F600;", "&bogus;", "<!--", "-->", "<![CDATA[",
        "]]>", "<?xml ?>", "<Button", "<Panel>", "</Panel>", "<Label text=\"", "<ScrollView>", "</ScrollView>",
        "<TextField", "<button", "style=\"", "class=\"", "id=\"",
        "{{name}}", "{{", "}}", "data-x=\"", "-count=\"", "-args=\"", "\xEF\xBB\xBF", "\0", "\xE2\x80\xAE",
    };
    inline constexpr const char* kKeymapTokens[] = {
        "Ctrl+", "Shift+", "Alt+", "Super+", "+", " ", "K", "F12", "Escape", "Pad.South", "Pad.", "&&", "||", "!",
        "(", ")", "editing", "menu && !popup", "\xC3\xA9", "Ctrl+K Ctrl+", "++", "  ",
        // As a binding writes them (ParseKeystroke's syntax), so mutations reach past the parse.
        "ctrl-", "shift-", "alt-", "super-", "-", "k", "f12", "escape", "pad-south", "pad-",
    };
    inline constexpr const char* kXmlTokens[] = {
        "<", ">", "</", "/>", "=\"", "\"", "<Entry", "<Locale", "</Locales>", "&lt;", "&#0;", "&#xFFFFFF;",
        "<?xml version=\"1.0\"?>", "<!DOCTYPE x>", "type=\"", "{0}", "{1, plural, one {a} other {b}}", "\xC3",
    };

    /// The file's bytes, or nothing (read through the engine's file system, as everything else is).
    inline core::String ReadBytes(core::StringView path)
    {
        const core::Result<core::Array<core::byte>> bytes = core::ReadFile(path);
        if (!bytes.HasValue())
            return {};
        return core::String(core::StringView(reinterpret_cast<const core::utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    /// Reads every file in `dir` (with `extension`, when one is given) into `seeds`.
    inline void ReadFolder(core::StringView dir, core::StringView extension, core::Array<core::String>& seeds)
    {
        struct Collect
        {
            core::StringView dir;
            core::StringView extension;
            core::Array<core::String>* seeds;
        };
        Collect collect{dir, extension, &seeds};
        (void)core::ListDirectory(
            dir,
            [](void* ctx, core::StringView name, bool isDirectory)
            {
                auto* c = static_cast<Collect*>(ctx);
                if (!isDirectory && (c->extension.IsEmpty() || core::PathExtension(name) == c->extension))
                    c->seeds->PushBack(ReadBytes(core::PathJoin(c->dir, name).AsView()));
            },
            &collect);
    }

    /// Returns the sandbox's pages with this extension (`.css`, `.xml`), including its main sheet:
    /// real input that uses every feature the GUI has at least once.
    inline core::Array<core::String> SandboxSeeds(core::StringView extension)
    {
        core::Array<core::String> seeds;
        const core::String root = foundation::vfs::FindDataRoot();
        const core::String base = core::PathJoin(root.AsView(), u8"Samples/UI/sandbox");
        ReadFolder(base.AsView(), extension, seeds);
        ReadFolder(core::PathJoin(base.AsView(), u8"pages").AsView(), extension, seeds);
        return seeds;
    }

    /// Returns the inputs a fuzzer found once, kept so they run every time:
    /// Data/Tests/GUI/Fuzz/<kind>/*.
    inline core::Array<core::String> FoundSeeds(core::StringView kind)
    {
        core::Array<core::String> seeds;
        const core::String root = foundation::vfs::FindDataRoot();
        const core::String dir = core::PathJoin(core::PathJoin(root.AsView(), u8"Tests/GUI/Fuzz").AsView(), kind);
        ReadFolder(dir.AsView(), core::StringView{}, seeds);
        return seeds;
    }

    // === The targets ===

    /// The tree a sheet is applied to: a small tree of the common element kinds, by their tags.
    inline constexpr const char8_t* kCssTree =
        u8"<Panel><Button text=\"b\"/><Label class=\"note\" text=\"l\"/><TextField/><Slider/>"
        u8"<CheckBox/><ListBox/><ScrollView><Label text=\"in\"/></ScrollView></Panel>";

    /// Parses a sheet, then applies it to kCssTree.
    inline void FuzzCss(core::StringView text)
    {
        gui::RegisterGuiElements();
        auto root = core::MakeRef<gui::SceneRoot>(core::DefaultAllocator(), core::MakeRef<gui::GUIContext>(core::DefaultAllocator()));
        root->SetSize(core::Float2{320.0f, 240.0f});
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, core::DefaultAllocator());
        if (core::RefPtr<gui::Node> tree = loader.LoadFromString(core::StringView(kCssTree)))
            root->AddChild(tree.Get());
        gui::StyleManager styles(gui::CSSParser::Parse(text));
        styles.ApplyTree(*root.Get());
        root->UpdateLayout();
    }

    /// Loads markup into a root, then styles, lays out and draws it.
    inline void FuzzMarkup(core::StringView text)
    {
        gui::RegisterGuiElements();
        auto root = core::MakeRef<gui::SceneRoot>(core::DefaultAllocator(), core::MakeRef<gui::GUIContext>(core::DefaultAllocator()));
        root->SetSize(core::Float2{320.0f, 240.0f});
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, core::DefaultAllocator());
        core::RefPtr<gui::Node> tree = loader.LoadFromString(text);
        if (!tree)
            return;
        root->AddChild(tree.Get());
        gui::StyleManager styles(gui::CSSParser::Parse(core::StringView(u8"* { padding: 2; } button { background-color: #333; }")));
        styles.ApplyTree(*root.Get());
        root->UpdateLayout();
        foundation::vg::VGContext canvas;
        gui::DrawContext dc{canvas};
        root->Draw(dc);
    }

    /// Binds lines of `keys|command|context` into a keymap, then matches keystrokes against it.
    /// Returns how many lines bound.
    inline usize FuzzKeymap(core::StringView text)
    {
        constexpr usize kNotFound = core::StringView::kNotFound;
        gui::Keymap keymap;
        usize bound = 0;
        usize start = 0;
        while (start <= text.Size())
        {
            const usize end = core::Min(text.Find(u8'\n', start), text.Size());
            const core::StringView line = text.SubStr(start, end - start);
            const usize a = line.Find(u8'|');
            const usize b = a == kNotFound ? kNotFound : line.Find(u8'|', a + 1);
            const core::StringView keys = a == kNotFound ? line : line.SubStr(0, a);
            const core::StringView command = a == kNotFound ? core::StringView(u8"c") : line.SubStr(a + 1, (b == kNotFound ? line.Size() : b) - a - 1);
            const core::StringView context = b == kNotFound ? core::StringView() : line.SubStr(b + 1, line.Size() - b - 1);
            if (keymap.Bind(keys, command, context))
                ++bound;
            (void)gui::ParseKeystroke(keys);
            start = end + 1;
        }
        return bound;
    }

    /// Reads a string table from XML, then asks it for its texts.
    inline void FuzzStringTable(core::StringView text)
    {
        foundation::i18n::RegisterStringTableType();
        foundation::xml::XmlDocument document(core::DefaultAllocator());
        if (document.Parse(text) != foundation::xml::XmlResult::Ok)
            return;
        foundation::xml::XmlSerializer reader(document);
        foundation::i18n::StringTable table;
        table.Serialize(reader);
        foundation::i18n::Localization localization;
        localization.AddTable(&table);
        (void)localization.Text(core::StringView(u8"lang.changed"));
        localization.SetLanguage(core::StringView(u8"ar"));
        (void)localization.Text(core::StringView(u8"gui.ok"));
    }

    /// Runs a sandbox page under a stream of input the bytes decide: pointer, wheel, keys, text and
    /// composition (broken UTF-8 included), touch, pad, file drops, and frames between them.
    inline void FuzzInput(core::StringView markup, core::StringView sheet, core::StringView bytes)
    {
        gui::RegisterGuiElements();
        auto root = core::MakeRef<gui::SceneRoot>(core::DefaultAllocator(), core::MakeRef<gui::GUIContext>(core::DefaultAllocator()));
        root->SetSize(core::Float2{640.0f, 480.0f});
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, core::DefaultAllocator());
        core::RefPtr<gui::Node> tree = loader.LoadFromString(markup);
        if (tree)
            root->AddChild(tree.Get());
        gui::StyleManager styles(gui::CSSParser::Parse(sheet));
        foundation::gui::testing::Tester tester(*root.Get(), &styles);
        gui::EventDispatcher& input = *root->GetEventDispatcher();
        usize at = 0;
        const auto next = [&]() -> u32 { return at < bytes.Size() ? static_cast<u32>(bytes[at++]) : 0u; };
        const auto point = [&]() { return core::Float2{static_cast<f32>(next()) * 2.6f - 10.0f, static_cast<f32>(next()) * 2.0f - 10.0f}; };
        while (at < bytes.Size())
        {
            switch (next() % 14)
            {
            case 0: (void)input.InjectMouseMove(point(), next() & 15u); break;
            case 1: (void)input.InjectMouseDown(point(), static_cast<gui::MouseButton>(next() % 5), next() & 15u); break;
            case 2: (void)input.InjectMouseUp(point(), static_cast<gui::MouseButton>(next() % 5), next() & 15u); break;
            case 3: (void)input.InjectMouseWheel(point(), core::Float2{static_cast<f32>(next()) - 128.0f, static_cast<f32>(next()) - 128.0f}); break;
            case 4: (void)input.InjectKeyDown(next() % 80u, next() & 15u); break; // unknown codes among them
            case 5: (void)input.InjectKeyUp(next() % 80u, next() & 15u); break;
            case 6:
            {
                core::String text;
                for (u32 n = next() % 6; n > 0; --n)
                    text.PushBack(static_cast<core::utf8char>(next()));
                (void)input.InjectText(text.AsView());
                break;
            }
            case 7:
            {
                core::String text;
                for (u32 n = next() % 6; n > 0; --n)
                    text.PushBack(static_cast<core::utf8char>(next()));
                (void)input.InjectComposition(text.AsView(), static_cast<core::i32>(next() % 8) - 2);
                break;
            }
            case 8: (void)input.InjectTouchDown(next() % 3u, point()); break;
            case 9: (void)input.InjectTouchMove(next() % 3u, point()); break;
            case 10: (void)input.InjectTouchUp(next() % 3u, point()); break;
            case 11: input.InjectPadButton(static_cast<gui::PadButton>(next() % 8u)); break;
            case 12:
            {
                core::Array<core::String> paths;
                paths.PushBack(core::String(u8"/tmp/a file.png"));
                (void)input.InjectFileDrop(point(), paths);
                break;
            }
            default: tester.Advance(core::Duration::FromSeconds(static_cast<f32>(next()) / 255.0f)); break;
            }
        }
        tester.Frame();
        foundation::vg::VGContext canvas;
        gui::DrawContext dc{canvas};
        root->Draw(dc);
    }
}
