// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PathBuilder: commands, closing, arcs and the immutable Path it builds.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

TEST_CASE("pathbuilder: MoveTo/LineTo creates commands")
{
    PathBuilder builder;
    builder.MoveTo(10, 20);
    builder.LineTo(30, 40);
    builder.LineTo(50, 60);

    const Path path = builder.ToPath();
    const Span<const PathCommand> cmds = path.Commands();
    CHECK(path.CommandCount() == 3u);
    CHECK(cmds[0] == PathCommand::MoveTo);
    CHECK(cmds[1] == PathCommand::LineTo);
    CHECK(cmds[2] == PathCommand::LineTo);
    CHECK(path.PointCount() == 3u);
}

TEST_CASE("pathbuilder: Close adds close command")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 0);
    builder.LineTo(10, 10);
    builder.Close();
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 4u);
    CHECK(path.Commands()[3] == PathCommand::Close);
}

TEST_CASE("pathbuilder: ToPath returns immutable copy")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 10);
    const Path path1 = builder.ToPath();
    builder.LineTo(20, 20);
    const Path path2 = builder.ToPath();
    CHECK(path1.CommandCount() == 2u);
    CHECK(path2.CommandCount() == 3u);
}

TEST_CASE("pathbuilder: ArcTo generates cubics")
{
    PathBuilder builder;
    builder.MoveTo(100, 0);
    builder.ArcTo(100, 100, 0, false, true, 0, 100);
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() >= 2u);

    bool hasCubic = false;
    const Span<const PathCommand> cmds = path.Commands();
    for (usize i = 0; i < cmds.Size(); ++i)
        if (cmds[i] == PathCommand::CubicTo)
            hasCubic = true;
    CHECK(hasCubic);
}

TEST_CASE("pathbuilder: Clear resets")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 10);
    builder.Clear();
    CHECK(builder.CommandCount() == 0u);
    CHECK(builder.CurrentPoint() == Float2::Zero);
}

TEST_CASE("pathbuilder: QuadTo adds two points")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.QuadTo(5, 10, 10, 0);
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 2u);
    CHECK(path.Commands()[1] == PathCommand::QuadTo);
    CHECK(path.PointCount() == 3u);
}

TEST_CASE("pathbuilder: CubicTo adds three points")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.CubicTo(5, 10, 15, 10, 20, 0);
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 2u);
    CHECK(path.Commands()[1] == PathCommand::CubicTo);
    CHECK(path.PointCount() == 4u);
}

TEST_CASE("pathbuilder: implicit MoveTo when none provided")
{
    PathBuilder builder;
    builder.LineTo(10, 10);
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 2u);
    CHECK(path.Commands()[0] == PathCommand::MoveTo);
}
