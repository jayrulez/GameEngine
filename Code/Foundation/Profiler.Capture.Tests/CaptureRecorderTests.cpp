// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The capture recorder (Specs/profiler.md, P3): arms the profiler for the time asked, waits for
// the last frames' GPU passes, then writes a profile stream file and Chrome trace JSON beside it,
// and lets the profiler go.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.profiler;
import foundation.profiler.capture;

using namespace foundation::core;
using namespace foundation::profiler;
namespace json = foundation::json;

TEST_CASE("capture recorder: a capture becomes a stream file and a Chrome trace, then lets go")
{
    Profiler p(DefaultAllocator());
    CaptureRecorder recorder(DefaultAllocator());
    recorder.SetApplication(StringView(u8"Probe"));
    recorder.SetBuild(StringView(u8"Linux64"), StringView(u8"test"));
    CHECK(recorder.LastCapture().IsEmpty());
    REQUIRE(recorder.Start(p, 0.0, StringView(u8"scratch_profile_captures")));
    CHECK(p.Armed());
    CHECK_FALSE(recorder.Start(p, 1.0, StringView(u8"scratch_profile_captures"))); // one at a time

    u32 frames = 0;
    bool written = false;
    while (!written && frames < 20)
    {
        p.BeginFrame();
        REQUIRE(p.BeginScope("Work"));
        p.EndScope();
        p.EndFrame();
        written = recorder.OnFrameEnd(p);
        ++frames;
    }
    REQUIRE(written);
    CHECK(frames == u32{kCaptureRecorderGpuGraceFrames}); // the time was up at once; then the grace
    CHECK_FALSE(p.Armed());
    CHECK_FALSE(recorder.Capturing());
    REQUIRE_FALSE(recorder.LastCapture().IsEmpty());

    // The stream reads back as a capture of the recorded frame, named and built as set.
    FileStream file(recorder.LastCapture(), FileMode::Read);
    REQUIRE(file.IsValid());
    ProfileCapture capture(DefaultAllocator());
    REQUIRE(capture.Read(file));
    CHECK(capture.Header().application == String(u8"Probe"));
    CHECK(capture.Header().build == String(u8"test"));
    REQUIRE(capture.FrameCount() >= 1u);
    CHECK(capture.NameOf(capture.Frame(capture.OldestFrame())->threads[0].scopes[0].name) == StringView(u8"Work"));

    // The Chrome trace beside it.
    String jsonPath(recorder.LastCapture());
    jsonPath = String(StringView(jsonPath.Data(), jsonPath.Size() - String(u8".aeprofile").Size()));
    jsonPath.Append(u8".json");
    FileStream jsonFile(jsonPath.AsView(), FileMode::Read);
    REQUIRE(jsonFile.IsValid());
    Array<char8_t> bytes;
    bytes.Resize(static_cast<usize>(jsonFile.Size()));
    REQUIRE(jsonFile.Read(bytes.Data(), bytes.Size()) == bytes.Size());
    const String text(StringView(bytes.Data(), bytes.Size()));
    CHECK(json::JsonValue::Parse(text).Get(String(u8"traceEvents")).IsArray());
}

TEST_CASE("capture files: any source's frames, a recording's too, as the stream and the Chrome trace")
{
    // The editor's panel saves what it recorded this way, as the recorder writes a capture.
    Profiler p(DefaultAllocator());
    p.Arm();
    LiveProfileSource live(p);
    ProfileRecording recording(DefaultAllocator());
    recording.Begin(live);
    for (u32 f = 0; f < 3; ++f)
    {
        p.BeginFrame();
        REQUIRE(p.BeginScope("Panel"));
        p.EndScope();
        p.EndFrame();
    }
    recording.Pull(live, 0);
    REQUIRE(CreateDirectories(StringView(u8"scratch_capture_files")));
    StreamHeader header;
    header.tickFrequency = recording.TickFrequency();
    header.application = String(u8"Editor");
    const String path = WriteCaptureFiles(DefaultAllocator(), recording, recording.OldestFrame(),
                                          recording.NewestFrame(), StringView(u8"scratch_capture_files/saved"), header);
    CHECK(path == StringView(u8"scratch_capture_files/saved.aeprofile"));
    FileStream file(path.AsView(), FileMode::Read);
    REQUIRE(file.IsValid());
    ProfileCapture capture(DefaultAllocator());
    REQUIRE(capture.Read(file));
    CHECK(capture.FrameCount() == 3u);
    CHECK(capture.Header().application == String(u8"Editor"));
    CHECK(FileExists(StringView(u8"scratch_capture_files/saved.json")));

    // A folder that is not there: nothing written, an empty answer.
    CHECK(WriteCaptureFiles(DefaultAllocator(), recording, 0, 0, StringView(u8"scratch_no_such_folder/x/saved"), header)
              .IsEmpty());
}
