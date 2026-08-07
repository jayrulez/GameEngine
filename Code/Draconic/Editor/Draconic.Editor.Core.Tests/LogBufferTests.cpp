// EditorLogBuffer tests (docs/design/editor.md §3.10): sink capture through the global logger,
// incremental CollectSince polling, bounded-ring overflow with drop counting, full-fidelity
// (untruncated) messages, and cross-thread writes.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

import draconic.foundation;
import draconic.editor.core;

using namespace draconic::foundation;
using namespace draconic::editor;

TEST_CASE("editor-log: captures dispatches and collects incrementally")
{
    EditorLogBuffer buffer(16);
    buffer.Write(LogLevel::Info, u8"Cat", u8"first");
    buffer.Write(LogLevel::Warning, u8"Cat", u8"second");
    CHECK(buffer.Count() == 2);

    Array<EditorLogEntry> out;
    u64 seq = buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 2);
    CHECK(out[0].message == u8"first");
    CHECK(out[0].category == u8"Cat");
    CHECK(out[0].level == LogLevel::Info);
    CHECK(out[1].message == u8"second");
    CHECK(out[1].sequence > out[0].sequence);

    // Incremental: nothing new since the returned high-water mark.
    Array<EditorLogEntry> more;
    const u64 seq2 = buffer.CollectSince(seq, more);
    CHECK(more.Size() == 0);
    CHECK(seq2 == seq);

    // A new entry shows up alone on the next poll.
    buffer.Write(LogLevel::Error, u8"Cat", u8"third");
    const u64 seq3 = buffer.CollectSince(seq2, more);
    REQUIRE(more.Size() == 1);
    CHECK(more[0].message == u8"third");
    CHECK(seq3 > seq2);
}

TEST_CASE("editor-log: bounded ring drops oldest and counts drops")
{
    EditorLogBuffer buffer(4);
    for (i32 i = 0; i < 10; ++i)
    {
        String msg(u8"m");
        msg.PushBack(static_cast<utf8char>('0' + i));
        buffer.Write(LogLevel::Info, u8"Cat", msg.AsView());
    }
    CHECK(buffer.Count() == 4);
    CHECK(buffer.DroppedCount() == 6);

    Array<EditorLogEntry> out;
    (void)buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 4);
    CHECK(out[0].message == u8"m6"); // oldest surviving
    CHECK(out[3].message == u8"m9");
    // Sequences reveal the gap (entries 1..6 were dropped before collection).
    CHECK(out[0].sequence == 7);
}

TEST_CASE("editor-log: messages are not truncated")
{
    EditorLogBuffer buffer(4);
    String longMessage;
    for (i32 i = 0; i < 100; ++i)
    {
        longMessage += u8"0123456789";
    } // 1000 chars
    buffer.Write(LogLevel::Error, u8"Build", longMessage.AsView());

    Array<EditorLogEntry> out;
    (void)buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 1);
    CHECK(out[0].message.Size() == longMessage.Size()); // foundation RingLogSink would cap at 192
}

TEST_CASE("editor-log: registered on the global logger it captures DRACONIC_LOG output")
{
    EditorLogBuffer buffer(16);
    GlobalLogger().AddSink(&buffer);
    DRACONIC_LOG_WARNING(u8"EditorTest", u8"hello {}", 42);
    GlobalLogger().RemoveSink(&buffer);

    Array<EditorLogEntry> out;
    (void)buffer.CollectSince(0, out);
    REQUIRE(out.Size() == 1);
    CHECK(out[0].level == LogLevel::Warning);
    CHECK(out[0].category == u8"EditorTest");
    CHECK(out[0].message == u8"hello 42");
}

TEST_CASE("editor-log: concurrent writers do not lose or corrupt entries")
{
    EditorLogBuffer buffer(4096);
    constexpr i32 kThreads = 4;
    constexpr i32 kPerThread = 200;

    Thread threads[kThreads];
    for (i32 t = 0; t < kThreads; ++t)
    {
        threads[t] = Thread(
            [&buffer]()
            {
                for (i32 i = 0; i < kPerThread; ++i)
                {
                    buffer.Write(LogLevel::Info, u8"Thread", u8"message");
                }
            });
    }
    for (i32 t = 0; t < kThreads; ++t)
    {
        threads[t].Join();
    }

    CHECK(buffer.Count() == static_cast<usize>(kThreads * kPerThread));
    CHECK(buffer.DroppedCount() == 0);

    // All sequences unique and dense (1..N).
    Array<EditorLogEntry> out;
    const u64 high = buffer.CollectSince(0, out);
    CHECK(out.Size() == static_cast<usize>(kThreads * kPerThread));
    CHECK(high == static_cast<u64>(kThreads * kPerThread));
}
