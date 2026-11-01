// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :sample_downloads partition.
//
// The sample games the project manager's Samples tab offers (Documentation/Specs/project-manager.md,
// P5): the catalog a release publishes (samples.json, from scripts/pack-samples.py), and one
// sample's download. A download streams the release's zip to a file beside where it goes, hashing
// it as it arrives, checks the SHA-256 the catalog gives, then extracts it into its folder and
// deletes the zip. Cancelled or failed, it keeps nothing it wrote. Everything happens inside the
// HTTP client's Pump, on the caller's thread.
module;
#include "Core/Prelude.h"

export module editor.app:sample_downloads;

import foundation.core;
import foundation.json;
import foundation.http;
import foundation.http.client;
import foundation.zip;

using namespace foundation::core;

export namespace editor::app
{
    /// One sample game a release offers.
    struct SampleInfo
    {
        String folder;      ///< its folder in the repository ("PlatformerGame"), and the one made for it
        String name;        ///< its project's name ("Sky Hopper")
        String description; ///< one line on what it is
        String archive;     ///< its zip in the release ("sample-PlatformerGame.zip")
        String picture;     ///< its picture in the release
        String sha256;      ///< the zip's digest, 64 lowercase hex digits
        u64 size = 0;         ///< the zip's bytes
        u64 unpackedSize = 0; ///< what it extracts to
    };

    /// A release's samples.json.
    struct SampleCatalog
    {
        String version;
        Array<SampleInfo> samples;

        /// Reads `json`. False with `error`, said for a person, when it is not a catalog: a
        /// sample missing its folder, archive or digest, or a folder or archive that is not a
        /// plain name, refuses the whole of it.
        static bool Parse(StringView json, SampleCatalog& out, String& error)
        {
            out = SampleCatalog{};
            const foundation::json::ParseResult parsed = foundation::json::Parse(json);
            if (!parsed.ok || !parsed.value.IsObject() || !parsed.value.Get(String(u8"samples")).IsArray())
            {
                error = String(u8"the samples list could not be read");
                return false;
            }
            out.version = parsed.value.Get(String(u8"version")).AsString();
            for (const foundation::json::JsonValue& item : parsed.value.Get(String(u8"samples")).Items())
            {
                const auto text = [&item](const char8_t* key) { return item.Get(String(key)).AsString(); };
                SampleInfo info;
                info.folder = text(u8"folder");
                info.name = text(u8"name");
                info.description = text(u8"description");
                info.archive = text(u8"archive");
                info.picture = text(u8"picture");
                info.sha256 = text(u8"sha256");
                info.size = static_cast<u64>(Max<i64>(0, item.Get(String(u8"size")).AsInt()));
                info.unpackedSize = static_cast<u64>(Max<i64>(0, item.Get(String(u8"unpackedSize")).AsInt()));
                if (info.name.IsEmpty())
                    info.name = info.folder;
                if (!PlainName(info.folder.AsView()) || !PlainName(info.archive.AsView()) || info.sha256.Size() != 64 ||
                    (!info.picture.IsEmpty() && !PlainName(info.picture.AsView())))
                {
                    error = String(u8"the samples list names a sample this editor cannot fetch");
                    out = SampleCatalog{};
                    return false;
                }
                out.samples.PushBack(Move(info));
            }
            return true;
        }

    private:
        // A file or folder name alone: nothing that walks out of where it is put.
        [[nodiscard]] static bool PlainName(StringView name)
        {
            if (name.IsEmpty() || name == StringView(u8".") || name == StringView(u8".."))
                return false;
            for (const utf8char c : name)
                if (c == u8'/' || c == u8'\\' || c == u8':')
                    return false;
            return true;
        }
    };

    /// One sample's download into `target` (made if need be; a new or empty folder, as New
    /// Project's check requires): streamed to `<target's parent>/.<folder>.download`, checked,
    /// extracted, the zip deleted. Keep it where it does not move: its callbacks hold it.
    class SampleDownload
    {
    public:
        enum class State : u8
        {
            Downloading,
            Done,      ///< the project is in `target`
            Failed,    ///< Error() says why; nothing it wrote is left
            Cancelled, ///< nothing it wrote is left
        };

        SampleDownload(IAllocator& allocator, foundation::http::IHttpClient& client, SampleInfo sample, String url, String target)
            : m_allocator(&allocator), m_client(&client), m_sample(Move(sample)), m_url(Move(url)), m_target(Move(target))
        {
            m_targetExisted = DirectoryExists(m_target.AsView());
            m_partial = PathJoin(PathParent(m_target.AsView()), Format(u8".{}.download", m_sample.folder).AsView());
            m_total = m_sample.size;
        }
        ~SampleDownload()
        {
            if (m_state == State::Downloading)
                Cancel();
        }
        SampleDownload(const SampleDownload&) = delete;
        SampleDownload& operator=(const SampleDownload&) = delete;

        /// Sends the request; what it hears comes inside the client's Pump.
        void Start()
        {
            if (!CreateDirectories(PathParent(m_partial.AsView())))
            {
                Fail(Format(u8"{} could not be made", PathParent(m_partial.AsView())));
                return;
            }
            foundation::http::HttpClientRequest request = foundation::http::HttpClientRequest::Get(m_url.AsView());
            request.timeoutSeconds = 600.0f;
            request.maxResponseBytes = static_cast<usize>(Max<u64>(m_sample.size, 1) * 2 + 1024 * 1024); // its size, with room
            SampleDownload* self = this;
            request.onHead = [self](u16 status, const Array<foundation::http::HttpHeader>&, i64 length) { self->OnHead(status, length); };
            request.onBody = [self](Span<const byte> piece) { self->OnBody(piece); };
            m_request = m_client->Send(Move(request), [self](const foundation::http::HttpClientResponse& response) { self->OnDone(response); });
        }

        /// Stops it and deletes what it wrote; the folder it would have filled is left as it was.
        void Cancel()
        {
            if (m_state != State::Downloading)
                return;
            m_client->Cancel(m_request);
            Discard();
            m_state = State::Cancelled;
        }

        [[nodiscard]] State GetState() const noexcept { return m_state; }
        [[nodiscard]] const SampleInfo& Sample() const noexcept { return m_sample; }
        [[nodiscard]] StringView Target() const noexcept { return m_target.AsView(); }
        [[nodiscard]] StringView Error() const noexcept { return m_error.AsView(); }
        /// Bytes so far, and of how many (the catalog's size, or the response's length).
        [[nodiscard]] u64 Received() const noexcept { return m_received; }
        [[nodiscard]] u64 Total() const noexcept { return m_total; }
        /// How far along it is, 0 to 1.
        [[nodiscard]] f32 Progress() const noexcept
        {
            return m_total == 0 ? 0.0f : Min(1.0f, static_cast<f32>(static_cast<f64>(m_received) / static_cast<f64>(m_total)));
        }

    private:
        void OnHead(u16 status, i64 length)
        {
            if (m_state != State::Downloading)
                return;
            if (status != 200)
            {
                m_client->Cancel(m_request);
                Fail(status == 404 ? String(u8"the release does not have this sample") : Format(u8"the download was refused ({})", status));
                return;
            }
            if (length > 0)
                m_total = static_cast<u64>(length);
            m_file = MakeUnique<FileStream>(*m_allocator, m_partial.AsView(), FileMode::Write);
            if (!m_file->IsValid())
            {
                m_client->Cancel(m_request);
                Fail(Format(u8"{} could not be written", m_partial));
            }
        }
        void OnBody(Span<const byte> piece)
        {
            if (m_state != State::Downloading || !m_file)
                return;
            if (m_file->Write(piece.Data(), piece.Size()) != piece.Size())
            {
                m_client->Cancel(m_request);
                Fail(String(u8"the download could not be written (is the disk full?)"));
                return;
            }
            m_hash.Update(piece);
            m_received += piece.Size();
        }
        void OnDone(const foundation::http::HttpClientResponse& response)
        {
            if (m_state != State::Downloading)
                return;
            if (!response.Ok())
            {
                Fail(Format(u8"the download failed: {}", response.error));
                return;
            }
            m_file.Reset(); // closed, before it is read back
            if (m_hash.Finish().ToHex() != m_sample.sha256)
            {
                Fail(String(u8"the download does not match its checksum: try again"));
                return;
            }
            FileStream archive(m_partial.AsView(), FileMode::Read);
            foundation::zip::ZipArchive zip;
            String error;
            if (!archive.IsValid() || !zip.Open(archive, error) || !zip.ExtractAll(m_target.AsView(), error))
            {
                ClearTarget(); // a part of a project is no project
                Fail(Format(u8"the sample could not be unpacked: {}", error));
                return;
            }
            (void)FileDelete(m_partial.AsView());
            m_state = State::Done;
        }

        void Fail(String why)
        {
            Discard();
            m_error = Move(why);
            m_state = State::Failed;
        }
        void Discard()
        {
            m_file.Reset();
            (void)FileDelete(m_partial.AsView());
        }

        // Takes back what extracting wrote: a folder it made goes; one that was there (empty, as the
        // check requires) is emptied and kept.
        void ClearTarget()
        {
            if (!m_targetExisted)
            {
                (void)RemoveDirectoryRecursive(m_target.AsView());
                return;
            }
            struct Found
            {
                const String* root;
                Array<String> paths;
            } found{&m_target, {}};
            (void)ListDirectory(m_target.AsView(), [](void* context, StringView name, bool)
                                { static_cast<Found*>(context)->paths.PushBack(PathJoin(static_cast<Found*>(context)->root->AsView(), name)); },
                                &found);
            for (const String& path : found.paths)
            {
                if (DirectoryExists(path.AsView()))
                    (void)RemoveDirectoryRecursive(path.AsView());
                else
                    (void)FileDelete(path.AsView());
            }
        }

        IAllocator* m_allocator;
        foundation::http::IHttpClient* m_client;
        SampleInfo m_sample;
        String m_url;
        String m_target;
        String m_partial;
        String m_error;
        UniquePtr<FileStream> m_file;
        Sha256 m_hash;
        foundation::http::HttpRequestId m_request = 0;
        u64 m_received = 0;
        u64 m_total = 0;
        State m_state = State::Downloading;
        bool m_targetExisted = false;
    };
}
