// LogView tests (headless): entry accumulation with category-prefixed text, level-bucket
// filtering (fold Trace+Debug / Error+Fatal), entry cap trimming, and Clear.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.editor.app;

using namespace draconic::foundation;
using namespace draconic::editor::app;

TEST_CASE("editor-logview: entries accumulate with category-prefixed, level-bucketed rows")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->AddEntry(LogLevel::Info, u8"Editor", u8"opened project");
    view->AddEntry(LogLevel::Trace, u8"RHI", u8"trace detail");
    view->AddEntry(LogLevel::Fatal, u8"RHI", u8"device lost");

    CHECK(view->EntryCount() == 3);
    CHECK(view->VisibleEntryCount() == 3);
    CHECK(view->VisibleEntryText(0) == u8"[Editor] opened project");

    CHECK(LogView::BucketOf(LogLevel::Trace) == LogView::Bucket::Debug);
    CHECK(LogView::BucketOf(LogLevel::Debug) == LogView::Bucket::Debug);
    CHECK(LogView::BucketOf(LogLevel::Info) == LogView::Bucket::Info);
    CHECK(LogView::BucketOf(LogLevel::Warning) == LogView::Bucket::Warning);
    CHECK(LogView::BucketOf(LogLevel::Error) == LogView::Bucket::Error);
    CHECK(LogView::BucketOf(LogLevel::Fatal) == LogView::Bucket::Error);
}

TEST_CASE("editor-logview: bucket filters hide and reshow entries")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->AddEntry(LogLevel::Info, u8"A", u8"info");
    view->AddEntry(LogLevel::Warning, u8"A", u8"warn");
    view->AddEntry(LogLevel::Error, u8"A", u8"error");

    view->SetBucketVisible(LogView::Bucket::Warning, false);
    CHECK(view->EntryCount() == 3);
    CHECK(view->VisibleEntryCount() == 2);
    CHECK(view->VisibleEntryText(1) == u8"[A] error");

    // Entries added while filtered out stay hidden...
    view->AddEntry(LogLevel::Warning, u8"A", u8"warn2");
    CHECK(view->VisibleEntryCount() == 2);

    // ...and reappear (in order) when the bucket is reshown.
    view->SetBucketVisible(LogView::Bucket::Warning, true);
    CHECK(view->VisibleEntryCount() == 4);
    CHECK(view->VisibleEntryText(1) == u8"[A] warn");
    CHECK(view->VisibleEntryText(3) == u8"[A] warn2");
}

TEST_CASE("editor-logview: entry cap trims oldest; Clear empties")
{
    auto view = MakeRef<LogView>(DefaultAllocator());
    view->MaxEntries = 3;
    view->AddEntry(LogLevel::Info, u8"A", u8"one");
    view->AddEntry(LogLevel::Info, u8"A", u8"two");
    view->AddEntry(LogLevel::Info, u8"A", u8"three");
    view->AddEntry(LogLevel::Info, u8"A", u8"four");

    CHECK(view->EntryCount() == 3);
    CHECK(view->VisibleEntryText(0) == u8"[A] two");
    CHECK(view->VisibleEntryText(2) == u8"[A] four");

    view->Clear();
    CHECK(view->EntryCount() == 0);
    CHECK(view->VisibleEntryCount() == 0);
}
