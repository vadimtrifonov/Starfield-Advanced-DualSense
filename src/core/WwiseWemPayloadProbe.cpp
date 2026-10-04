#include <StarfieldDualSense/WwiseWemPayloadProbe.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
    [[nodiscard]] std::uint32_t readU32Le(const unsigned char* bytes)
    {
        return static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
    }

    [[nodiscard]] bool seekAbsolute(std::ifstream& stream, std::uint64_t offset)
    {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return false;
        }
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        return static_cast<bool>(stream);
    }

    [[nodiscard]] std::string narrow(std::wstring_view value)
    {
        std::string out;
        out.reserve(value.size());
        for (const wchar_t ch : value) {
            out.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : '?');
        }
        return out;
    }

    [[nodiscard]] bool hasFourCc(
        const std::vector<unsigned char>& payload,
        std::size_t offset,
        const std::array<unsigned char, 4>& expected)
    {
        return payload.size() >= offset + expected.size() &&
            std::equal(expected.begin(), expected.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    void readPayload(std::ifstream& stream, sds::VoiceWemPayloadProbeResult& result)
    {
        if (!seekAbsolute(stream, result.dataOffset)) {
            result.error = "WEM payload seek failed";
            return;
        }

        result.payload.resize(result.expectedSize);
        stream.read(
            reinterpret_cast<char*>(result.payload.data()),
            static_cast<std::streamsize>(result.payload.size()));
        result.bytesRead = static_cast<std::uint64_t>(stream.gcount());
        if (result.bytesRead != result.expectedSize) {
            result.payload.clear();
            result.error = "WEM payload read truncated";
            return;
        }
        result.readSucceeded = true;

        constexpr std::array<unsigned char, 4> riff{ 'R', 'I', 'F', 'F' };
        constexpr std::array<unsigned char, 4> wave{ 'W', 'A', 'V', 'E' };
        result.riffWave = hasFourCc(result.payload, 0, riff) && hasFourCc(result.payload, 8, wave);
        if (result.riffWave && result.payload.size() >= 12) {
            result.riffDeclaredSize = readU32Le(result.payload.data() + 4);
            result.riffTotalBytes = static_cast<std::uint64_t>(result.riffDeclaredSize) + 8ULL;
            result.riffSizeMatchesPayload = result.riffTotalBytes == result.payload.size();
        }
    }
}

sds::VoiceWemPayloadProbeResult sds::probeVoiceWemPayload(const VoiceBa2IndexProbeResult& indexResult)
{
    VoiceWemPayloadProbeResult result{};
    result.capturedPath = indexResult.capturedPath;
    result.archiveName = indexResult.archiveName;
    result.archivePath = indexResult.archivePath;
    result.attempted = true;
    result.dataOffset = indexResult.dataOffset;
    result.expectedSize = indexResult.unpackedSize;

    if (!indexResult.targetFound) {
        result.error = "matched BA2 record unavailable";
        return result;
    }
    if (!indexResult.openSucceeded || !indexResult.validStarfieldGnrlV2 || !indexResult.paddingValid) {
        result.error = "matched BA2 record metadata failed validation";
        return result;
    }
    if (indexResult.extension != "wem") {
        result.error = "matched BA2 record is not a validated WEM";
        return result;
    }
    if (indexResult.compressed || indexResult.packedSize != 0) {
        result.error = "compressed BA2 payload unsupported in v0.3.03";
        return result;
    }
    if (indexResult.archivePath.empty()) {
        result.error = "archive path unavailable";
        return result;
    }
    if (result.expectedSize == 0) {
        result.error = "WEM payload size unavailable";
        return result;
    }

    std::ifstream stream(indexResult.archivePath, std::ios::binary);
    if (!stream) {
        result.error = "archive open failed";
        return result;
    }
    result.openSucceeded = true;

    std::error_code sizeError{};
    const auto archiveSizeRaw = std::filesystem::file_size(indexResult.archivePath, sizeError);
    if (sizeError || archiveSizeRaw > std::numeric_limits<std::uint64_t>::max()) {
        result.error = "archive size unavailable";
        return result;
    }
    const auto archiveSize = static_cast<std::uint64_t>(archiveSizeRaw);
    if (result.dataOffset > archiveSize ||
        static_cast<std::uint64_t>(result.expectedSize) > archiveSize - result.dataOffset) {
        result.error = "WEM payload extends beyond archive";
        return result;
    }

    readPayload(stream, result);
    return result;
}

sds::VoiceWemPayloadProbeResult sds::probeVoiceWemPayload(const std::filesystem::path& path)
{
    VoiceWemPayloadProbeResult result{};
    result.capturedPath = path.wstring();
    result.attempted = true;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "loose file open failed";
        return result;
    }
    result.openSucceeded = true;

    std::error_code sizeError{};
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize == 0 || fileSize > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "WEM payload size unavailable";
        return result;
    }
    result.expectedSize = static_cast<std::uint32_t>(fileSize);

    readPayload(stream, result);
    return result;
}

std::string sds::formatVoiceWemPayloadProbe(
    const VoiceWemPayloadProbeResult& result,
    std::size_t prefixBytes)
{
    std::ostringstream out;
    out << "Voice WEM payload probe: target=\"" << narrow(result.capturedPath) << "\"";
    if (!result.archiveName.empty()) {
        out << " archive=\"" << result.archiveName << "\"";
    }
    out << " offset=" << result.dataOffset
        << " size=" << result.expectedSize
        << " read=" << (result.readSucceeded ? "success" : "failed")
        << " bytesRead=" << result.bytesRead;

    if (result.readSucceeded) {
        out << " prefix=";
        const auto count = std::min(prefixBytes, result.payload.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (i != 0) {
                out << ' ';
            }
            out << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(result.payload[i]);
        }
        out << std::dec << std::nouppercase << std::setfill(' ')
            << " container=" << (result.riffWave ? "RIFF/WAVE" : "unknown");

        if (result.riffWave) {
            out << " riffDeclaredSize=" << result.riffDeclaredSize
                << " riffTotalBytes=" << result.riffTotalBytes
                << " riffSizeMatches=" << (result.riffSizeMatchesPayload ? "yes" : "no");
        }
    }

    if (!result.error.empty()) {
        out << " error=\"" << result.error << "\"";
    }
    return out.str();
}
