// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - locale ids: the parent chain, normal form, shapes.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.i18n;

using namespace foundation::core;
using namespace foundation::i18n;

TEST_CASE("i18n.locale: a locale's parent is the id one subtag shorter")
{
    CHECK(ParentLocale(u8"pt-BR") == StringView(u8"pt"));
    CHECK(ParentLocale(u8"zh-Hans-CN") == StringView(u8"zh-Hans"));
    CHECK(ParentLocale(u8"en_US") == StringView(u8"en"));
    CHECK(ParentLocale(u8"en").Size() == 0u);
}

TEST_CASE("i18n.locale: ids compare by their normal form")
{
    CHECK(NormalizeLocale(u8"EN_us") == String(u8"en-us"));
    CHECK(NormalizeLocale(u8"pt-BR") == String(u8"pt-BR"));
    CHECK(SameLocale(u8"en-US", u8"en_us"));
    CHECK_FALSE(SameLocale(u8"en-US", u8"en-GB"));
}

TEST_CASE("i18n.locale: an id's shape, never whether it exists")
{
    const char8_t* shaped[] = {u8"en", u8"fil", u8"pt-BR", u8"zh-Hans", u8"zh-Hans-CN", u8"es-419", u8"xx"};
    for (const char8_t* ok : shaped)
        CHECK(IsLocaleShaped(ok));
    const char8_t* unshaped[] = {u8"", u8"e", u8"english", u8"en-", u8"en-USA1", u8"en-US-x", u8"12"};
    for (const char8_t* bad : unshaped)
        CHECK_FALSE(IsLocaleShaped(bad));
}
