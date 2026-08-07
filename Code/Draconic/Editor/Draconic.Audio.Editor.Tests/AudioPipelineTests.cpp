// Full audio pipeline: author a wav (sources mount) -> AudioClipAsset -> cook via the
// builder into an output db -> load the AudioClip product through the factory. Covers
// write-through byte identity (NO PCM sidecars - the container bytes ARE the cook), the
// stream flag's ContentInstanceStreamSource wiring, cook validation, the destructive
// transforms (force-mono / trim / normalize), the Stream auto-default, the WAV `smpl`
// loop-point parser, and the importer's asset fan-out through a real EditorProject.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <cmath>
#include <initializer_list>

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.editor;
import draconic.editor.core;
import draconic.audio;
import draconic.audio.resource;
import draconic.audio.editor;

using namespace draconic::foundation;
using namespace draconic::resource;
using namespace draconic::audio;
namespace content = draconic::content;

namespace
{
    [[nodiscard]] Array<i16> MakeTone(f32 seconds, u32 sampleRate, u32 channels,
                                      f32 amplitude = 0.5f)
    {
        Array<i16> samples;
        const usize frameCount = static_cast<usize>(seconds * static_cast<f32>(sampleRate));
        for (usize frame = 0; frame < frameCount; ++frame)
        {
            const f32 t = static_cast<f32>(frame) / static_cast<f32>(sampleRate);
            const i16 sample =
                static_cast<i16>(amplitude * std::sin(2.0f * 3.14159265f * 440.0f * t) * 32000.0f);
            for (u32 channel = 0; channel < channels; ++channel)
            {
                samples.PushBack(sample);
            }
        }
        return samples;
    }

    [[nodiscard]] Array<byte> MakeToneWav(f32 seconds, u32 sampleRate = 8000, u32 channels = 1,
                                          f32 amplitude = 0.5f)
    {
        const Array<i16> samples = MakeTone(seconds, sampleRate, channels, amplitude);
        Array<byte> wav;
        REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), channels,
                                   sampleRate, wav));
        return wav;
    }

    // Appends an `smpl` chunk with one loop and patches the RIFF size.
    void AppendSampleLoopChunk(Array<byte>& wav, u32 loopStart, u32 loopEnd)
    {
        auto pushU32 = [&](u32 value)
        {
            wav.PushBack(static_cast<byte>(value & 0xFF));
            wav.PushBack(static_cast<byte>((value >> 8) & 0xFF));
            wav.PushBack(static_cast<byte>((value >> 16) & 0xFF));
            wav.PushBack(static_cast<byte>((value >> 24) & 0xFF));
        };
        auto pushTag = [&](const char* tag)
        {
            for (int i = 0; i < 4; ++i)
            {
                wav.PushBack(static_cast<byte>(tag[i]));
            }
        };
        pushTag("smpl");
        pushU32(36 + 24); // chunk size: sampler fields + one loop record
        for (int i = 0; i < 7; ++i)
        {
            pushU32(0);
        } // manufacturer .. SMPTE offset
        pushU32(1); // numLoops
        pushU32(0); // sampler data
        pushU32(0); // loop id
        pushU32(0); // loop type (forward)
        pushU32(loopStart);
        pushU32(loopEnd);
        pushU32(0); // fraction
        pushU32(0); // play count (infinite)
        // Patch the RIFF size field (bytes 4..7) = file size - 8.
        const u32 riffSize = static_cast<u32>(wav.Size()) - 8;
        wav[4] = static_cast<byte>(riffSize & 0xFF);
        wav[5] = static_cast<byte>((riffSize >> 8) & 0xFF);
        wav[6] = static_cast<byte>((riffSize >> 16) & 0xFF);
        wav[7] = static_cast<byte>((riffSize >> 24) & 0xFF);
    }

    void RemoveDbTree(StringView dir)
    {
        for (const utf8char* name :
             {u8"clip.rasset", u8"cooked.rasset", u8"cooked.data.bin", u8"tone.wav",
              u8"mixer.rasset", u8"tree.rasset", u8"cyclic.rasset"})
        {
            String path(dir);
            path.Append(u8"/");
            path.Append(name);
            FileDelete(path.AsView());
        }
        RemoveDirectory(dir);
    }
}

TEST_CASE("audio.pipeline: Stream auto-default - long OR large sources stream")
{
    CHECK_FALSE(ShouldStreamAudioByDefault(1.0f, 100 * 1024));
    CHECK(ShouldStreamAudioByDefault(11.0f, 100 * 1024));            // long
    CHECK(ShouldStreamAudioByDefault(1.0f, 3 * 1024 * 1024));        // large
    CHECK_FALSE(ShouldStreamAudioByDefault(10.0f, 2 * 1024 * 1024)); // exactly at the line
}

TEST_CASE("audio.pipeline: WAV smpl loop points parse (and absent/garbage inputs do not)")
{
    Array<byte> wav = MakeToneWav(0.2f);
    u64 loopStart = 0;
    u64 loopEnd = 0;
    CHECK_FALSE(ParseWavSampleLoop(Span<const byte>(wav.Data(), wav.Size()), loopStart, loopEnd));

    AppendSampleLoopChunk(wav, 100, 1500);
    REQUIRE(ParseWavSampleLoop(Span<const byte>(wav.Data(), wav.Size()), loopStart, loopEnd));
    CHECK(loopStart == 100u);
    CHECK(loopEnd == 1500u);

    Array<byte> garbage;
    for (int i = 0; i < 128; ++i)
    {
        garbage.PushBack(static_cast<byte>(i));
    }
    CHECK_FALSE(
        ParseWavSampleLoop(Span<const byte>(garbage.Data(), garbage.Size()), loopStart, loopEnd));
}

TEST_CASE("audio.pipeline: wav -> AudioClipAsset cook -> AudioClip keeps the ORIGINAL "
          "container bytes (write-through, no PCM sidecar)")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_src");
    RemoveDbTree(u8"draconic_audiopipe_out");

    draconic::vfs::NativeFileSystem sourceMount(u8"draconic_audiopipe_src");
    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_out");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    const Array<byte> wav = MakeToneWav(0.25f, 8000, 2);
    REQUIRE(CreateDirectory(u8"draconic_audiopipe_src"));
    REQUIRE(WriteFile(u8"draconic_audiopipe_src/tone.wav", Span<const byte>(wav.Data(), wav.Size()))
                .IsOk());

    AudioClipAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"tone.wav");
    asset.gain = 0.8f;
    asset.loop = true;
    asset.loopStartFrame = 10;
    asset.loopEndFrame = 900;

    AudioClipAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &sourceMount;
    auto* outputInstance =
        outputDb.RootGroup()->CreateInstance(u8"cooked", AudioClipSource::StaticType());
    ctx.output = outputInstance;
    REQUIRE(builder.Build(asset, ctx).IsOk());

    AudioClipFactory factory;
    ResourceManager manager(outputDb);
    manager.AddFactory(&factory);
    Proxy<AudioClip> clip = manager.Bind<AudioClip>(outputInstance->Id());
    REQUIRE(clip);
    CHECK(clip->channels == 2u);
    CHECK(clip->sampleRate == 8000u);
    CHECK(clip->durationSeconds == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(clip->gain == doctest::Approx(0.8f));
    CHECK(clip->loop);
    CHECK(clip->loopStartFrame == 10u);
    CHECK(clip->loopEndFrame == 900u);
    CHECK_FALSE(clip->stream);
    REQUIRE(clip->encodedData.Size() == wav.Size()); // byte-identical write-through
    bool identical = true;
    for (usize i = 0; i < wav.Size(); ++i)
    {
        if (clip->encodedData[i] != wav[i])
        {
            identical = false;
            break;
        }
    }
    CHECK(identical);

    RemoveDbTree(u8"draconic_audiopipe_src");
    RemoveDbTree(u8"draconic_audiopipe_out");
}

TEST_CASE("audio.pipeline: async clip load matches the synchronous product byte-for-byte")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audioasync_src");
    RemoveDbTree(u8"draconic_audioasync_out");

    draconic::vfs::NativeFileSystem sourceMount(u8"draconic_audioasync_src");
    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audioasync_out");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    const Array<byte> wav = MakeToneWav(0.25f, 8000, 2);
    REQUIRE(CreateDirectory(u8"draconic_audioasync_src"));
    REQUIRE(
        WriteFile(u8"draconic_audioasync_src/tone.wav", Span<const byte>(wav.Data(), wav.Size()))
            .IsOk());

    AudioClipAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"tone.wav");
    asset.gain = 0.8f;
    AudioClipAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &sourceMount;
    auto* inst = outputDb.RootGroup()->CreateInstance(u8"cooked", AudioClipSource::StaticType());
    ctx.output = inst;
    REQUIRE(builder.Build(asset, ctx).IsOk());
    const Guid id = inst->Id();

    AudioClipFactory factory;

    ResourceManager syncManager(outputDb);
    syncManager.AddFactory(&factory);
    Proxy<AudioClip> a = syncManager.Bind<AudioClip>(id);
    REQUIRE(a);

    JobSystem jobs;
    ResourceManager asyncManager(outputDb, &jobs);
    asyncManager.AddFactory(&factory);
    Proxy<AudioClip> b = asyncManager.BindAsync<AudioClip>(id);
    asyncManager.WaitAll();
    REQUIRE(b);
    CHECK(b.Handle()->State() == ResourceState::Ready);

    CHECK(b->channels == a->channels);
    CHECK(b->sampleRate == a->sampleRate);
    CHECK(b->gain == doctest::Approx(a->gain));
    CHECK(b->stream == a->stream);
    REQUIRE(b->encodedData.Size() == a->encodedData.Size());
    bool identical = true;
    for (usize i = 0; i < a->encodedData.Size(); ++i)
    {
        if (a->encodedData[i] != b->encodedData[i])
        {
            identical = false;
            break;
        }
    }
    CHECK(identical); // the worker-side ReadData produced the identical container bytes

    RemoveDbTree(u8"draconic_audioasync_src");
    RemoveDbTree(u8"draconic_audioasync_out");
}

TEST_CASE("audio.pipeline: many concurrent async clip decodes run on workers without a race")
{
    // N clips bound async at once means N DecodeStages reading the content DB + registries
    // concurrently on workers. Run under TSAN to validate the concurrent-read safety.
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audioconc_src");
    RemoveDbTree(u8"draconic_audioconc_out");

    draconic::vfs::NativeFileSystem sourceMount(u8"draconic_audioconc_src");
    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audioconc_out");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    const Array<byte> wav = MakeToneWav(0.1f, 8000, 1);
    REQUIRE(CreateDirectory(u8"draconic_audioconc_src"));
    REQUIRE(
        WriteFile(u8"draconic_audioconc_src/tone.wav", Span<const byte>(wav.Data(), wav.Size()))
            .IsOk());

    AudioClipAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"tone.wav");
    AudioClipAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &sourceMount;

    constexpr int kCount = 10;
    Array<Guid> ids;
    for (int i = 0; i < kCount; ++i)
    {
        char8_t name[8] = {u8'c', u8'l', u8'i', u8'p', static_cast<char8_t>(u8'0' + i / 10),
                           static_cast<char8_t>(u8'0' + i % 10), 0};
        auto* inst = outputDb.RootGroup()->CreateInstance(StringView(name),
                                                          AudioClipSource::StaticType());
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
        ids.PushBack(inst->Id());
    }

    AudioClipFactory factory;
    JobSystem jobs;
    ResourceManager manager(outputDb, &jobs);
    manager.AddFactory(&factory);

    Array<Proxy<AudioClip>> clips;
    for (const Guid& id : ids)
    {
        clips.PushBack(manager.BindAsync<AudioClip>(id));
    }
    manager.WaitAll();

    for (Proxy<AudioClip>& clip : clips)
    {
        REQUIRE(clip);
        CHECK(clip.Handle()->State() == ResourceState::Ready);
        CHECK(clip->encodedData.Size() == wav.Size());
    }

    RemoveDbTree(u8"draconic_audioconc_src");
    RemoveDbTree(u8"draconic_audioconc_out");
}

TEST_CASE("audio.pipeline: stream-flagged cooks bind a re-openable content stream source")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_stream_src");
    RemoveDbTree(u8"draconic_audiopipe_stream_out");

    draconic::vfs::NativeFileSystem sourceMount(u8"draconic_audiopipe_stream_src");
    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_stream_out");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    const Array<byte> wav = MakeToneWav(0.5f);
    REQUIRE(CreateDirectory(u8"draconic_audiopipe_stream_src"));
    REQUIRE(WriteFile(u8"draconic_audiopipe_stream_src/tone.wav",
                      Span<const byte>(wav.Data(), wav.Size()))
                .IsOk());

    AudioClipAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"tone.wav");
    asset.stream = true;

    AudioClipAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &sourceMount;
    auto* outputInstance =
        outputDb.RootGroup()->CreateInstance(u8"cooked", AudioClipSource::StaticType());
    ctx.output = outputInstance;
    REQUIRE(builder.Build(asset, ctx).IsOk());

    AudioClipFactory factory;
    ResourceManager manager(outputDb);
    manager.AddFactory(&factory);
    Proxy<AudioClip> clip = manager.Bind<AudioClip>(outputInstance->Id());
    REQUIRE(clip);
    CHECK(clip->stream);
    CHECK(clip->encodedData.IsEmpty()); // no bytes held in memory
    REQUIRE(clip->streamSource.Get() != nullptr);

    // The source re-opens independently and serves the exact container bytes.
    for (int pass = 0; pass < 2; ++pass)
    {
        UniquePtr<IStream> stream = clip->streamSource->OpenStream();
        REQUIRE(stream.Get() != nullptr);
        REQUIRE(stream->Size() == static_cast<i64>(wav.Size()));
        Array<byte> readBack;
        readBack.Resize(wav.Size());
        REQUIRE(stream->Read(readBack.Data(), wav.Size()) == wav.Size());
        CHECK(readBack[40] == wav[40]);
    }

    RemoveDbTree(u8"draconic_audiopipe_stream_src");
    RemoveDbTree(u8"draconic_audiopipe_stream_out");
}

TEST_CASE("audio.pipeline: the builder VALIDATES - undecodable sources fail the cook")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_bad_src");
    RemoveDbTree(u8"draconic_audiopipe_bad_out");

    draconic::vfs::NativeFileSystem sourceMount(u8"draconic_audiopipe_bad_src");
    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_bad_out");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    Array<byte> garbage;
    for (int i = 0; i < 256; ++i)
    {
        garbage.PushBack(static_cast<byte>(i * 3));
    }
    REQUIRE(CreateDirectory(u8"draconic_audiopipe_bad_src"));
    REQUIRE(WriteFile(u8"draconic_audiopipe_bad_src/tone.wav",
                      Span<const byte>(garbage.Data(), garbage.Size()))
                .IsOk());

    AudioClipAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"tone.wav");
    AudioClipAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &sourceMount;
    ctx.output = outputDb.RootGroup()->CreateInstance(u8"cooked", AudioClipSource::StaticType());
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());

    RemoveDbTree(u8"draconic_audiopipe_bad_src");
    RemoveDbTree(u8"draconic_audiopipe_bad_out");
}

TEST_CASE("audio.pipeline: destructive options - force-mono downmixes, trim drops the "
          "silent tail, normalize lifts the peak to -1 dBFS")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_fx_src");
    RemoveDbTree(u8"draconic_audiopipe_fx_out");

    draconic::vfs::NativeFileSystem sourceMount(u8"draconic_audiopipe_fx_src");
    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_fx_out");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    // A quiet stereo tone with half a second of pure silence appended.
    Array<i16> samples = MakeTone(0.25f, 8000, 2, 0.1f);
    const usize toneFrames = samples.Size() / 2;
    for (usize i = 0; i < 8000 / 2 * 2; ++i)
    {
        samples.PushBack(0);
    }
    Array<byte> wav;
    REQUIRE(EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 2, 8000, wav));
    REQUIRE(CreateDirectory(u8"draconic_audiopipe_fx_src"));
    REQUIRE(
        WriteFile(u8"draconic_audiopipe_fx_src/tone.wav", Span<const byte>(wav.Data(), wav.Size()))
            .IsOk());

    AudioClipAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"tone.wav");
    asset.forceMono = true;
    asset.trimTrailingSilence = true;
    asset.normalize = true;

    AudioClipAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &sourceMount;
    auto* outputInstance =
        outputDb.RootGroup()->CreateInstance(u8"cooked", AudioClipSource::StaticType());
    ctx.output = outputInstance;
    REQUIRE(builder.Build(asset, ctx).IsOk());

    AudioClipFactory factory;
    ResourceManager manager(outputDb);
    manager.AddFactory(&factory);
    Proxy<AudioClip> clip = manager.Bind<AudioClip>(outputInstance->Id());
    REQUIRE(clip);
    CHECK(clip->channels == 1u);                       // downmixed
    CHECK(clip->frameCount < toneFrames + 4000u / 2u); // tail trimmed
    CHECK(clip->frameCount >= toneFrames);             // tone kept

    Array<i16> cookedSamples;
    AudioClipMetadata cookedMetadata;
    REQUIRE(DecodeAudioClipToPcm16(clip->EncodedBytes(), 0, cookedSamples, cookedMetadata));
    i32 peak = 0;
    for (i16 sample : cookedSamples)
    {
        const i32 magnitude = sample < 0 ? -static_cast<i32>(sample) : sample;
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    CHECK(peak > 27000); // ~-1 dBFS (was ~3200)

    RemoveDbTree(u8"draconic_audiopipe_fx_src");
    RemoveDbTree(u8"draconic_audiopipe_fx_out");
}

TEST_CASE("audio.pipeline: the file importer creates an AudioClipAsset with probed "
          "defaults (stream auto for long sources, smpl loops detected)")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    const StringView projectDir = u8"draconic_audiopipe_project";
    auto cleanProject = [&]()
    {
        FileDelete(PathJoin(projectDir, u8"Project.xml"));
        FileDelete(PathJoin(projectDir, u8"Sources/short.wav"));
        FileDelete(PathJoin(projectDir, u8"Sources/long.wav"));
        FileDelete(PathJoin(projectDir, u8"Content/draconic_audiopipe_short.xasset"));
        FileDelete(PathJoin(projectDir, u8"Content/draconic_audiopipe_long.xasset"));
        for (StringView sub : {u8"Content", u8"Sources", u8"Cooked", u8"Editor", u8".cache"})
        {
            RemoveDirectory(PathJoin(projectDir, sub));
        }
        RemoveDirectory(projectDir);
    };
    cleanProject();
    REQUIRE(draconic::editor::EditorProject::Create(projectDir, u8"AudioTest").IsOk());
    UniquePtr<draconic::editor::EditorProject> project =
        draconic::editor::EditorProject::Open(projectDir);
    REQUIRE(static_cast<bool>(project));

    AudioFileImporter importer;
    CHECK(importer.Accepts(u8"wav"));
    CHECK(importer.Accepts(u8"ogg"));
    CHECK(importer.Accepts(u8"mp3"));
    CHECK(importer.Accepts(u8"flac"));
    CHECK_FALSE(importer.Accepts(u8"png"));

    // Short SFX with an authored smpl loop: stays in-memory, loop points imported.
    Array<byte> shortWav = MakeToneWav(0.25f, 8000, 1);
    AppendSampleLoopChunk(shortWav, 50, 1900);
    REQUIRE(WriteFile(u8"draconic_audiopipe_short.wav",
                      Span<const byte>(shortWav.Data(), shortWav.Size()))
                .IsOk());
    Result<content::Instance*> shortImport =
        importer.Import(u8"draconic_audiopipe_short.wav", *project,
                        *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(shortImport.HasValue());
    {
        RefPtr<ISerializable> object = shortImport.Value()->ReadObject();
        auto* imported = Cast<AudioClipAsset>(object.Get());
        REQUIRE(imported != nullptr);
        CHECK_FALSE(imported->stream); // small + short = in-memory
        CHECK(imported->loop);         // smpl chunk detected
        CHECK(imported->loopStartFrame == 50u);
        CHECK(imported->loopEndFrame == 1900u);
    }

    // An 11-second source crosses the duration line: stream pre-checks on.
    const Array<byte> longWav = MakeToneWav(11.0f, 8000, 1);
    REQUIRE(
        WriteFile(u8"draconic_audiopipe_long.wav", Span<const byte>(longWav.Data(), longWav.Size()))
            .IsOk());
    Result<content::Instance*> longImport =
        importer.Import(u8"draconic_audiopipe_long.wav", *project, *project->SourceDb().RootGroup(),
                        nullptr, nullptr, nullptr);
    REQUIRE(longImport.HasValue());
    {
        RefPtr<ISerializable> object = longImport.Value()->ReadObject();
        auto* imported = Cast<AudioClipAsset>(object.Get());
        REQUIRE(imported != nullptr);
        CHECK(imported->stream);
    }

    // Garbage is refused before anything lands in the project.
    Array<byte> garbage;
    for (int i = 0; i < 100; ++i)
    {
        garbage.PushBack(static_cast<byte>(i));
    }
    REQUIRE(WriteFile(u8"draconic_audiopipe_garbage.wav",
                      Span<const byte>(garbage.Data(), garbage.Size()))
                .IsOk());
    CHECK_FALSE(importer
                    .Import(u8"draconic_audiopipe_garbage.wav", *project,
                            *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr)
                    .HasValue());

    // Import options object exposes the five toggles.
    RefPtr<draconic::editor::ImportOptions> options = importer.CreateOptions();
    REQUIRE(options.Get() != nullptr);
    CHECK(options->Toggles().Size() == 5u);

    FileDelete(u8"draconic_audiopipe_short.wav");
    FileDelete(u8"draconic_audiopipe_long.wav");
    FileDelete(u8"draconic_audiopipe_garbage.wav");
    FileDelete(PathJoin(projectDir, u8"Sources/draconic_audiopipe_short.wav"));
    FileDelete(PathJoin(projectDir, u8"Sources/draconic_audiopipe_long.wav"));
    FileDelete(PathJoin(projectDir, u8"Content/draconic_audiopipe_short.xasset"));
    FileDelete(PathJoin(projectDir, u8"Content/draconic_audiopipe_long.xasset"));
    cleanProject();
}

TEST_CASE("audio.pipeline: bus layout asset cooks flat fields into the effect-chain wire "
          "and resolves as a resource")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_bus");

    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_bus");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    AudioBusLayoutAsset asset;
    asset.music.volume = 0.5f;
    asset.ui.muted = true;
    asset.effects.lowpassHz = 3000.0f; // slot order: lowpass -> highpass -> delay
    asset.effects.delaySeconds = 0.2f;
    asset.effects.delayDecay = 1.5f; // out of range: the builder clamps to 0.99

    AudioBusLayoutAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    auto* outputInstance =
        outputDb.RootGroup()->CreateInstance(u8"mixer", AudioBusLayoutSource::StaticType());
    ctx.output = outputInstance;
    REQUIRE(builder.Build(asset, ctx).IsOk());

    AudioBusLayoutFactory factory;
    ResourceManager manager(outputDb);
    manager.AddFactory(&factory);
    Proxy<AudioBusLayoutResource> layout =
        manager.Bind<AudioBusLayoutResource>(outputInstance->Id());
    REQUIRE(layout);
    const AudioBusSettings& music = layout->layout.buses[static_cast<usize>(AudioBus::Music)];
    CHECK(music.volume == doctest::Approx(0.5f));
    const AudioBusSettings& ui = layout->layout.buses[static_cast<usize>(AudioBus::UI)];
    CHECK(ui.muted);
    const AudioBusSettings& effects = layout->layout.buses[static_cast<usize>(AudioBus::Effects)];
    REQUIRE(effects.effects.Size() == 2u);
    CHECK(effects.effects[0].kind == AudioBusEffectKind::Lowpass);
    CHECK(effects.effects[0].frequencyHz == doctest::Approx(3000.0f));
    CHECK(effects.effects[1].kind == AudioBusEffectKind::Delay);
    CHECK(effects.effects[1].delayDecay == doctest::Approx(0.99f));

    // The cooked layout applies to a live (headless) engine.
    AudioEngineSettings settings;
    settings.headless = true;
    AudioEngine engine(settings);
    engine.ApplyBusLayout(layout->layout);
    CHECK(engine.BusVolume(AudioBus::Music) == doctest::Approx(0.5f));
    CHECK(engine.BusEffectCount(AudioBus::Effects) == 2u);

    RemoveDbTree(u8"draconic_audiopipe_bus");
}

TEST_CASE("audio.pipeline: custom-bus slots cook into the NAMED wire section and load "
          "back through the resource")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_named");

    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_named");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    AudioBusLayoutAsset asset;
    asset.custom[0].name = String(u8"drums");
    asset.custom[0].parent = String(u8"effects"); // case-insensitive fixed parent
    asset.custom[0].bus.volume = 0.6f;
    asset.custom[0].bus.lowpassHz = 2500.0f;
    asset.custom[2].name = String(u8"quiet");   // sparse slots fold down
    asset.custom[2].parent = String(u8"drums"); // custom-under-custom
    asset.custom[2].bus.muted = true;
    asset.custom[4].name = String(u8"drums"); // duplicate: skipped with a warning
    asset.custom[5].name = String(u8"Music"); // fixed-name shadow: skipped

    AudioBusLayoutAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    auto* outputInstance =
        outputDb.RootGroup()->CreateInstance(u8"tree", AudioBusLayoutSource::StaticType());
    ctx.output = outputInstance;
    REQUIRE(builder.Build(asset, ctx).IsOk());

    AudioBusLayoutFactory factory;
    ResourceManager manager(outputDb);
    manager.AddFactory(&factory);
    Proxy<AudioBusLayoutResource> layout =
        manager.Bind<AudioBusLayoutResource>(outputInstance->Id());
    REQUIRE(layout);
    REQUIRE(layout->layout.customBuses.Size() == 2u);
    CHECK(layout->layout.customBuses[0].name.AsView() == StringView(u8"drums"));
    CHECK(layout->layout.customBuses[0].parent.AsView() == StringView(u8"effects"));
    CHECK(layout->layout.customBuses[0].settings.volume == doctest::Approx(0.6f));
    REQUIRE(layout->layout.customBuses[0].settings.effects.Size() == 1u);
    CHECK(layout->layout.customBuses[0].settings.effects[0].kind == AudioBusEffectKind::Lowpass);
    CHECK(layout->layout.customBuses[1].name.AsView() == StringView(u8"quiet"));
    CHECK(layout->layout.customBuses[1].settings.muted);

    // The cooked tree realizes on a live (headless) engine.
    AudioEngineSettings settings;
    settings.headless = true;
    AudioEngine engine(settings);
    engine.ApplyBusLayout(layout->layout);
    CHECK(engine.NamedBusCount() == 2u);
    CHECK(engine.NamedBusEffectCount(u8"drums") == 1u);
    CHECK(engine.NamedBusMuted(u8"quiet"));

    RemoveDbTree(u8"draconic_audiopipe_named");
}

TEST_CASE("audio.pipeline: a custom-bus parent CYCLE fails the cook")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_cycle");

    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_cycle");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    AudioBusLayoutAsset asset;
    asset.custom[0].name = String(u8"a");
    asset.custom[0].parent = String(u8"b");
    asset.custom[1].name = String(u8"b");
    asset.custom[1].parent = String(u8"a");

    AudioBusLayoutAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    auto* outputInstance =
        outputDb.RootGroup()->CreateInstance(u8"cyclic", AudioBusLayoutSource::StaticType());
    ctx.output = outputInstance;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());

    // Self-parenting is the one-bus cycle.
    AudioBusLayoutAsset selfish;
    selfish.custom[0].name = String(u8"a");
    selfish.custom[0].parent = String(u8"a");
    CHECK_FALSE(builder.Build(selfish, ctx).IsOk());

    // An UNKNOWN parent is not a cycle - it cooks (falls back to Master at apply).
    AudioBusLayoutAsset orphan;
    orphan.custom[0].name = String(u8"a");
    orphan.custom[0].parent = String(u8"ghost");
    CHECK(builder.Build(orphan, ctx).IsOk());

    RemoveDbTree(u8"draconic_audiopipe_cycle");
}

TEST_CASE("audio.pipeline: bus layout wire is version-tolerant - v0 payloads (no named "
          "section) still load, v2 round-trips the custom buses")
{
    // Simulate an OLD cook: write the source under an explicit version-0 scope (the
    // Serialize body then writes the pre-named wire exactly) and read it back.
    AudioBusLayoutSource oldSource;
    oldSource.layout.buses[static_cast<usize>(AudioBus::Music)].volume = 0.4f;
    MemoryStream oldStream;
    {
        BinarySerializer ar(oldStream, SerializeMode::Write);
        SerializedDataVersion v0{AudioBusLayoutSource::StaticType().id, 0u};
        ar.PushVersionScope(&v0, 1);
        oldSource.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    REQUIRE(oldStream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(oldStream, SerializeMode::Read);
        SerializedDataVersion v0{AudioBusLayoutSource::StaticType().id, 0u};
        ar.PushVersionScope(&v0, 1);
        AudioBusLayoutSource loaded;
        loaded.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
        CHECK(loaded.layout.buses[static_cast<usize>(AudioBus::Music)].volume ==
              doctest::Approx(0.4f));
        CHECK(loaded.layout.customBuses.IsEmpty());
    }

    // Current-version write/read carries the named section whole.
    AudioBusLayoutSource newSource;
    AudioNamedBus named;
    named.name = String(u8"drums");
    named.parent = String(u8"Effects");
    named.settings.volume = 0.7f;
    newSource.layout.customBuses.PushBack(named);
    MemoryStream newStream;
    {
        BinarySerializer ar(newStream, SerializeMode::Write);
        BeginVersionedPayload(ar, AudioBusLayoutSource::StaticType());
        newSource.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(newStream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(newStream, SerializeMode::Read);
        BeginVersionedPayload(ar, AudioBusLayoutSource::StaticType());
        CHECK(ar.Version() == 2u);
        AudioBusLayoutSource loaded;
        loaded.Serialize(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
        REQUIRE(loaded.layout.customBuses.Size() == 1u);
        CHECK(loaded.layout.customBuses[0].name.AsView() == StringView(u8"drums"));
        CHECK(loaded.layout.customBuses[0].parent.AsView() == StringView(u8"Effects"));
        CHECK(loaded.layout.customBuses[0].settings.volume == doctest::Approx(0.7f));
    }
}

TEST_CASE("audio.pipeline: sound cue cooks slots -> variants and resolves clip refs")
{
    RegisterAudioResource();
    RegisterAudioAssets();
    RemoveDbTree(u8"draconic_audiopipe_cue");

    draconic::vfs::NativeFileSystem outputMount(u8"draconic_audiopipe_cue");
    content::ContentDatabase outputDb(outputMount, BinarySerializerFactory(), u8".rasset");

    // Two cooked clips the cue references.
    const Array<byte> wav = MakeToneWav(0.1f, 8000, 1);
    Guid clipIds[2];
    for (int i = 0; i < 2; ++i)
    {
        auto* clipInstance = outputDb.RootGroup()->CreateInstance(
            i == 0 ? StringView(u8"stepA") : StringView(u8"stepB"), AudioClipSource::StaticType());
        AudioClipSource clipSource;
        clipSource.channels = 1;
        clipSource.sampleRate = 8000;
        clipSource.durationSeconds = 0.1f;
        REQUIRE(clipInstance->WriteObject(clipSource).IsOk());
        REQUIRE(clipInstance->WriteData(u8"data", Span<const byte>(wav.Data(), wav.Size())).IsOk());
        clipIds[i] = clipInstance->Id();
    }

    SoundCueAsset asset;
    asset.clipIds[0] = clipIds[0];
    asset.weights[0] = 2.0f;
    asset.clipIds[3] = clipIds[1]; // sparse slots fold down
    asset.weights[3] = 1.0f;
    asset.clipIds[5] = clipIds[1];
    asset.weights[5] = 0.0f; // weighted-out slot: warned + dropped
    asset.pitchMin = 1.2f;
    asset.pitchMax = 0.8f; // reversed range: builder normalizes

    SoundCueAssetBuilder builder;
    draconic::editor::AssetBuildContext ctx;
    auto* outputInstance =
        outputDb.RootGroup()->CreateInstance(u8"footsteps", SoundCueSource::StaticType());
    ctx.output = outputInstance;
    REQUIRE(builder.Build(asset, ctx).IsOk());

    AudioClipFactory clipFactory;
    SoundCueFactory cueFactory;
    ResourceManager manager(outputDb);
    manager.AddFactory(&clipFactory);
    manager.AddFactory(&cueFactory);
    Proxy<SoundCue> cue = manager.Bind<SoundCue>(outputInstance->Id());
    REQUIRE(cue);
    REQUIRE(cue->variants.Size() == 2u);
    CHECK(cue->variants[0].weight == doctest::Approx(2.0f));
    CHECK(cue->variants[0].clip.Get() != nullptr);
    CHECK(cue->variants[0].clip->sampleRate == 8000u);
    CHECK(cue->variants[1].clip.Get() != nullptr);
    CHECK(cue->pitchMin == doctest::Approx(0.8f));
    CHECK(cue->pitchMax == doctest::Approx(1.2f));

    // No playable variant fails the cook.
    SoundCueAsset empty;
    auto* emptyInstance =
        outputDb.RootGroup()->CreateInstance(u8"empty", SoundCueSource::StaticType());
    ctx.output = emptyInstance;
    CHECK_FALSE(builder.Build(empty, ctx).IsOk());

    RemoveDbTree(u8"draconic_audiopipe_cue");
}
