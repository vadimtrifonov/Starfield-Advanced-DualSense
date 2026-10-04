#pragma once

#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/RemoteVoSpeakerPlayback.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

namespace sds
{
    [[nodiscard]] RemoteVoControllerPlaybackPreparation prepareRemoteVoAudio(
        std::wstring_view filePath,
        std::uint32_t codecId,
        SpeakerVoiceLanguage language,
        const std::filesystem::path& executablePath,
        const std::function<void(std::string_view)>& log);
}
