#include <doctest/doctest.h>

#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Log/Log.h"

import draconic.foundation;

using namespace draconic::foundation;

namespace
{
    // Builds a formatted string and returns whether it equals `expected`.
    template <typename... Args>
    bool FormatEquals(const utf8char* expected, const utf8char* fmt, const Args&... args)
    {
        FormatBuffer buffer;
        FormatToV(buffer, fmt, args...);
        return buffer.View() == StringView(expected);
    }
}

// --- Format ----------------------------------------------------------------

TEST_CASE("format: substitution and types")
{
    CHECK(FormatEquals(u8"no args", u8"no args"));
    CHECK(FormatEquals(u8"a=1 b=2", u8"a={} b={}", 1, 2));
    CHECK(FormatEquals(u8"neg -42", u8"neg {}", -42));
    CHECK(FormatEquals(u8"big 4294967295", u8"big {}", 4294967295u));
    CHECK(FormatEquals(u8"flag true and false", u8"flag {} and {}", true, false));
    CHECK(FormatEquals(u8"char X", u8"char {}", 'X'));
    CHECK(FormatEquals(u8"str hello", u8"str {}", u8"hello"));
    CHECK(FormatEquals(u8"pi 3.5", u8"pi {}", 3.5));
}

TEST_CASE("format: brace escapes and extra/missing args")
{
    CHECK(FormatEquals(u8"{literal}", u8"{{literal}}"));
    CHECK(FormatEquals(u8"set {x} = 7", u8"set {{x}} = {}", 7));
    CHECK(FormatEquals(u8"only 1", u8"only {}", 1, 2, 3)); // extra args ignored
    CHECK(FormatEquals(u8"missing {}", u8"missing {}"));   // unmatched placeholder left as-is
}

// --- Log -------------------------------------------------------------------

namespace
{
    struct CapturingSink : ILogSink
    {
        int count = 0;
        LogLevel lastLevel = LogLevel::Off;
        String lastCategory;
        String lastMessage;

        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            ++count;
            lastLevel = level;
            lastCategory = String(category);
            lastMessage = String(message);
        }
    };
}

TEST_CASE("log: dispatch, formatting, and level filtering")
{
    CapturingSink sink;
    Logger& logger = GlobalLogger();
    const LogLevel previousLevel = logger.MinLevel();

    logger.AddSink(&sink);
    logger.SetMinLevel(LogLevel::Info);

    DRACONIC_LOG_INFO(u8"Renderer", u8"loaded {} meshes", 12);
    CHECK(sink.count == 1);
    CHECK(sink.lastLevel == LogLevel::Info);
    CHECK(sink.lastCategory == u8"Renderer");
    CHECK(sink.lastMessage == u8"loaded 12 meshes");

    // Below the min level -> filtered out.
    DRACONIC_LOG_DEBUG(u8"Renderer", u8"verbose {}", 1);
    CHECK(sink.count == 1);

    // At/above min level -> delivered.
    DRACONIC_LOG_ERROR(u8"Audio", u8"device {} lost", 3);
    CHECK(sink.count == 2);
    CHECK(sink.lastLevel == LogLevel::Error);
    CHECK(sink.lastMessage == u8"device 3 lost");

    logger.RemoveSink(&sink);
    logger.SetMinLevel(previousLevel);

    // After removal, no more delivery.
    DRACONIC_LOG_ERROR(u8"Audio", u8"ignored");
    CHECK(sink.count == 2);
}

// --- Log: thread safety ----------------------------------------------------

namespace
{
    struct CountingSink : ILogSink
    {
        Atomic<int> count{0};
        void Write(LogLevel, StringView, StringView) noexcept override { count.fetch_add(1); }
    };
}

#if !DRACONIC_PLATFORM_WEB // spawns threads; web v1 is single-threaded
TEST_CASE("log: concurrent logging is serialized by the logger")
{
    Logger& logger = GlobalLogger();
    const LogLevel previous = logger.MinLevel();
    logger.SetMinLevel(LogLevel::Info);

    CountingSink sink;
    logger.AddSink(&sink);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;

    Array<Thread> threads;
    for (int i = 0; i < kThreads; ++i)
    {
        threads.PushBack(Thread(
            []()
            {
                for (int j = 0; j < kPerThread; ++j)
                {
                    DRACONIC_LOG_INFO(u8"Worker", u8"tick {}", j);
                }
            }));
    }
    for (Thread& t : threads)
    {
        t.Join();
    }

    logger.RemoveSink(&sink);
    logger.SetMinLevel(previous);

    CHECK(sink.count.load() == kThreads * kPerThread);
}

// --- Format: wide / UTF-8 string arguments ---------------------------------

#endif // !DRACONIC_PLATFORM_WEB

TEST_CASE("format: wide and utf8 string arguments")
{
    // A WideString / WideStringView argument transcodes to UTF-8.
    WideString wide = u"café";
    CHECK(FormatEquals(u8"name=café", u8"name={}", wide));
    CHECK(FormatEquals(u8"v=héllo", u8"v={}", WideStringView(u"héllo")));

    // A UTF-8 view appends directly.
    CHECK(FormatEquals(u8"u=café", u8"u={}", StringView(u8"café")));
}

// --- Log: in-memory ring sink ----------------------------------------------

TEST_CASE("log: RingLogSink keeps the most recent records")
{
    RingLogSink ring(3);
    CHECK(ring.Count() == 0u);

    for (int i = 0; i < 5; ++i)
    {
        utf8char msg[3];
        msg[0] = utf8char('m');
        msg[1] = static_cast<utf8char>(utf8char('0') + i);
        msg[2] = utf8char('\0');
        ring.Write(LogLevel::Info, u8"Cat", StringView(msg, 2));
    }

    // Capacity 3 -> keeps the last three (m2, m3, m4).
    CHECK(ring.Count() == 3u);
    CHECK(StringView(ring.Record(0).message) == StringView(u8"m2"));
    CHECK(StringView(ring.Record(2).message) == StringView(u8"m4"));
    CHECK(ring.Record(0).level == LogLevel::Info);
    CHECK(StringView(ring.Record(0).category) == StringView(u8"Cat"));
}

TEST_CASE("format: checked FormatTo validates arg count at compile time")
{
    // Correct placeholder/arg count compiles and formats as usual.
    FormatBuffer buffer;
    FormatTo(buffer, u8"a={} b={}", 1, 2);
    CHECK(buffer.View() == StringView(u8"a=1 b=2"));

    // A mismatched count, e.g. FormatTo(buffer, u8"x={}", 1, 2), would fail to
    // compile via FormatString's consteval constructor.
}
