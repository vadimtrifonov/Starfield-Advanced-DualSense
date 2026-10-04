#pragma once

#include <StarfieldDualSense/WwiseVoiceBa2IndexProbe.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sds
{
    struct VoiceWemPayloadProbeResult
    {
        std::wstring capturedPath{};
        std::string archiveName{};
        std::filesystem::path archivePath{};
        bool attempted{ false };
        bool openSucceeded{ false };
        bool readSucceeded{ false };
        std::uint64_t dataOffset{ 0 };
        std::uint32_t expectedSize{ 0 };
        std::uint64_t bytesRead{ 0 };
        std::vector<unsigned char> payload{};
        bool riffWave{ false };
        std::uint32_t riffDeclaredSize{ 0 };
        std::uint64_t riffTotalBytes{ 0 };
        bool riffSizeMatchesPayload{ false };
        std::string error{};
    };

    [[nodiscard]] VoiceWemPayloadProbeResult probeVoiceWemPayload(
        const VoiceBa2IndexProbeResult& indexResult);

    [[nodiscard]] VoiceWemPayloadProbeResult probeVoiceWemPayload(
        const std::filesystem::path& path);

    [[nodiscard]] std::string formatVoiceWemPayloadProbe(
        const VoiceWemPayloadProbeResult& result,
        std::size_t prefixBytes = 32);
}
