#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sds
{
    struct RemoteVoFilesystemCandidate
    {
        std::string label{};
        std::filesystem::path path{};
    };

    struct RemoteVoFilesystemCandidates
    {
        RemoteVoFilesystemCandidate raw{};
        RemoteVoFilesystemCandidate dataRoot{};
    };

    [[nodiscard]] RemoteVoFilesystemCandidates buildRemoteVoFilesystemCandidates(
        std::wstring_view capturedPath,
        const std::filesystem::path& executablePath);

    [[nodiscard]] std::string formatRemoteVoFilesystemProbeContext(
        std::wstring_view capturedPath,
        const std::filesystem::path& executablePath);
}
