#include <StarfieldDualSense/RemoteVoAudioPreparation.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    unsigned char decodedMarker = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition) {
            throw std::runtime_error(message);
        }
        std::cout << "PASS " << message << '\n';
    }

    template <class T>
    void put(std::vector<unsigned char>& bytes, std::size_t offset, T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes[offset + i] = static_cast<unsigned char>(value >> (8 * i));
        }
    }

    std::vector<unsigned char> wem(unsigned char marker)
    {
        // Structurally valid Wwise new-0x30 headers and packets; decoding is stubbed below.
        std::vector<unsigned char> bytes(100, 0);
        const auto fourcc = [&](std::size_t at, std::string_view text) {
            std::copy(text.begin(), text.end(), bytes.begin() + at);
        };
        fourcc(0, "RIFF");
        put(bytes, 4, std::uint32_t{92});
        fourcc(8, "WAVE");
        fourcc(12, "fmt ");
        put(bytes, 16, std::uint32_t{0x42});
        put(bytes, 20, std::uint16_t{0xffff});
        put(bytes, 22, std::uint16_t{1});
        put(bytes, 24, std::uint32_t{48000});
        put(bytes, 36, std::uint16_t{0x30});
        put(bytes, 40, std::uint32_t{0x4101});
        put(bytes, 44, std::uint32_t{4});
        put(bytes, 64, std::uint32_t{3}); // audio follows a one-byte setup packet
        bytes[84] = 8;
        bytes[85] = 11;
        fourcc(86, "data");
        put(bytes, 90, std::uint32_t{6});
        bytes[94] = 1;
        bytes[97] = 1;
        bytes[99] = marker;
        return bytes;
    }

    void writeFile(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void writeArchive(const std::filesystem::path& path, std::string_view name, unsigned char marker)
    {
        const auto payload = wem(marker);
        std::vector<unsigned char> bytes(68 + payload.size() + 2 + name.size(), 0);
        std::copy_n("BTDX", 4, bytes.begin());
        put(bytes, 4, std::uint32_t{2});
        std::copy_n("GNRL", 4, bytes.begin() + 8);
        put(bytes, 12, std::uint32_t{1});
        put(bytes, 16, std::uint64_t{68 + payload.size()});
        std::copy_n("wem", 3, bytes.begin() + 36);
        put(bytes, 48, std::uint64_t{68});
        put(bytes, 60, static_cast<std::uint32_t>(payload.size()));
        put(bytes, 64, std::uint32_t{0xbaadf00d});
        std::copy(payload.begin(), payload.end(), bytes.begin() + 68);
        put(bytes, 68 + payload.size(), static_cast<std::uint16_t>(name.size()));
        std::copy(name.begin(), name.end(), bytes.begin() + 70 + payload.size());
        writeFile(path, bytes);
    }
}

namespace sds
{
    // The test exercises source selection and real BA2/WEM parsing, not Vorbis decoding.
    VoiceWwiseVorbisDecodeResult decodeWwiseVorbisToPcm(
        const VoiceWemPayloadProbeResult& payload,
        const VoiceWemStructureProbeResult&,
        const VoiceWwiseVorbisPacketProbeResult&)
    {
        decodedMarker = payload.payload.back();
        VoiceWwiseVorbisDecodeResult result{};
        result.decodeSucceeded = true;
        result.channels = 1;
        result.sampleRate = 48000;
        result.pcm = {0, 1000, -1000, 0};
        return result;
    }

    std::string formatWwiseVorbisDecodeSummary(const VoiceWwiseVorbisDecodeResult&)
    {
        return {};
    }
}

int main() try
{
    const auto root = std::filesystem::temp_directory_path() /
        ("sds-voice-source-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto data = root / "Data";
    constexpr std::string_view name = "sound/voice/starfield.esm/test/voice.wem";
    writeArchive(data / "base.ba2", name, 1);
    writeArchive(data / "patch.ba2", name, 2);
    const auto configure = [&](std::string_view archives) {
        std::ofstream ini(root / "Starfield.ini");
        ini << "[Archive]\nsResourceEnglishVoiceList=" << archives << '\n';
    };
    const auto prepare = [&] {
        decodedMarker = 0;
        return sds::prepareRemoteVoAudio(
            L"sound/voice/starfield.esm/test/voice.wem", 4,
            sds::SpeakerVoiceLanguage::English, root / "Starfield.exe", [](std::string_view) {});
    };

    configure("base.ba2,patch.ba2");
    expect(prepare().prepared && decodedMarker == 2, "later archive wins when both contain the same voice");
    configure("patch.ba2,base.ba2");
    expect(prepare().prepared && decodedMarker == 1, "priority follows list order, not an archive's name");
    configure("base.ba2,patch.ba2");
    {
        std::fstream patch(data / "patch.ba2", std::ios::in | std::ios::out | std::ios::binary);
        patch.seekp(68);
        patch.put('X');
    }
    expect(!prepare().prepared && decodedMarker == 0,
        "invalid winning archive does not expose an older recording");
    writeArchive(data / "patch.ba2", "sound/voice/other.wem", 2);
    configure("base.ba2,patch.ba2");
    expect(prepare().prepared && decodedMarker == 1, "missing higher-priority entry falls back to lower archive");
    writeFile(data / name, wem(3));
    expect(prepare().prepared && decodedMarker == 3, "loose voice overrides archives");
    const auto absoluteVoice = (data / name).wstring();
    decodedMarker = 0;
    expect(sds::prepareRemoteVoAudio(
        absoluteVoice, 4, sds::SpeakerVoiceLanguage::English,
        root / "Starfield.exe", [](std::string_view) {}).prepared && decodedMarker == 3,
        "absolute loose path uses the same preparation");
    writeFile(data / name, {'b', 'a', 'd'});
    expect(!prepare().prepared && decodedMarker == 0, "invalid loose override does not expose an archived voice");
    std::filesystem::remove_all(root);
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
