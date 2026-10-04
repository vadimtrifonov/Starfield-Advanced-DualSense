#undef NDEBUG
#include <StarfieldDualSense/WwiseWemPayloadProbe.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    void writeU32Le(std::vector<unsigned char>& bytes, std::size_t offset, std::uint32_t value)
    {
        bytes.at(offset + 0) = static_cast<unsigned char>(value & 0xFFU);
        bytes.at(offset + 1) = static_cast<unsigned char>((value >> 8U) & 0xFFU);
        bytes.at(offset + 2) = static_cast<unsigned char>((value >> 16U) & 0xFFU);
        bytes.at(offset + 3) = static_cast<unsigned char>((value >> 24U) & 0xFFU);
    }

    std::vector<unsigned char> makeRiffWavePayload()
    {
        std::vector<unsigned char> payload{
            'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
            'f', 'm', 't', ' ', 4, 0, 0, 0, 1, 2, 3, 4,
            'd', 'a', 't', 'a', 4, 0, 0, 0, 5, 6, 7, 8,
        };
        writeU32Le(payload, 4, static_cast<std::uint32_t>(payload.size() - 8));
        return payload;
    }
}

int main()
{
    const auto tempRoot = std::filesystem::temp_directory_path() / "sds-v0303-wem-payload-test";
    std::error_code cleanupError{};
    std::filesystem::remove_all(tempRoot, cleanupError);
    std::filesystem::create_directories(tempRoot);

    const auto archivePath = tempRoot / "Starfield - Voices01.ba2";
    const auto payload = makeRiffWavePayload();
    constexpr std::uint64_t payloadOffset = 0x100;

    {
        std::ofstream out(archivePath, std::ios::binary);
        std::vector<unsigned char> filler(static_cast<std::size_t>(payloadOffset), 0xCC);
        out.write(reinterpret_cast<const char*>(filler.data()), static_cast<std::streamsize>(filler.size()));
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    sds::VoiceBa2IndexProbeResult index{};
    index.capturedPath = L"Sound\\Voice\\Starfield.esm\\GenericFemaleEvenToned\\00D215A9.wem";
    index.archiveName = "Starfield - Voices01.ba2";
    index.archivePath = archivePath;
    index.openSucceeded = true;
    index.validStarfieldGnrlV2 = true;
    index.targetFound = true;
    index.extension = "wem";
    index.paddingValid = true;
    index.dataOffset = payloadOffset;
    index.packedSize = 0;
    index.unpackedSize = static_cast<std::uint32_t>(payload.size());
    index.compressed = false;

    const auto result = sds::probeVoiceWemPayload(index);
    assert(result.attempted);
    assert(result.openSucceeded);
    assert(result.readSucceeded);
    assert(result.dataOffset == payloadOffset);
    assert(result.expectedSize == payload.size());
    assert(result.bytesRead == payload.size());
    assert(result.payload == payload);
    assert(result.riffWave);
    assert(result.riffDeclaredSize == payload.size() - 8);
    assert(result.riffTotalBytes == payload.size());
    assert(result.riffSizeMatchesPayload);
    assert(result.error.empty());

    const auto line = sds::formatVoiceWemPayloadProbe(result, 16);
    assert(line.find("Voice WEM payload probe:") != std::string::npos);
    assert(line.find("archive=\"Starfield - Voices01.ba2\"") != std::string::npos);
    assert(line.find("offset=256") != std::string::npos);
    assert(line.find("size=36") != std::string::npos);
    assert(line.find("read=success") != std::string::npos);
    assert(line.find("bytesRead=36") != std::string::npos);
    assert(line.find("container=RIFF/WAVE") != std::string::npos);
    assert(line.find("riffSizeMatches=yes") != std::string::npos);
    assert(line.find("prefix=52 49 46 46") != std::string::npos);

    const auto loosePath = tempRoot / "voice.wem";
    {
        std::ofstream out(loosePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    const auto loose = sds::probeVoiceWemPayload(loosePath);
    assert(loose.openSucceeded && loose.readSucceeded);
    assert(loose.dataOffset == 0);
    assert(loose.payload == result.payload);
    assert(loose.riffWave && loose.riffSizeMatchesPayload);
    assert(loose.error.empty());

    std::filesystem::resize_file(loosePath, payload.size() - 1);
    const auto truncatedLoose = sds::probeVoiceWemPayload(loosePath);
    assert(truncatedLoose.openSucceeded && truncatedLoose.readSucceeded);
    assert(truncatedLoose.riffWave && !truncatedLoose.riffSizeMatchesPayload);

    std::filesystem::resize_file(loosePath, 0);
    const auto emptyLoose = sds::probeVoiceWemPayload(loosePath);
    assert(emptyLoose.openSucceeded && !emptyLoose.readSucceeded);
    assert(!emptyLoose.error.empty());

    std::filesystem::remove(loosePath);
    const auto missingLoose = sds::probeVoiceWemPayload(loosePath);
    assert(!missingLoose.openSucceeded && !missingLoose.readSucceeded);
    assert(!missingLoose.error.empty());

    auto wrongExtensionIndex = index;
    wrongExtensionIndex.extension = "txt";
    const auto wrongExtension = sds::probeVoiceWemPayload(wrongExtensionIndex);
    assert(wrongExtension.attempted);
    assert(!wrongExtension.openSucceeded);
    assert(!wrongExtension.readSucceeded);
    assert(wrongExtension.error == "matched BA2 record is not a validated WEM");

    auto badPaddingIndex = index;
    badPaddingIndex.paddingValid = false;
    const auto badPadding = sds::probeVoiceWemPayload(badPaddingIndex);
    assert(badPadding.attempted);
    assert(!badPadding.openSucceeded);
    assert(!badPadding.readSucceeded);
    assert(badPadding.error == "matched BA2 record metadata failed validation");

    auto compressedIndex = index;
    compressedIndex.packedSize = 20;
    compressedIndex.compressed = true;
    const auto compressed = sds::probeVoiceWemPayload(compressedIndex);
    assert(compressed.attempted);
    assert(!compressed.openSucceeded);
    assert(!compressed.readSucceeded);
    assert(compressed.payload.empty());
    assert(compressed.error == "compressed BA2 payload unsupported in v0.3.03");

    auto outOfBoundsIndex = index;
    outOfBoundsIndex.dataOffset = payloadOffset + payload.size() - 4;
    outOfBoundsIndex.unpackedSize = 64;
    const auto outOfBounds = sds::probeVoiceWemPayload(outOfBoundsIndex);
    assert(outOfBounds.attempted);
    assert(outOfBounds.openSucceeded);
    assert(!outOfBounds.readSucceeded);
    assert(outOfBounds.error == "WEM payload extends beyond archive");

    std::filesystem::remove_all(tempRoot, cleanupError);
    return 0;
}
