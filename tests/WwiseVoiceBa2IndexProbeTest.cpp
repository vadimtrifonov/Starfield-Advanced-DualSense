#undef NDEBUG
#include <StarfieldDualSense/WwiseVoiceBa2IndexProbe.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    void writeU16Le(std::ofstream& out, std::uint16_t value)
    {
        const std::array<char, 2> bytes{
            static_cast<char>(value & 0xFFU),
            static_cast<char>((value >> 8U) & 0xFFU),
        };
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void writeU32Le(std::ofstream& out, std::uint32_t value)
    {
        const std::array<char, 4> bytes{
            static_cast<char>(value & 0xFFU),
            static_cast<char>((value >> 8U) & 0xFFU),
            static_cast<char>((value >> 16U) & 0xFFU),
            static_cast<char>((value >> 24U) & 0xFFU),
        };
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void writeU64Le(std::ofstream& out, std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            out.put(static_cast<char>((value >> shift) & 0xFFU));
        }
    }

    struct Record
    {
        std::uint32_t nameHash{};
        std::array<char, 4> extension{};
        std::uint32_t directoryHash{};
        std::uint32_t flags{};
        std::uint64_t offset{};
        std::uint32_t packedSize{};
        std::uint32_t unpackedSize{};
        std::uint32_t padding{};
    };

    void writeRecord(std::ofstream& out, const Record& record)
    {
        writeU32Le(out, record.nameHash);
        out.write(record.extension.data(), static_cast<std::streamsize>(record.extension.size()));
        writeU32Le(out, record.directoryHash);
        writeU32Le(out, record.flags);
        writeU64Le(out, record.offset);
        writeU32Le(out, record.packedSize);
        writeU32Le(out, record.unpackedSize);
        writeU32Le(out, record.padding);
    }

    void writeName(std::ofstream& out, std::string_view name)
    {
        assert(name.size() <= 0xFFFFU);
        writeU16Le(out, static_cast<std::uint16_t>(name.size()));
        out.write(name.data(), static_cast<std::streamsize>(name.size()));
    }

    void writeSyntheticVoiceBa2(const std::filesystem::path& path)
    {
        constexpr std::uint32_t fileCount = 3;
        constexpr std::uint64_t headerSize = 32;
        constexpr std::uint64_t recordSize = 36;
        constexpr std::uint64_t nameTableOffset = headerSize + (fileCount * recordSize);

        std::ofstream out(path, std::ios::binary);
        out.write("BTDX", 4);
        writeU32Le(out, 2);
        out.write("GNRL", 4);
        writeU32Le(out, fileCount);
        writeU64Le(out, nameTableOffset);
        writeU32Le(out, 1); // Starfield v2 extra header field observed in shipped archives.
        writeU32Le(out, 0);

        writeRecord(out, Record{ 0x11111111U, { 'w', 'e', 'm', '\0' }, 0xAAAA0001U, 0, 0x0000000012345678ULL, 0, 400, 0xBAADF00DU });
        writeRecord(out, Record{ 0x22222222U, { 'w', 'e', 'm', '\0' }, 0xAAAA0002U, 0x10U, 0x0000000100001234ULL, 321, 654, 0xBAADF00DU });
        writeRecord(out, Record{ 0x33333333U, { 'w', 'e', 'm', '\0' }, 0xAAAA0003U, 0, 0x0000000023456789ULL, 0, 777, 0xBAADF00DU });

        writeName(out, "sound/voice/starfield.esm/genericfemaleeventoned/not-this-one.wem");
        writeName(out, "SOUND/VOICE/STARFIELD.ESM/GENERICFEMALEEVENTONED/007CAD96.WEM");
        writeName(out, "sound\\voice\\starfield.esm\\genericfemaleeventoned\\also-not-this.wem");
    }
}

int main()
{
    const auto tempRoot = std::filesystem::temp_directory_path() / "sds-v0302-voice-ba2-index-test";
    std::error_code cleanupError{};
    std::filesystem::remove_all(tempRoot, cleanupError);
    std::filesystem::create_directories(tempRoot);

    const auto archivePath = tempRoot / "Starfield - Voices01.ba2";
    writeSyntheticVoiceBa2(archivePath);

    sds::VoiceArchiveManifestEntry archive{};
    archive.archiveName = "Starfield - Voices01.ba2";
    archive.path = archivePath;
    archive.exists = true;
    archive.openSucceeded = true;
    archive.magic = "BTDX";
    archive.version = 2;
    archive.type = "GNRL";
    archive.validBtdxHeader = true;

    const std::wstring captured = L"Sound\\Voice\\Starfield.esm\\GenericFemaleEvenToned\\007CAD96.wem";
    const auto result = sds::probeVoiceBa2Index(captured, archive);

    assert(result.attempted);
    assert(result.openSucceeded);
    assert(result.validStarfieldGnrlV2);
    assert(result.version == 2);
    assert(result.fileCount == 3);
    assert(result.nameTableOffset == 140);
    assert(result.targetFound);
    assert(result.recordIndex == 1);
    assert(result.matchedName == "SOUND/VOICE/STARFIELD.ESM/GENERICFEMALEEVENTONED/007CAD96.WEM");
    assert(result.nameHash == 0x22222222U);
    assert(result.extension == "wem");
    assert(result.directoryHash == 0xAAAA0002U);
    assert(result.flags == 0x10U);
    assert(result.dataOffset == 0x0000000100001234ULL); // proves the parser keeps 64-bit BA2 offsets
    assert(result.packedSize == 321);
    assert(result.unpackedSize == 654);
    assert(result.compressed);
    assert(result.padding == 0xBAADF00DU);
    assert(result.paddingValid);

    const auto first = sds::probeVoiceBa2Index(
        L"Sound\\Voice\\Starfield.esm\\GenericFemaleEvenToned\\not-this-one.wem", archive);
    assert(first.targetFound && first.recordIndex == 0);
    assert(first.nameHash == 0x11111111U);

    const auto last = sds::probeVoiceBa2Index(
        L"SOUND/VOICE/STARFIELD.ESM/GENERICFEMALEEVENTONED/ALSO-NOT-THIS.WEM", archive);
    assert(last.targetFound && last.recordIndex == 2);
    assert(last.nameHash == 0x33333333U);

    const auto line = sds::formatVoiceBa2IndexProbe(result);
    assert(line.find("Voice BA2 index probe:") != std::string::npos);
    assert(line.find("archive=\"Starfield - Voices01.ba2\"") != std::string::npos);
    assert(line.find("match=yes") != std::string::npos);
    assert(line.find("recordIndex=1") != std::string::npos);
    assert(line.find("offset=4294971956") != std::string::npos);
    assert(line.find("packedSize=321") != std::string::npos);
    assert(line.find("unpackedSize=654") != std::string::npos);
    assert(line.find("compressed=yes") != std::string::npos);
    assert(line.find("extension=wem") != std::string::npos);
    assert(line.find("padding=0xBAADF00D") != std::string::npos);

    archive.archiveName = "Starfield - Voices02.ba2";
    archive.path = archivePath;
    const auto miss = sds::probeVoiceBa2Index(L"Sound\\Voice\\Starfield.esm\\Nope\\DEADBEEF.wem", archive);
    assert(miss.attempted);
    assert(miss.openSucceeded);
    assert(miss.validStarfieldGnrlV2);
    assert(!miss.targetFound);
    assert(miss.error.empty());
    const auto missLine = sds::formatVoiceBa2IndexProbe(miss);
    assert(missLine.find("match=no") != std::string::npos);

    std::filesystem::resize_file(archivePath, result.nameTableOffset - 1);
    const auto truncated = sds::probeVoiceBa2Index(captured, archive);
    assert(!truncated.targetFound);
    assert(truncated.error == "BA2 index extends beyond archive");

    std::filesystem::remove_all(tempRoot, cleanupError);
    return 0;
}
