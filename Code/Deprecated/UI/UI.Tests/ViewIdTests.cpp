// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/ViewIdTests.bf (faithful).
// ViewId.ToString appends a debug string via core::AppendFormat (Sedulous ViewId.ToString).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;

using namespace foundation::ui;
using namespace foundation::core;

TEST_CASE("view-id: Create_ReturnsValidId")
{
    ViewId id = ViewId::Create();
    CHECK(id.IsValid());
    CHECK(id.RawValue() > 0u);
}

TEST_CASE("view-id: Create_ReturnsUniqueIds")
{
    ViewId a = ViewId::Create();
    ViewId b = ViewId::Create();
    CHECK(a != b);
}

TEST_CASE("view-id: Invalid_IsNotValid")
{
    ViewId id = ViewId::Invalid;
    CHECK_FALSE(id.IsValid());
    CHECK(id.RawValue() == 0u);
}

TEST_CASE("view-id: Equality_SameValue")
{
    ViewId a = ViewId::Create();
    ViewId b = a;
    CHECK(a == b);
    CHECK(a.Equals(b));
}

TEST_CASE("view-id: Inequality_DifferentValues")
{
    ViewId a = ViewId::Create();
    ViewId b = ViewId::Create();
    CHECK(a != b);
    CHECK_FALSE(a.Equals(b));
}

TEST_CASE("view-id: GetHashCode_SameForEqual")
{
    ViewId a = ViewId::Create();
    ViewId b = a;
    CHECK(a.GetHashCode() == b.GetHashCode());
}

TEST_CASE("view-id: ToString_ContainsValue")
{
    ViewId id = ViewId::Create();
    String str;
    id.ToString(str);
    // Local substring check for "ViewId(" (StringView has no Contains).
    const StringView s = str.AsView();
    const StringView needle(u8"ViewId(");
    bool found = false;
    if (needle.Size() <= s.Size())
        for (usize i = 0; i + needle.Size() <= s.Size(); ++i)
        {
            bool m = true;
            for (usize j = 0; j < needle.Size(); ++j)
                if (s[i + j] != needle[j])
                {
                    m = false;
                    break;
                }
            if (m)
            {
                found = true;
                break;
            }
        }
    CHECK(found);
}
