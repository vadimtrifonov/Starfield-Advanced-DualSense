#include <StarfieldDualSense/WwiseVoiceBa2IndexProbe.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace
{
    constexpr std::uint64_t kStarfieldV2HeaderSize = 32;
    constexpr std::uint64_t kGnrlRecordSize = 36;
    constexpr std::uint32_t kExpectedPadding = 0xBAADF00D;

    [[nodiscard]] std::uint16_t readU16Le(const unsigned char* bytes)
    {
        return static_cast<std::uint16_t>(bytes[0]) |
            (static_cast<std::uint16_t>(bytes[1]) << 8U);
    }

    [[nodiscard]] std::uint32_t readU32Le(const unsigned char* bytes)
    {
        return static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[3]) << 24U);
    }

    [[nodiscard]] std::uint64_t readU64Le(const unsigned char* bytes)
    {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(bytes[shift / 8]) << shift;
        }
        return value;
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

    [[nodiscard]] bool archivePathsEqual(std::string_view left, std::string_view right)
    {
        return std::equal(left.begin(), left.end(), right.begin(), right.end(),
            [](unsigned char a, unsigned char b) {
                return (a == '/' ? '\\' : std::tolower(a)) ==
                    (b == '/' ? '\\' : std::tolower(b));
            });
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

    [[nodiscard]] bool readExact(std::ifstream& stream, void* destination, std::size_t size)
    {
        stream.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(size));
        return stream.gcount() == static_cast<std::streamsize>(size);
    }

    [[nodiscard]] std::string extensionFromRecord(const unsigned char* bytes)
    {
        std::string extension;
        for (std::size_t i = 0; i < 4 && bytes[i] != 0; ++i) {
            extension.push_back(static_cast<char>(bytes[i]));
        }
        return extension;
    }

    void parseMatchedRecord(
        const std::array<unsigned char, kGnrlRecordSize>& record,
        sds::VoiceBa2IndexProbeResult& result)
    {
        result.nameHash = readU32Le(record.data());
        result.extension = extensionFromRecord(record.data() + 4);
        result.directoryHash = readU32Le(record.data() + 8);
        result.flags = readU32Le(record.data() + 12);
        result.dataOffset = readU64Le(record.data() + 16);
        result.packedSize = readU32Le(record.data() + 24);
        result.unpackedSize = readU32Le(record.data() + 28);
        result.compressed = result.packedSize != 0;
        result.padding = readU32Le(record.data() + 32);
        result.paddingValid = result.padding == kExpectedPadding;
    }
}

sds::VoiceBa2IndexProbeResult sds::probeVoiceBa2Index(
    std::wstring_view capturedPath,
    const VoiceArchiveManifestEntry& archive)
{
    VoiceBa2IndexProbeResult result{};
    result.capturedPath = std::wstring(capturedPath);
    result.archiveName = archive.archiveName;
    result.archivePath = archive.path;
    result.attempted = true;

    if (capturedPath.empty()) {
        result.error = "captured path unavailable";
        return result;
    }
    if (archive.path.empty()) {
        result.error = "archive path unavailable";
        return result;
    }

    std::ifstream stream(archive.path, std::ios::binary);
    if (!stream) {
        result.error = "open failed";
        return result;
    }
    result.openSucceeded = true;

    std::array<unsigned char, kStarfieldV2HeaderSize> header{};
    if (!readExact(stream, header.data(), header.size())) {
        result.error = "header truncated";
        return result;
    }

    const std::string magic(reinterpret_cast<const char*>(header.data()), 4);
    result.version = readU32Le(header.data() + 4);
    const std::string type(reinterpret_cast<const char*>(header.data() + 8), 4);
    result.fileCount = readU32Le(header.data() + 12);
    result.nameTableOffset = readU64Le(header.data() + 16);

    result.validStarfieldGnrlV2 = magic == "BTDX" && result.version == 2 && type == "GNRL";
    if (!result.validStarfieldGnrlV2) {
        result.error = "unsupported BA2 header (expected BTDX v2 GNRL)";
        return result;
    }

    const std::uint64_t recordTableBytes = static_cast<std::uint64_t>(result.fileCount) * kGnrlRecordSize;
    const std::uint64_t recordTableEnd = kStarfieldV2HeaderSize + recordTableBytes;
    if (result.nameTableOffset < recordTableEnd) {
        result.error = "name table overlaps GNRL record table";
        return result;
    }

    std::error_code sizeError{};
    const auto archiveSizeRaw = std::filesystem::file_size(archive.path, sizeError);
    if (sizeError || archiveSizeRaw > std::numeric_limits<std::uint64_t>::max()) {
        result.error = "archive size unavailable";
        return result;
    }
    const auto archiveSize = static_cast<std::uint64_t>(archiveSizeRaw);
    if (recordTableEnd > archiveSize || result.nameTableOffset > archiveSize) {
        result.error = "BA2 index extends beyond archive";
        return result;
    }

    if (!seekAbsolute(stream, result.nameTableOffset)) {
        result.error = "name table seek failed";
        return result;
    }

    const auto target = narrow(capturedPath);
    std::string name;
    for (std::uint32_t index = 0; index < result.fileCount; ++index) {
        std::array<unsigned char, 2> lengthBytes{};
        if (!readExact(stream, lengthBytes.data(), lengthBytes.size())) {
            result.error = "name table length truncated";
            return result;
        }

        const auto nameLength = readU16Le(lengthBytes.data());
        name.resize(nameLength);
        if (nameLength != 0 && !readExact(stream, name.data(), name.size())) {
            result.error = "name table entry truncated";
            return result;
        }

        if (!archivePathsEqual(name, target)) {
            continue;
        }

        result.targetFound = true;
        result.recordIndex = index;
        result.matchedName = std::move(name);

        std::array<unsigned char, kGnrlRecordSize> record{};
        const auto recordOffset = kStarfieldV2HeaderSize + (static_cast<std::uint64_t>(index) * kGnrlRecordSize);
        if (!seekAbsolute(stream, recordOffset) || !readExact(stream, record.data(), record.size())) {
            result.error = "matched GNRL record read failed";
            result.targetFound = false;
            return result;
        }
        parseMatchedRecord(record, result);
        return result;
    }

    return result;
}

std::string sds::formatVoiceBa2IndexProbe(const VoiceBa2IndexProbeResult& result)
{
    std::ostringstream out;
    out << "Voice BA2 index probe: target=\"" << narrow(result.capturedPath) << "\""
        << " archive=\"" << result.archiveName << "\""
        << " open=" << (result.openSucceeded ? "success" : "failed")
        << " version=" << result.version
        << " fileCount=" << result.fileCount
        << " nameTableOffset=" << result.nameTableOffset
        << " match=" << (result.targetFound ? "yes" : "no");

    if (result.targetFound) {
        out << " recordIndex=" << result.recordIndex
            << " offset=" << result.dataOffset
            << " packedSize=" << result.packedSize
            << " unpackedSize=" << result.unpackedSize
            << " compressed=" << (result.compressed ? "yes" : "no")
            << " extension=" << result.extension
            << " nameHash=0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << result.nameHash
            << " directoryHash=0x" << std::setw(8) << result.directoryHash
            << " flags=0x" << std::setw(8) << result.flags
            << " padding=0x" << std::setw(8) << result.padding
            << std::dec << std::nouppercase << std::setfill(' ')
            << " paddingValid=" << (result.paddingValid ? "yes" : "no")
            << " matchedName=\"" << result.matchedName << "\"";
    }
    if (!result.error.empty()) {
        out << " error=\"" << result.error << "\"";
    }
    return out.str();
}
