#include <StarfieldDualSense/WwiseWemVorbisDecode.h>

#include <cstdlib>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#define STB_VORBIS_HEADER_ONLY
#include <stb/stb_vorbis.c>

namespace
{
    constexpr unsigned char kAoTuV603PackedCodebooks[] = {
#include <StarfieldDualSense/packed_codebooks_aoTuV_603.inc>
    };

    bool decodeWithStbVorbis(
        std::span<const unsigned char> ogg,
        std::uint16_t& channels,
        std::uint32_t& sampleRate,
        std::vector<std::int16_t>& pcm,
        std::string& error)
    {
        if (ogg.empty() || ogg.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            error = "reconstructed Ogg size is invalid for stb_vorbis";
            return false;
        }

        int decodedChannels = 0;
        int decodedRate = 0;
        short* decoded = nullptr;
        int samplesPerChannel = 0;
        {
            // stb_vorbis writes its shared CRC table each time it opens a decoder.
            static std::mutex decodeMutex;
            std::scoped_lock lock(decodeMutex);
            samplesPerChannel = stb_vorbis_decode_memory(
                ogg.data(),
                static_cast<int>(ogg.size()),
                &decodedChannels,
                &decodedRate,
                &decoded);
        }
        if (samplesPerChannel < 0 || decoded == nullptr || decodedChannels <= 0 || decodedRate <= 0) {
            std::free(decoded);
            error = "stb_vorbis_decode_memory failed";
            return false;
        }
        if (decodedChannels > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
            std::free(decoded);
            error = "decoded channel count is out of range";
            return false;
        }

        const auto totalSamples = static_cast<std::size_t>(samplesPerChannel) * static_cast<std::size_t>(decodedChannels);
        pcm.assign(decoded, decoded + totalSamples);
        std::free(decoded);
        channels = static_cast<std::uint16_t>(decodedChannels);
        sampleRate = static_cast<std::uint32_t>(decodedRate);
        error.clear();
        return true;
    }
}

namespace sds
{
    VoiceWwiseVorbisDecodeResult decodeWwiseVorbisToPcm(
        const VoiceWemPayloadProbeResult& payloadResult,
        const VoiceWemStructureProbeResult& structureResult,
        const VoiceWwiseVorbisPacketProbeResult& packetProbe)
    {
        return decodeWwiseVorbisToPcmWithBackend(
            payloadResult,
            structureResult,
            packetProbe,
            std::span<const unsigned char>(kAoTuV603PackedCodebooks),
            decodeWithStbVorbis,
            "stb_vorbis");
    }
}
