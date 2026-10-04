#include <StarfieldDualSense/RemoteVoOutputPolicy.h>
#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/ControllerSpeakerManager.h>
#include <StarfieldDualSense/RemoteVoAudioPipeline.h>

#include <memory>
#include <string>

#include <cstdio>

namespace
{
    int failures = 0;
    std::string calls;
    bool playing = true;

    void expect(bool condition, const char* message)
    {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    }
    bool isPlaying(std::uint32_t id)
    {
        expect(id == 42, "query the captured playing ID");
        calls += 'l';
        return playing;
    }

    bool stopPlaying(std::uint32_t id)
    {
        expect(id == 42 && calls == "lp", "stop the original only after accepted playback");
        calls += 's';
        return true;
    }

    class Backend final : public sds::IControllerSpeakerBackend
    {
    public:
        bool accepted = true;
        void start() override {}
        void stop() noexcept override {}
        bool active() const noexcept override { return true; }
        bool enqueue(const sds::SpeakerCommand&) noexcept override { return false; }
        bool enqueuePreparedPcm(const sds::PreparedSpeakerPcm&) noexcept override { return false; }
        bool replacePreparedPcm(sds::PreparedSpeakerPcm) noexcept override
        {
            calls += 'p';
            return accepted;
        }
        bool setPersistentPreparedPcm(sds::PersistentPreparedSpeakerPcm) noexcept override { return false; }
        bool clearPersistentPreparedPcm(std::uint64_t, bool) noexcept override { return false; }
    };

    void preparedVoiceApplication()
    {
        using namespace sds;
        struct Case {
            const char* name;
            bool enabled = true;
            bool comms = true;
            bool prepared = true;
            bool alive = true;
            bool accepted = true;
            std::uint32_t playingId = 42;
            SpeakerVoiceLanguage language = SpeakerVoiceLanguage::English;
            SpeakerOutputMode mode = SpeakerOutputMode::ControllerOnly;
            RemoteVoPlaybackStatus status = RemoteVoPlaybackStatus::Submitted;
            const char* trace = "lps";
        };
        const Case cases[]{
            {"accepted ControllerOnly"},
            {.name="Both preserves original", .mode=SpeakerOutputMode::Both, .trace="lp"},
            {.name="rejection preserves original", .accepted=false, .status=RemoteVoPlaybackStatus::Rejected, .trace="lp"},
            {.name="finished or cancelled voice", .alive=false, .status=RemoteVoPlaybackStatus::Skipped, .trace="l"},
            {.name="world blocked", .enabled=false, .status=RemoteVoPlaybackStatus::Skipped, .trace=""},
            {.name="comms disabled", .comms=false, .status=RemoteVoPlaybackStatus::Skipped, .trace=""},
            {.name="preparation failed", .prepared=false, .status=RemoteVoPlaybackStatus::Skipped, .trace=""},
            {.name="missing playing ID", .playingId=0, .status=RemoteVoPlaybackStatus::Skipped, .trace=""},
            {.name="language changed", .language=SpeakerVoiceLanguage::French, .status=RemoteVoPlaybackStatus::Skipped, .trace=""},
        };
        for (const auto& test : cases) {
            auto config = Config::defaults();
            config.controllerSpeaker = true;
            config.speakerComms = test.comms;
            config.speakerOutputMode = test.mode;
            auto backend = std::make_unique<Backend>();
            backend->accepted = test.accepted;
            ControllerSpeakerManager manager(config, std::move(backend));
            manager.start();
            RemoteVoAudioResult result{};
            result.request.originalPlayingId = test.playingId;
            result.playback.prepared = test.prepared;
            result.playback.pcm.frames.push_back({0.1F, 0.1F});
            playing = test.alive;
            calls.clear();
            const auto applied = applyRemoteVoAudio(
                result, manager, test.language, test.enabled, isPlaying, stopPlaying);
            expect(applied.status == test.status && calls == test.trace &&
                applied.originalStopped == (calls == "lps"), test.name);
        }
    }
}

int main()
{
    using sds::RemoteVoOriginalOutputAction;
    using sds::SpeakerOutputMode;

    expect(
        sds::decideRemoteVoOriginalOutput(SpeakerOutputMode::Both, true, 42) ==
            RemoteVoOriginalOutputAction::KeepOriginal,
        "Both keeps Starfield original audio after accepted controller playback");

    expect(
        sds::decideRemoteVoOriginalOutput(SpeakerOutputMode::ControllerOnly, true, 42) ==
            RemoteVoOriginalOutputAction::StopOriginal,
        "ControllerOnly stops Starfield original audio after accepted controller playback");

    expect(
        sds::decideRemoteVoOriginalOutput(SpeakerOutputMode::ControllerOnly, false, 42) ==
            RemoteVoOriginalOutputAction::KeepOriginal,
        "ControllerOnly keeps Starfield original audio when controller submission fails");

    expect(
        sds::decideRemoteVoOriginalOutput(SpeakerOutputMode::ControllerOnly, true, 0) ==
            RemoteVoOriginalOutputAction::KeepOriginal,
        "ControllerOnly keeps Starfield original audio when there is no valid playing ID to stop");

    preparedVoiceApplication();
    return failures == 0 ? 0 : 1;
}
