set_xmakever("3.0.0")
set_project("StarfieldDualSense")
set_version("0.8.0")
set_arch("x64")
set_languages("c++23")
set_encodings("utf-8")
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")
add_requires("zlib")
add_requires("libopus")

local kStbVorbisSource = "external/stb/stb_vorbis.c"
local kWw2OggCodebookInclude = "external/ww2ogg/include/StarfieldDualSense/packed_codebooks_aoTuV_603.inc"

local function add_vorbis_decoder_dependencies()
    if not os.isfile(kStbVorbisSource) or not os.isfile(kWw2OggCodebookInclude) then
        raise("v0.3.06 Vorbis decoder dependencies are missing. Run scripts/bootstrap-vorbis-deps.ps1 from the project root, then rerun xmake f.")
    end
    add_sysincludedirs("external", "external/ww2ogg/include")
end

local core_sources = {
    "src/core/Config.cpp",
    "src/core/MusicReconProbe.cpp",
    "src/core/MusicHapticsMixer.cpp",
    "src/core/ShipPilotContext.cpp",
    "src/core/ShipPropulsionProbe.cpp",
    "src/core/ShipPropulsionHaptics.cpp",
    "src/core/ShipWeaponSemanticCatalog.cpp",
    "src/core/ShipBallisticFireGate.cpp",
    "src/core/ShipLaserFireGate.cpp",
    "src/core/ShipParticleFireGate.cpp",
    "src/core/ShipMissileFireGate.cpp",
    "src/core/ShipEMFireGate.cpp",
    "src/core/ShipLaserReconProbe.cpp",
    "src/core/ShipLaunchLandingReconProbe.cpp",
    "src/core/BoostpackFeedbackAuthority.cpp",
    "src/core/BoostpackSpeakerPreparedCache.cpp",
    "src/core/BoostpackSpeakerPlayback.cpp",
    "src/core/LandVehicleReconProbe.cpp",
    "src/core/LandVehicleDriverEventRecon.cpp",
    "src/core/LandVehicleWwiseReconAggregator.cpp",
    "src/core/LandVehicleTelemetryProbe.cpp",
    "src/core/LandVehiclePhysicsProbe.cpp",
    "src/core/LandVehicleActionGate.cpp",
    "src/core/IncomingDamageDiagnostic.cpp",
    "src/core/WeaponSfxDiscoveryProbe.cpp",
    "src/core/UiAudioDiscoveryProbe.cpp",
    "src/core/UiSpeakerPreparedCache.cpp",
    "src/core/UiSpeakerPlayback.cpp",
    "src/core/WeaponSfxMediaCorrelation.cpp",
    "src/core/DeviceClassifier.cpp",
    "src/core/EffectsEngine.cpp",
    "src/core/WeaponProfiles.cpp",
    "src/core/InputDiagnostics.cpp",
    "src/core/NativeInputInjection.cpp",
    "src/core/SemanticInputInjection.cpp",
    "src/core/Touchpad.cpp",
    "src/core/DualSenseReports.cpp",
    "src/core/BluetoothDualSenseReports.cpp",
    "src/core/HapticsEngine.cpp",
    "src/core/HapticWaveforms.cpp",
    "src/core/HapticMixer.cpp",
    "src/core/HapticEndpointSelection.cpp",
    "src/core/HapticsManager.cpp",
    "src/core/LandVehicleControllerFeel.cpp",
    "src/core/RuntimeEventRouter.cpp",
    "src/core/DualSenseAudioRenderBlock.cpp",
    "src/core/SpeakerPcm.cpp",
    "src/core/CommsProcessor.cpp",
    "src/core/SpeakerMixer.cpp",
    "src/core/SpeakerEventClassifier.cpp",
    "src/core/ControllerSpeakerManager.cpp",
    "src/core/WeaponSpeakerProfile.cpp",
    "src/core/WeaponSpeakerPreparedCache.cpp",
    "src/core/WeaponSpeakerPlayback.cpp",
    "src/core/WeaponSpeakerWemDecode.cpp",
    "src/core/WeaponAudioPipeline.cpp",
    "src/core/WeaponAudioPipelineBackend.cpp",
    "src/core/RemoteVoSpeakerPlayback.cpp",
    "src/core/RemoteVoAudioPreparation.cpp",
    "src/core/RemoteVoAudioPipeline.cpp",
    "src/core/RemoteVoAudioPlayback.cpp",
    "src/core/WwiseReconSelection.cpp",
    "src/core/WwiseCallgraphAnalysis.cpp",
    "src/core/WwiseCanarySafety.cpp",
    "src/core/WwiseRemoteVoMirrorGate.cpp",
    "src/core/WwiseRemoteVoDelay.cpp",
    "src/core/WwiseWindowsDeviceId.cpp",
    "src/core/WwiseSpatialProbeGate.cpp",
    "src/core/WwiseRemoteVoFilesystemProbe.cpp",
    "src/core/WwiseVoiceArchiveManifestProbe.cpp",
    "src/core/WwiseVoiceBa2IndexProbe.cpp",
    "src/core/WwiseWemPayloadProbe.cpp",
    "src/core/WwiseWemStructureProbe.cpp",
    "src/core/WwiseWemSmplLoop.cpp",
    "src/core/WwisePcmWemDecode.cpp",
    "src/core/WwiseBa2ArchiveIndex.cpp",
    "src/core/WwiseSoundBanksInfo.cpp",
    "src/core/WwiseBankHircResolver.cpp",
    "src/core/WwiseResolvedMediaExtractor.cpp",
    "src/core/WwiseEventMediaResolver.cpp",
    "src/core/WwiseWemVorbisPacketProbe.cpp",
    "src/core/WwiseVorbisRebuild.cpp",
    "src/core/WwiseWemVorbisDecode.cpp",
    "src/core/WwiseWemVorbisDecodeStb.cpp",
    "src/core/StbVorbisImpl.cpp",
}







target("sds-v0383-land-vehicle-physics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0383LandVehiclePhysicsProbeTest.cpp",
        "src/core/LandVehiclePhysicsProbe.cpp")
end)

target("sds-v0382-land-vehicle-telemetry-probe-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0382LandVehicleTelemetryProbeTest.cpp",
        "src/core/LandVehicleTelemetryProbe.cpp")
end)

target("sds-v0381-land-vehicle-wwise-aggregator-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0381LandVehicleWwiseAggregatorTest.cpp",
        "src/core/LandVehicleWwiseReconAggregator.cpp")
end)

target("sds-v0380-r2-land-vehicle-context-suppression-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0380R2LandVehicleContextSuppressionTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp")
end)

target("sds-v0380-land-vehicle-recon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0380LandVehicleReconProbeTest.cpp",
        "src/core/LandVehicleReconProbe.cpp")
end)

target("sds-v0380-land-vehicle-driver-event-recon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0380LandVehicleDriverEventReconTest.cpp",
        "src/core/LandVehicleDriverEventRecon.cpp")
end)

target("sds-v0372-ship-launch-landing-recon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0372ShipLaunchLandingReconTest.cpp",
        "src/core/ShipLaunchLandingReconProbe.cpp")
end)

target("sds-v0373-ship-landing-sequence-recon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0373LandingSequenceReconTest.cpp",
        "src/core/ShipLaunchLandingReconProbe.cpp")
end)

target("sds-v0374-ship-landing-cinematic-tail-recon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0374LandingCinematicTailReconTest.cpp",
        "src/core/ShipLaunchLandingReconProbe.cpp")
end)

target("sds-v0375-post-load-touchdown-recon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0375PostLoadTouchdownReconTest.cpp",
        "src/core/ShipLaunchLandingReconProbe.cpp")
end)

target("sds-v0376-touchdown-precision-correlation-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0376TouchdownPrecisionCorrelationTest.cpp",
        "src/core/ShipLaunchLandingReconProbe.cpp")
end)

target("sds-v0377-ship-touchdown-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0377ShipTouchdownHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0379-ship-launch-landing-trigger-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0379ShipLaunchLandingTriggerTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp")
end)

target("sds-v0379-aggressive-launch-landing-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0379AggressiveLaunchLandingHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0378-ship-launch-landing-rumble-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0378ShipLaunchLandingRumbleTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0371-ship-em-fire-gate-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0371ShipEMFireGateTest.cpp",
        "src/core/ShipEMFireGate.cpp")
end)

target("sds-v0371-ship-em-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0371ShipEMHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0371-ship-em-trigger-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0371ShipEMTriggerTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp")
end)

target("sds-v0369-ship-missile-fire-gate-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0369ShipMissileFireGateTest.cpp",
        "src/core/ShipMissileFireGate.cpp")
end)

target("sds-v0369-ship-missile-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0369ShipMissileHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0369-ship-missile-trigger-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0369ShipMissileTriggerTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp")
end)

target("sds-v0367-ship-particle-fire-gate-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0367ShipParticleFireGateTest.cpp",
        "src/core/ShipParticleFireGate.cpp")
end)

target("sds-v0367-ship-particle-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0367ShipParticleHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0367-ship-particle-trigger-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0367ShipParticleTriggerTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp")
end)

target("sds-v0364-ship-laser-recon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0364ShipLaserReconProbeTest.cpp",
        "src/core/ShipLaserReconProbe.cpp")
end)

target("sds-v0365-ship-laser-fire-gate-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0365ShipLaserFireGateTest.cpp",
        "src/core/ShipLaserFireGate.cpp")
end)

target("sds-v0365-ship-laser-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0365ShipLaserHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0365-ship-laser-trigger-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0365ShipLaserTriggerTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp")
end)

target("sds-v0363-ship-ballistic-semantic-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0363ShipBallisticSemanticTest.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp")
end)

target("sds-v0363-ship-weapon-catalog-pipeline-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0363ShipWeaponCatalogPipelineTest.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPreparedCache.cpp")
end)

target("sds-v0363-ship-ballistic-fire-gate-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0363ShipBallisticFireGateTest.cpp",
        "src/core/ShipBallisticFireGate.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp")
end)

target("sds-v0363-ship-ballistic-effects-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0363ShipBallisticEffectsTest.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/WeaponProfiles.cpp")
end)

target("sds-v0363-ship-ballistic-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0363ShipBallisticHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0362-ship-propulsion-probe-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0362ShipPropulsionProbeTest.cpp",
        "src/core/ShipPropulsionProbe.cpp")
end)

target("sds-v0362-ship-propulsion-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0362ShipPropulsionHapticsTest.cpp",
        "src/core/ShipPropulsionHaptics.cpp",
        "src/core/HapticMixer.cpp")
end)

target("sds-v0362-ship-propulsion-ownership-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0362ShipPropulsionOwnershipTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0361-ship-pilot-context-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0361ShipPilotContextTest.cpp",
        "src/core/ShipPilotContext.cpp")
end)

target("sds-v0360-main-menu-controller-speaker-startup-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/V0360MainMenuControllerSpeakerStartupTest.cpp")
end)

target("sds-v0359-expanded-menu-promotion-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0359ExpandedMenuPromotionTest.cpp",
        "src/core/UiAudioDiscoveryProbe.cpp")
end)

target("sds-v0359-ui-diagnostic-resolution-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0359UiDiagnosticResolutionTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-v0359-ui-diagnostic-backend-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_vorbis_decoder_dependencies()
    add_files(
        "tests/V0359UiDiagnosticBackendTest.cpp",
        "src/core/WeaponAudioPipelineBackend.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/WeaponSfxMediaCorrelation.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp",
        "src/core/WeaponSpeakerWemDecode.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WwisePcmWemDecode.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp",
        "src/core/WwiseVorbisRebuild.cpp",
        "src/core/WwiseWemVorbisDecode.cpp",
        "src/core/WwiseWemVorbisDecodeStb.cpp",
        "src/core/StbVorbisImpl.cpp",
        "src/core/WwiseWemSmplLoop.cpp")
end)

target("sds-v0358-ui-discovery-expansion-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0358UiDiscoveryPromotionFilterTest.cpp",
        "src/core/UiAudioDiscoveryProbe.cpp")
end)

target("sds-v0357-ui-speaker-playback-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0357UiSpeakerPlaybackTest.cpp",
        "src/core/Config.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPlayback.cpp")
end)

target("sds-v0357-ui-audio-pipeline-startup-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_vorbis_decoder_dependencies()
    add_files(
        "tests/V0357UiAudioPipelineStartupTest.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WeaponAudioPipelineBackend.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/WeaponSfxMediaCorrelation.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp",
        "src/core/WeaponSpeakerWemDecode.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/WwisePcmWemDecode.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp",
        "src/core/WwiseVorbisRebuild.cpp",
        "src/core/WwiseWemVorbisDecode.cpp",
        "src/core/WwiseWemVorbisDecodeStb.cpp",
        "src/core/StbVorbisImpl.cpp",
        "src/core/WwiseWemSmplLoop.cpp")
end)

target("sds-v0357-ui-inmemory-resolution-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0357UiInMemoryResolutionTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-v0357-ui-speaker-cache-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0357UiSpeakerPreparedCacheTest.cpp",
        "src/core/UiSpeakerPreparedCache.cpp")
end)

target("sds-v100-digipick-crafting-speaker-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100DigipickCraftingSpeakerTest.cpp",
        "src/core/Config.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPlayback.cpp")
end)

target("sds-incoming-damage-diagnostic-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/IncomingDamageDiagnosticTest.cpp",
        "src/core/IncomingDamageDiagnostic.cpp")
end)

target("sds-confirmed-incoming-damage-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/ConfirmedIncomingDamageHapticsTest.cpp",
        "src/core/IncomingDamageDiagnostic.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp")
end)

target("sds-synthetic-speaker-cue-removal-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/SyntheticSpeakerCueRemovalTest.cpp",
        "src/core/Config.cpp",
        "src/core/SpeakerEventClassifier.cpp",
        "src/core/ControllerSpeakerManager.cpp")
end)

target("sds-weapon-sfx-discovery-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WeaponSfxDiscoveryProbeTest.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp")
end)

target("sds-grendel-weapon-sfx-discovery-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/GrendelWeaponSfxDiscoveryTest.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp")
end)

target("sds-batch-weapon-sfx-discovery-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/BatchWeaponSfxDiscoveryTest.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp")
end)

target("sds-null-target-incoming-damage-fallback-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/NullTargetIncomingDamageFallbackTest.cpp",
        "src/core/IncomingDamageDiagnostic.cpp")
end)

target("sds-wwise-ba2-index-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files("tests/WwiseBa2ArchiveIndexTest.cpp", "src/core/WwiseBa2ArchiveIndex.cpp")
end)

target("sds-wwise-soundbanks-info-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseSoundBanksInfoTest.cpp", "src/core/WwiseSoundBanksInfo.cpp")
end)

target("sds-wwise-hirc-resolver-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseBankHircResolverTest.cpp", "src/core/WwiseBankHircResolver.cpp")
end)

target("sds-wwise-resolved-media-extractor-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseResolvedMediaExtractorTest.cpp", "src/core/WwiseResolvedMediaExtractor.cpp")
end)

target("sds-wwise-event-resolver-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/WwiseEventMediaResolverTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-wwise-grendel-media-discovery-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/WwiseGrendelMediaDiscoveryTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-wwise-batch-weapon-media-discovery-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/WwiseBatchWeaponMediaDiscoveryTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-wwise-observed-event-resolution-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/WwiseObservedEventMediaResolutionTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)


target("sds-wwise-runtime-event-resolution-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/WwiseRuntimeEventResolutionTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-weapon-sfx-media-correlation-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/WeaponSfxMediaCorrelationTest.cpp",
        "src/core/WeaponSfxMediaCorrelation.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-wwise-recon-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseReconSelectionTest.cpp", "src/core/WwiseReconSelection.cpp")
end)

target("sds-wwise-callgraph-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseCallgraphAnalysisTest.cpp", "src/core/WwiseCallgraphAnalysis.cpp")
end)

target("sds-wwise-canary-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseCanarySafetyTest.cpp", "src/core/WwiseCanarySafety.cpp")
end)

target("sds-wwise-remote-vo-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseRemoteVoMirrorGateTest.cpp", "src/core/WwiseRemoteVoMirrorGate.cpp")
end)

target("sds-wwise-remote-vo-delay-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseRemoteVoDelayTest.cpp", "src/core/WwiseRemoteVoDelay.cpp")
end)


target("sds-wwise-device-id-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseWindowsDeviceIdTest.cpp", "src/core/WwiseWindowsDeviceId.cpp")
end)

target("sds-wwise-spatial-probe-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/WwiseSpatialProbeGateTest.cpp", "src/core/WwiseSpatialProbeGate.cpp")
end)

target("sds-wwise-remote-vo-filesystem-probe-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WwiseRemoteVoFilesystemProbeTest.cpp",
        "src/core/WwiseRemoteVoFilesystemProbe.cpp")
end)

target("sds-wwise-voice-archive-manifest-probe-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WwiseVoiceArchiveManifestProbeTest.cpp",
        "src/core/WwiseVoiceArchiveManifestProbe.cpp")
end)

target("sds-wwise-voice-ba2-index-probe-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WwiseVoiceBa2IndexProbeTest.cpp",
        "src/core/WwiseVoiceBa2IndexProbe.cpp")
end)

target("sds-wwise-wem-payload-probe-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WwiseWemPayloadProbeTest.cpp",
        "src/core/WwiseWemPayloadProbe.cpp")
end)

target("sds-wwise-wem-structure-probe-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WwiseWemStructureProbeTest.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-wwise-pcm-wem-decode-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WwisePcmWemDecodeTest.cpp",
        "src/core/WwisePcmWemDecode.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/SpeakerPcm.cpp")
end)

target("sds-wwise-vorbis-packet-probe-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WwiseWemVorbisPacketProbeTest.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp")
end)

target("sds-remote-vo-audio-pipeline-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/RemoteVoAudioPipelineTest.cpp", "src/core/RemoteVoAudioPipeline.cpp")
end)

target("sds-remote-vo-audio-preparation-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/RemoteVoAudioPreparationTest.cpp",
        "src/core/RemoteVoAudioPreparation.cpp",
        "src/core/RemoteVoSpeakerPlayback.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/CommsProcessor.cpp",
        "src/core/WwiseRemoteVoFilesystemProbe.cpp",
        "src/core/WwiseVoiceArchiveManifestProbe.cpp",
        "src/core/WwiseVoiceBa2IndexProbe.cpp",
        "src/core/WwiseWemPayloadProbe.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp")
end)

target("sds-remote-vo-speaker-playback-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/RemoteVoSpeakerPlaybackTest.cpp",
        "src/core/RemoteVoSpeakerPlayback.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/CommsProcessor.cpp")
end)

target("sds-continuous-remote-vo-playback-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/ContinuousRemoteVoPlaybackTest.cpp",
        "src/core/WwiseRemoteVoMirrorGate.cpp",
        "src/core/SpeakerMixer.cpp",
        "src/core/ControllerSpeakerManager.cpp",
        "src/core/Config.cpp",
        "src/core/SpeakerEventClassifier.cpp")
end)


target("sds-remote-vo-output-policy-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/RemoteVoOutputPolicyTest.cpp",
        "src/core/RemoteVoAudioPlayback.cpp",
        "src/core/ControllerSpeakerManager.cpp",
        "src/core/SpeakerEventClassifier.cpp",
        "src/core/Config.cpp")
end)

target("sds-wwise-vorbis-decode-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_vorbis_decoder_dependencies()
    add_files(
        "tests/WwiseWemVorbisDecodeTest.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp",
        "src/core/WwiseVorbisRebuild.cpp",
        "src/core/WwiseWemVorbisDecode.cpp",
        "src/core/WwiseWemVorbisDecodeStb.cpp",
        "src/core/StbVorbisImpl.cpp")
end)

target("sds-core-tests", function()
    set_kind("binary")
    add_packages("zlib")
    set_default(false)
    add_includedirs("include")
    add_vorbis_decoder_dependencies()
    add_files("tests/TestMain.cpp", "tests/CoreTests.cpp", "src/windows/ControllerManager.cpp")
    for _, source in ipairs(core_sources) do
        if os.isfile(source) then
            add_files(source)
        end
    end
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    end
end)

target("sds-shared-audio-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/SharedAudioTransportTest.cpp", "src/core/DualSenseAudioRenderBlock.cpp", "src/core/SpeakerMixer.cpp")
end)

target("sds-weapon-speaker-profile-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WeaponSpeakerProfileTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

target("sds-v0337-batch1-weapon-speaker-profiles-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0337Batch1WeaponSpeakerProfilesTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

target("sds-v0339-batch2-weapon-speaker-profiles-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0339Batch2WeaponSpeakerProfilesTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

target("sds-v0342-batch3-energy-speaker-profiles-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0342Batch3EnergySpeakerProfilesTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponProfiles.cpp")
end)


target("sds-v0346-starlash-speaker-profile-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0346StarlashSpeakerProfileTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

target("sds-v0348-shattered-space-speaker-profile-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0348ShatteredSpaceSpeakerProfilesTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)


target("sds-v0355-ui-audio-hud-followup-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0355UiAudioHudFollowupTest.cpp",
        "src/core/UiAudioDiscoveryProbe.cpp")
end)

target("sds-v0355-ui-candidate-resolution-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0355UiCandidateResolutionTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-v0356-hud-scanner-candidate-resolution-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0356HudScannerCandidateResolutionTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-v0354-ui-audio-discovery-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0354UiAudioDiscoveryTest.cpp",
        "src/core/UiAudioDiscoveryProbe.cpp")
end)

target("sds-v0353-teshit-shutdown-verification-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files("tests/V0353TESHitShutdownVerificationTest.cpp")
end)

target("sds-v0352-weapon-speaker-completeness-audit-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0352WeaponSpeakerCompletenessAuditTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp")
end)

target("sds-v0351-starstorm-sustained-body-loudness-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0351StarstormSustainedBodyLoudnessTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

target("sds-v0350-starstorm-speaker-diagnostic-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0350StarstormSpeakerDiagnosticTest.cpp",
        "src/core/SpeakerPcmDiagnostics.cpp",
        "src/core/DualSenseAudioRenderBlock.cpp"
    )
end)

target("sds-v0349-starstorm-sustained-body-gain-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0349StarstormSustainedBodyGainTest.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

target("sds-v0348-pinned-media-resolver-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0348PinnedMediaResolverTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp")
end)

target("sds-v0348-weapon-speaker-vorbis-decode-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_vorbis_decoder_dependencies()
    add_files(
        "tests/V0348WeaponSpeakerVorbisDecodeTest.cpp",
        "src/core/WeaponSpeakerWemDecode.cpp",
        "src/core/WwisePcmWemDecode.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp",
        "src/core/WwiseVorbisRebuild.cpp",
        "src/core/WwiseWemVorbisDecode.cpp",
        "src/core/WwiseWemVorbisDecodeStb.cpp",
        "src/core/StbVorbisImpl.cpp")
end)

target("sds-v0348-authored-loop-preparation-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0348AuthoredLoopPreparationTest.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseWemSmplLoop.cpp")
end)

target("sds-v0348-authored-loop-backend-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_vorbis_decoder_dependencies()
    add_packages("zlib")
    add_files(
        "tests/V0348AuthoredLoopBackendTest.cpp",
        "src/core/WeaponAudioPipelineBackend.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerWemDecode.cpp",
        "src/core/WwiseWemSmplLoop.cpp",
        "src/core/WwisePcmWemDecode.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/WeaponSfxMediaCorrelation.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp",
        "src/core/WwiseVorbisRebuild.cpp",
        "src/core/WwiseWemVorbisDecode.cpp",
        "src/core/WwiseWemVorbisDecodeStb.cpp",
        "src/core/StbVorbisImpl.cpp")
end)

target("sds-v0348-shattered-space-playback-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0348ShatteredSpacePlaybackTest.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

target("sds-v0347-shattered-space-wem-capture-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0347ShatteredSpaceWemCaptureTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)

target("sds-v0346-shattered-space-resolver-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0346ShatteredSpaceResolverTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp")
end)
target("sds-v0343-sustained-weapon-speaker-profiles-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0343SustainedWeaponSpeakerProfilesTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp")
end)

if is_plat("windows") then
    target("sds-v0343-persistent-speaker-transport-tests", function()
        set_kind("binary")
        set_default(false)
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "SDS_TESTING")
        add_files(
            "tests/V0343PersistentSpeakerTransportTest.cpp",
            "src/windows/DualSenseAudioTransport.cpp",
            "src/core/MusicHapticsMixer.cpp",
            "src/windows/DualSenseAudioSpeakerClient.cpp",
            "src/core/SpeakerMixer.cpp",
            "src/core/DualSenseAudioRenderBlock.cpp",
            "src/core/HapticMixer.cpp",
            "src/core/HapticWaveforms.cpp",
            "src/core/HapticEndpointSelection.cpp")
        add_syslinks("ole32", "uuid")
    end)
end

target("sds-v0343-persistent-speaker-mixer-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0343PersistentSpeakerMixerTest.cpp",
        "src/core/SpeakerMixer.cpp")
end)

target("sds-v0343-sustained-weapon-speaker-lifecycle-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0343SustainedWeaponSpeakerLifecycleTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp")
end)

target("sds-v0343-sustained-weapon-preparation-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0343SustainedWeaponPreparationTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp")
end)


target("sds-v0344-layered-persistent-speaker-mixer-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0344LayeredPersistentSpeakerMixerTest.cpp",
        "src/core/SpeakerMixer.cpp")
end)

target("sds-v0344-layered-sustained-speaker-playback-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0344LayeredSustainedSpeakerPlaybackTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp")
end)

if is_plat("windows") then
    target("sds-v0344-layered-persistent-speaker-transport-tests", function()
        set_kind("binary")
        set_default(false)
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "SDS_TESTING")
        add_files(
            "tests/V0344LayeredPersistentSpeakerTransportTest.cpp",
            "src/windows/DualSenseAudioTransport.cpp",
            "src/core/MusicHapticsMixer.cpp",
            "src/windows/DualSenseAudioSpeakerClient.cpp",
            "src/core/SpeakerMixer.cpp",
            "src/core/DualSenseAudioRenderBlock.cpp",
            "src/core/HapticMixer.cpp",
            "src/core/HapticWaveforms.cpp",
            "src/core/HapticEndpointSelection.cpp")
        add_syslinks("ole32", "uuid")
    end)
end

target("sds-v0339-auto-rivet-tension-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0339AutoRivetTensionHapticsTest.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp")
end)

target("sds-v0339-auto-rivet-pause-safety-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0339AutoRivetPauseSafetyTest.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp")
end)

target("sds-v0340-auto-rivet-gameplay-gate-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0340AutoRivetGameplayGateTest.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp")
end)

target("sds-v0340-auto-rivet-runtime-safety-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/V0340AutoRivetRuntimeSafetyTest.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0340-auto-rivet-wwise-semantic-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/V0340AutoRivetWwiseSemanticTest.cpp")
end)

target("sds-weapon-speaker-playback-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WeaponSpeakerPlaybackTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/MaelstromSpeakerFireProof.cpp")
end)

target("sds-weapon-audio-pipeline-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/WeaponAudioPipelineTest.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/BoostpackSpeakerPreparedCache.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WwiseSoundBanksInfo.cpp")
end)

target("sds-maelstrom-speaker-fire-proof-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/MaelstromSpeakerFireProofTest.cpp",
        "src/core/MaelstromSpeakerFireProof.cpp")
end)

target("sds-maelstrom-speaker-reload-proof-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/MaelstromSpeakerReloadProofTest.cpp",
        "src/core/MaelstromSpeakerReloadProof.cpp")
end)

target("sds-maelstrom-speaker-equip-proof-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/MaelstromSpeakerEquipProofTest.cpp",
        "src/core/MaelstromSpeakerEquipProof.cpp")
end)

target("sds-speaker-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/SpeakerTest.cpp",
        "src/core/Config.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/CommsProcessor.cpp",
        "src/core/SpeakerMixer.cpp",
        "src/core/SpeakerEventClassifier.cpp",
        "src/core/ControllerSpeakerManager.cpp")
end)

target("sds-hid-ownership-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/HidOutputOwnershipTest.cpp", "src/core/DualSenseReports.cpp")
end)

target("sds-fire-marker-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/FireMarkerEventTagTest.cpp")
end)

target("sds-v0361-ship-pilot-ownership-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0361ShipPilotOwnershipTest.cpp",
        "src/core/Config.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/WeaponProfiles.cpp")
end)


target("sds-v0384-land-vehicle-controller-feel-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0384LandVehicleControllerFeelTest.cpp",
        "src/core/LandVehicleControllerFeel.cpp")
end)


target("sds-v0384-land-vehicle-action-gate-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0384LandVehicleActionGateTest.cpp",
        "src/core/LandVehicleActionGate.cpp")
end)

target("sds-recoil-tuning-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files(
        "tests/RecoilTuningTest.cpp",
        "src/core/Config.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/WeaponProfiles.cpp")
end)

target("sds-v0361-ship-pilot-consumer-safety-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0361ShipPilotConsumerSafetyTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp")
end)

target("sds-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/HapticsTest.cpp")
    add_files(
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticEndpointSelection.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp",
        "src/core/RuntimeEventRouter.cpp")
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_files(
            "src/windows/DualSenseAudioTransport.cpp",
            "src/core/MusicHapticsMixer.cpp",
            "src/windows/DualSenseAudioHapticsClient.cpp",
            "src/core/DualSenseAudioRenderBlock.cpp",
            "src/core/SpeakerMixer.cpp")
        add_syslinks("ole32", "uuid")
    end
end)

if is_plat("windows") then
    target("sds-hid-probe", function()
        set_kind("binary")
        set_default(false)
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi")
        add_files("src/tools/HidProbeMain.cpp", "src/tools/HidProbePackets.cpp")
    end)

    target("sds-overlapped-io-tests", function()
        set_kind("binary")
        set_default(false)
        add_includedirs("src/windows")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_files("tests/OverlappedIoTest.cpp")
    end)

    target("sds-hid-owner-probe", function()
        set_kind("binary")
        set_default(false)
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi", "advapi32")
        add_files("src/tools/HidOwnerProbeMain.cpp")
    end)
end

if os.isfile("external/CommonLibSF/xmake.lua") then
    includes("external/CommonLibSF")

    target("StarfieldDualSense", function()
        add_rules("commonlibsf.plugin", {
            name = "StarfieldDualSense",
            author = "AD Mixon",
            description = "Native DualSense support for Starfield on PC"
        })
        set_version("0.8.0")
        set_license("GPL-3.0-or-later")
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi", "ole32", "uuid", "Bthprops")
        add_vorbis_decoder_dependencies()
        add_packages("zlib")
        add_packages("libopus")
        for _, source in ipairs(core_sources) do
            add_files(source)
        end
        add_files(
            "src/windows/ControllerManager.cpp",
            "src/windows/DualSenseAudioTransport.cpp",
            "src/windows/DualSenseAudioHapticsClient.cpp",
            "src/windows/DualModeHapticsBackend.cpp",
            "src/windows/DualSenseAudioSpeakerClient.cpp",
            "src/windows/BluetoothSpeakerBackend.cpp",
            "src/windows/DualModeSpeakerBackend.cpp",
            "src/windows/NativeUsbBackend.cpp",
            "src/windows/NativeBluetoothBackend.cpp",
            "src/windows/NativeDualSenseBackend.cpp",
            "src/windows/HidWriteTrace.cpp",
            "src/starfield/GameStateAdapter.cpp",
            "src/starfield/StarfieldAudioCapture.cpp",
            "src/starfield/WwiseApiRecon.cpp",
            "src/starfield/WwiseSecondaryOutputCanary.cpp",
            "src/starfield/WwiseSpatialOutputProbe.cpp",
            "src/starfield/WwiseRemoteVoMirror.cpp",
            "src/starfield/WwisePlayingId.cpp",
            "src/core/SettingsService.cpp",
            "src/starfield/SettingsMenu.cpp",
            "src/starfield/Plugin.cpp")
        add_installfiles("config/StarfieldDualSense.toml", { prefixdir = "SFSE/Plugins" })
    end)
else
    on_config(function()
        if has_config("sds_require_plugin") then
            raise("CommonLibSF is missing. Run scripts/bootstrap.ps1 first.")
        end
    end)
end

target("sds-v0384-land-vehicle-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0384LandVehicleHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp")
end)

target("sds-v0384-land-vehicle-effects-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0384LandVehicleEffectsTest.cpp",
        "src/core/Config.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/WeaponProfiles.cpp")
end)

target("sds-v0385-music-recon-probe-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_files(
        "tests/V0385MusicReconProbeTest.cpp",
        "src/core/MusicReconProbe.cpp")
end)


target("sds-v0385-music-recon-resolver-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0385MusicReconResolverTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp")
end)


target("sds-v0385-music-recon-pipeline-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_packages("zlib")
    add_vorbis_decoder_dependencies()
    add_files(
        "tests/V0385MusicReconPipelineTest.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/WeaponAudioPipelineBackend.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WeaponSfxMediaCorrelation.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp",
        "src/core/WeaponSpeakerWemDecode.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/WwisePcmWemDecode.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp",
        "src/core/WwiseVorbisRebuild.cpp",
        "src/core/WwiseWemVorbisDecode.cpp",
        "src/core/WwiseWemVorbisDecodeStb.cpp",
        "src/core/StbVorbisImpl.cpp",
        "src/core/WwiseWemSmplLoop.cpp")
end)


target("sds-v0386-music-selection-recon-gate-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_files("tests/V0386MusicSelectionReconGateTest.cpp")
end)

target("sds-v0387-music-selection-catalog-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_files("tests/V0387MusicSelectionTargetCatalogTest.cpp")
end)

target("sds-v0387-music-selection-catalog-resolver-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_packages("zlib")
    add_files(
        "tests/V0387MusicSelectionCatalogResolverTest.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp")
end)

target("sds-v0389-music-haptics-mixer-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_files(
        "tests/V0389MusicHapticsMixerTest.cpp",
        "src/core/MusicHapticsMixer.cpp",
        "src/core/Config.cpp")
end)

target("sds-v0389-music-haptics-priority-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_files(
        "tests/V0389MusicHapticsPriorityTest.cpp",
        "src/core/MusicHapticsMixer.cpp")
end)

target("sds-v0389-music-haptics-pipeline-tests", function()
    add_packages("stb")
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_packages("zlib")
    add_vorbis_decoder_dependencies()
    add_files(
        "tests/V0389MusicHapticsPipelineTest.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/WeaponAudioPipelineBackend.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/WwiseEventMediaResolver.cpp",
        "src/core/WwiseBa2ArchiveIndex.cpp",
        "src/core/WwiseSoundBanksInfo.cpp",
        "src/core/WwiseBankHircResolver.cpp",
        "src/core/WwiseResolvedMediaExtractor.cpp",
        "src/core/WwiseWemStructureProbe.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WeaponSfxMediaCorrelation.cpp",
        "src/core/WeaponSfxDiscoveryProbe.cpp",
        "src/core/WeaponSpeakerWemDecode.cpp",
        "src/core/SpeakerPcm.cpp",
        "src/core/WwisePcmWemDecode.cpp",
        "src/core/WwiseWemVorbisPacketProbe.cpp",
        "src/core/WwiseVorbisRebuild.cpp",
        "src/core/WwiseWemVorbisDecode.cpp",
        "src/core/WwiseWemVorbisDecodeStb.cpp",
        "src/core/StbVorbisImpl.cpp",
        "src/core/WwiseWemSmplLoop.cpp")
end)

target("sds-v0389-music-selection-production-observation-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_files("tests/V0389MusicSelectionProductionObservationTest.cpp")
end)

target("sds-v0388-music-selection-callback-chain-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    add_includedirs("include")
    add_files("tests/V0388MusicSelectionCallbackChainTest.cpp")
end)


-- SAD 1.0 settings/menu contract
target("sds-v100-settings-menu-contract-tests")
    set_kind("binary")
    set_languages("cxx23")
    add_files("tests/V100SettingsMenuContractTest.cpp")
    add_includedirs("include")
target_end()
-- SAD 1.0 public config parsing
target("sds-v100-config-parsing-tests")
    set_kind("binary")
    set_languages("cxx23")
    add_files(
        "tests/V100ConfigParsingTest.cpp",
        "src/core/Config.cpp"
    )
    add_includedirs("include")
target_end()
-- SAD 1.0 canonical SettingsService
target("sds-v100-settings-service-tests")
    set_kind("binary")
    set_languages("cxx23")
    add_files(
        "tests/V100SettingsServiceTest.cpp",
        "src/core/Config.cpp",
        "src/core/SettingsService.cpp"
    )
    add_includedirs("include")
target_end()
-- SAD 1.0 SFSE Menu Framework integration contract
target("sds-v100-settings-menu-framework-tests")
    set_kind("binary")
    set_languages("cxx23")
    add_files("tests/V100SettingsMenuFrameworkTest.cpp")
    add_includedirs("include")
target_end()

-- SAD 1.0 Task 5B immediate live-settings behavior
target("sds-v100-immediate-live-settings-tests")
    set_kind("binary")
    set_languages("cxx23")
    add_files("tests/V100ImmediateLiveSettingsTest.cpp")
    add_includedirs("include")
target_end()
-- SAD 1.0 Task 5C controller worker live settings
target("sds-v100-controller-live-settings-tests")
    set_kind("binary")
    set_languages("cxx23")
    add_includedirs("include")
    add_files(
        "tests/V100ControllerLiveSettingsTest.cpp",
        "src/core/Config.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/WeaponProfiles.cpp"
    )
target_end()


-- SAD 1.0 Task 5D gameplay haptics live settings

target("sds-v100-gameplay-haptics-live-settings-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/V100GameplayHapticsLiveSettingsTest.cpp")
    add_files(
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticEndpointSelection.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp",
        "src/core/RuntimeEventRouter.cpp")
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_files(
            "src/windows/DualSenseAudioTransport.cpp",
            "src/core/MusicHapticsMixer.cpp",
            "src/windows/DualSenseAudioHapticsClient.cpp",
            "src/core/DualSenseAudioRenderBlock.cpp",
            "src/core/SpeakerMixer.cpp")
        add_syslinks("ole32", "uuid")
    end
end)

target("sds-v100-controller-speaker-live-settings-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "SDS_TESTING")
    add_files(
        "tests/V100ControllerSpeakerLiveSettingsTest.cpp",
        "src/core/Config.cpp",
        "src/core/SpeakerEventClassifier.cpp",
        "src/core/ControllerSpeakerManager.cpp",
        "src/windows/DualSenseAudioTransport.cpp",
        "src/windows/DualSenseAudioSpeakerClient.cpp",
        "src/core/SpeakerMixer.cpp",
        "src/core/DualSenseAudioRenderBlock.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticEndpointSelection.cpp",
        "src/core/MusicHapticsMixer.cpp")
    add_syslinks("ole32", "uuid")
end)

target("sds-v100-controller-speaker-routing-worker-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100ControllerSpeakerRoutingWorkerTest.cpp",
        "src/core/DualSenseReports.cpp")
end)

target("sds-v100-boostpack-feedback-authority-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100BoostpackFeedbackAuthorityTest.cpp",
        "src/core/BoostpackFeedbackAuthority.cpp")
end)

target("sds-v100-boostpack-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100BoostpackHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticEndpointSelection.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp",
        "src/core/RuntimeEventRouter.cpp")
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_files(
            "src/windows/DualSenseAudioTransport.cpp",
            "src/core/MusicHapticsMixer.cpp",
            "src/windows/DualSenseAudioHapticsClient.cpp",
            "src/core/DualSenseAudioRenderBlock.cpp",
            "src/core/SpeakerMixer.cpp")
        add_syslinks("ole32", "uuid")
    end
end)
target("sds-v100-boostpack-speaker-audio-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100BoostpackSpeakerAudioTest.cpp",
        "src/core/Config.cpp",
        "src/core/SpeakerEventClassifier.cpp",
        "src/core/ControllerSpeakerManager.cpp",
        "src/core/BoostpackSpeakerPreparedCache.cpp",
        "src/core/BoostpackSpeakerPlayback.cpp",
        "src/core/WeaponAudioPipeline.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/ShipWeaponSemanticCatalog.cpp",
        "src/core/WwiseSoundBanksInfo.cpp")
end)

target("sds-v100-digipick-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100DigipickHapticsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/HapticWaveforms.cpp",
        "src/core/HapticMixer.cpp",
        "src/core/HapticEndpointSelection.cpp",
        "src/core/HapticsManager.cpp",
        "src/core/LandVehicleControllerFeel.cpp",
        "src/core/ShipPropulsionHaptics.cpp",
        "src/core/RuntimeEventRouter.cpp")
    if is_plat("windows") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_files(
            "src/windows/DualSenseAudioTransport.cpp",
            "src/core/MusicHapticsMixer.cpp",
            "src/windows/DualSenseAudioHapticsClient.cpp",
            "src/core/DualSenseAudioRenderBlock.cpp",
            "src/core/SpeakerMixer.cpp")
        add_syslinks("ole32", "uuid")
    end
end)

target("sds-v100-digipick-equip-speaker-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100DigipickEquipSpeakerTest.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPlayback.cpp")
end)

target("sds-v100-digipick-undo-speaker-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V100DigipickUndoSpeakerTest.cpp",
        "src/core/UiSpeakerPreparedCache.cpp",
        "src/core/UiSpeakerPlayback.cpp")
end)

target("sds-v0391-controller-indicators-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V0391ControllerIndicatorsTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/Touchpad.cpp",
        "src/core/DualSenseReports.cpp",
        "src/core/BluetoothDualSenseReports.cpp")
end)

target("sds-bluetooth-report-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/BluetoothDualSenseReportsTest.cpp",
        "src/core/BluetoothDualSenseReports.cpp",
        "src/core/DualSenseReports.cpp")
end)


target("sds-bluetooth-input-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/BluetoothTouchpadTest.cpp",
        "src/core/Touchpad.cpp")
end)


if is_plat("windows") then
    target("sds-bluetooth-backend-compile-tests", function()
        set_kind("binary")
        set_default(false)
        set_languages("c++23")
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi")
        add_files(
            "tests/NativeBluetoothBackendCompileTest.cpp",
            "src/windows/NativeBluetoothBackend.cpp",
            "src/core/BluetoothDualSenseReports.cpp",
            "src/core/DualSenseReports.cpp",
            "src/core/DeviceClassifier.cpp",
            "src/core/Touchpad.cpp")
    end)
end


if is_plat("windows") then
    target("sds-native-dualsense-backend-compile-tests", function()
        set_kind("binary")
        set_default(false)
        set_languages("c++23")
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi")
        add_files(
            "tests/NativeDualSenseBackendCompileTest.cpp",
            "src/windows/NativeDualSenseBackend.cpp",
            "src/windows/NativeUsbBackend.cpp",
            "src/windows/NativeBluetoothBackend.cpp",
            "src/windows/HidWriteTrace.cpp",
            "src/core/BluetoothDualSenseReports.cpp",
            "src/core/DualSenseReports.cpp",
            "src/core/DeviceClassifier.cpp",
            "src/core/Touchpad.cpp")
    end)
end


if is_plat("windows") then
    target("sds-bluetooth-backend-hardware-test", function()
        set_kind("binary")
        set_default(false)
        set_languages("c++23")
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi")
        add_files(
            "tests/BluetoothBackendHardwareTest.cpp",
            "src/windows/NativeBluetoothBackend.cpp",
            "src/core/BluetoothDualSenseReports.cpp",
            "src/core/DualSenseReports.cpp",
            "src/core/DeviceClassifier.cpp",
            "src/core/Touchpad.cpp")
    end)
end


if is_plat("windows") then
    target("sds-bluetooth-backend-input-hardware-test", function()
        set_kind("binary")
        set_default(false)
        set_languages("c++23")
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi")
        add_files(
            "tests/BluetoothBackendInputHardwareTest.cpp",
            "src/windows/NativeBluetoothBackend.cpp",
            "src/core/BluetoothDualSenseReports.cpp",
            "src/core/DualSenseReports.cpp",
            "src/core/DeviceClassifier.cpp",
            "src/core/Touchpad.cpp")
    end)
end


if is_plat("windows") then
    target("sds-bluetooth-backend-reconnect-hardware-test", function()
        set_kind("binary")
        set_default(false)
        set_languages("c++23")
        add_includedirs("include")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_syslinks("hid", "setupapi")
        add_files(
            "tests/BluetoothBackendReconnectHardwareTest.cpp",
            "src/windows/NativeBluetoothBackend.cpp",
            "src/core/BluetoothDualSenseReports.cpp",
            "src/core/DualSenseReports.cpp",
            "src/core/DeviceClassifier.cpp",
            "src/core/Touchpad.cpp")
    end)
end

target("sds-v078-reload-haptics-tests", function()
    set_kind("binary")
    set_default(false)
    add_includedirs("include")
    add_files("tests/V078ReloadHapticsTest.cpp",
              "src/core/HapticWaveforms.cpp")
end)

target("sds-v064-custom-weapon-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++23")
    add_includedirs("include")
    add_files(
        "tests/V064CustomWeaponMappingTest.cpp",
        "src/core/Config.cpp",
        "src/core/WeaponProfiles.cpp",
        "src/core/EffectsEngine.cpp",
        "src/core/HapticsEngine.cpp",
        "src/core/WeaponSpeakerProfile.cpp",
        "src/core/WeaponSpeakerPreparedCache.cpp",
        "src/core/WeaponSpeakerPlayback.cpp",
        "src/core/MaelstromSpeakerFireProof.cpp")
end)
