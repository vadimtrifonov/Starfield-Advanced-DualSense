#include <StarfieldDualSense/RemoteVoAudioPreparation.h>

#include <StarfieldDualSense/WwiseRemoteVoFilesystemProbe.h>
#include <StarfieldDualSense/WwiseVoiceArchiveManifestProbe.h>
#include <StarfieldDualSense/WwiseVoiceBa2IndexProbe.h>
#include <StarfieldDualSense/WwiseWemPayloadProbe.h>
#include <StarfieldDualSense/WwiseWemStructureProbe.h>
#include <StarfieldDualSense/WwiseWemVorbisPacketProbe.h>

#include <ranges>

namespace sds
{
    RemoteVoControllerPlaybackPreparation prepareRemoteVoAudio(
        std::wstring_view filePath,
        std::uint32_t codecId,
        SpeakerVoiceLanguage language,
        const std::filesystem::path& executablePath,
        const std::function<void(std::string_view)>& log)
    {
        const auto preparePayload = [&](const VoiceWemPayloadProbeResult& payload) -> RemoteVoControllerPlaybackPreparation {
            log(formatVoiceWemPayloadProbe(payload));
            if (!payload.readSucceeded) {
                return {};
            }

            const auto structure = probeVoiceWemStructure(payload, codecId);
            log(formatVoiceWemStructureProbeSummary(structure));
            for (std::size_t index = 0; index < structure.chunks.size(); ++index) {
                log(formatVoiceWemStructureProbeChunk(structure.chunks[index], index));
            }
            if (!structure.scanComplete) {
                return {};
            }

            const auto vorbis = probeWwiseVorbisPackets(payload, structure, 5u, 16u);
            log(formatWwiseVorbisPacketProbeSummary(vorbis));
            if (vorbis.setupPacket.valid) {
                log(formatWwiseVorbisSetupPacketProbe(vorbis));
            }
            for (std::size_t index = 0; index < vorbis.audioPackets.size(); ++index) {
                log(formatWwiseVorbisAudioPacketProbe(vorbis.audioPackets[index], index));
            }
            if (!vorbis.probeComplete) {
                return {};
            }

            const auto decoded = decodeWwiseVorbisToPcm(payload, structure, vorbis);
            log(formatWwiseVorbisDecodeSummary(decoded));
            if (!decoded.decodeSucceeded) {
                return {};
            }
            return prepareRemoteVoControllerPlayback(decoded);
        };

        const auto candidates = buildRemoteVoFilesystemCandidates(filePath, executablePath);
        log(formatRemoteVoFilesystemProbeContext(filePath, executablePath));
        for (const auto* candidate : { &candidates.raw, &candidates.dataRoot }) {
            const auto payload = probeVoiceWemPayload(candidate->path);
            if (payload.openSucceeded) {
                // A loose file overrides archives even when it cannot be decoded.
                return preparePayload(payload);
            }
            log(formatVoiceWemPayloadProbe(payload));
        }

        const auto manifest = probeVoiceArchiveManifest(filePath, executablePath, language);
        log(formatVoiceArchiveManifestContext(filePath, manifest));
        // Later archives in the configured list override earlier ones.
        for (const auto& entry : manifest.entries | std::views::reverse) {
            log(formatVoiceArchiveManifestEntry(entry));
            const auto index = probeVoiceBa2Index(filePath, entry);
            log(formatVoiceBa2IndexProbe(index));
            if (index.targetFound) {
                // A matching override owns the voice even if its payload is unusable.
                return preparePayload(probeVoiceWemPayload(index));
            }
        }
        return {};
    }
}
