#pragma once

#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/RemoteVoSpeakerPlayback.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace sds
{
    struct RemoteVoAudioRequest
    {
        std::wstring filePath{};
        std::uint32_t codecId{ 0 };
        SpeakerVoiceLanguage language{ SpeakerVoiceLanguage::English };
        CapturedSoundIdentity identity{};
        std::uint64_t sequence{ 0 };
        std::int64_t captureSteadyMicros{ 0 };
        std::uint32_t originalPlayingId{ 0 };
    };

    struct RemoteVoAudioResult
    {
        RemoteVoAudioRequest request{};
        RemoteVoControllerPlaybackPreparation playback{};
        std::chrono::steady_clock::time_point preparedAt{};
    };

    class ControllerSpeakerManager;
    enum class RemoteVoPlaybackStatus { Skipped, Rejected, Submitted };
    struct RemoteVoPlaybackResult
    {
        RemoteVoPlaybackStatus status{ RemoteVoPlaybackStatus::Skipped };
        bool originalStopped{ false };
    };

    [[nodiscard]] RemoteVoPlaybackResult applyRemoteVoAudio(
        RemoteVoAudioResult& result,
        ControllerSpeakerManager& speaker,
        SpeakerVoiceLanguage language,
        bool enabled,
        bool (*isPlaying)(std::uint32_t),
        bool (*stopPlaying)(std::uint32_t));

    class RemoteVoAudioPipeline
    {
    public:
        using PrepareCallback = std::function<RemoteVoControllerPlaybackPreparation(const RemoteVoAudioRequest&)>;

        explicit RemoteVoAudioPipeline(PrepareCallback prepare);
        ~RemoteVoAudioPipeline();

        void enqueue(RemoteVoAudioRequest request);
        [[nodiscard]] std::optional<RemoteVoAudioResult> tryTake();
        void clear();
        void stop();

    private:
        void run();

        PrepareCallback _prepare;
        std::mutex _mutex;
        std::condition_variable _wake;
        // Remote VO replaces the previous voice; do not build a dialogue backlog.
        std::optional<RemoteVoAudioRequest> _pending;
        std::optional<RemoteVoAudioResult> _ready;
        std::uint64_t _generation{ 0 };
        bool _stopping{ false };
        std::thread _worker;
    };
}
