#undef NDEBUG
#include <StarfieldDualSense/WwiseWemVorbisDecode.h>
#include <StarfieldDualSense/WwiseVorbisRebuild.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <span>
#include <string>
#include <vector>

namespace
{
    struct TestBitWriter
    {
        std::vector<unsigned char> bytes{};
        std::size_t bitCount{ 0 };

        void put(std::uint64_t value, unsigned bits)
        {
            for (unsigned i = 0; i < bits; ++i) {
                const auto bit = static_cast<unsigned>((value >> i) & 1u);
                const auto byteIndex = bitCount / 8u;
                const auto bitIndex = bitCount % 8u;
                if (byteIndex == bytes.size()) {
                    bytes.push_back(0u);
                }
                bytes[byteIndex] = static_cast<unsigned char>(bytes[byteIndex] | (bit << bitIndex));
                ++bitCount;
            }
        }
    };

    void put16(std::vector<unsigned char>& bytes, std::uint16_t value)
    {
        bytes.push_back(static_cast<unsigned char>(value & 0xFFu));
        bytes.push_back(static_cast<unsigned char>((value >> 8u) & 0xFFu));
    }

    void put32(std::vector<unsigned char>& bytes, std::uint32_t value)
    {
        bytes.push_back(static_cast<unsigned char>(value & 0xFFu));
        bytes.push_back(static_cast<unsigned char>((value >> 8u) & 0xFFu));
        bytes.push_back(static_cast<unsigned char>((value >> 16u) & 0xFFu));
        bytes.push_back(static_cast<unsigned char>((value >> 24u) & 0xFFu));
    }

    std::vector<unsigned char> makePackedCodebookLibrary()
    {
        TestBitWriter packed;
        packed.put(1u, 4u);   // dimensions
        packed.put(1u, 14u);  // entries
        packed.put(1u, 1u);   // ordered
        packed.put(0u, 5u);   // initial length - 1
        packed.put(1u, 1u);   // one entry at this length
        packed.put(0u, 1u);   // lookup type 0
        assert(packed.bytes.size() == 4u);

        std::vector<unsigned char> library = packed.bytes;
        put32(library, 0u); // codebook 0 offset
        put32(library, 4u); // sentinel + offset-table location
        return library;
    }

    std::vector<unsigned char> makeStrippedSetup()
    {
        TestBitWriter b;
        b.put(0u, 8u);   // codebook_count_minus_1 => one codebook
        b.put(0u, 10u);  // external codebook id 0

        b.put(0u, 6u);   // floor_count_minus_1 => one floor
        b.put(0u, 5u);   // floor1 partitions
        b.put(0u, 3u);   // class dimensions minus 1
        b.put(0u, 2u);   // class subclasses
        b.put(0u, 8u);   // subclass book plus 1 => unused
        b.put(0u, 2u);   // floor multiplier minus 1
        b.put(0u, 4u);   // range bits

        b.put(0u, 6u);   // residue_count_minus_1 => one residue
        b.put(0u, 2u);   // residue type
        b.put(0u, 24u);  // residue begin
        b.put(0u, 24u);  // residue end
        b.put(0u, 24u);  // residue partition size minus 1
        b.put(0u, 6u);   // residue classifications minus 1
        b.put(0u, 8u);   // residue classbook
        b.put(0u, 3u);   // residue low cascade bits
        b.put(0u, 1u);   // no high cascade bits

        b.put(0u, 6u);   // mapping_count_minus_1 => one mapping
        b.put(0u, 1u);   // no submaps
        b.put(0u, 1u);   // no coupling
        b.put(0u, 2u);   // mapping reserved
        b.put(0u, 8u);   // time config
        b.put(0u, 8u);   // floor number
        b.put(0u, 8u);   // residue number

        b.put(1u, 6u);   // mode_count_minus_1 => two modes
        b.put(0u, 1u);   // mode 0 short block
        b.put(0u, 8u);   // mode 0 mapping
        b.put(1u, 1u);   // mode 1 long block
        b.put(0u, 8u);   // mode 1 mapping
        return b.bytes;
    }

    struct Fixture
    {
        sds::VoiceWemPayloadProbeResult payload{};
        sds::VoiceWemStructureProbeResult structure{};
        sds::VoiceWwiseVorbisPacketProbeResult packetProbe{};
        std::vector<unsigned char> codebooks{ makePackedCodebookLibrary() };
    };

    Fixture makeFixture()
    {
        Fixture fixture{};
        const auto setup = makeStrippedSetup();

        std::vector<unsigned char> data;
        put16(data, static_cast<std::uint16_t>(setup.size()));
        data.insert(data.end(), setup.begin(), setup.end());
        const auto audioOffset = static_cast<std::uint32_t>(data.size());

        // Wwise modified packets: low mode bit is mode number.
        put16(data, 2u);
        data.push_back(0b10101010u); // mode 0
        data.push_back(0x55u);
        put16(data, 2u);
        data.push_back(0b11001101u); // mode 1
        data.push_back(0x33u);
        put16(data, 2u);
        data.push_back(0b01110000u); // mode 0
        data.push_back(0x77u);

        fixture.payload.attempted = true;
        fixture.payload.readSucceeded = true;
        fixture.payload.riffWave = true;
        fixture.payload.riffSizeMatchesPayload = true;
        fixture.payload.payload = data;

        fixture.structure.attempted = true;
        fixture.structure.validRiffWave = true;
        fixture.structure.scanComplete = true;
        fixture.structure.fmtFound = true;
        fixture.structure.formatTag = 0xFFFFu;
        fixture.structure.channels = 1u;
        fixture.structure.sampleRate = 44100u;
        fixture.structure.averageBytesPerSecond = 7606u;
        fixture.structure.fmtExtraDeclaredSize = 0x30u;
        fixture.structure.dataFound = true;
        fixture.structure.dataOffset = 0u;
        fixture.structure.dataSize = static_cast<std::uint32_t>(data.size());

        fixture.packetProbe.attempted = true;
        fixture.packetProbe.recognizedNewFmt30 = true;
        fixture.packetProbe.probeComplete = true;
        fixture.packetProbe.variant = "new-0x30";
        fixture.packetProbe.channels = 1u;
        fixture.packetProbe.sampleRate = 44100u;
        fixture.packetProbe.numSamples = 12u;
        fixture.packetProbe.durationMs = 0u;
        fixture.packetProbe.setupOffset = 0u;
        fixture.packetProbe.audioOffset = audioOffset;
        fixture.packetProbe.blockExpSmall = 8u;
        fixture.packetProbe.blockExpLarge = 11u;
        fixture.packetProbe.packetHeaderBytes = 2u;
        fixture.packetProbe.modifiedPackets = true;
        fixture.packetProbe.packetMode = "modified";
        fixture.packetProbe.dataOffset = 0u;
        fixture.packetProbe.dataSize = static_cast<std::uint32_t>(data.size());
        fixture.packetProbe.setupEndsAtAudio = true;
        return fixture;
    }

    bool fakeDecode(
        std::span<const unsigned char> ogg,
        std::uint16_t& channels,
        std::uint32_t& sampleRate,
        std::vector<std::int16_t>& pcm,
        std::string& error)
    {
        assert(ogg.size() > 32u);
        assert(ogg[0] == 'O' && ogg[1] == 'g' && ogg[2] == 'g' && ogg[3] == 'S');
        channels = 1u;
        sampleRate = 44100u;
        pcm = { 0, 16384, -32768, 8192, -8192, 4096, -4096, 2048, -2048, 1024, -1024, 0 };
        error.clear();
        return true;
    }
}

int main()
{
    const auto codebooks = makePackedCodebookLibrary();
    const auto setup = makeStrippedSetup();

    const auto setupResult = sds::rebuildWwiseVorbisSetupPacket(setup, codebooks, 1u);
    assert(setupResult.success);
    assert(setupResult.modeBits == 1u);
    assert(setupResult.modeBlockFlags.size() == 2u);
    assert(!setupResult.modeBlockFlags[0]);
    assert(setupResult.modeBlockFlags[1]);
    assert(setupResult.packet.size() > setup.size());
    assert(setupResult.packet[0] == 5u);
    assert(std::string(setupResult.packet.begin() + 1, setupResult.packet.begin() + 7) == "vorbis");

    const std::vector<unsigned char> modifiedInput{ 0b11001101u, 0x33u };
    const std::vector<bool> modes{ false, true };
    const auto rebuiltAudio = sds::rebuildWwiseVorbisAudioPacket(modifiedInput, modes, false, false);
    assert(rebuiltAudio.success);
    assert(rebuiltAudio.modeNumber == 1u);
    assert(rebuiltAudio.blockFlag);
    assert(!rebuiltAudio.packet.empty());
    // Standard Vorbis audio packets always begin with packet-type bit 0.
    assert((rebuiltAudio.packet[0] & 0x01u) == 0u);

    const auto fixture = makeFixture();
    const auto rebuilt = sds::rebuildWwiseVorbisOgg(
        fixture.payload,
        fixture.structure,
        fixture.packetProbe,
        fixture.codebooks);
    assert(rebuilt.attempted);
    assert(rebuilt.success);
    assert(rebuilt.audioPacketCount == 3u);
    assert(rebuilt.modeBits == 1u);
    assert(rebuilt.ogg.size() > 64u);
    assert(rebuilt.ogg[0] == 'O' && rebuilt.ogg[1] == 'g' && rebuilt.ogg[2] == 'g' && rebuilt.ogg[3] == 'S');
    assert(rebuilt.error.empty());

    const auto decoded = sds::decodeWwiseVorbisToPcmWithBackend(
        fixture.payload,
        fixture.structure,
        fixture.packetProbe,
        fixture.codebooks,
        fakeDecode);
    assert(decoded.attempted);
    assert(decoded.reconstructed);
    assert(decoded.decodeSucceeded);
    assert(decoded.channels == 1u);
    assert(decoded.sampleRate == 44100u);
    assert(decoded.expectedSamples == 12u);
    assert(decoded.decodedSamples == 12u);
    assert(decoded.sampleCountMatches);
    assert(decoded.pcm.size() == 12u);
    assert(decoded.pcmBytes == 24u);
    assert(decoded.firstSample == 0);
    assert(decoded.lastSample == 0);
    assert(std::abs(decoded.peak - 1.0f) < 0.0001f);
    assert(decoded.rms > 0.0f && decoded.rms < 1.0f);
    assert(decoded.error.empty());

    const auto summary = sds::formatWwiseVorbisDecodeSummary(decoded);
    assert(summary.find("Voice Wwise Vorbis decode:") != std::string::npos);
    assert(summary.find("status=success") != std::string::npos);
    assert(summary.find("channels=1") != std::string::npos);
    assert(summary.find("sampleRate=44100") != std::string::npos);
    assert(summary.find("expectedSamples=12") != std::string::npos);
    assert(summary.find("decodedSamples=12") != std::string::npos);
    assert(summary.find("sampleCountMatches=yes") != std::string::npos);

    // Exercise the production stb backend concurrently, not just the injected decoder.
    const auto reference = sds::decodeWwiseVorbisToPcm(
        fixture.payload, fixture.structure, fixture.packetProbe);
    assert(reference.reconstructed && reference.backend == "stb_vorbis");
    std::vector<std::future<bool>> concurrent;
    for (unsigned thread = 0; thread < 4; ++thread) {
        concurrent.push_back(std::async(std::launch::async, [&] {
            for (unsigned iteration = 0; iteration < 32; ++iteration) {
                const auto result = sds::decodeWwiseVorbisToPcm(
                    fixture.payload, fixture.structure, fixture.packetProbe);
                if (result.decodeSucceeded != reference.decodeSucceeded ||
                    result.error != reference.error || result.pcm != reference.pcm ||
                    result.channels != reference.channels || result.sampleRate != reference.sampleRate) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& result : concurrent) {
        if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) std::abort();
        assert(result.get());
    }

    auto invalidCodebooks = fixture.codebooks;
    invalidCodebooks.resize(3u);
    const auto invalid = sds::rebuildWwiseVorbisOgg(
        fixture.payload,
        fixture.structure,
        fixture.packetProbe,
        invalidCodebooks);
    assert(invalid.attempted);
    assert(!invalid.success);
    assert(!invalid.error.empty());

    return 0;
}
