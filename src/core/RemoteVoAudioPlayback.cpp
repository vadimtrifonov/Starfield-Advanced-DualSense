#include <StarfieldDualSense/RemoteVoAudioPipeline.h>

#include <StarfieldDualSense/ControllerSpeakerManager.h>
#include <StarfieldDualSense/RemoteVoOutputPolicy.h>

#include <utility>

sds::RemoteVoPlaybackResult sds::applyRemoteVoAudio(
    RemoteVoAudioResult& result,
    ControllerSpeakerManager& speaker,
    SpeakerVoiceLanguage language,
    bool enabled,
    bool (*isPlaying)(std::uint32_t),
    bool (*stopPlaying)(std::uint32_t))
{
    const auto& request = result.request;
    if (!enabled || request.language != language || !result.playback.prepared ||
        !speaker.categoryEnabled(SpeakerCategory::Comms) ||
        request.originalPlayingId == 0 || !isPlaying(request.originalPlayingId)) {
        return {};
    }

    if (!speaker.submitCaptured(
            std::move(result.playback.pcm), SpeakerCategory::Comms, request.identity, true)) {
        return { RemoteVoPlaybackStatus::Rejected };
    }

    const bool stopOriginal = decideRemoteVoOriginalOutput(
        speaker.outputMode(), true, request.originalPlayingId) == RemoteVoOriginalOutputAction::StopOriginal;
    return { RemoteVoPlaybackStatus::Submitted,
        stopOriginal && stopPlaying(request.originalPlayingId) };
}
