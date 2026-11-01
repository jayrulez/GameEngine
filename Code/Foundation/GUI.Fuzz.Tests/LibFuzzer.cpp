// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Fuzz - a libFuzzer entry for one target (GUI_FUZZ_TARGET: 1 CSS, 2 markup, 3 keymap, 4 string
// table, 5 input on the Controls page), for long runs with -fsanitize=fuzzer,address
// (OPTION_GUI_LIBFUZZER). Seed it with the sandbox's files: `GUI.Fuzz.Css Data/Samples/UI/sandbox`.
#include "Core/Prelude.h"
#include <cstdint>

import foundation.core;
import foundation.vfs;
import foundation.vg;
import foundation.xml;
import foundation.xml.serialization;
import foundation.i18n;
import foundation.gui;
import foundation.gui.testing;

#include "Fuzz.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const foundation::core::StringView bytes(reinterpret_cast<const foundation::core::utf8char*>(data), size);
#if GUI_FUZZ_TARGET == 1
    guifuzz::FuzzCss(bytes);
#elif GUI_FUZZ_TARGET == 2
    guifuzz::FuzzMarkup(bytes);
#elif GUI_FUZZ_TARGET == 3
    guifuzz::FuzzKeymap(bytes);
#elif GUI_FUZZ_TARGET == 4
    guifuzz::FuzzStringTable(bytes);
#else
    static const foundation::core::Array<foundation::core::String> pages = guifuzz::SandboxSeeds(u8".xml");
    static const foundation::core::Array<foundation::core::String> sheets = guifuzz::SandboxSeeds(u8".css");
    if (!pages.IsEmpty())
        guifuzz::FuzzInput(pages[size % pages.Size()].AsView(),
                           sheets.IsEmpty() ? foundation::core::StringView() : sheets[size % sheets.Size()].AsView(), bytes);
#endif
    return 0;
}
