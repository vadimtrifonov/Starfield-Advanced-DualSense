#include <StarfieldDualSense/BoostpackFeedbackAuthority.h>
#include <StarfieldDualSense/OnFootLandingGate.h>
#include <StarfieldDualSense/LandVehicleSpeakerAudio.h>

#include <REL/Relocation.h>
#include <cstring>
#include <StarfieldDualSense/BoostpackSpeakerPreparedCache.h>
#include <StarfieldDualSense/BoostpackSpeakerPlayback.h>
#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/SettingsMenu.h>
#include <StarfieldDualSense/ImmediateLiveSettings.h>
#include <StarfieldDualSense/ControllerLiveSettings.h>
#include <StarfieldDualSense/ControllerSpeakerLiveSettings.h>
#include <StarfieldDualSense/GameplayHapticsLiveSettings.h>
#include <StarfieldDualSense/AutoRivetChargeSemantics.h>
#include <StarfieldDualSense/ControllerManager.h>
#include <StarfieldDualSense/DualSenseAudioTransport.h>
#include <StarfieldDualSense/DualSenseAudioHapticsClient.h>
#include <StarfieldDualSense/DualModeHapticsBackend.h>
#include <StarfieldDualSense/DualSenseAudioSpeakerClient.h>
#include <StarfieldDualSense/BluetoothSpeakerBackend.h>
#include <StarfieldDualSense/DualModeSpeakerBackend.h>
#include <StarfieldDualSense/ControllerSpeakerManager.h>
#include <StarfieldDualSense/WeaponSpeakerPlayback.h>
#include <StarfieldDualSense/WeaponSpeakerPreparedCache.h>
#include <StarfieldDualSense/WeaponAudioPipeline.h>
#include <StarfieldDualSense/WeaponProfiles.h>
#include <StarfieldDualSense/WeaponSpeakerProfile.h>
#include <StarfieldDualSense/CustomWeaponReloadHapticStage.h>
#include <StarfieldDualSense/WeaponSfxDiscoveryProbe.h>
#include <StarfieldDualSense/RemoteVoSpeakerPlayback.h>
#include <StarfieldDualSense/RemoteVoAudioPreparation.h>
#include <StarfieldDualSense/RemoteVoAudioPipeline.h>
#include <StarfieldDualSense/GameStateAdapter.h>
#include <StarfieldDualSense/LandVehicleActionGate.h>
#include <StarfieldDualSense/LandVehicleWwiseReconAggregator.h>
#include <StarfieldDualSense/FireMarkerBridge.h>
#include <StarfieldDualSense/HidWriteTrace.h>
#include <StarfieldDualSense/HapticsManager.h>
#include <StarfieldDualSense/HapticStrength.h>
#include <StarfieldDualSense/MusicReconProbe.h>
#include <StarfieldDualSense/RuntimeEventRouter.h>
#include <StarfieldDualSense/NativeDualSenseBackend.h>
#include <StarfieldDualSense/NativeUsbBackend.h>
#include <StarfieldDualSense/StarfieldAudioCapture.h>
#include <StarfieldDualSense/ShipBallisticFireGate.h>
#include <StarfieldDualSense/ShipLaserReconProbe.h>
#include <StarfieldDualSense/ShipLaserFireGate.h>
#include <StarfieldDualSense/ShipParticleFireGate.h>
#include <StarfieldDualSense/ShipMissileFireGate.h>
#include <StarfieldDualSense/ShipEMFireGate.h>
#include <StarfieldDualSense/ShipLaunchLandingReconProbe.h>
#include <StarfieldDualSense/ShipWeaponSemanticCatalog.h>
#include <StarfieldDualSense/UiAudioDiscoveryProbe.h>
#include <StarfieldDualSense/UiAudioCandidateCatalog.h>
#include <StarfieldDualSense/UiSpeakerPreparedCache.h>
#include <StarfieldDualSense/UiSpeakerPlayback.h>
#include <StarfieldDualSense/WwiseApiRecon.h>
#include <StarfieldDualSense/WwisePlayingId.h>

#include <RE/Starfield.h>
#include <SFSE/SFSE.h>

#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{
    constexpr std::string_view kVersion = "0.8.0";
    constexpr std::uint32_t kShipWeaponCaptureProbeLimit = 256;
    constexpr std::uint32_t kShipEmReconLogLimit = 512;
    constexpr std::uint32_t kLandVehicleRareWwiseLogLimit = 1024;
    constexpr std::uint64_t kMusicHapticsSampleRate = 48000u;
    constexpr std::size_t kMusicHapticsAuthoritySlots = 128u;
    constexpr auto kShipTakeoffHapticsMax = std::chrono::seconds(30);
    constexpr const char* kConfigPath = "Data/SFSE/Plugins/StarfieldDualSense.toml";
    constexpr std::array<std::string_view, 0> kWeaponSfxDiscoveryTargets{};

    enum class ShipLaunchLandingRumblePhase : std::uint8_t
    {
        None,
        Takeoff,
        Landing,
    };
    std::unique_ptr<sds::ControllerManager> g_controller;
    std::shared_ptr<sds::DualSenseAudioTransport> g_audioTransport;
    std::unique_ptr<sds::HapticsManager> g_haptics;
    std::unique_ptr<sds::ControllerSpeakerManager> g_speakerManager;
    std::shared_ptr<sds::WeaponSpeakerPreparedCache> g_weaponSpeakerPreparedCache;
    std::unique_ptr<sds::WeaponSpeakerPlayback> g_weaponSpeakerPlayback;
    std::unique_ptr<sds::WeaponSfxDiscoveryProbe> g_weaponSfxDiscovery;
    std::unique_ptr<sds::WeaponAudioPipeline> g_weaponAudioPipeline;
    std::unique_ptr<sds::WeaponAudioPipeline> g_mainMenuUiAudioPipeline;
    std::unique_ptr<sds::RuntimeEventRouter> g_eventRouter;
    std::unique_ptr<sds::GameStateAdapter> g_gameState;
    std::unique_ptr<sds::FireMarkerBridge> g_fireMarkerBridge;
    std::unique_ptr<sds::StarfieldAudioCapture> g_audioCapture;
    std::unique_ptr<sds::MusicReconProbe> g_musicRecon;
    std::shared_ptr<sds::ShipWeaponSemanticCache> g_shipWeaponSemanticCache;
    sds::ShipBallisticFireGate g_shipBallisticFireGate;
    sds::ShipLaserFireGate g_shipLaserFireGate;
    sds::ShipParticleFireGate g_shipParticleFireGate;
    sds::ShipMissileFireGate g_shipMissileFireGate;
    sds::ShipEMFireGate g_shipEMFireGate;
    sds::ShipLaserReconProbe g_shipEmReconProbe;
    sds::ShipLaunchLandingReconProbe g_shipLaunchLandingReconProbe;
    std::mutex g_boostpackFeedbackMutex{};
    std::unique_ptr<sds::BoostpackFeedbackAuthority> g_boostpackFeedbackAuthority;
    sds::OnFootLandingGate g_onFootLandingGate{};
    std::shared_ptr<sds::BoostpackSpeakerPreparedCache> g_boostpackSpeakerPreparedCache;
    std::shared_ptr<sds::LandVehicleSpeakerCache> g_landVehicleSpeakerCache;
    std::mutex g_landVehicleSpeakerMutex;
    int g_landVehicleEngineBand = -1;
    std::uint64_t g_landVehicleEngineEpoch = 0;
    float g_landVehicleEngineVolume = -1.0F;
    int g_landVehicleThrustKind = 0; // 0=engine, 1=forward boost, 2=vertical boost
    std::chrono::steady_clock::time_point g_landVehicleLastGun{};
    std::unique_ptr<sds::BoostpackSpeakerPlayback> g_boostpackSpeakerPlayback;
    std::atomic_uint8_t g_boostpackProductionBlockingMenuMask{ 0 };
    std::atomic_bool g_boostpackProductionLandVehicleActive{ false };
    std::atomic_bool g_boostpackProductionControllerConnected{ false };
    std::atomic_bool g_boostpackProductionContextEligible{ false };
    bool boostpackProductionCaptureArmed() noexcept;

    std::uintptr_t g_nativeDualSenseReselectionHandlerAddress = 0;
    std::atomic_bool g_nativeDualSenseReconnectFixEnabled{ true };
    bool g_nativeDualSenseReselectionStartedFromDualSense = false;
    bool g_nativeDualSenseReselectionSawDisconnect = false;
    bool g_nativeDualSenseReselectionPending = false;
    bool g_nativeDualSenseReselectionAttemptedThisCycle = false;
    std::uint64_t g_nativeDualSenseReselectionCycle = 0;
    std::chrono::steady_clock::time_point
        g_nativeDualSenseReselectionDue{};
    std::chrono::steady_clock::time_point
        g_nativeDualSenseReselectionDiscoveryDue{};

    std::unique_ptr<sds::UiAudioDiscoveryProbe> g_uiAudioDiscovery;
    std::shared_ptr<sds::UiSpeakerPreparedCache> g_uiSpeakerPreparedCache;
    std::unique_ptr<sds::UiSpeakerPlayback> g_uiSpeakerPlayback;
    std::chrono::steady_clock::time_point g_remoteVoPlaybackDeadline{};
    std::unique_ptr<sds::RemoteVoAudioPipeline> g_remoteVoAudioPipeline;
    bool g_remoteVoBlocked = true;

    struct DataslateQueuedVoiceChunk
    {
        sds::PreparedSpeakerPcm pcm{};
        std::uint64_t durationMs{ 0 };
        std::uint64_t sequence{ 0 };
    };

    constexpr std::uint32_t kDataslatePauseEventId = 0xB7129071u;
    constexpr std::uint32_t kDataslatePlayEventId = 0xCED3059Bu;
    std::atomic_bool g_dataslateMenuActive{ false };
    std::atomic_bool g_dataslateControllerVoiceActive{ false };
    std::atomic_bool g_dataslatePauseRequested{ false };
    std::mutex g_dataslateVoiceQueueMutex{};
    std::vector<DataslateQueuedVoiceChunk> g_dataslateVoiceQueue{};
    std::chrono::steady_clock::time_point g_dataslatePlaybackDeadline{};
    std::chrono::steady_clock::time_point g_dataslateContinuationDeadline{};
    constexpr auto kDataslateContinuationGrace =
        std::chrono::milliseconds(5000);
    bool g_runtimeInitialized = false;
    std::optional<sds::Config> g_startupConfig{};
    // The last equipped EditorID is shared with the Wwise capture worker.
    // An immutable shared_ptr avoids a race on weapon switches and on reloads.
    std::atomic<std::shared_ptr<const std::string>> g_customReloadEquippedIdentity{};
    std::atomic<bool> g_autoRivetChargeCaptureEnabled{ false };
    std::atomic<bool> g_autoRivetChargeCaptureArmed{ false };
    std::atomic_bool g_shipPilotActive{ false };
    std::atomic<std::uint8_t> g_shipBallisticBlockingMenuMask{ 0 };
    std::atomic<std::uint8_t> g_musicHapticsBlockingMenuMask{ 0 };
    std::atomic_bool g_musicHapticsEnabled{ false };
    std::atomic<float> g_musicHapticsStrength{ 1.0F };
    std::atomic<float> g_musicHapticsUserScale{ 1.0F };
    void musicHapticsClearAuthority() noexcept;

    std::string readTextFile(
        const std::filesystem::path& path);

    void pluginLog(
        std::string_view message) noexcept;

    bool refreshCurrentEquippedWeaponState(
        std::uint32_t* refreshedFormId) noexcept;

    sds::SpeakerVoiceLanguage resolvedSpeakerVoiceLanguage() noexcept
    {
        const auto configured =
            g_speakerManager ?
                g_speakerManager->voiceLanguage() :
                sds::SpeakerVoiceLanguage::English;

        if (configured !=
            sds::SpeakerVoiceLanguage::Auto) {
            return configured;
        }

        try {
            bool useLocaleVoices = false;

            const auto* localeVoiceSetting =
                RE::GetINISetting(
                    "bUseLocaleVoices:Controls");

            if (localeVoiceSetting &&
                localeVoiceSetting->Is<bool>()) {
                useLocaleVoices =
                    localeVoiceSetting->GetBool();
            }

            if (!useLocaleVoices) {
                return
                    sds::SpeakerVoiceLanguage::English;
            }

            const auto* languageSetting =
                RE::GetINISetting(
                    "sLanguage:General");

            if (languageSetting &&
                languageSetting->Is<std::string_view>()) {
                return
                    sds::resolveAutoSpeakerVoiceLanguage(
                        true,
                        languageSetting->GetString());
            }
        } catch (...) {
        }

        return sds::SpeakerVoiceLanguage::English;
    }

    void applyImmediateLiveSettings(const sds::Config& config) noexcept
    {
        // Capture the old live enable states before configure*() applies the
        // newly selected SFSE menu values. The custom registries themselves
        // already support enable/disable correctly; the missing piece was
        // re-arming the weapon that is currently equipped.
        static std::atomic_bool lastCustomHapticsEnabled{ true };
        static std::atomic_bool lastCustomTriggersEnabled{ true };
        const bool customFeedbackChanged =
            lastCustomHapticsEnabled.exchange(
                config.customWeaponsEnabled,
                std::memory_order_acq_rel) != config.customWeaponsEnabled;
        const bool customTriggersChanged =
            lastCustomTriggersEnabled.exchange(
                config.customWeaponAdaptiveTriggersEnabled,
                std::memory_order_acq_rel) != config.customWeaponAdaptiveTriggersEnabled;

        const bool customSpeakerChanged =
            sds::customWeaponSpeakerAudioEnabled() !=
            config.customWeaponSpeakerAudioEnabled;

        sds::setCustomWeaponReloadHapticsEnabled(config.customWeaponsEnabled);
        bool customMappingsReloaded = false;

        // Custom mappings are TOML-authored. Re-read the mapping table when
        // the SFSE menu applies or reloads settings so edits can go live
        // without creating dynamic per-weapon menu controls.
        try {
            const auto customWeaponText =
                readTextFile(
                    std::filesystem::path(
                        kConfigPath));

            // Keep the shared profile mapping registered independently of
            // either output switch; the haptics and trigger engines gate output.
            (void)sds::configureCustomWeaponProfiles(
                true,
                customWeaponText);

            (void)sds::configureCustomWeaponSpeakerProfiles(
                config.customWeaponSpeakerAudioEnabled,
                customWeaponText);

            customMappingsReloaded = true;
        } catch (...) {
            // Leave the previously parsed mapping set intact on an unexpected
            // file-read/allocation failure. The checkbox itself will apply on
            // the next successful reload.
        }

        g_nativeDualSenseReconnectFixEnabled.store(
            config.operatingMode == sds::OperatingMode::ReconnectFixOnly ||
                config.dualSenseReconnectFix,
            std::memory_order_release);

        const auto live = sds::immediateLiveSettings(config);

        const bool wasMusicEnabled =
            g_musicHapticsEnabled.exchange(live.musicHapticsEnabled, std::memory_order_acq_rel);
        g_musicHapticsStrength.store(live.musicHapticsBaseStrength, std::memory_order_release);
        g_musicHapticsUserScale.store(live.musicHapticsUserScale, std::memory_order_release);

        if (wasMusicEnabled && !live.musicHapticsEnabled) {
            musicHapticsClearAuthority();
        }

        if (g_speakerManager) {
            g_speakerManager->setSpeakerVolume(
                live.speakerVolume);
        } else if (g_audioTransport) {
            // Startup/fail-soft fallback before the speaker manager exists.
            g_audioTransport->setSpeakerVolume(
                live.speakerVolume);
        }

        if (g_controller) {
            g_controller->applyLiveSettings(
                sds::controllerLiveSettings(config));

            g_controller->applyBluetoothPowerSettings(
                config.bluetoothPowerOffOnExit,
                config.bluetoothIdleTimeoutEnabled,
                config.bluetoothIdleTimeoutMinutes);
        }

        const bool previousCommsEnabled =

            g_speakerManager &&

            g_speakerManager->categoryEnabled(

                sds::SpeakerCategory::Comms);



        const auto previousOutputMode =

            g_speakerManager ?

                g_speakerManager->outputMode() :

                config.speakerOutputMode;



        const auto previousVoiceLanguage =

            g_speakerManager ?

                g_speakerManager->voiceLanguage() :

                config.speakerVoiceLanguage;



        const auto speakerLive = sds::controllerSpeakerLiveSettings(config);



        const bool nextCommsEnabled =

            speakerLive.controllerSpeaker &&

            speakerLive.speakerComms;



        const bool dataslateSpeakerPolicyChanged =

            g_speakerManager &&

            (

                previousCommsEnabled != nextCommsEnabled ||

                (

                    (previousCommsEnabled || nextCommsEnabled) &&

                    (

                        previousOutputMode != speakerLive.outputMode ||

                        previousVoiceLanguage != speakerLive.speakerVoiceLanguage

                    )

                )

            );



        // Native dataslate silence is a ControllerOnly + Comms policy.

        // Disable it for Both, Comms-off, or ControllerSpeaker-off so

        // Starfield keeps its normal audio route.

        if (g_audioCapture) {

            g_audioCapture->setDataslateControllerOnlyRoutingEnabled(

                nextCommsEnabled &&

                speakerLive.outputMode ==

                    sds::SpeakerOutputMode::ControllerOnly);

        }



        if (speakerLive.controllerSpeaker) {

            // Queue the native route first. The controller worker owns the

            // physical backend mutation; speaker playback then enables against

            // the already-requested route.

            if (g_controller) {

                g_controller->setControllerSpeakerRoutingEnabled(true);

            }

            if (g_speakerManager) {

                g_speakerManager->applyLiveSettings(speakerLive);

            }

        } else {

            // Fail closed: stop/clear controller-speaker playback before the

            // controller worker releases the native speaker route.

            if (g_speakerManager) {

                g_speakerManager->applyLiveSettings(speakerLive);

            }

            if (g_controller) {

                g_controller->setControllerSpeakerRoutingEnabled(false);

            }

        }



        if (dataslateSpeakerPolicyChanged) {

            g_dataslateControllerVoiceActive.store(

                false,

                std::memory_order_release);



            g_dataslatePauseRequested.store(

                false,

                std::memory_order_release);



            {

                std::scoped_lock lock(

                    g_dataslateVoiceQueueMutex);



                g_dataslateVoiceQueue.clear();

                g_dataslatePlaybackDeadline = {};

                g_dataslateContinuationDeadline = {};

            }



        }
        if (g_haptics) {
            // Keep exact Auto-Rivet charge semantics available while gameplay
            // haptic output is muted so a later live re-enable has current state.
            g_autoRivetChargeCaptureEnabled.store(true, std::memory_order_release);
            g_haptics->applyLiveSettings(
                sds::gameplayHapticsLiveSettings(config));
        }

        if (
            customMappingsReloaded &&
            (
                customFeedbackChanged ||
                customTriggersChanged ||
                customSpeakerChanged
            )) {

            const bool refreshed =
                g_gameState &&
                g_fireMarkerBridge &&
                refreshCurrentEquippedWeaponState(
                    nullptr);

            std::ostringstream line;
            line
                << "Custom weapons: live feedbackEnabled="
                << (
                    config.customWeaponsEnabled ?
                        "yes" :
                        "no"
                )
                << " triggersEnabled="
                << (config.customWeaponAdaptiveTriggersEnabled ? "yes" : "no")
                << " speakerEnabled="
                << (
                    config.customWeaponSpeakerAudioEnabled ?
                        "yes" :
                        "no"
                )
                << " currentWeaponRefresh="
                << (
                    refreshed ?
                        "yes" :
                        "no"
                )
                << " source=SFSE-live";

            pluginLog(
                line.str());
        }
    }
    std::array<std::atomic<std::uint32_t>, kMusicHapticsAuthoritySlots> g_musicHapticsPlayingIds{};
    std::atomic<std::uint32_t> g_shipWeaponCaptureProbeCount{ 0 };
    std::atomic<std::uint32_t> g_shipEmReconLogCount{ 0 };
    std::atomic<std::uint32_t> g_landVehicleRareWwiseLogCount{ 0 };
    sds::LandVehicleWwiseReconAggregator g_landVehicleWwiseAggregator{};
    std::mutex g_landVehicleWwiseAggregatorMutex{};
    sds::LandVehicleActionGate g_landVehicleActionGate{};
    std::mutex g_landVehicleActionGateMutex{};
    std::atomic_bool g_shipEmReconEnabled{ false };
    std::atomic_bool g_shipLaunchLandingReconEnabled{ false };
    std::atomic_bool g_onFootRefreshPending{ false };
    std::atomic_bool g_lastShipStateValid{ false };
    std::atomic_bool g_lastShipLanded{ false };
    std::atomic_bool g_lastShipDocked{ false };
    std::atomic<ShipLaunchLandingRumblePhase> g_shipLaunchLandingRumblePhase{ ShipLaunchLandingRumblePhase::None };
    std::atomic_bool g_shipTakeoffFirstFaderClosed{ false };
    std::atomic_bool g_shipTakeoffBoundaryObserved{ false };
    std::atomic<std::int64_t> g_shipLaunchLandingStartedAtUs{ 0 };
    bool g_runtimeShutdown = false;

    // TEMP DIAGNOSTIC:
    // Valid Starfield gamepad object used only to provide Bluetooth
    // controller presence/identity. Its normal controller polling
    // vfunc is replaced with a no-op so SAD remains sole HID owner.
    std::uintptr_t g_bluetoothPresentationHandlerAddress = 0;
    void* g_bluetoothShadowDelegate = nullptr;
    std::array<std::uintptr_t, 19> g_bluetoothShadowVtable{};
    bool g_bluetoothShadowDelegateLogged = false;
    bool g_bluetoothNativeHandoffRefreshPending = false;
    std::uint32_t g_bluetoothNativeHandoffRefreshAttempts = 0;
    bool g_bluetoothNativeHandoffExhaustedLogged = false;
    std::chrono::steady_clock::time_point
        g_bluetoothNativeHandoffRefreshDue{};

    // USB -> Bluetooth recovery:
    // Starfield can retain its native USB DualSense delegate after the USB
    // transport disappears. When one guarded selector refresh still leaves
    // that known native object installed, temporarily park the pointer and
    // install SAD's already-proven Bluetooth shadow. Never free or modify the
    // parked native object's vtable. Restore it when SAD leaves Bluetooth.
    std::uintptr_t g_bluetoothParkedNativeDelegate = 0;
    std::uint8_t g_bluetoothParkedNativeActive = 0;
    std::uint8_t g_bluetoothParkedHandlerPresent = 0;
    std::int32_t g_bluetoothParkedHandlerStatus = -1;

    std::atomic_bool g_startupWeaponBootstrapComplete{ false };
    // NG+ replaces the live player/world between the end credits and the new
    // gameplay HUD. Game-world polling must not run against that transition.
    std::atomic_bool g_ngPlusTransitionActive{ false };
    bool g_weaponAudioPipelineEnabled = false;
    std::filesystem::path g_weaponAudioPipelineDataPath{};

    void pluginLog(std::string_view message) noexcept
    {
        try {
            REX::INFO("{}", message);
        } catch (...) {
            // Logging cannot be allowed to escape into SFSE callbacks.
        }
    }


    void drainRemoteVoPlayback()
    {
        if (!g_remoteVoAudioPipeline || !g_speakerManager) {
            return;
        }
        auto ready = g_remoteVoAudioPipeline->tryTake();
        if (!ready) {
            return;
        }

        const auto& request = ready->request;
        auto& playback = ready->playback;
        pluginLog(sds::formatRemoteVoControllerPlaybackPreparation(playback));
        const auto latencyMs = [&request](std::chrono::steady_clock::time_point when) noexcept -> long long {
            if (request.captureSteadyMicros <= 0) {
                return -1;
            }
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(when.time_since_epoch()).count();
            return (micros - request.captureSteadyMicros) / 1000;
        };

        const bool speakerActive = g_speakerManager->active();
        const auto applied = sds::applyRemoteVoAudio(
            *ready, *g_speakerManager, resolvedSpeakerVoiceLanguage(),
            !g_remoteVoBlocked, sds::isWwisePlayingIdActive, sds::stopWwisePlayingId);
        if (applied.status == sds::RemoteVoPlaybackStatus::Skipped) {
            return;
        }
        const bool submitted = applied.status == sds::RemoteVoPlaybackStatus::Submitted;
        const auto submitNow = std::chrono::steady_clock::now();
        const bool replacedActive = submitted &&
            g_remoteVoPlaybackDeadline.time_since_epoch().count() != 0 &&
            submitNow < g_remoteVoPlaybackDeadline;
        if (submitted) {
            g_remoteVoPlaybackDeadline = submitNow + std::chrono::milliseconds(playback.outputDurationMs);
        }

        const auto speakerOutputMode = g_speakerManager->outputMode();
        const bool originalStopIssued = applied.originalStopped;

        const char* outputModeName = speakerOutputMode == sds::SpeakerOutputMode::ControllerOnly
            ? "ControllerOnly"
            : "Both";
        const char* originalOutput = "passthrough-both";
        if (speakerOutputMode == sds::SpeakerOutputMode::ControllerOnly) {
            if (!submitted) {
                originalOutput = "failsafe-passthrough-submit-rejected";
            } else {
                originalOutput = originalStopIssued ? "stopped" : "failsafe-passthrough-stop-unavailable";
            }
        }

        char playbackLog[704]{};
        std::snprintf(
            playbackLog,
            sizeof(playbackLog),
            "Voice controller playback: seq=%llu submit=%s speakerManager=yes speakerActive=%s replacedActive=%s sourceFrames=%u sourceRate=%u outputFrames=%zu outputRate=%u durationMs=%llu captureToPreparedMs=%lld captureToSubmitMs=%lld category=Comms continuous=yes speakerOutputMode=%s originalPlayingId=%u originalOutput=%s",
            static_cast<unsigned long long>(request.sequence),
            submitted ? "accepted" : "rejected",
            speakerActive ? "yes" : "no",
            replacedActive ? "yes" : "no",
            playback.sourceFrames,
            playback.sourceSampleRate,
            playback.outputFrames,
            playback.outputSampleRate,
            static_cast<unsigned long long>(playback.outputDurationMs),
            latencyMs(ready->preparedAt),
            latencyMs(submitNow),
            outputModeName,
            request.originalPlayingId,
            originalOutput);
        pluginLog(playbackLog);
    }

    enum class GamepadProbeKind : std::uint8_t
    {
        Other,
        DualSense,
        GenericGamepad,
        GamepadHandler,
        BaseGamepad,
    };

    struct GamepadProbeVtables
    {
        std::uintptr_t dualSense{ 0 };
        std::uintptr_t genericGamepad{ 0 };
        std::uintptr_t gamepadHandler{ 0 };
        std::uintptr_t baseGamepad{ 0 };
        std::array<std::uintptr_t, 3> inputManagers{};
    };

    std::uintptr_t resolveGamepadProbeId(std::uint64_t id) noexcept
    {
        try {
            REL::Relocation<std::uintptr_t> relocation{ REL::ID(id) };
            return relocation.address();
        } catch (...) {
            return 0;
        }
    }

    GamepadProbeVtables gamepadProbeVtables() noexcept
    {
        return {
            .dualSense = resolveGamepadProbeId(1037215),
            .genericGamepad = resolveGamepadProbeId(470133),
            .gamepadHandler = resolveGamepadProbeId(470111),
            .baseGamepad = resolveGamepadProbeId(470102),
            .inputManagers = {
                resolveGamepadProbeId(469745),
                resolveGamepadProbeId(469743),
                resolveGamepadProbeId(469747),
            },
        };
    }

    GamepadProbeKind classifyGamepadProbeVtable(
        std::uintptr_t vtable,
        const GamepadProbeVtables& known) noexcept
    {
        if (vtable != 0 && vtable == known.dualSense) {
            return GamepadProbeKind::DualSense;
        }
        if (vtable != 0 && vtable == known.genericGamepad) {
            return GamepadProbeKind::GenericGamepad;
        }
        if (vtable != 0 && vtable == known.gamepadHandler) {
            return GamepadProbeKind::GamepadHandler;
        }
        if (vtable != 0 && vtable == known.baseGamepad) {
            return GamepadProbeKind::BaseGamepad;
        }
        return GamepadProbeKind::Other;
    }

    std::string_view gamepadProbeKindName(GamepadProbeKind kind) noexcept
    {
        switch (kind) {
        case GamepadProbeKind::DualSense:
            return "DualSense";
        case GamepadProbeKind::GenericGamepad:
            return "GenericGamepad";
        case GamepadProbeKind::GamepadHandler:
            return "GamepadHandler";
        case GamepadProbeKind::BaseGamepad:
            return "BaseGamepad";
        default:
            return "Other";
        }
    }

    constexpr std::uintptr_t kNativeGamepadSelectorRva = 0x22FE670u;
    constexpr std::uint32_t kNativeDualSenseReselectionSettleMs = 250;
    constexpr std::uint32_t kNativeDualSenseDiscoveryRetryMs = 1000;
    constexpr std::uint32_t kBluetoothNativeHandoffMaxAttempts = 1;
    constexpr std::uint32_t kBluetoothNativeHandoffRetryMs = 250;

    bool nativeGamepadSelectorSignatureMatches(
        std::uintptr_t selectorAddress) noexcept
    {
        // Task 5I/5M verified Starfield 1.16.244.0 begins the selector with:
        // 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC B8 00 00 00
        constexpr std::uint8_t expected[] = {
            0x48, 0x89, 0x4C, 0x24, 0x08,
            0x53, 0x55, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00
        };

        std::uint8_t actual[sizeof(expected)]{};
        SIZE_T bytesRead = 0;

        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(selectorAddress),
                actual,
                sizeof(actual),
                &bytesRead) ||
            bytesRead != sizeof(actual)) {
            return false;
        }

        for (std::size_t i = 0; i < sizeof(expected); ++i) {
            if (actual[i] != expected[i]) {
                return false;
            }
        }

        return true;
    }

    bool readGamepadProbePointer(
        std::uintptr_t address,
        std::uintptr_t& value) noexcept;

    bool isInputManagerVtable(
        std::uintptr_t vtable,
        const GamepadProbeVtables& known) noexcept;

    bool discoverNativeDualSenseReselectionHandler() noexcept
    {
        const auto known = gamepadProbeVtables();
        if (known.dualSense == 0 ||
            known.gamepadHandler == 0) {
            return false;
        }

        const auto module =
            reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));
        if (module == 0) {
            return false;
        }

        const auto* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }

        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                module +
                static_cast<std::uintptr_t>(
                    dos->e_lfanew));
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }

        const auto* section = IMAGE_FIRST_SECTION(nt);

        for (unsigned i = 0;
             i < nt->FileHeader.NumberOfSections;
             ++i) {
            const auto characteristics =
                section[i].Characteristics;

            if ((characteristics & IMAGE_SCN_MEM_WRITE) == 0 ||
                (characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0) {
                continue;
            }

            const auto start =
                module + section[i].VirtualAddress;
            const auto size =
                static_cast<std::size_t>(
                    section[i].Misc.VirtualSize != 0
                        ? section[i].Misc.VirtualSize
                        : section[i].SizeOfRawData);
            const auto end = start + size;

            auto managerAddress =
                (start + 7u) &
                ~static_cast<std::uintptr_t>(7u);

            for (;
                 managerAddress +
                         sizeof(std::uintptr_t) <= end;
                 managerAddress +=
                     sizeof(std::uintptr_t)) {
                std::uintptr_t managerVtable = 0;
                if (!readGamepadProbePointer(
                        managerAddress,
                        managerVtable) ||
                    !isInputManagerVtable(
                        managerVtable,
                        known)) {
                    continue;
                }

                std::uintptr_t handlerAddress = 0;
                if (!readGamepadProbePointer(
                        managerAddress + 0x78u,
                        handlerAddress) ||
                    handlerAddress < 0x10000u) {
                    continue;
                }

                std::uintptr_t handlerVtable = 0;
                if (!readGamepadProbePointer(
                        handlerAddress,
                        handlerVtable) ||
                    handlerVtable !=
                        known.gamepadHandler) {
                    continue;
                }

                std::uintptr_t delegateAddress = 0;
                if (!readGamepadProbePointer(
                        handlerAddress + 0xC0u,
                        delegateAddress) ||
                    delegateAddress < 0x10000u) {
                    continue;
                }

                std::uintptr_t delegateVtable = 0;
                if (!readGamepadProbePointer(
                        delegateAddress,
                        delegateVtable) ||
                    delegateVtable != known.dualSense) {
                    continue;
                }

                g_nativeDualSenseReselectionHandlerAddress =
                    handlerAddress;
                g_nativeDualSenseReselectionStartedFromDualSense =
                    true;

                std::ostringstream line;
                line << "Native DualSense reconnect:"
                     << " armed=yes"
                     << " source=BSInputDeviceManager+0x78"
                     << " handler=0x"
                     << std::hex << std::uppercase
                     << handlerAddress
                     << " initialDelegate=0x"
                     << delegateAddress
                     << std::dec
                     << " policy=one-attempt-per-physical-reconnect"
                     << " settleMs=250";
                pluginLog(line.str());
                return true;
            }
        }

        return false;
    }

    void updateNativeDualSenseReselection(
        std::chrono::steady_clock::time_point now) noexcept
    {
        if (!g_nativeDualSenseReconnectFixEnabled.load(std::memory_order_acquire)) {
            g_nativeDualSenseReselectionSawDisconnect = false;
            g_nativeDualSenseReselectionPending = false;
            g_nativeDualSenseReselectionAttemptedThisCycle = false;
            return;
        }

        if (!g_controller) {
            return;
        }

        const bool connected =
            g_controller->connected();


// Starfield has no native Bluetooth DualSense path to reselect.

// SAD owns Bluetooth independently, so skip native Starfield

// handler discovery while the active SAD transport is Bluetooth.

if (connected && g_controller->bluetoothTransport()) {

    g_nativeDualSenseReselectionPending = false;


    static bool bluetoothReselectionBypassLogged = false;

    if (!bluetoothReselectionBypassLogged) {

        pluginLog(

            "Native DualSense reconnect: bypassed transport=Bluetooth reason=no-Starfield-native-Bluetooth-DualSense");

        bluetoothReselectionBypassLogged = true;

    }

    return;

}


        if (!g_nativeDualSenseReselectionStartedFromDualSense) {
            if (!connected ||
                now <
                    g_nativeDualSenseReselectionDiscoveryDue) {
                return;
            }

            g_nativeDualSenseReselectionDiscoveryDue =
                now + std::chrono::milliseconds(
                    kNativeDualSenseDiscoveryRetryMs);

            if (!discoverNativeDualSenseReselectionHandler()) {
                return;
            }
        }

        if (g_nativeDualSenseReselectionHandlerAddress == 0) {
            return;
        }

        if (!connected) {
            if (!g_nativeDualSenseReselectionSawDisconnect) {
                g_nativeDualSenseReselectionSawDisconnect = true;
                g_nativeDualSenseReselectionPending = false;
                g_nativeDualSenseReselectionAttemptedThisCycle = false;
                ++g_nativeDualSenseReselectionCycle;
            } else {
                g_nativeDualSenseReselectionPending = false;
            }
            return;
        }

        if (!g_nativeDualSenseReselectionSawDisconnect ||
            g_nativeDualSenseReselectionAttemptedThisCycle) {
            return;
        }

        if (!g_nativeDualSenseReselectionPending) {
            g_nativeDualSenseReselectionPending = true;
            g_nativeDualSenseReselectionDue =
                now + std::chrono::milliseconds(
                    kNativeDualSenseReselectionSettleMs);

            std::ostringstream line;
            line << "Native DualSense reconnect:"
                 << " cycle="
                 << g_nativeDualSenseReselectionCycle
                 << " state=scheduled"
                 << " settleMs=250"
                 << " attempts=0/1";
            pluginLog(line.str());
            return;
        }

        if (now < g_nativeDualSenseReselectionDue) {
            return;
        }

        // One attempt for this physical disconnect/reconnect cycle. Consume
        // it before every guarded exit or native call so no failure can turn
        // into a retry loop.
        g_nativeDualSenseReselectionPending = false;
        g_nativeDualSenseReselectionAttemptedThisCycle = true;
        g_nativeDualSenseReselectionSawDisconnect = false;

        const auto handlerAddress =
            g_nativeDualSenseReselectionHandlerAddress;

        const auto known = gamepadProbeVtables();

        std::uintptr_t handlerVtable = 0;
        if (!readGamepadProbePointer(
                handlerAddress,
                handlerVtable) ||
            handlerVtable != known.gamepadHandler) {
            g_nativeDualSenseReselectionHandlerAddress = 0;
            g_nativeDualSenseReselectionStartedFromDualSense = false;

            std::ostringstream line;
            line << "Native DualSense reconnect:"
                 << " cycle="
                 << g_nativeDualSenseReselectionCycle
                 << " attempts=1/1"
                 << " result=skipped-invalid-handler";
            pluginLog(line.str());
            return;
        }

        std::uintptr_t beforeDelegate = 0;
        std::uintptr_t beforeVtable = 0;

        readGamepadProbePointer(
            handlerAddress + 0xC0u,
            beforeDelegate);
        if (beforeDelegate != 0) {
            readGamepadProbePointer(
                beforeDelegate,
                beforeVtable);
        }

        const auto beforeKind =
            classifyGamepadProbeVtable(
                beforeVtable,
                known);

        if (beforeKind !=
            GamepadProbeKind::GenericGamepad) {
            std::ostringstream line;
            line << "Native DualSense reconnect:"
                 << " cycle="
                 << g_nativeDualSenseReselectionCycle
                 << " before="
                 << gamepadProbeKindName(beforeKind)
                 << " after="
                 << gamepadProbeKindName(beforeKind)
                 << " attempts=1/1"
                 << " result=skipped-before-not-generic";
            pluginLog(line.str());
            return;
        }

        const auto starfieldBase =
            reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));

        if (starfieldBase == 0) {
            std::ostringstream line;
            line << "Native DualSense reconnect:"
                 << " cycle="
                 << g_nativeDualSenseReselectionCycle
                 << " before=GenericGamepad"
                 << " after=GenericGamepad"
                 << " attempts=1/1"
                 << " result=skipped-no-starfield-base";
            pluginLog(line.str());
            return;
        }

        const auto selectorAddress =
            starfieldBase +
            kNativeGamepadSelectorRva;

        if (!nativeGamepadSelectorSignatureMatches(
                selectorAddress)) {
            std::ostringstream line;
            line << "Native DualSense reconnect:"
                 << " cycle="
                 << g_nativeDualSenseReselectionCycle
                 << " before=GenericGamepad"
                 << " after=GenericGamepad"
                 << " attempts=1/1"
                 << " result=selector-signature-mismatch";
            pluginLog(line.str());
            return;
        }

        using NativeGamepadSelector =
            void (*)(void*);
        const auto selector =
            reinterpret_cast<NativeGamepadSelector>(
                selectorAddress);

        selector(
            reinterpret_cast<void*>(
                handlerAddress));

        std::uintptr_t afterDelegate = 0;
        std::uintptr_t afterVtable = 0;

        readGamepadProbePointer(
            handlerAddress + 0xC0u,
            afterDelegate);
        if (afterDelegate != 0) {
            readGamepadProbePointer(
                afterDelegate,
                afterVtable);
        }

        const auto afterKind =
            classifyGamepadProbeVtable(
                afterVtable,
                known);

        std::ostringstream line;
        line << "Native DualSense reconnect:"
             << " cycle="
             << g_nativeDualSenseReselectionCycle
             << " before="
             << gamepadProbeKindName(beforeKind)
             << " after="
             << gamepadProbeKindName(afterKind)
             << " attempts=1/1"
             << " beforeDelegate=0x"
             << std::hex << std::uppercase
             << beforeDelegate
             << " afterDelegate=0x"
             << afterDelegate
             << std::dec
             << " result=native-selector-called";
        pluginLog(line.str());
    }

    bool isInputManagerVtable(
        std::uintptr_t vtable,
        const GamepadProbeVtables& known) noexcept
    {
        for (const auto candidate : known.inputManagers) {
            if (vtable != 0 && vtable == candidate) {
                return true;
            }
        }
        return false;
    }

    bool readGamepadProbePointer(
        std::uintptr_t address,
        std::uintptr_t& value) noexcept
    {
        value = 0;
        if (address < 0x10000u) {
            return false;
        }

        SIZE_T bytesRead = 0;
        return ReadProcessMemory(
                   GetCurrentProcess(),
                   reinterpret_cast<const void*>(address),
                   &value,
                   sizeof(value),
                   &bytesRead) != FALSE &&
            bytesRead == sizeof(value);
    }

    std::string_view eventIdentity(const sds::GameEvent& event) noexcept
    {
        const auto* begin = event.text.data();
        std::size_t length = 0;
        while (length < event.text.size() && begin[length] != '\0') {
            ++length;
        }
        return { begin, length };
    }

    std::int64_t steadyMicros(std::chrono::steady_clock::time_point when) noexcept
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(when.time_since_epoch()).count();
    }

    std::uint8_t musicHapticsMenuBit(std::string_view menu) noexcept
    {
        if (menu == "MainMenu") {
            return 0x01u;
        }
        if (menu == "DataMenu") {
            return 0x02u;
        }
        if (menu == "PauseMenu") {
            return 0x04u;
        }
        if (menu == "LoadingMenu") {
            return 0x08u;
        }
        if (menu == "FaderMenu") {
            return 0x10u;
        }
        return 0u;
    }

    bool musicHapticsBlocked() noexcept
    {
        return g_musicHapticsBlockingMenuMask.load(std::memory_order_acquire) != 0u;
    }

    bool musicHapticsPlayingIdActive(std::uint32_t playingId) noexcept
    {
        if (playingId == 0u) {
            return false;
        }
        for (const auto& slot : g_musicHapticsPlayingIds) {
            if (slot.load(std::memory_order_acquire) == playingId) {
                return true;
            }
        }
        return false;
    }

    bool musicHapticsMarkPlayingIdActive(std::uint32_t playingId) noexcept
    {
        if (playingId == 0u) {
            return false;
        }
        if (musicHapticsPlayingIdActive(playingId)) {
            return true;
        }
        for (auto& slot : g_musicHapticsPlayingIds) {
            std::uint32_t expected = 0u;
            if (slot.compare_exchange_strong(
                    expected, playingId, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
            if (expected == playingId) {
                return true;
            }
        }
        return false;
    }

    void musicHapticsErasePlayingId(std::uint32_t playingId) noexcept
    {
        if (playingId == 0u) {
            return;
        }
        for (auto& slot : g_musicHapticsPlayingIds) {
            auto current = slot.load(std::memory_order_acquire);
            while (current == playingId &&
                   !slot.compare_exchange_weak(
                       current, 0u, std::memory_order_acq_rel, std::memory_order_acquire)) {
            }
        }
    }

    void musicHapticsClearAuthority() noexcept
    {
        for (auto& slot : g_musicHapticsPlayingIds) {
            slot.store(0u, std::memory_order_release);
        }
        if (g_audioTransport) {
            g_audioTransport->clearMusicHaptics();
        }
    }

    void syncMusicHapticsForMenu(std::string_view menu, bool opening) noexcept
    {
        const auto bit = musicHapticsMenuBit(menu);
        if (bit == 0u) {
            return;
        }

        if (opening) {
            (void)g_musicHapticsBlockingMenuMask.fetch_or(bit, std::memory_order_acq_rel);
            musicHapticsClearAuthority();
        } else {
            (void)g_musicHapticsBlockingMenuMask.fetch_and(
                static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
        }
    }

    void initializeMusicHapticsMenuMask() noexcept
    {
        std::uint8_t mask = musicHapticsMenuBit("MainMenu");
        if (auto* ui = RE::UI::GetSingleton()) {
            mask = 0u;
            if (ui->IsMenuOpen(RE::BSFixedString("MainMenu"))) { mask |= musicHapticsMenuBit("MainMenu"); }
            if (ui->IsMenuOpen(RE::BSFixedString("DataMenu"))) { mask |= musicHapticsMenuBit("DataMenu"); }
            if (ui->IsMenuOpen(RE::BSFixedString("PauseMenu"))) { mask |= musicHapticsMenuBit("PauseMenu"); }
            if (ui->IsMenuOpen(RE::BSFixedString("LoadingMenu"))) { mask |= musicHapticsMenuBit("LoadingMenu"); }
            if (ui->IsMenuOpen(RE::BSFixedString("FaderMenu"))) { mask |= musicHapticsMenuBit("FaderMenu"); }
        }
        g_musicHapticsBlockingMenuMask.store(mask, std::memory_order_release);
        if (mask != 0u) {
            musicHapticsClearAuthority();
        }
    }

    const char* shipEmReconPhaseName(sds::ShipLaserReconPhase phase) noexcept
    {
        switch (phase) {
        case sds::ShipLaserReconPhase::PrePress:
            return "pre-press";
        case sds::ShipLaserReconPhase::Held:
            return "held";
        case sds::ShipLaserReconPhase::PostRelease:
            return "post-release";
        default:
            return "unknown";
        }
    }

    void logShipEmReconSample(const sds::ShipLaserReconSample& sample) noexcept
    {
        if (!g_shipEmReconEnabled.load(std::memory_order_acquire)) {
            return;
        }
        const auto ordinal = g_shipEmReconLogCount.fetch_add(1, std::memory_order_relaxed);
        if (ordinal >= kShipEmReconLogLimit) {
            if (ordinal == kShipEmReconLogLimit) {
                pluginLog("Ship EM recon: Wwise log limit reached; further raw samples suppressed, burst summaries remain active");
            }
            return;
        }

        const bool knownBallistic = sds::isHardwareObservedShipBallisticFireEvent(sample.eventId) ||
            (g_shipWeaponSemanticCache && g_shipWeaponSemanticCache->isPlayerBallisticFire(sample.eventId));
        const bool knownLaser = sds::isHardwareObservedShipPulseLaserFireEvent(sample.eventId);
        const bool knownParticle = sds::isHardwareObservedShipProtonBeamFireEvent(sample.eventId);
        const bool knownMissile = sds::isHardwareObservedShipMissileFireEvent(sample.eventId);
        std::ostringstream line;
        line << "Ship EM recon: Wwise burst=" << sample.burstId
             << " phase=" << shipEmReconPhaseName(sample.phase)
             << " seq=" << sample.sequence
             << " event=0x" << std::uppercase << std::hex
             << std::setw(8) << std::setfill('0') << sample.eventId
             << " gameObject=0x" << sample.gameObjectId
             << " callsite=Starfield+0x" << sample.callsiteRva
             << std::dec << std::setfill(' ')
             << " r2=" << static_cast<unsigned>(sample.triggerValue)
             << " deltaUs=" << sample.deltaMicros
             << " knownBallistic=" << (knownBallistic ? "yes" : "no")
             << " knownLaser=" << (knownLaser ? "yes" : "no")
             << " knownParticle=" << (knownParticle ? "yes" : "no")
             << " knownMissile=" << (knownMissile ? "yes" : "no")
             << " returnedPlayingId=" << sample.returnedPlayingId;
        pluginLog(line.str());
    }

    void observeShipEmReconWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_shipEmReconEnabled.load(std::memory_order_acquire)) {
            return;
        }
        const auto sample = g_shipEmReconProbe.observeWwise({
            .sequence = observation.sequence,
            .eventId = observation.eventId,
            .gameObjectId = observation.gameObjectId,
            .callsiteRva = observation.callsiteRva,
            .returnedPlayingId = observation.returnedPlayingId,
            .when = observation.when,
        });
        if (sample) {
            logShipEmReconSample(*sample);
        }
    }

    void logLandVehicleWwiseAggregateSummary(
        const sds::LandVehicleWwiseAggregateSummary& summary,
        std::string_view reason) noexcept
    {
        if (summary.totalAggregated == 0) {
            return;
        }
        std::ostringstream line;
        line << "Land vehicle recon: Wwise aggregate reason=" << reason
             << " total=" << summary.totalAggregated
             << " families=" << summary.bucketCount;
        for (std::size_t i = 0; i < summary.bucketCount; ++i) {
            const auto& bucket = summary.buckets[i];
            line << " [event=0x" << std::uppercase << std::hex
                 << std::setw(8) << std::setfill('0') << bucket.eventId
                 << std::dec << std::setfill(' ')
                 << " count=" << bucket.count
                 << " seq=" << bucket.firstSequence << '-' << bucket.lastSequence
                 << "]";
        }
        line << " authority=correlation-only diagnostic-only controllerOutput=none";
        pluginLog(line.str());
    }

    void resetLandVehicleWwiseRecon(std::chrono::steady_clock::time_point when) noexcept
    {
        std::scoped_lock lock(g_landVehicleWwiseAggregatorMutex);
        g_landVehicleWwiseAggregator.reset(when);
        g_landVehicleRareWwiseLogCount.store(0, std::memory_order_release);
    }

    void flushLandVehicleWwiseRecon(std::string_view reason, std::chrono::steady_clock::time_point when) noexcept
    {
        sds::LandVehicleWwiseAggregateSummary summary{};
        {
            std::scoped_lock lock(g_landVehicleWwiseAggregatorMutex);
            summary = g_landVehicleWwiseAggregator.takeSummary(when);
        }
        logLandVehicleWwiseAggregateSummary(summary, reason);
    }

    void setLandVehicleActionGateAuthority(bool active, std::uint64_t epoch) noexcept
    {
        std::scoped_lock lock(g_landVehicleActionGateMutex);
        g_landVehicleActionGate.setAuthority(active, epoch);
    }

    void resetLandVehicleActionGate() noexcept
    {
        std::scoped_lock lock(g_landVehicleActionGateMutex);
        g_landVehicleActionGate.reset();
    }

    void syncLandVehicleActionGateForMenu(std::string_view menu, bool opening) noexcept
    {
        if (menu != "DataMenu" && menu != "PauseMenu" && menu != "LoadingMenu") {
            return;
        }
        if (opening) {
            setLandVehicleActionGateAuthority(false, 0);
            return;
        }
        if (menu == "LoadingMenu" || !g_gameState) {
            return;
        }
        const auto physics = g_gameState->latestLandVehiclePhysicsSnapshot();
        if (physics.authorityActive && physics.authorityEpoch != 0) {
            setLandVehicleActionGateAuthority(true, physics.authorityEpoch);
        }
    }

    void observeLandVehicleProductionSemantic(
        std::string_view semantic,
        bool active,
        std::chrono::steady_clock::time_point when) noexcept
    {
        // Button-down only expresses player intent. The REV-8 can
        // exhaust its boost/thrust charge while the button remains
        // held, so actual sustained feedback is started/stopped by
        // the observed vehicle Wwise lifecycle below. A physical
        // release remains an immediate fail-safe clear.
        if (g_haptics && !active) {
            if (semantic == "VehicleBoost") {
                (void)g_haptics->setLandVehicleBoostInput(
                    false,
                    false,
                    when);
            } else if (semantic == "VehicleVertBoost") {
                (void)g_haptics->setLandVehicleBoostInput(
                    true,
                    false,
                    when);
            }
        }

        sds::LandVehicleAction action = sds::LandVehicleAction::None;
        {
            std::scoped_lock lock(g_landVehicleActionGateMutex);
            if (semantic == "VehicleFireWeapon") {
                action = g_landVehicleActionGate.observeFireSemantic(active, when);
            }
            if (semantic == "VehicleAim") {
                action = g_landVehicleActionGate.observeAimSemantic(active);
            }
        }

        if (action == sds::LandVehicleAction::None || !g_eventRouter) {
            return;
        }

        sds::GameEvent event{};
        event.when = when;
        switch (action) {
        case sds::LandVehicleAction::GunFired:
            event.type = sds::GameEventType::LandVehicleGunFired;
            break;
        case sds::LandVehicleAction::AimStarted:
            event.type = sds::GameEventType::LandVehicleAimStarted;
            break;
        case sds::LandVehicleAction::AimStopped:
            event.type = sds::GameEventType::LandVehicleAimStopped;
            break;
        case sds::LandVehicleAction::None:
            return;
        }
        (void)g_eventRouter->dispatch(std::move(event));
    }

    std::string_view landVehicleRareWwiseClassification(std::uint32_t eventId) noexcept
    {
        if (eventId == sds::kHardwareObservedLandVehicleGunFireEventId) {
            return "gun-fire-hardware-observed";
        }
        if (eventId == sds::kHardwareObservedLandVehicleVerticalBoostEventId) {
            return "vertical-boost-hardware-observed";
        }
        if (eventId == sds::kLandVehicleTouchdownCandidatePrimaryEventId) {
            return "contact-suspension-corroboration";
        }
        if (eventId == sds::kLandVehicleTouchdownCandidateSecondaryEventId) {
            return "terrain-contact-not-touchdown";
        }
        return "rare-preserved";
    }
    bool isLandVehicleTouchdownCandidate(std::uint32_t eventId) noexcept
    {
        return eventId == sds::kLandVehicleTouchdownCandidatePrimaryEventId ||
            eventId == sds::kLandVehicleTouchdownCandidateSecondaryEventId;
    }

    void stopLandVehicleEngineSpeaker() noexcept
    {
        std::scoped_lock lock(g_landVehicleSpeakerMutex);
        if (g_landVehicleEngineBand != -1 && g_speakerManager) {
            (void)g_speakerManager->clearPersistentCaptured(sds::kRoverEngineSpeakerOwner);
        }
        g_landVehicleEngineBand = -1;
        g_landVehicleEngineEpoch = 0;
        g_landVehicleEngineVolume = -1.0F;
        g_landVehicleThrustKind = 0;
    }

    void updateLandVehicleEngineSpeaker(const sds::LandVehicleMotionState& state) noexcept
    {
        try {
            if (!state.authorityActive || !g_speakerManager || !g_landVehicleSpeakerCache ||
                !g_speakerManager->categoryEnabled(sds::SpeakerCategory::LandVehicle) ||
                g_speakerManager->landVehicleVolume() <= 0.0F) {
                stopLandVehicleEngineSpeaker();
                return;
            }
            const auto sounds = g_landVehicleSpeakerCache->snapshot();
            if (!sounds) return; // Startup decoder has not published yet.
            std::scoped_lock lock(g_landVehicleSpeakerMutex);
            if (g_landVehicleThrustKind != 0) return; // Thrust takes priority over engine.
            const float speed = std::max(0.0F, state.speed);
            // Hysteresis avoids rapid re-starts when hovering near the thresholds.
            int band = speed < 0.60F ? 0 : (speed < 8.0F ? 1 : 2);
            if (g_landVehicleEngineBand == 0 && speed < 1.2F) band = 0;
            if (g_landVehicleEngineBand == 1 && speed > 0.35F && speed < 9.5F) band = 1;
            if (g_landVehicleEngineBand == 2 && speed > 6.5F) band = 2;
            const float volume = g_speakerManager->landVehicleVolume();
            if (band == g_landVehicleEngineBand && state.authorityEpoch == g_landVehicleEngineEpoch &&
                volume == g_landVehicleEngineVolume) return;
            const sds::LandVehicleSpeakerClip* clip = band == 0 ? &sounds->idle :
                (band == 1 ? &sounds->low : &sounds->high);
            sds::PersistentPreparedSpeakerPcm voice{};
            if (!clip->pcm) return; // Allow incomplete WEM decoding without muting the gun.
            voice.owner = sds::kRoverEngineSpeakerOwner;
            voice.pcm = clip->pcm;
            voice.loopResumeFrame = clip->loopResumeFrame;
            voice.gainScale = band == 0 ? 0.33F : (band == 1 ? 0.42F : 0.50F);
            if (g_speakerManager->setPersistentCaptured(std::move(voice), sds::SpeakerCategory::LandVehicle)) {
                g_landVehicleEngineBand = band;
                g_landVehicleEngineEpoch = state.authorityEpoch;
                g_landVehicleEngineVolume = volume;
            }
        } catch (...) { stopLandVehicleEngineSpeaker(); }
    }

    void observeLandVehicleThrustSpeaker(const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        constexpr std::uintptr_t kStartRva = 0xF1C21Du;
        constexpr std::uintptr_t kStopRva = 0xF1C4ABu;
        constexpr std::uint32_t kForwardStart = 0x6F4CEC6Cu;
        constexpr std::uint32_t kForwardStop = 0xE4B57376u;
        constexpr std::uint32_t kVerticalStop = 0x59BD1A2Eu;
        const int startKind = observation.callsiteRva == kStartRva ?
            (observation.eventId == kForwardStart ? 1 :
             observation.eventId == sds::kRoverVerticalBoostEventId ? 2 : 0) : 0;
        const int stopKind = observation.callsiteRva == kStopRva ?
            (observation.eventId == kForwardStop ? 1 :
             observation.eventId == kVerticalStop ? 2 : 0) : 0;
        if (!startKind && !stopKind) return;
        if (!g_speakerManager || !g_landVehicleSpeakerCache) return;
        const auto clips = g_landVehicleSpeakerCache->snapshot();
        if (!clips) return;
        if (!g_speakerManager->categoryEnabled(sds::SpeakerCategory::LandVehicle) ||
            g_speakerManager->landVehicleVolume() <= 0.0F) return;
        std::scoped_lock lock(g_landVehicleSpeakerMutex);
        if (stopKind) {
            if (g_landVehicleThrustKind == stopKind) {
                g_landVehicleThrustKind = 0;
                g_landVehicleEngineBand = -1; // Next vehicle motion tick resumes idle/driving.
                (void)g_speakerManager->clearPersistentCaptured(sds::kRoverEngineSpeakerOwner);
            }
            return;
        }
        if (g_landVehicleThrustKind == startKind) return;
        const auto& clip = startKind == 1 ? clips->forwardThrust : clips->verticalThrust;
        if (!clip.pcm) return;
        sds::PersistentPreparedSpeakerPcm voice{};
        voice.owner = sds::kRoverEngineSpeakerOwner;
        voice.pcm = clip.pcm;
        voice.loopResumeFrame = clip.loopResumeFrame;
        voice.gainScale = startKind == 1 ? 0.45F : 0.55F;
        if (g_speakerManager->setPersistentCaptured(std::move(voice), sds::SpeakerCategory::LandVehicle)) {
            g_landVehicleThrustKind = startKind;
            g_landVehicleEngineBand = -1;
        }
    }

    void observeLandVehicleGunSpeaker(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        const bool deimog = observation.eventId == sds::kDeimogRocketLaunchEventId;
        if (!deimog && observation.eventId != sds::kRoverMountedLaserEventId) return;
        if (observation.externalCount != 0 || observation.hasExternalSources ||
            !g_gameState || !g_gameState->landVehicleReconCorrelationArmed() ||
            !g_speakerManager || !g_speakerManager->categoryEnabled(sds::SpeakerCategory::LandVehicle) ||
            !g_landVehicleSpeakerCache) return;
        const auto sounds = g_landVehicleSpeakerCache->snapshot();
        if (!sounds) return;
        const sds::PreparedSpeakerPcm* selected = nullptr;
        std::uint32_t selectedMediaId = 543730516u; // Existing mounted laser, unchanged.
        {
            std::scoped_lock lock(g_landVehicleSpeakerMutex);
            if (g_landVehicleLastGun != std::chrono::steady_clock::time_point{} &&
                observation.when >= g_landVehicleLastGun &&
                observation.when - g_landVehicleLastGun < std::chrono::milliseconds(85)) return;
            if (deimog) {
                // Exact, Creation-owned firing media, in the same order as prewarm.
                constexpr std::array<std::uint32_t, 4> kRocketMediaIds{{
                    872335090u, 530932654u, 1007612472u, 564989739u,
                }};
                static std::size_t nextDeimogVariant = 0u; // Guarded by speaker mutex.
                for (std::size_t offset = 0; offset < kRocketMediaIds.size(); ++offset) {
                    const auto index = (nextDeimogVariant + offset) % kRocketMediaIds.size();
                    if (sounds->deimogRocketShots[index]) {
                        selected = sounds->deimogRocketShots[index].get();
                        selectedMediaId = kRocketMediaIds[index];
                        nextDeimogVariant = (index + 1u) % kRocketMediaIds.size();
                        break;
                    }
                }
            } else if (sounds->laser) {
                selected = sounds->laser.get();
            }
            if (!selected) return; // Creation absent? No sound; all other features remain.
            g_landVehicleLastGun = observation.when;
        }
        auto pcm = *selected;
        pcm.gain *= 0.65F;
        (void)g_speakerManager->submitCaptured(
            std::move(pcm), sds::SpeakerCategory::LandVehicle,
            {.id = (static_cast<std::uint64_t>(observation.eventId) << 32u) | selectedMediaId,
             .controllerOnlySafe = false}, false);
    }

    void observeLandVehicleReconWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_gameState || !g_gameState->landVehicleReconCorrelationArmed()) {
            return;
        }
        observeLandVehicleGunSpeaker(observation);
        observeLandVehicleThrustSpeaker(observation);

        // Hardware-observed REV-8 boost/thrust lifecycle.
        // Unlike the input semantic, these events follow actual
        // vehicle output and stop when the available charge ends
        // even if the player continues holding the button.
        constexpr std::uint32_t kRev8ForwardBoostStartEventId =
            0x6F4CEC6Cu;

        constexpr std::uint32_t kRev8ForwardBoostStopEventId =
            0xE4B57376u;

        constexpr std::uint32_t kRev8VerticalBoostStopEventId =
            0x59BD1A2Eu;

        constexpr std::uintptr_t kRev8BoostStartCallsiteRva =
            0xF1C21Du;

        constexpr std::uintptr_t kRev8BoostStopCallsiteRva =
            0xF1C4ABu;

        const bool rev8BoostStartCallsite =
            observation.callsiteRva ==
                kRev8BoostStartCallsiteRva;

        const bool rev8BoostStopCallsite =
            observation.callsiteRva ==
                kRev8BoostStopCallsiteRva;

        if (g_haptics) {
            if (rev8BoostStartCallsite &&
                observation.eventId ==
                    kRev8ForwardBoostStartEventId) {

                (void)g_haptics->setLandVehicleBoostInput(
                    false,
                    true,
                    observation.when);
            }
            else if (rev8BoostStopCallsite &&
                     observation.eventId ==
                         kRev8ForwardBoostStopEventId) {

                (void)g_haptics->setLandVehicleBoostInput(
                    false,
                    false,
                    observation.when);
            }
            else if (rev8BoostStartCallsite &&
                     observation.eventId ==
                         sds::kHardwareObservedLandVehicleVerticalBoostEventId) {

                (void)g_haptics->setLandVehicleBoostInput(
                    true,
                    true,
                    observation.when);
            }
            else if (rev8BoostStopCallsite &&
                     observation.eventId ==
                         kRev8VerticalBoostStopEventId) {

                (void)g_haptics->setLandVehicleBoostInput(
                    true,
                    false,
                    observation.when);
            }
        }

        if (observation.eventId == sds::kHardwareObservedLandVehicleVerticalBoostEventId) {
            (void)g_gameState->armLandVehicleVerticalBoost();
        }

        sds::LandVehicleAction productionAction = sds::LandVehicleAction::None;
        {
            std::scoped_lock lock(g_landVehicleActionGateMutex);
            productionAction = g_landVehicleActionGate.observeWwise(observation.eventId, observation.when);
        }
        if (productionAction == sds::LandVehicleAction::GunFired && g_eventRouter) {
            sds::GameEvent event{};
            event.type = sds::GameEventType::LandVehicleGunFired;
            event.when = observation.when;
            (void)g_eventRouter->dispatch(std::move(event));
        }

        sds::LandVehicleWwiseReconObserveResult result{};
        std::optional<sds::LandVehicleWwiseAggregateSummary> summary{};
        {
            std::scoped_lock lock(g_landVehicleWwiseAggregatorMutex);
            result = g_landVehicleWwiseAggregator.observe({
                .sequence = observation.sequence,
                .eventId = observation.eventId,
                .gameObjectId = observation.gameObjectId,
                .callsiteRva = observation.callsiteRva,
                .returnedPlayingId = observation.returnedPlayingId,
                .when = observation.when,
            });
            summary = g_landVehicleWwiseAggregator.takeSummaryIfDue(observation.when);
        }

        if (summary) {
            logLandVehicleWwiseAggregateSummary(*summary, "periodic");
        }
        if (!result.preserveIndividually) {
            return;
        }

        const auto index = g_landVehicleRareWwiseLogCount.fetch_add(1, std::memory_order_acq_rel);
        if (index >= kLandVehicleRareWwiseLogLimit) {
            return;
        }
        const auto classification = landVehicleRareWwiseClassification(observation.eventId);
        std::ostringstream line;
        line << "Land vehicle recon: Wwise rare seq=" << observation.sequence
             << " event=0x" << std::uppercase << std::hex
             << std::setw(8) << std::setfill('0') << observation.eventId
             << " gameObject=0x" << observation.gameObjectId
             << " callsite=Starfield+0x" << observation.callsiteRva
             << std::dec << std::setfill(' ')
             << " returnedPlayingId=" << observation.returnedPlayingId
             << " classification=" << classification;
        if (isLandVehicleTouchdownCandidate(observation.eventId) && g_gameState) {
            const auto telemetry = g_gameState->latestLandVehicleTelemetrySnapshot();
            line << " telemetryRef=0x" << std::uppercase << std::hex
                 << std::setw(8) << std::setfill('0') << telemetry.referenceFormId
                 << std::dec << std::setfill(' ')
                 << " positionReadable=" << (telemetry.positionReadable ? "yes" : "no")
                 << " velocityReadable=" << (telemetry.velocityReadable ? "yes" : "no")
                 << " speed=" << std::fixed << std::setprecision(3) << telemetry.speed
                 << " verticalSpeed=" << telemetry.velocityZ
                 << " accelerationReadable=" << (telemetry.accelerationReadable ? "yes" : "no")
                 << " acceleration=" << telemetry.acceleration
                 << " sampleMs=" << (telemetry.sampleIntervalSeconds * 1000.0F);
        }
        line << " authority=correlation-only diagnostic-only controllerOutput=none";
        pluginLog(line.str());
    }

    void logShipEmReconSummary(const sds::ShipLaserReconBurstSummary& summary) noexcept
    {
        for (const auto& event : summary.events) {
            std::string phases;
            if (event.sawPrePress) phases += "pre";
            if (event.sawHeld) phases += phases.empty() ? "held" : "+held";
            if (event.sawPostRelease) phases += phases.empty() ? "post" : "+post";

            const bool knownBallistic = sds::isHardwareObservedShipBallisticFireEvent(event.eventId) ||
                (g_shipWeaponSemanticCache && g_shipWeaponSemanticCache->isPlayerBallisticFire(event.eventId));
            const bool knownLaser = sds::isHardwareObservedShipPulseLaserFireEvent(event.eventId);
            const bool knownParticle = sds::isHardwareObservedShipProtonBeamFireEvent(event.eventId);
            const bool knownMissile = sds::isHardwareObservedShipMissileFireEvent(event.eventId);
            std::ostringstream line;
            line << "Ship EM recon summary: burst=" << summary.burstId
                 << " heldUs=" << summary.heldDurationMicros
                 << " event=0x" << std::uppercase << std::hex
                 << std::setw(8) << std::setfill('0') << event.eventId
                 << " gameObject=0x" << event.gameObjectId
                 << std::dec << std::setfill(' ')
                 << " posts=" << event.posts
                 << " firstDeltaUs=" << event.firstDeltaMicros
                 << " lastDeltaUs=" << event.lastDeltaMicros
                 << " minIntervalUs=" << event.minIntervalMicros
                 << " maxIntervalUs=" << event.maxIntervalMicros
                 << " phases=" << (phases.empty() ? "none" : phases)
                 << " knownBallistic=" << (knownBallistic ? "yes" : "no")
                 << " knownLaser=" << (knownLaser ? "yes" : "no")
                 << " knownParticle=" << (knownParticle ? "yes" : "no")
                 << " knownMissile=" << (knownMissile ? "yes" : "no")
                 << " interpretation=diagnostic-only";
            pluginLog(line.str());
        }
        if (summary.events.empty()) {
            std::ostringstream line;
            line << "Ship EM recon summary: burst=" << summary.burstId
                 << " heldUs=" << summary.heldDurationMicros
                 << " events=0 interpretation=no-zero-external-Wwise-posts-in-window diagnostic-only";
            pluginLog(line.str());
        }
    }

    const char* shipLaunchLandingTransitionName(sds::ShipLaunchLandingTransition type) noexcept
    {
        switch (type) {
        case sds::ShipLaunchLandingTransition::Takeoff:
            return "takeoff";
        case sds::ShipLaunchLandingTransition::Touchdown:
            return "touchdown";
        case sds::ShipLaunchLandingTransition::LandingSequence:
            return "landing-sequence";
        default:
            return "unknown";
        }
    }

    const char* shipLaunchLandingPhaseName(sds::ShipLaunchLandingReconPhase phase) noexcept
    {
        switch (phase) {
        case sds::ShipLaunchLandingReconPhase::PreBoundary:
            return "pre";
        case sds::ShipLaunchLandingReconPhase::PostBoundary:
            return "post";
        default:
            return "unknown";
        }
    }

    void logShipLaunchLandingBoundary(const sds::ShipLaunchLandingReconBoundary& boundary) noexcept
    {
        std::ostringstream line;
        line << "Ship launch/landing recon: boundary=" << shipLaunchLandingTransitionName(boundary.type)
             << " transition=" << boundary.transitionId
             << " beforeLanded=" << (boundary.before.landed ? "yes" : "no")
             << " afterLanded=" << (boundary.after.landed ? "yes" : "no")
             << " beforeDocked=" << (boundary.before.docked ? "yes" : "no")
             << " afterDocked=" << (boundary.after.docked ? "yes" : "no")
             << " throttleTarget=" << boundary.after.throttleTarget
             << " throttleReadable=" << (boundary.after.throttleTargetReadable ? "yes" : "no")
             << " effectiveThrottle=" << boundary.after.effectiveThrottle
             << " effectiveReadable=" << (boundary.after.effectiveThrottleReadable ? "yes" : "no")
             << " velocity=" << boundary.after.velocity
             << " velocityReadable=" << (boundary.after.velocityReadable ? "yes" : "no")
             << " interpretation=state-boundary-only diagnostic-only";
        pluginLog(line.str());
    }

    const char* shipLaunchLandingRumblePhaseName(ShipLaunchLandingRumblePhase phase) noexcept
    {
        switch (phase) {
        case ShipLaunchLandingRumblePhase::Takeoff: return "takeoff";
        case ShipLaunchLandingRumblePhase::Landing: return "landing";
        default: return "none";
        }
    }

    void dispatchShipLaunchLandingTriggerSemantic(
        sds::GameEventType type,
        ShipLaunchLandingRumblePhase phase,
        std::chrono::steady_clock::time_point when) noexcept
    {
        if (!g_eventRouter) {
            return;
        }
        sds::GameEvent semantic{};
        semantic.type = type;
        semantic.when = when;
        const auto phaseName = std::string_view(shipLaunchLandingRumblePhaseName(phase));
        std::copy_n(phaseName.data(), std::min(phaseName.size(), semantic.text.size() - 1), semantic.text.begin());
        (void)g_eventRouter->dispatch(std::move(semantic));
    }

    void stopShipLaunchLandingHaptics(std::string_view reason) noexcept
    {
        const auto previous = g_shipLaunchLandingRumblePhase.exchange(
            ShipLaunchLandingRumblePhase::None, std::memory_order_acq_rel);
        if (previous == ShipLaunchLandingRumblePhase::None) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        g_shipLaunchLandingStartedAtUs.store(0, std::memory_order_release);
        g_shipTakeoffFirstFaderClosed.store(false, std::memory_order_release);
        g_shipTakeoffBoundaryObserved.store(false, std::memory_order_release);

        if (g_haptics) {
            (void)g_haptics->setShipLaunchLandingRumble(false, reason);
        }
        dispatchShipLaunchLandingTriggerSemantic(
            sds::GameEventType::ShipLaunchLandingHapticsStopped, previous, now);
        dispatchShipLaunchLandingTriggerSemantic(
            sds::GameEventType::ShipLaunchLandingLightbarStopped, previous, now);

        std::ostringstream line;
        line << "Ship launch/landing haptics: stage=stop phase="
             << shipLaunchLandingRumblePhaseName(previous)
             << " reason=" << reason
             << " triggers=release";
        pluginLog(line.str());
    }

    void startShipLaunchLandingHaptics(ShipLaunchLandingRumblePhase phase) noexcept
    {
        if (phase == ShipLaunchLandingRumblePhase::None) {
            return;
        }
        auto expected = ShipLaunchLandingRumblePhase::None;
        if (!g_shipLaunchLandingRumblePhase.compare_exchange_strong(
            expected, phase, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        g_shipLaunchLandingStartedAtUs.store(steadyMicros(now), std::memory_order_release);
        g_shipTakeoffFirstFaderClosed.store(false, std::memory_order_release);
        g_shipTakeoffBoundaryObserved.store(false, std::memory_order_release);

        // Lightbar cinematic authority starts when the transition is armed.
        // Takeoff body haptics still wait for the landed -> airborne boundary.
        dispatchShipLaunchLandingTriggerSemantic(
            sds::GameEventType::ShipLaunchLandingLightbarStarted, phase, now);

        if (phase == ShipLaunchLandingRumblePhase::Takeoff) {
            pluginLog(
                "Ship launch/landing haptics: stage=armed phase=takeoff waiting=landed-to-airborne-boundary");
            return;
        }

        if (g_haptics) {
            (void)g_haptics->setShipLaunchLandingRumble(true, shipLaunchLandingRumblePhaseName(phase));
        }
        dispatchShipLaunchLandingTriggerSemantic(
            sds::GameEventType::ShipLaunchLandingHapticsStarted, phase, now);

        std::ostringstream line;
        line << "Ship launch/landing haptics: stage=start phase="
             << shipLaunchLandingRumblePhaseName(phase)
             << " body=ShipBoost gain=1.000 level=1.000"
             << " triggers=both-EffectEx-sustained";
        pluginLog(line.str());
    }

    void maybeStartShipTakeoffRumble(bool pilotActive, bool precisionTouchdownPoll) noexcept
    {
        if (!pilotActive || precisionTouchdownPoll ||
            g_shipLaunchLandingRumblePhase.load(std::memory_order_acquire) != ShipLaunchLandingRumblePhase::None ||
            !g_lastShipStateValid.load(std::memory_order_acquire) ||
            !g_lastShipLanded.load(std::memory_order_acquire) ||
            g_lastShipDocked.load(std::memory_order_acquire) ||
            g_shipBallisticBlockingMenuMask.load(std::memory_order_acquire) != 0) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui ||
            !ui->IsMenuOpen(RE::BSFixedString("FaderMenu")) ||
            ui->IsMenuOpen(RE::BSFixedString("TakeoffMenu"))) {
            return;
        }

        startShipLaunchLandingHaptics(ShipLaunchLandingRumblePhase::Takeoff);
    }

    void dispatchShipTouchdownHaptic(
        const sds::ShipLaunchLandingReconBoundary& boundary,
        std::chrono::steady_clock::time_point when) noexcept
    {
        if (!g_eventRouter ||
            boundary.type != sds::ShipLaunchLandingTransition::Touchdown ||
            boundary.before.landed || !boundary.after.landed ||
            boundary.before.docked || boundary.after.docked) {
            return;
        }

        sds::GameEvent semantic{};
        semantic.type = sds::GameEventType::ShipTouchdown;
        semantic.when = when;
        (void)g_eventRouter->dispatch(std::move(semantic));

        std::ostringstream line;
        line << "Ship touchdown haptics: authority=landing-sequence-landed-false-to-true"
             << " transition=" << boundary.transitionId
             << " eventWhenUs="
             << std::chrono::duration_cast<std::chrono::microseconds>(when.time_since_epoch()).count()
             << " WwiseAuthority=none diagnosticCorroborationOnly=yes";
        pluginLog(line.str());
    }

    void observeShipLaunchLandingReconWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_shipLaunchLandingReconEnabled.load(std::memory_order_acquire)) {
            return;
        }
        g_shipLaunchLandingReconProbe.observeWwise({
            .sequence = observation.sequence,
            .eventId = observation.eventId,
            .gameObjectId = observation.gameObjectId,
            .callsiteRva = observation.callsiteRva,
            .returnedPlayingId = observation.returnedPlayingId,
            .when = observation.when,
        });
    }

    void logShipLaunchLandingReconReport(const sds::ShipLaunchLandingReconReport& report) noexcept
    {
        {
            std::ostringstream line;
            line << "Ship launch/landing recon report: transition=" << report.transitionId
                 << " boundary=" << shipLaunchLandingTransitionName(report.type)
                 << " samples=" << report.samples.size();
            if (report.type == sds::ShipLaunchLandingTransition::LandingSequence) {
                const auto appendDelta = [&line](const char* name, const auto& value) {
                    line << ' ' << name;
                    if (value) {
                        line << *value;
                    } else {
                        line << "na";
                    }
                };
                line << " anchor=FaderMenu-open-or-LoadingMenu-open"
                     << " beforeLanded=" << (report.before.landed ? "yes" : "no")
                     << " stateBoundaryObserved="
                     << (report.touchdownObservedDeltaMicros ? "yes" : "no")
                     << " afterLanded=" << (report.after.landed ? "yes" : "no");
                appendDelta("galaxyStarMapClosedDeltaUs=", report.galaxyStarMapClosedDeltaMicros);
                appendDelta("faderOpenedDeltaUs=", report.faderOpenedDeltaMicros);
                appendDelta("loadingOpenedDeltaUs=", report.loadingOpenedDeltaMicros);
                appendDelta("spaceshipHudClosedDeltaUs=", report.spaceshipHudClosedDeltaMicros);
                appendDelta("loadingClosedDeltaUs=", report.loadingClosedDeltaMicros);
                appendDelta("faderClosedDeltaUs=", report.faderClosedDeltaMicros);
                appendDelta("touchdownObservedDeltaUs=", report.touchdownObservedDeltaMicros);
                appendDelta("takeoffMenuOpenedDeltaUs=", report.takeoffMenuOpenedDeltaMicros);
                appendDelta("takeoffMenuClosedDeltaUs=", report.takeoffMenuClosedDeltaMicros);
                appendDelta("secondFaderOpenedDeltaUs=", report.secondFaderOpenedDeltaMicros);
                appendDelta("secondLoadingOpenedDeltaUs=", report.secondLoadingOpenedDeltaMicros);
                appendDelta("secondLoadingClosedDeltaUs=", report.secondLoadingClosedDeltaMicros);
                appendDelta("secondFaderClosedDeltaUs=", report.secondFaderClosedDeltaMicros);
                line << " cinematicTail="
                     << (report.takeoffMenuOpenedDeltaMicros ? "observed" : "fallback")
                     << " diagnosticOnly=yes";
            } else {
                line << " preMs=2000 postMs=2000"
                     << " beforeLanded=" << (report.before.landed ? "yes" : "no")
                     << " afterLanded=" << (report.after.landed ? "yes" : "no")
                     << " diagnosticOnly=yes";
            }
            pluginLog(line.str());
        }

        for (const auto& sample : report.samples) {
            const bool knownBallistic = sds::isHardwareObservedShipBallisticFireEvent(sample.eventId) ||
                (g_shipWeaponSemanticCache && g_shipWeaponSemanticCache->isPlayerBallisticFire(sample.eventId));
            const bool knownLaser = sds::isHardwareObservedShipPulseLaserFireEvent(sample.eventId);
            const bool knownParticle = sds::isHardwareObservedShipProtonBeamFireEvent(sample.eventId);
            const bool knownMissile = sds::isHardwareObservedShipMissileFireEvent(sample.eventId);
            const bool knownEM = sds::isHardwareObservedShipEMFireEvent(sample.eventId);
            std::ostringstream line;
            line << "Ship launch/landing recon report: transition=" << report.transitionId
                 << " boundary=" << shipLaunchLandingTransitionName(report.type)
                 << " phase=" << shipLaunchLandingPhaseName(sample.phase)
                 << " deltaUs=" << sample.deltaMicros
                 << " seq=" << sample.sequence
                 << " event=0x" << std::uppercase << std::hex
                 << std::setw(8) << std::setfill('0') << sample.eventId
                 << " gameObject=0x" << sample.gameObjectId
                 << " callsite=Starfield+0x" << sample.callsiteRva
                 << std::dec << std::setfill(' ')
                 << " returnedPlayingId=" << sample.returnedPlayingId
                 << " menus=map:" << (sample.galaxyStarMapOpen ? "open" : "closed")
                 << ",fader:" << (sample.faderOpen ? "open" : "closed")
                 << ",loading:" << (sample.loadingOpen ? "open" : "closed")
                 << ",hud:" << (sample.spaceshipHudOpen ? "open" : "closed")
                 << ",takeoff:" << (sample.takeoffMenuOpen ? "open" : "closed")
                 << " knownBallistic=" << (knownBallistic ? "yes" : "no")
                 << " knownLaser=" << (knownLaser ? "yes" : "no")
                 << " knownParticle=" << (knownParticle ? "yes" : "no")
                 << " knownMissile=" << (knownMissile ? "yes" : "no")
                 << " knownEM=" << (knownEM ? "yes" : "no")
                 << " diagnosticOnly=yes";
            pluginLog(line.str());
        }
    }

    void observeAutoRivetChargeSemantic(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_haptics ||
            !g_autoRivetChargeCaptureEnabled.load(std::memory_order_acquire)) {
            return;
        }

        const auto action = sds::routeAutoRivetChargeWwise(
            observation.eventId,
            observation.gameObjectId);
        const auto marker = sds::autoRivetChargeMarker(action);
        if (marker.empty()) {
            return;
        }

        sds::GameEvent semantic{};
        semantic.type = sds::GameEventType::WeaponFired;
        semantic.when = observation.when;
        const auto count = (std::min)(marker.size(), semantic.text.size() - 1);
        std::copy_n(marker.data(), count, semantic.text.data());
        semantic.text[count] = '\0';
        (void)g_haptics->handle(std::move(semantic));

        pluginLog(
            action == sds::AutoRivetChargeAction::Start ?
                "Auto-Rivet tension: charge-start semantic observed source=player-Wwise" :
                "Auto-Rivet tension: charge-stop semantic observed source=player-Wwise");
    }

    void observeShipBallisticWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_shipWeaponSemanticCache || !g_eventRouter) {
            return;
        }

        const auto correlation = g_shipBallisticFireGate.inspectCorrelation(observation.when);
        if (correlation.recentR2) {
            const auto ordinal = g_shipWeaponCaptureProbeCount.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < kShipWeaponCaptureProbeLimit) {
                const bool catalogMatch = g_shipWeaponSemanticCache->isPlayerBallisticFire(observation.eventId);
                const bool hardwareAlias =
                    observation.eventId == sds::kHardwareObservedShipBallisticFireEventId;
                std::ostringstream probe;
                probe << "Ship weapon capture probe: correlated seq=" << observation.sequence
                      << " event=0x" << std::uppercase << std::hex
                      << std::setw(8) << std::setfill('0') << observation.eventId
                      << " gameObject=0x" << observation.gameObjectId
                      << " callsite=Starfield+0x" << observation.callsiteRva
                      << std::dec << std::setfill(' ')
                      << " r2=" << static_cast<unsigned>(correlation.r2)
                      << " deltaUs=" << correlation.deltaMicros
                      << " catalogMatch=" << (catalogMatch ? "yes" : "no")
                      << " hardwareAlias=" << (hardwareAlias ? "yes" : "no")
                      << " returnedPlayingId=" << observation.returnedPlayingId;
                pluginLog(probe.str());
            } else if (ordinal == kShipWeaponCaptureProbeLimit) {
                pluginLog("Ship weapon capture probe: correlated log limit reached; further R2-window Wwise observations suppressed");
            }
        }

        const bool catalogMatch =
            g_shipWeaponSemanticCache->isPlayerBallisticFire(observation.eventId);
        const bool hardwareAlias =
            observation.eventId == sds::kHardwareObservedShipBallisticFireEventId;
        if (catalogMatch || hardwareAlias) {
            std::ostringstream candidate;
            candidate << "Ship ballistic capture: candidate event=0x" << std::uppercase << std::hex
                      << std::setw(8) << std::setfill('0') << observation.eventId
                      << " gameObject=0x" << observation.gameObjectId
                      << std::dec << std::setfill(' ')
                      << " catalogMatch=" << (catalogMatch ? "yes" : "no")
                      << " hardwareAlias=" << (hardwareAlias ? "yes" : "no")
                      << " semanticSource=" << (hardwareAlias ? "r3-hardware-correlation" : "SoundBanksInfo")
                      << " stage=pre-R2-correlation";
            pluginLog(candidate.str());
        }

        if (!g_shipBallisticFireGate.authorizeWwiseFire(
                observation.eventId, observation.gameObjectId, observation.when, *g_shipWeaponSemanticCache)) {
            return;
        }

        sds::GameEvent semantic{};
        semantic.type = sds::GameEventType::ShipBallisticWeaponFired;
        semantic.when = observation.when;
        (void)g_eventRouter->dispatch(std::move(semantic));

        std::ostringstream line;
        line << "Ship ballistic fire: event=0x" << std::uppercase << std::hex
             << std::setw(8) << std::setfill('0') << observation.eventId
             << " gameObject=0x" << observation.gameObjectId
             << std::dec << std::setfill(' ')
             << " family=ballistic control=R2 source=player-Wwise+R2-correlation"
             << " semanticSource="
             << (hardwareAlias ? "r3-hardware-correlation" : "SoundBanksInfo");
        pluginLog(line.str());
    }

    void observeShipLaserWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_eventRouter ||
            !g_shipLaserFireGate.authorizeWwiseFire(
                observation.eventId, observation.gameObjectId, observation.when)) {
            return;
        }

        sds::GameEvent semantic{};
        semantic.type = sds::GameEventType::ShipLaserWeaponFired;
        semantic.when = observation.when;
        (void)g_eventRouter->dispatch(std::move(semantic));

        std::ostringstream line;
        line << "Ship laser fire: event=0x" << std::uppercase << std::hex
             << std::setw(8) << std::setfill('0') << observation.eventId
             << " gameObject=0x" << observation.gameObjectId
             << std::dec << std::setfill(' ')
             << " family=laser control=R2 source=player-Wwise+R2-correlation"
             << " semanticSource=v0364-hardware-recon";
        pluginLog(line.str());
    }

    void observeShipParticleWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_eventRouter ||
            !g_shipParticleFireGate.authorizeWwiseFire(
                observation.eventId, observation.gameObjectId, observation.when)) {
            return;
        }

        sds::GameEvent semantic{};
        semantic.type = sds::GameEventType::ShipParticleWeaponFired;
        semantic.when = observation.when;
        (void)g_eventRouter->dispatch(std::move(semantic));

        std::ostringstream line;
        line << "Ship particle fire: event=0x" << std::uppercase << std::hex
             << std::setw(8) << std::setfill('0') << observation.eventId
             << " gameObject=0x" << observation.gameObjectId
             << std::dec << std::setfill(' ')
             << " family=particle control=R2 source=player-Wwise+R2-correlation"
             << " semanticSource=v0366-proton-beam-hardware-recon";
        pluginLog(line.str());
    }


    void observeShipMissileWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_eventRouter ||
            !g_shipMissileFireGate.authorizeWwiseFire(
                observation.eventId, observation.gameObjectId, observation.when)) {
            return;
        }

        sds::GameEvent semantic{};
        semantic.type = sds::GameEventType::ShipMissileWeaponFired;
        semantic.when = observation.when;
        (void)g_eventRouter->dispatch(std::move(semantic));

        std::ostringstream line;
        line << "Ship missile fire: event=0x" << std::uppercase << std::hex
             << std::setw(8) << std::setfill('0') << observation.eventId
             << " gameObject=0x" << observation.gameObjectId
             << std::dec << std::setfill(' ')
             << " family=missile control=R2 source=player-Wwise+R2-correlation"
             << " semanticSource=v0368-missile-hardware-recon";
        pluginLog(line.str());
    }

    void observeShipEMWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!g_eventRouter ||
            !g_shipEMFireGate.authorizeWwiseFire(
                observation.eventId, observation.gameObjectId, observation.when)) {
            return;
        }

        sds::GameEvent semantic{};
        semantic.type = sds::GameEventType::ShipEMWeaponFired;
        semantic.when = observation.when;
        (void)g_eventRouter->dispatch(std::move(semantic));

        std::ostringstream line;
        line << "Ship EM fire: event=0x" << std::uppercase << std::hex
             << std::setw(8) << std::setfill('0') << observation.eventId
             << " gameObject=0x" << observation.gameObjectId
             << std::dec << std::setfill(' ')
             << " family=em control=R2 source=player-Wwise+R2-correlation"
             << " semanticSource=v0370-em-hardware-recon";
        pluginLog(line.str());
    }

    std::string readTextFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return {};
        }
        std::ostringstream text;
        text << stream.rdbuf();
        return text.str();
    }

    std::filesystem::path currentExecutablePath() noexcept
    {
        std::array<wchar_t, 32768> buffer{};
        const auto length = ::GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) {
            return {};
        }
        return std::filesystem::path(std::wstring(buffer.data(), length));
    }

    sds::Config loadRuntimeConfig()
    {
        const auto text =
            readTextFile(
                std::filesystem::path(
                    kConfigPath));

        if (text.empty()) {
            pluginLog(
                "Config: using built-in defaults (file absent or empty)");

            const auto config =
                sds::Config::defaults();

            (void)sds::configureCustomWeaponProfiles(
                true,
                {});

            (void)sds::configureCustomWeaponSpeakerProfiles(
                config.customWeaponSpeakerAudioEnabled,
                {});
            sds::setCustomWeaponReloadHapticsEnabled(config.customWeaponsEnabled);

            return config;
        }

        pluginLog(
            "Config: loaded Data/SFSE/Plugins/StarfieldDualSense.toml");

        const auto config =
            sds::loadConfig(text);

        sds::setCustomWeaponReloadHapticsEnabled(config.customWeaponsEnabled);
        const auto customResult =
            sds::configureCustomWeaponProfiles(
                true,
                text);

        const auto customSpeakerResult =
            sds::configureCustomWeaponSpeakerProfiles(
                config.customWeaponSpeakerAudioEnabled,
                text);

        std::ostringstream customLine;
        customLine
            << "Custom weapons: feedbackEnabled="
            << (config.customWeaponsEnabled ? "yes" : "no")
            << " triggersEnabled="
            << (config.customWeaponAdaptiveTriggersEnabled ? "yes" : "no")
            << " feedbackMappings="
            << customResult.loaded
            << " feedbackIgnored="
            << customResult.ignored
            << " speakerEnabled="
            << (config.customWeaponSpeakerAudioEnabled ? "yes" : "no")
            << " speakerMappings="
            << customSpeakerResult.loaded
            << " speakerIgnored="
            << customSpeakerResult.ignored
            << " handlingMappings="
            << customSpeakerResult.handlingLoaded
            << " handlingIgnored="
            << customSpeakerResult.handlingIgnored
            << " reloadHapticsMappings="
            << customSpeakerResult.reloadHapticsLoaded
            << " reloadHapticsIgnored="
            << customSpeakerResult.reloadHapticsIgnored
            << " source=TOML-array";

        pluginLog(customLine.str());

        return config;
    }

    void startEarlyMainMenuUiPreparation() noexcept
    {
        try {
            // Prewarm is producer/cache preparation, not live playback policy.
            // Keep it available even when ControllerSpeaker or ScannerUI starts OFF;
            // ControllerSpeakerManager remains the live admission authority.
            const auto executablePath = currentExecutablePath();
            if (executablePath.empty()) {
                pluginLog("Main-menu UI speaker prewarm: INACTIVE reason=data-path-unavailable");
                return;
            }
            g_weaponAudioPipelineDataPath = executablePath.parent_path() / "Data";
            if (!g_uiSpeakerPreparedCache) {
                g_uiSpeakerPreparedCache = std::make_shared<sds::UiSpeakerPreparedCache>();
            }
            if (!g_weaponSpeakerPreparedCache) {
                g_weaponSpeakerPreparedCache = std::make_shared<sds::WeaponSpeakerPreparedCache>();
            }
            // Early and post-PostDataLoad workers publish into the same
            // atomic prepared-cache slots. No live speaker/HID objects exist
            // at this stage, and neither worker controls Bluetooth ownership.
            if (!g_boostpackSpeakerPreparedCache) {
                g_boostpackSpeakerPreparedCache = std::make_shared<sds::BoostpackSpeakerPreparedCache>();
            }
            if (!g_landVehicleSpeakerCache) {
                g_landVehicleSpeakerCache = std::make_shared<sds::LandVehicleSpeakerCache>();
            }

            const sds::AudioPipelineStartupOptions startupOptions{
                .prepareUi = true,
                .prepareMainMenuUiOnly = true,
                .prepareWeapons = false,
                .resolveUiDiagnostics = false,
                .existingWeaponCache = g_weaponSpeakerPreparedCache,
            };
            g_mainMenuUiAudioPipeline = std::make_unique<sds::WeaponAudioPipeline>(
                g_weaponSpeakerPreparedCache,
                sds::makeRealWeaponAudioPipelineBackend(g_weaponAudioPipelineDataPath, startupOptions),
                g_uiSpeakerPreparedCache,
                std::shared_ptr<sds::ShipWeaponSemanticCache>{},
                g_boostpackSpeakerPreparedCache,
                g_landVehicleSpeakerCache);
            g_mainMenuUiAudioPipeline->start();
            pluginLog("Main-menu UI speaker prewarm: worker started cues=3 events=GeneralFocus,GeneralOK,GeneralCancel preparation=pre-PostDataLoad deferredPriority=43-UI+boostpack+rover then exhaustive-weapons-495");
        } catch (const std::exception& exception) {
            pluginLog(std::string("Main-menu UI speaker prewarm: failed error=\"") + exception.what() + "\"");
        } catch (...) {
            pluginLog("Main-menu UI speaker prewarm: failed error=\"unknown exception\"");
        }
    }

    bool refreshCurrentEquippedWeaponState(std::uint32_t* refreshedFormId = nullptr) noexcept
    {
        if (g_runtimeShutdown || !g_gameState || !g_fireMarkerBridge) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        RE::TESBoundObject* equippedWeapon = nullptr;
        player->ForEachEquippedItem([&](const RE::BGSInventoryItem& item) {
            if (item.object && item.object->GetFormType() == RE::FormType::kWEAP) {
                equippedWeapon = item.object;
                return RE::BSContainer::ForEachResult::kStop;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        if (!equippedWeapon) {
            return false;
        }

        RE::ActorItemEquipped::Event equipEvent{};
        equipEvent.item = equippedWeapon;
        equipEvent.actor = RE::NiPointer<RE::Actor>(player);

        (void)g_gameState->ProcessEvent(equipEvent, nullptr);
        (void)g_fireMarkerBridge->ProcessEvent(equipEvent, nullptr);

        if (refreshedFormId) {
            *refreshedFormId = equippedWeapon->GetFormID();
        }
        return true;
    }

    void bootstrapStartupEquippedWeaponIfReady() noexcept
    {
        if (g_startupWeaponBootstrapComplete.load(std::memory_order_acquire) || g_runtimeShutdown || !g_gameState || !g_fireMarkerBridge) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!ui || !player || !ui->IsMenuOpen(RE::BSFixedString("HUDMenu"))) {
            return;
        }

        // HUD availability is the first stable in-game boundary after save loading.
        // Query the loaded inventory exactly once, then reuse the same equip-event
        // handlers that service real ActorItemEquipped notifications.
        g_startupWeaponBootstrapComplete.store(true, std::memory_order_release);

        std::uint32_t formId = 0;
        if (!refreshCurrentEquippedWeaponState(&formId)) {
            pluginLog("Game state: startup equipped weapon bootstrap: no weapon equipped");
            return;
        }

        std::ostringstream diagnostic;
        diagnostic << "Game state: startup equipped weapon bootstrapped form=0x"
                   << std::hex << std::uppercase << formId;
        pluginLog(diagnostic.str());
    }

    std::uint8_t boostpackProductionMenuBit(std::string_view menu) noexcept
    {
        if (menu == "MainMenu") return 0x01u;
        if (menu == "DataMenu") return 0x02u;
        if (menu == "PauseMenu") return 0x04u;
        if (menu == "LoadingMenu") return 0x08u;
        if (menu == "FaderMenu") return 0x10u;
        return 0u;
    }

    bool boostpackProductionCaptureArmed() noexcept
    {
        return g_boostpackFeedbackAuthority != nullptr &&
            g_boostpackProductionContextEligible.load(std::memory_order_acquire) &&
            !g_runtimeShutdown;
    }

    void refreshBoostpackSemanticArming() noexcept
    {
        sds::setBoostpackSemanticObservationArmed(boostpackProductionCaptureArmed());
    }

    void setBoostpackProductionContextEligible(
        bool eligible,
        std::string_view reason) noexcept
    {
        bool wasActive = false;
        bool changed = false;
        {
            std::scoped_lock lock(g_boostpackFeedbackMutex);
            if (!g_boostpackFeedbackAuthority) {
                g_boostpackProductionContextEligible.store(false, std::memory_order_release);
                refreshBoostpackSemanticArming();
                return;
            }

            wasActive = g_boostpackFeedbackAuthority->active();
            const bool previous =
                g_boostpackProductionContextEligible.exchange(eligible, std::memory_order_acq_rel);
            changed = previous != eligible;
            g_boostpackFeedbackAuthority->setContextEligible(eligible);
        }

        if (!eligible && wasActive) {
            if (g_haptics) {
                (void)g_haptics->stopBoostpackThrust();
            }
            if (g_audioTransport) {
                g_audioTransport->clearSpeakerPlayback();
            }
        }

        refreshBoostpackSemanticArming();

        if (changed) {
            std::ostringstream line;
            line << "Boostpack production context: "
                 << (eligible ? "ELIGIBLE" : "BLOCKED")
                 << " reason=" << reason
                 << " authority=exact-Wwise-0x1BE06B49"
                 << " playerObject=0x2";
            pluginLog(line.str());
        }
    }

    void refreshBoostpackProductionContext(std::string_view reason) noexcept
    {
        const bool eligible =
            g_boostpackFeedbackAuthority != nullptr &&
            g_boostpackProductionControllerConnected.load(std::memory_order_acquire) &&
            !g_shipPilotActive.load(std::memory_order_acquire) &&
            !g_boostpackProductionLandVehicleActive.load(std::memory_order_acquire) &&
            g_boostpackProductionBlockingMenuMask.load(std::memory_order_acquire) == 0u &&
            !g_runtimeShutdown;
        setBoostpackProductionContextEligible(eligible, reason);
    }

    void syncBoostpackProductionForMenu(std::string_view menu, bool opened) noexcept
    {
        const auto bit = boostpackProductionMenuBit(menu);
        if (bit == 0u) {
            return;
        }

        if (opened) {
            g_boostpackProductionBlockingMenuMask.fetch_or(bit, std::memory_order_acq_rel);
        } else {
            g_boostpackProductionBlockingMenuMask.fetch_and(
                static_cast<std::uint8_t>(~bit),
                std::memory_order_acq_rel);
        }
        refreshBoostpackProductionContext(opened ? "menu-open" : "menu-close");
    }

    void initializeBoostpackProductionMenuMask() noexcept
    {
        std::uint8_t mask = 0u;
        if (auto* ui = RE::UI::GetSingleton()) {
            const std::pair<std::string_view, std::uint8_t> menus[]{
                { "MainMenu", 0x01u },
                { "DataMenu", 0x02u },
                { "PauseMenu", 0x04u },
                { "LoadingMenu", 0x08u },
                { "FaderMenu", 0x10u },
            };
            for (const auto& [name, bit] : menus) {
                if (ui->IsMenuOpen(RE::BSFixedString(name))) {
                    mask = static_cast<std::uint8_t>(mask | bit);
                }
            }
        }
        g_boostpackProductionBlockingMenuMask.store(mask, std::memory_order_release);
        refreshBoostpackProductionContext("startup-menu-snapshot");
    }

    void applyBoostpackProductionTransition(
        const sds::BoostpackFeedbackUpdate& update,
        std::chrono::steady_clock::time_point when) noexcept
    {
        if (!g_haptics) {
            return;
        }

        switch (update.transition) {
        case sds::BoostpackFeedbackTransition::Started:
            if (update.ignition) {
                (void)g_haptics->emitBoostpackIgnition(when);
            }
            (void)g_haptics->startBoostpackThrust();
            break;
        case sds::BoostpackFeedbackTransition::Refreshed:
            (void)g_haptics->refreshBoostpackThrust();
            break;
        case sds::BoostpackFeedbackTransition::Stopped:
            (void)g_haptics->stopBoostpackThrust();
            break;
        case sds::BoostpackFeedbackTransition::None:
        default:
            break;
        }
    }

    void observeBoostpackProductionWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        if (!boostpackProductionCaptureArmed() ||
            observation.externalCount != 0u || observation.hasExternalSources) {
            return;
        }

        sds::BoostpackFeedbackUpdate update{};
        {
            std::scoped_lock lock(g_boostpackFeedbackMutex);
            if (!g_boostpackFeedbackAuthority) {
                return;
            }
            update = g_boostpackFeedbackAuthority->observeWwise(
                observation.eventId,
                observation.gameObjectId,
                observation.when);
        }

        applyBoostpackProductionTransition(update, observation.when);

        if (update.speakerCue && g_boostpackSpeakerPlayback) {
            (void)g_boostpackSpeakerPlayback->observeWwise(observation);
        }
    }

    void observeOnFootLandingWwise(
        const sds::WeaponSfxWwiseObservation& observation) noexcept
    {
        // This native Wwise observation is the physical JumpDown sound, not
        // a jump-button edge. The same cue occurs after a boostpack descent.
        if (observation.externalCount != 0u || observation.hasExternalSources) {
            return;
        }
        const auto hit = g_onFootLandingGate.observe(
            observation.eventId, observation.gameObjectId, observation.when,
            boostpackProductionCaptureArmed());
        if (!hit || !g_haptics) {
            return;
        }
        const bool emitted = g_haptics->emitOnFootLanding(hit->baseGain, observation.when);
        char line[256]{};
        std::snprintf(
            line, sizeof(line),
            "On-foot landing haptic: nativeJumpDown=0x%08X object=0x%llX boostAssisted=%s gain=%.2f submitted=%s authority=player-Wwise",
            observation.eventId,
            static_cast<unsigned long long>(observation.gameObjectId),
            hit->boostAssisted ? "yes" : "no",
            static_cast<double>(hit->baseGain),
            emitted ? "yes" : "no");
        pluginLog(line);
    }

    void observeBoostpackProductionSemantic(
        std::string_view semantic,
        bool active,
        std::chrono::steady_clock::time_point when) noexcept
    {
        if (semantic != "Jump" || active) {
            return;
        }

        sds::BoostpackFeedbackUpdate update{};
        {
            std::scoped_lock lock(g_boostpackFeedbackMutex);
            if (!g_boostpackFeedbackAuthority) {
                return;
            }
            update = g_boostpackFeedbackAuthority->observeJumpRelease(when);
        }

        applyBoostpackProductionTransition(update, when);
    }

    void tickBoostpackProduction(std::chrono::steady_clock::time_point now) noexcept
    {
        sds::BoostpackFeedbackUpdate update{};
        {
            std::scoped_lock lock(g_boostpackFeedbackMutex);
            if (!g_boostpackFeedbackAuthority) {
                return;
            }
            update = g_boostpackFeedbackAuthority->tick(now);
        }

        applyBoostpackProductionTransition(update, now);
    }
    void removeBluetoothShadowDelegate() noexcept;
    void shutdownRuntime() noexcept
    {
        if (g_runtimeShutdown) {
            return;
        }
        g_runtimeShutdown = true;

        // Shutdown must release every synthetic Bluetooth physical input
        // while Starfield's SAD shadow is still installed. The ordinary
        // transport-inactive path does this during gameplay, but quit sets
        // g_runtimeShutdown before another runtime tick can run.
        //
        // First neutralize the native-slot2 context path used by Scanner /
        // land-vehicle R2 and vehicle/scanner sticks, then neutralize the
        // runtime physical-event bridge. Only after both releases are sent
        // do we detach the shadow delegate.
        if (g_gameState) {
            if (g_controller &&
                g_controller->connected() &&
                g_controller->bluetoothTransport() &&
                g_bluetoothShadowDelegate) {

                sds::TouchState neutral{};
                g_gameState->dispatchBluetoothScannerSticksAtNativePoll(
                    neutral,
                    0.0F,
                    g_bluetoothShadowDelegate);
            }

            g_gameState->resetBluetoothPhysicalInput();

            pluginLog(
                "Bluetooth gameplay input bridge: RESET "
                "reason=runtime-shutdown");
        }

        removeBluetoothShadowDelegate();

        if (g_boostpackSpeakerPlayback) {
            g_boostpackSpeakerPlayback->beginShutdown();
        }
        setBoostpackProductionContextEligible(false, "shutdown-runtime");

        pluginLog("Runtime shutdown: Starfield quit requested; stopping haptics/controller workers before DLL teardown");

        g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
        g_musicHapticsEnabled.store(false, std::memory_order_release);
        g_musicHapticsBlockingMenuMask.store(0u, std::memory_order_release);
        musicHapticsClearAuthority();
        g_shipPilotActive.store(false, std::memory_order_release);
        g_onFootRefreshPending.store(false, std::memory_order_release);
        g_lastShipStateValid.store(false, std::memory_order_release);
        stopShipLaunchLandingHaptics("shutdown");

        if (g_uiSpeakerPlayback) {
            g_uiSpeakerPlayback->beginShutdown();
        }
        if (g_audioCapture) {
            g_audioCapture->setUiAudioPlaybackArmed(false);
            g_audioCapture->setMusicReconArmed(false);
            g_audioCapture->setMusicSelectionArmed(false);
        }

        if (g_weaponSfxDiscovery) {
            sds::GameEvent shutdown{};
            shutdown.type = sds::GameEventType::Shutdown;
            shutdown.when = std::chrono::steady_clock::now();
            g_weaponSfxDiscovery->observeGameEvent(shutdown);
        }

        if (g_weaponSpeakerPlayback) {
            sds::GameEvent shutdown{};
            shutdown.type = sds::GameEventType::Shutdown;
            shutdown.when = std::chrono::steady_clock::now();
            (void)g_weaponSpeakerPlayback->observeGameEvent(shutdown);
        }

        if (g_audioCapture) {
            g_audioCapture->drainDiagnostics();
            if (g_uiAudioDiscovery) {
                g_uiAudioDiscovery->noteDroppedWwise(g_audioCapture->takeUiAudioDropped());
            }
            if (g_musicRecon) {
                g_musicRecon->noteDroppedWwise(g_audioCapture->takeMusicReconDropped());
            }
        }
        if (g_musicRecon) {
            try {
                sds::GameEvent shutdown{};
                shutdown.type = sds::GameEventType::Shutdown;
                shutdown.when = std::chrono::steady_clock::now();
                g_musicRecon->observeGameEvent(shutdown);
                pluginLog(g_musicRecon->finalize(shutdown.when));
                for (auto& line : g_musicRecon->takeDiagnostics(4096u)) {
                    pluginLog(line);
                }
            } catch (...) {
                pluginLog("Music recon: finalization exception ignored; shutdown ordering unchanged");
            }
        }
        if (g_uiAudioDiscovery) {
            for (auto& line : g_uiAudioDiscovery->finalize(std::chrono::steady_clock::now())) {
                pluginLog(line);
            }
        }
        if (g_audioCapture) {
            g_audioCapture->setUiAudioDiscoveryArmed(false);
            g_audioCapture->setUiAudioPlaybackArmed(false);
            g_audioCapture->setMusicReconArmed(false);
            g_audioCapture->setMusicSelectionArmed(false);
            g_audioCapture->stop();
        }

        if (g_remoteVoAudioPipeline) {
            g_remoteVoAudioPipeline->stop();
        }

        if (g_fireMarkerBridge) {
            g_fireMarkerBridge->stop();
        }

        if (g_gameState) {
            g_gameState->unregisterSinks();
        }

        if (g_weaponAudioPipeline) {
            g_weaponAudioPipeline->stop();
        }
        g_boostpackSpeakerPlayback.reset();
        g_boostpackSpeakerPreparedCache.reset();
        {
            std::scoped_lock lock(g_boostpackFeedbackMutex);
            g_boostpackFeedbackAuthority.reset();
        }
        g_boostpackProductionContextEligible.store(false, std::memory_order_release);

        if (g_mainMenuUiAudioPipeline) {
            g_mainMenuUiAudioPipeline->stop();
        }

        if (g_haptics) {
            sds::GameEvent shutdown{};
            shutdown.type = sds::GameEventType::Shutdown;
            shutdown.when = std::chrono::steady_clock::now();
            (void)g_haptics->handle(std::move(shutdown));
        }

        if (g_controller) {
            // shutdownRuntime has already neutralized the Bluetooth shadow
            // and physical-input bridge. Request physical power-off only
            // after those release frames have been dispatched.
            g_controller->requestBluetoothPowerOffOnStop();
            g_controller->stop();
        }

        if (g_speakerManager) {
            g_speakerManager->stop();
        }

        if (g_haptics) {
            g_haptics->stop();
        }

        sds::HidWriteTrace::stop();
        pluginLog("Runtime shutdown: haptics/controller workers stopped");
    }

    void shutdownReconnectFixOnlyRuntime() noexcept
    {
        if (g_runtimeShutdown) {
            return;
        }
        g_runtimeShutdown = true;
        pluginLog(
            "Reconnect-fix-only shutdown: stopping passive controller presence monitor");
        if (g_controller) {
            g_controller->stop();
        }
        pluginLog(
            "Reconnect-fix-only shutdown: passive controller presence monitor stopped");
    }

    void reconnectFixOnlyRuntimeTick()
    {
        if (g_runtimeShutdown) {
            return;
        }

        if (auto* main = RE::Main::GetSingleton(); main && main->quitGame) {
            shutdownReconnectFixOnlyRuntime();
            return;
        }

        updateNativeDualSenseReselection(std::chrono::steady_clock::now());
    }

    // Starfield's generic BSPCGamepadDevice slot 1 allocates two 0x58-byte
    // state buffers at +0xC8 and +0xD0. Native generic slot 2 copies the
    // current state to the previous state each frame, then fills a fresh
    // gamepad reading and derived trigger/stick values.
    //
    // SAD owns the physical Bluetooth HID handle, so Starfield's native
    // Bluetooth polling must remain disabled. This mirror reproduces only
    // the safe state-buffer update using SAD's already-cached HID snapshot.
    struct BluetoothShadowGenericGamepadState
    {
        std::uint64_t timestamp{ 0 };
        std::uint32_t buttons{ 0 };
        std::uint32_t reserved0C{ 0 };

        double leftTrigger{ 0.0 };
        double rightTrigger{ 0.0 };

        double leftX{ 0.0 };
        double leftY{ 0.0 };
        double rightX{ 0.0 };
        double rightY{ 0.0 };

        float leftTriggerProcessed{ 0.0F };
        float rightTriggerProcessed{ 0.0F };

        float leftXProcessed{ 0.0F };
        float leftYProcessed{ 0.0F };
        float rightXProcessed{ 0.0F };
        float rightYProcessed{ 0.0F };
    };

    static_assert(
        sizeof(BluetoothShadowGenericGamepadState) ==
        0x58u);

    [[nodiscard]] float bluetoothShadowRawAxis(
        std::uint8_t raw,
        bool invert) noexcept
    {
        float value =
            (static_cast<float>(raw) / 255.0F) *
                2.0F -
            1.0F;

        if (invert) {
            value = -value;
        }

        if (value > 1.0F) {
            value = 1.0F;
        }
        else if (value < -1.0F) {
            value = -1.0F;
        }

        return value;
    }

    void bluetoothShadowShapeStick(
        std::uint8_t rawX,
        std::uint8_t rawY,
        float& x,
        float& y) noexcept
    {
        x =
            bluetoothShadowRawAxis(
                rawX,
                false);

        y =
            bluetoothShadowRawAxis(
                rawY,
                true);

        const float magnitude =
            std::sqrt(
                x * x +
                y * y);

        constexpr float kDeadzone =
            0.10F;

        if (magnitude <= kDeadzone) {
            x = 0.0F;
            y = 0.0F;
            return;
        }

        const float clampedMagnitude =
            magnitude > 1.0F ?
                1.0F :
                magnitude;

        const float scaledMagnitude =
            (clampedMagnitude - kDeadzone) /
            (1.0F - kDeadzone);

        const float scale =
            magnitude > 0.0F ?
                scaledMagnitude / magnitude :
                0.0F;

        x *= scale;
        y *= scale;
    }

    [[nodiscard]] std::uint32_t
    bluetoothShadowGamepadButtons(
        const sds::TouchState& state) noexcept
    {
        // Raw generic state uses the Windows Gaming Input button bits that
        // Starfield's native GenericGamepad slot 2 consumes at state +0x08.
        constexpr std::uint32_t kMenu =
            0x0001u;

        constexpr std::uint32_t kA =
            0x0004u;

        constexpr std::uint32_t kB =
            0x0008u;

        constexpr std::uint32_t kX =
            0x0010u;

        constexpr std::uint32_t kY =
            0x0020u;

        constexpr std::uint32_t kDpadUp =
            0x0040u;

        constexpr std::uint32_t kDpadDown =
            0x0080u;

        constexpr std::uint32_t kDpadLeft =
            0x0100u;

        constexpr std::uint32_t kDpadRight =
            0x0200u;

        constexpr std::uint32_t kLeftShoulder =
            0x0400u;

        constexpr std::uint32_t kRightShoulder =
            0x0800u;

        constexpr std::uint32_t kLeftThumb =
            0x1000u;

        constexpr std::uint32_t kRightThumb =
            0x2000u;

        std::uint32_t buttons = 0;

        if (state.options) {
            buttons |= kMenu;
        }

        if (state.cross) {
            buttons |= kA;
        }

        if (state.circle) {
            buttons |= kB;
        }

        if (state.square) {
            buttons |= kX;
        }

        if (state.triangle) {
            buttons |= kY;
        }

        const auto hat =
            static_cast<std::uint8_t>(
                state.dpad);

        if (
            hat == 0 ||
            hat == 1 ||
            hat == 7) {

            buttons |= kDpadUp;
        }

        if (
            hat == 3 ||
            hat == 4 ||
            hat == 5) {

            buttons |= kDpadDown;
        }

        if (
            hat == 5 ||
            hat == 6 ||
            hat == 7) {

            buttons |= kDpadLeft;
        }

        if (
            hat == 1 ||
            hat == 2 ||
            hat == 3) {

            buttons |= kDpadRight;
        }

        if (state.l1) {
            buttons |= kLeftShoulder;
        }

        if (state.r1) {
            buttons |= kRightShoulder;
        }

        if (state.l3) {
            buttons |= kLeftThumb;
        }

        if (state.r3) {
            buttons |= kRightThumb;
        }

        // Create/View intentionally remains outside this physical state mirror.
        // SAD already owns Create/touchpad behavior separately.

        return buttons;
    }

    void bluetoothShadowGamepadPollMirror(
        void* gamepad,
        float deltaSeconds) noexcept
    {
        try {
            if (!gamepad ||
                !g_controller ||
                !g_controller->connected() ||
                !g_controller->bluetoothTransport()) {

                return;
            }

            const auto snapshot =
                g_controller->latestInputSnapshot();

            if (!snapshot) {
                return;
            }

            static std::atomic_bool invocationLogged{ false };

            if (!invocationLogged.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                pluginLog(
                    "Bluetooth shadow state mirror: INVOKED source=Starfield-slot2");
            }

            const auto gamepadAddress =
                reinterpret_cast<std::uintptr_t>(
                    gamepad);

            if (gamepadAddress < 0x10000u) {
                return;
            }

            auto* previous =
                *reinterpret_cast<
                    BluetoothShadowGenericGamepadState**>(
                        gamepadAddress +
                        0xC8u);

            auto* current =
                *reinterpret_cast<
                    BluetoothShadowGenericGamepadState**>(
                        gamepadAddress +
                        0xD0u);

            if (
                reinterpret_cast<std::uintptr_t>(
                    previous) <
                    0x10000u ||
                reinterpret_cast<std::uintptr_t>(
                    current) <
                    0x10000u) {

                return;
            }

            // Match native GenericGamepad slot 2:
            // old current -> previous, fresh reading -> current.
            std::memcpy(
                previous,
                current,
                sizeof(
                    BluetoothShadowGenericGamepadState));

            BluetoothShadowGenericGamepadState next{};

            next.timestamp =
                snapshot->generation;

            const auto& state =
                snapshot->state;
            // Bluetooth native digital timing:
            //
            // USB creates its L3/Circle native button transitions during
            // Starfield's native gamepad polling stage. Reproduce only that
            // timing here on the safe SAD shadow.
            //
            // Analog and every other digital input remain on the proven
            // runtime-tick bridge. No hardware polling occurs here.
            using NativeGamepadButtonTransition =
                void (*)(
                    void*,
                    std::int32_t,
                    float,
                    float,
                    float);

            constexpr std::uintptr_t
                kNativeGamepadButtonTransitionRva =
                    0x22FC2A0u;

            constexpr std::int32_t
                kNativeL3ButtonId =
                    0x0002;

            constexpr std::int32_t
                kNativeCircleButtonId =
                    0x2000;

            constexpr std::int32_t
                kNativeSquareButtonId =
                    0x8000;

            constexpr std::int32_t
                kNativeTriangleButtonId =
                    0x1000;

            static void* transitionGamepad = nullptr;
            static bool previousL3 = false;
            static bool previousCircle = false;
            static bool previousSquare = false;
            static bool previousTriangle = false;
            static bool timingLogged = false;

            if (transitionGamepad != gamepad) {
                transitionGamepad = gamepad;
                previousL3 = false;
                previousCircle = false;
                previousSquare = false;
                previousTriangle = false;
            }

            float nativeDeltaSeconds =
                deltaSeconds;

            if (nativeDeltaSeconds < 0.0F) {
                nativeDeltaSeconds = 0.0F;
            }
            else if (nativeDeltaSeconds > 0.100F) {
                nativeDeltaSeconds = 0.100F;
            }

            const auto starfieldBase =
                reinterpret_cast<std::uintptr_t>(
                    GetModuleHandleW(nullptr));

            if (starfieldBase != 0) {
                const auto transition =
                    reinterpret_cast<
                        NativeGamepadButtonTransition>(
                            starfieldBase +
                            kNativeGamepadButtonTransitionRva);

                const bool currentL3 =
                    state.l3;

                const bool currentCircle =
                    state.circle;

                if (currentL3 || previousL3) {
                    transition(
                        gamepad,
                        kNativeL3ButtonId,
                        nativeDeltaSeconds,
                        previousL3 ? 1.0F : 0.0F,
                        currentL3 ? 1.0F : 0.0F);
                }

                if (currentCircle || previousCircle) {
                    transition(
                        gamepad,
                        kNativeCircleButtonId,
                        nativeDeltaSeconds,
                        previousCircle ? 1.0F : 0.0F,
                        currentCircle ? 1.0F : 0.0F);
                }

                // X/Square and Y/Triangle were being submitted from the
                // separate runtime tick. Starfield saw those events but
                // gameplay intermittently ignored them. Move ONLY these
                // buttons to the exact native slot-2 timing used by L3 and
                // Circle; maintain the original shadowless fallback below.
                const bool currentSquare = state.square;
                const bool currentTriangle = state.triangle;

                if (currentSquare || previousSquare) {
                    transition(
                        gamepad,
                        kNativeSquareButtonId,
                        nativeDeltaSeconds,
                        previousSquare ? 1.0F : 0.0F,
                        currentSquare ? 1.0F : 0.0F);
                }

                if (currentTriangle || previousTriangle) {
                    transition(
                        gamepad,
                        kNativeTriangleButtonId,
                        nativeDeltaSeconds,
                        previousTriangle ? 1.0F : 0.0F,
                        currentTriangle ? 1.0F : 0.0F);
                }

                if (currentSquare != previousSquare ||
                    currentTriangle != previousTriangle) {
                    pluginLog(
                        currentSquare != previousSquare
                            ? (currentSquare
                                ? "BT XY SLOT2: X/Square DOWN"
                                : "BT XY SLOT2: X/Square UP")
                            : (currentTriangle
                                ? "BT XY SLOT2: Y/Triangle DOWN"
                                : "BT XY SLOT2: Y/Triangle UP"));
                }

                previousSquare = currentSquare;
                previousTriangle = currentTriangle;

                if (!timingLogged &&
                    (currentL3 ||
                     previousL3 ||
                     currentCircle ||
                     previousCircle)) {

                    pluginLog(
                        "Bluetooth native digital timing: ACTIVE "
                        "rva=Starfield+0x22FC2A0 "
                        "buttons=L3,Circle "
                        "source=Starfield-slot2 "
                        "hardwarePolling=no "
                        "runtimeTickButtons=masked");

                    timingLogged = true;
                }

                previousL3 =
                    currentL3;

                previousCircle =
                    currentCircle;
            }

            static std::atomic_bool activeSampleLogged{ false };

            const bool diagnosticActiveSample =
                state.l3 ||
                state.circle ||
                state.leftY <= 24u ||
                state.leftY >= 231u;

            if (diagnosticActiveSample &&
                !activeSampleLogged.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                pluginLog(
                    "Bluetooth shadow state mirror: ACTIVE-SAMPLE "
                    "left-stick-or-L3-or-Circle observed");
            }

            next.buttons =
                bluetoothShadowGamepadButtons(
                    state);

            next.leftTrigger =
                static_cast<double>(
                    state.l2) /
                255.0;

            next.rightTrigger =
                static_cast<double>(
                    state.r2) /
                255.0;

            const float rawLeftX =
                bluetoothShadowRawAxis(
                    state.leftX,
                    false);

            const float rawLeftY =
                bluetoothShadowRawAxis(
                    state.leftY,
                    true);

            const float rawRightX =
                bluetoothShadowRawAxis(
                    state.rightX,
                    false);

            const float rawRightY =
                bluetoothShadowRawAxis(
                    state.rightY,
                    true);

            next.leftX =
                static_cast<double>(
                    rawLeftX);

            next.leftY =
                static_cast<double>(
                    rawLeftY);

            next.rightX =
                static_cast<double>(
                    rawRightX);

            next.rightY =
                static_cast<double>(
                    rawRightY);

            next.leftTriggerProcessed =
                static_cast<float>(
                    next.leftTrigger);

            next.rightTriggerProcessed =
                static_cast<float>(
                    next.rightTrigger);

            bluetoothShadowShapeStick(
                state.leftX,
                state.leftY,
                next.leftXProcessed,
                next.leftYProcessed);

            bluetoothShadowShapeStick(
                state.rightX,
                state.rightY,
                next.rightXProcessed,
                next.rightYProcessed);

            std::memcpy(
                current,
                &next,
                sizeof(next));

            // Context-sensitive Bluetooth input timing parity.
            //
            // Native USB trigger/stick events originate during Starfield's
            // own gamepad polling stage. Scanner R2 and land-vehicle R2 need
            // that timing too; REV-8 vehicle/scanner sticks keep their proven
            // native-slot2 path.
            if (g_gameState) {
                g_gameState->dispatchBluetoothScannerSticksAtNativePoll(
                    state,
                    deltaSeconds,
                    gamepad);
            }

            // Do NOT touch gamepad +0x08 here.
            // Dynamic last-device presentation owns that field.
        }
        catch (...) {
            // Slot 2 must remain fail-safe. Input events continue through
            // SAD's proven native event bridge even if state mirroring fails.
        }
    }
    void removeBluetoothShadowDelegate() noexcept
    {
        if (!g_bluetoothShadowDelegate) {
            sds::setInputPresentationObserver(nullptr);
            return;
        }

        const auto shadow =
            g_bluetoothShadowDelegate;

        const auto handler =
            g_bluetoothPresentationHandlerAddress;

        if (handler >= 0x10000u) {

            std::uintptr_t currentDelegate = 0;

            if (readGamepadProbePointer(
                    handler + 0xC0u,
                    currentDelegate) &&
                currentDelegate ==
                    reinterpret_cast<std::uintptr_t>(shadow)) {

                // Detach before destruction so Starfield can never
                // call back through an object being torn down.
                //
                // If this was a USB -> Bluetooth shadow rebind, restore the
                // parked Starfield-owned native delegate first. We never
                // destroy or vtable-modify that native object.
                if (g_bluetoothParkedNativeDelegate != 0) {
                    *reinterpret_cast<std::uintptr_t*>(
                        handler + 0xC0u) =
                        g_bluetoothParkedNativeDelegate;

                    *reinterpret_cast<std::uint8_t*>(
                        handler + 0xB8u) =
                        g_bluetoothParkedHandlerPresent;

                    *reinterpret_cast<std::int32_t*>(
                        handler + 0x0Cu) =
                        g_bluetoothParkedHandlerStatus;

                    *reinterpret_cast<std::uint8_t*>(
                        g_bluetoothParkedNativeDelegate +
                        0x08u) =
                        g_bluetoothParkedNativeActive;

                    pluginLog(
                        "Bluetooth transport handoff: "
                        "native-delegate-restored "
                        "behavior=parked-object-preserved");
                }
                else {
                    *reinterpret_cast<std::uintptr_t*>(
                        handler + 0xC0u) = 0;

                    *reinterpret_cast<std::uint8_t*>(
                        handler + 0xB8u) = 0;

                    *reinterpret_cast<std::int32_t*>(
                        handler + 0x0Cu) = -1;
                }

                const auto vtable =
                    *reinterpret_cast<std::uintptr_t**>(
                        shadow);

                if (vtable && vtable[0]) {
                    using NativeDeletingDestructor =
                        void (*)(void*, std::uint32_t);

                    const auto destroy =
                        reinterpret_cast<
                            NativeDeletingDestructor>(
                                vtable[0]);

                    destroy(shadow, 1u);
                }

                pluginLog(
                    "Bluetooth shadow delegate: REMOVED");
            }
            else {
                // If Starfield replaced it itself, its native replacement
                // path owns destruction. Never double-free it here.
                pluginLog(
                    "Bluetooth shadow delegate: detached externally; "
                    "local ownership released without destructor");
            }
        }

        g_bluetoothShadowDelegate = nullptr;
        g_bluetoothShadowDelegateLogged = false;

        g_bluetoothParkedNativeDelegate = 0;
        g_bluetoothParkedNativeActive = 0;
        g_bluetoothParkedHandlerPresent = 0;
        g_bluetoothParkedHandlerStatus = -1;

        sds::setInputPresentationObserver(nullptr);
    }

    void applyBluetoothInputPresentationDevice(
        sds::InputPresentationDevice device) noexcept
    {
        try {
            if (!g_controller ||
                !g_controller->connected() ||
                !g_controller->bluetoothTransport()) {

                return;
            }

            std::uintptr_t target = 0;
            const char* targetName = "none";

            if (g_bluetoothShadowDelegate) {
                target =
                    reinterpret_cast<std::uintptr_t>(
                        g_bluetoothShadowDelegate);
                targetName = "shadow";
            }
            else {
                const auto handler =
                    g_bluetoothPresentationHandlerAddress;

                if (handler < 0x10000u) {
                    return;
                }

                std::uintptr_t retainedDelegate = 0;
                if (!readGamepadProbePointer(
                        handler + 0xC0u,
                        retainedDelegate) ||
                    retainedDelegate < 0x10000u) {

                    return;
                }

                std::uintptr_t retainedVtable = 0;
                if (!readGamepadProbePointer(
                        retainedDelegate,
                        retainedVtable)) {

                    return;
                }

                const auto retainedKind =
                    classifyGamepadProbeVtable(
                        retainedVtable,
                        gamepadProbeVtables());

                if (retainedKind != GamepadProbeKind::DualSense &&
                    retainedKind != GamepadProbeKind::GenericGamepad) {

                    return;
                }

                target = retainedDelegate;
                targetName = "native-retained";
            }

            if (target < 0x10000u) {
                return;
            }

            const std::uint8_t desired =
                device ==
                    sds::InputPresentationDevice::Gamepad ?
                    1u :
                    0u;

            auto* activeFlag =
                reinterpret_cast<std::uint8_t*>(
                    target + 0x08u);

            if (*activeFlag == desired) {
                return;
            }

            *activeFlag = desired;

            std::ostringstream line;
            line
                << "Bluetooth input presentation: device="
                << (desired != 0 ? "gamepad" : "keyboard-mouse")
                << " active="
                << static_cast<unsigned>(desired)
                << " target="
                << targetName;
            pluginLog(line.str());
        }
        catch (...) {
            pluginLog(
                "Bluetooth input presentation: transition failed");
        }
    }

    bool installBluetoothShadowDelegate() noexcept
    {
        if (!g_controller ||
            !g_controller->connected() ||
            !g_controller->bluetoothTransport()) {

            removeBluetoothShadowDelegate();
            return false;
        }

        if (g_bluetoothShadowDelegate) {
            return true;
        }

        const auto handler =
            g_bluetoothPresentationHandlerAddress;

        if (handler < 0x10000u) {
            return false;
        }

        const auto known =
            gamepadProbeVtables();

        if (known.genericGamepad == 0 ||
            known.dualSense == 0 ||
            known.gamepadHandler == 0) {
            return false;
        }

        std::uintptr_t handlerVtable = 0;

        if (!readGamepadProbePointer(
                handler,
                handlerVtable) ||
            handlerVtable != known.gamepadHandler) {

            pluginLog(
                "Bluetooth shadow delegate: SKIPPED "
                "reason=invalid-handler");
            return false;
        }

        std::uintptr_t existingDelegate = 0;

        if (!readGamepadProbePointer(
                handler + 0xC0u,
                existingDelegate)) {
            return false;
        }

        if (existingDelegate != 0) {
            std::uintptr_t existingVtable = 0;
            (void)readGamepadProbePointer(
                existingDelegate,
                existingVtable);

            auto existingKind =
                classifyGamepadProbeVtable(
                    existingVtable,
                    known);

            const bool staleNativeCandidate =
                existingKind == GamepadProbeKind::DualSense ||
                existingKind == GamepadProbeKind::GenericGamepad;

            if (!staleNativeCandidate) {
                // Never overwrite or destroy a real Starfield delegate.
                return false;
            }

            const auto now =
                std::chrono::steady_clock::now();

            if (g_bluetoothNativeHandoffRefreshAttempts >=
                kBluetoothNativeHandoffMaxAttempts) {

                if (g_bluetoothParkedNativeDelegate != 0) {
                    return false;
                }

                std::uintptr_t currentDelegate = 0;
                if (!readGamepadProbePointer(
                        handler + 0xC0u,
                        currentDelegate) ||
                    currentDelegate != existingDelegate) {

                    pluginLog(
                        "Bluetooth transport handoff: "
                        "native-delegate-park skipped "
                        "reason=handler-changed behavior=fail-safe");
                    return false;
                }

                g_bluetoothParkedNativeDelegate =
                    existingDelegate;

                g_bluetoothParkedNativeActive =
                    *reinterpret_cast<std::uint8_t*>(
                        existingDelegate + 0x08u);

                g_bluetoothParkedHandlerPresent =
                    *reinterpret_cast<std::uint8_t*>(
                        handler + 0xB8u);

                g_bluetoothParkedHandlerStatus =
                    *reinterpret_cast<std::int32_t*>(
                        handler + 0x0Cu);

                // Detach only the handler pointer. The native Starfield
                // delegate stays alive and untouched while SAD's Bluetooth
                // shadow is installed.
                *reinterpret_cast<std::uintptr_t*>(
                    handler + 0xC0u) = 0;

                *reinterpret_cast<std::uint8_t*>(
                    handler + 0xB8u) = 0;

                *reinterpret_cast<std::int32_t*>(
                    handler + 0x0Cu) = -1;

                existingDelegate = 0;

                pluginLog(
                    "Bluetooth transport handoff: "
                    "native-delegate-parked "
                    "kind=DualSense "
                    "behavior=temporary-shadow-rebind");
            }

            if (!g_bluetoothNativeHandoffRefreshPending) {
                g_bluetoothNativeHandoffRefreshPending = true;
                g_bluetoothNativeHandoffRefreshDue =
                    now + std::chrono::milliseconds(
                        kBluetoothNativeHandoffRetryMs);

                if (g_bluetoothNativeHandoffRefreshAttempts == 0) {
                    std::ostringstream line;
                    line
                        << "Bluetooth transport handoff:"
                        << " state=scheduled"
                        << " before="
                        << gamepadProbeKindName(existingKind)
                        << " settleMs="
                        << kBluetoothNativeHandoffRetryMs
                        << " attempts=0/"
                        << kBluetoothNativeHandoffMaxAttempts;
                    pluginLog(line.str());
                }

                return false;
            }

            if (now <
                g_bluetoothNativeHandoffRefreshDue) {
                return false;
            }

            g_bluetoothNativeHandoffRefreshPending = false;

            const auto starfieldBase =
                reinterpret_cast<std::uintptr_t>(
                    GetModuleHandleW(nullptr));

            if (starfieldBase == 0) {
                pluginLog(
                    "Bluetooth transport handoff: "
                    "native-selector-refresh=skipped "
                    "reason=no-starfield-base "
                    "behavior=fail-safe");
                g_bluetoothNativeHandoffRefreshAttempts =
                    kBluetoothNativeHandoffMaxAttempts;
                return false;
            }

            const auto selectorAddress =
                starfieldBase +
                kNativeGamepadSelectorRva;

            if (!nativeGamepadSelectorSignatureMatches(
                    selectorAddress)) {

                pluginLog(
                    "Bluetooth transport handoff: "
                    "native-selector-refresh=skipped "
                    "reason=selector-signature-mismatch "
                    "behavior=fail-safe");
                g_bluetoothNativeHandoffRefreshAttempts =
                    kBluetoothNativeHandoffMaxAttempts;
                return false;
            }

            using NativeGamepadSelector =
                void (*)(void*);

            const auto selector =
                reinterpret_cast<
                    NativeGamepadSelector>(
                        selectorAddress);

            selector(
                reinterpret_cast<void*>(
                    handler));

            ++g_bluetoothNativeHandoffRefreshAttempts;

            existingDelegate = 0;
            existingVtable = 0;

            (void)readGamepadProbePointer(
                handler + 0xC0u,
                existingDelegate);

            if (existingDelegate != 0) {
                (void)readGamepadProbePointer(
                    existingDelegate,
                    existingVtable);
            }

            existingKind =
                classifyGamepadProbeVtable(
                    existingVtable,
                    known);

            if (existingDelegate == 0) {
                std::ostringstream line;
                line
                    << "Bluetooth transport handoff:"
                    << " native-selector-refresh=done"
                    << " after=None"
                    << " attempts="
                    << g_bluetoothNativeHandoffRefreshAttempts
                    << "/"
                    << kBluetoothNativeHandoffMaxAttempts
                    << " result=handler-free";
                pluginLog(line.str());
            }
            else if (
                g_bluetoothNativeHandoffRefreshAttempts <
                    kBluetoothNativeHandoffMaxAttempts &&
                (existingKind == GamepadProbeKind::DualSense ||
                 existingKind == GamepadProbeKind::GenericGamepad)) {

                g_bluetoothNativeHandoffRefreshPending = true;
                g_bluetoothNativeHandoffRefreshDue =
                    now + std::chrono::milliseconds(
                        kBluetoothNativeHandoffRetryMs);

                std::ostringstream line;
                line
                    << "Bluetooth transport handoff:"
                    << " native-selector-refresh=done"
                    << " after="
                    << gamepadProbeKindName(existingKind)
                    << " attempts="
                    << g_bluetoothNativeHandoffRefreshAttempts
                    << "/"
                    << kBluetoothNativeHandoffMaxAttempts
                    << " result=retry-scheduled";
                pluginLog(line.str());

                return false;
            }
            else {
                std::ostringstream line;
                line
                    << "Bluetooth transport handoff:"
                    << " native-selector-refresh=done"
                    << " after="
                    << gamepadProbeKindName(existingKind)
                    << " attempts="
                    << g_bluetoothNativeHandoffRefreshAttempts
                    << "/"
                    << kBluetoothNativeHandoffMaxAttempts
                    << " result=";

                if (g_bluetoothNativeHandoffRefreshAttempts >=
                    kBluetoothNativeHandoffMaxAttempts) {
                    line << "exhausted-fail-safe";
                    g_bluetoothNativeHandoffExhaustedLogged = true;
                }
                else {
                    line << "delegate-changed-fail-safe";
                }

                pluginLog(line.str());

                // Never overwrite or destroy a real Starfield delegate.
                return false;
            }
        }

        auto restoreParkedNativeOnInstallFailure =
            [&]() noexcept
        {
            if (g_bluetoothParkedNativeDelegate == 0) {
                return;
            }

            std::uintptr_t currentDelegate = 0;
            (void)readGamepadProbePointer(
                handler + 0xC0u,
                currentDelegate);

            if (currentDelegate == 0) {
                *reinterpret_cast<std::uintptr_t*>(
                    handler + 0xC0u) =
                    g_bluetoothParkedNativeDelegate;

                *reinterpret_cast<std::uint8_t*>(
                    handler + 0xB8u) =
                    g_bluetoothParkedHandlerPresent;

                *reinterpret_cast<std::int32_t*>(
                    handler + 0x0Cu) =
                    g_bluetoothParkedHandlerStatus;

                *reinterpret_cast<std::uint8_t*>(
                    g_bluetoothParkedNativeDelegate +
                    0x08u) =
                    g_bluetoothParkedNativeActive;
            }

            g_bluetoothParkedNativeDelegate = 0;
            g_bluetoothParkedNativeActive = 0;
            g_bluetoothParkedHandlerPresent = 0;
            g_bluetoothParkedHandlerStatus = -1;
        };

        const auto module =
            reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));

        if (module == 0) {
            restoreParkedNativeOnInstallFailure();
            return false;
        }

        constexpr std::uintptr_t
            kStarfieldAllocatorRva = 0x22C7320u;

        constexpr std::uintptr_t
            kGamepadBaseConstructorRva = 0x22FA620u;

        constexpr std::size_t
            kGenericGamepadBytes = 0xD8u;

        using NativeAllocate =
            void* (*)(std::size_t);

        using NativeGamepadCtor =
            void* (*)(void*);

        const auto allocate =
            reinterpret_cast<NativeAllocate>(
                module +
                kStarfieldAllocatorRva);

        const auto construct =
            reinterpret_cast<NativeGamepadCtor>(
                module +
                kGamepadBaseConstructorRva);

        auto* shadow =
            allocate(kGenericGamepadBytes);

        if (!shadow) {
            pluginLog(
                "Bluetooth shadow delegate: SKIPPED "
                "reason=allocation-failed");
            restoreParkedNativeOnInstallFailure();
            return false;
        }

        std::memset(
            shadow,
            0,
            kGenericGamepadBytes);

        if (!construct(shadow)) {
            pluginLog(
                "Bluetooth shadow delegate: SKIPPED "
                "reason=constructor-failed");
            restoreParkedNativeOnInstallFailure();
            return false;
        }

        const auto* genericVtable =
            reinterpret_cast<
                const std::uintptr_t*>(
                    known.genericGamepad);

        const auto* dualVtable =
            reinterpret_cast<
                const std::uintptr_t*>(
                    known.dualSense);

        for (std::size_t i = 0;
             i < g_bluetoothShadowVtable.size();
             ++i) {

            g_bluetoothShadowVtable[i] =
                genericVtable[i];
        }

        // slot 2:
        // Starfield's normal controller backend polling.
        // Replace only this entry with a no-op.
        g_bluetoothShadowVtable[2] =
            reinterpret_cast<std::uintptr_t>(
                &bluetoothShadowGamepadPollMirror);

        // slot 11:
        // Proven controller-type discriminator.
        //
        // Real DualSense => true
        // Generic gamepad => false
        //
        // Reuse Starfield's own DualSense implementation.
        g_bluetoothShadowVtable[11] =
            dualVtable[11];

        *reinterpret_cast<std::uintptr_t*>(
            shadow) =
            reinterpret_cast<std::uintptr_t>(
                g_bluetoothShadowVtable.data());

        // Match the native selector's installation contract.
        *reinterpret_cast<std::uintptr_t*>(
            reinterpret_cast<std::uintptr_t>(shadow) +
            0xC0u) =
            handler;

        *reinterpret_cast<std::int32_t*>(
            reinterpret_cast<std::uintptr_t>(shadow) +
            0x0Cu) =
            0;

        // Handler presence first.
        *reinterpret_cast<std::int32_t*>(
            handler + 0x0Cu) =
            0;

        *reinterpret_cast<std::uint8_t*>(
            handler + 0xB8u) =
            1;

        // Then publish the delegate pointer.
        *reinterpret_cast<std::uintptr_t*>(
            handler + 0xC0u) =
            reinterpret_cast<std::uintptr_t>(
                shadow);

        // Native generic slot 1 allocates/initializes its two
        // controller-state buffers at +0xC8/+0xD0.
        using NativeInit =
            void (*)(void*);

        const auto initialize =
            reinterpret_cast<NativeInit>(
                g_bluetoothShadowVtable[1]);

        initialize(shadow);

        // Install Starfield's native DualSense/PSN physical-button
        // mapping table into the common BSPCGamepadDevice base.
        //
        // Starfield+0x22FB890 is the same common mapping parser used
        // by the 0xD8 base gamepad constructor, so it is safe for the
        // generic-layout shadow. Native DualSense slot 1 calls this
        // helper with Starfield+0x4A56AF0 before performing its
        // ScePad-specific work. We intentionally reproduce ONLY this
        // common mapping step.
        constexpr std::uintptr_t
            kGamepadMappingParserRva =
                0x22FB890u;

        constexpr std::uintptr_t
            kDualSenseMappingDescriptorRva =
                0x4A56AF0u;

        using NativeGamepadMappingParser =
            bool (*)(
                void*,
                const wchar_t*);

        const auto starfieldBase =
            reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));

        bool dualSenseMappingInstalled = false;

        if (starfieldBase != 0) {
            const auto installMapping =
                reinterpret_cast<
                    NativeGamepadMappingParser>(
                        starfieldBase +
                        kGamepadMappingParserRva);

            const auto dualSenseMapping =
                reinterpret_cast<
                    const wchar_t*>(
                        starfieldBase +
                        kDualSenseMappingDescriptorRva);

            dualSenseMappingInstalled =
                installMapping(
                    shadow,
                    dualSenseMapping);
        }

        if (dualSenseMappingInstalled) {
            pluginLog(
                "Bluetooth shadow mapping: DualSense PSN descriptor installed "
                "parser=Starfield+0x22FB890 descriptor=Starfield+0x4A56AF0");
        } else {
            pluginLog(
                "Bluetooth shadow mapping: DualSense PSN descriptor returned false "
                "parser=Starfield+0x22FB890");
        }

        g_bluetoothNativeHandoffRefreshPending = false;
        g_bluetoothNativeHandoffRefreshAttempts = 0;
        g_bluetoothNativeHandoffExhaustedLogged = false;
        g_bluetoothNativeHandoffRefreshDue = {};

        g_bluetoothShadowDelegate =
            shadow;

        sds::setInputPresentationObserver(
            &applyBluetoothInputPresentationDevice);

        pluginLog(
            "Bluetooth shadow delegate: INSTALLED "
            "object=native-generic-layout "
            "slot2=SAD-state-mirror "
            "slot11=DualSense-true "
            "hidPolling=SAD-only");

        return true;
    }

    void updateBluetoothShadowDelegate() noexcept
    {
        try {
            if (!g_controller ||
                !g_controller->connected() ||
                !g_controller->bluetoothTransport()) {

                g_bluetoothNativeHandoffRefreshPending = false;
                g_bluetoothNativeHandoffRefreshAttempts = 0;
                g_bluetoothNativeHandoffExhaustedLogged = false;
                g_bluetoothNativeHandoffRefreshDue = {};

                removeBluetoothShadowDelegate();
                return;
            }

            // Presentation switching must remain available even when
            // Starfield retains its native USB DualSense delegate and the SAD
            // shadow cannot be reinstalled.
            sds::setInputPresentationObserver(
                &applyBluetoothInputPresentationDevice);

            if (!installBluetoothShadowDelegate()) {
                return;
            }

            const auto handler =
                g_bluetoothPresentationHandlerAddress;

            const auto shadow =
                reinterpret_cast<std::uintptr_t>(
                    g_bluetoothShadowDelegate);

            if (handler < 0x10000u ||
                shadow < 0x10000u) {
                return;
            }

            std::uintptr_t currentDelegate = 0;

            if (!readGamepadProbePointer(
                    handler + 0xC0u,
                    currentDelegate) ||
                currentDelegate != shadow) {

                pluginLog(
                    "Bluetooth shadow delegate: LOST "
                    "reason=handler-delegate-changed");

                g_bluetoothShadowDelegate = nullptr;
                g_bluetoothShadowDelegateLogged = false;
                sds::setInputPresentationObserver(nullptr);
                return;
            }

            // Maintain the same presence contract proven by the
            // previous live-memory test.
            *reinterpret_cast<std::int32_t*>(
                handler + 0x0Cu) =
                0;

            *reinterpret_cast<std::uint8_t*>(
                handler + 0xB8u) =
                1;

            // The shadow +0x08 active-input field is intentionally NOT
            // forced here. Actual Bluetooth controller activity sets it
            // to 1; native keyboard/mouse activity clears it to 0.




            if (!g_bluetoothShadowDelegateLogged) {
                const auto vtable =
                    *reinterpret_cast<
                        std::uintptr_t*>(
                            shadow);

                std::ostringstream line;

                line
                    << "Bluetooth shadow delegate: ACTIVE"
                    << " handler=0x"
                    << std::hex << std::uppercase
                    << handler
                    << " delegate=0x"
                    << shadow
                    << " vtable=0x"
                    << vtable
                    << std::dec
                    << " slot2=SAD-state-mirror"
                    << " slot11=DualSense-true"
                    << " StarfieldPolling=no"
                    << " SADHidOwner=yes"
                    << " activeInput=dynamic-last-device";

                pluginLog(line.str());

                g_bluetoothShadowDelegateLogged = true;
            }
        }
        catch (...) {
            pluginLog(
                "Bluetooth shadow delegate: exception; "
                "diagnostic disabled");
        }
    }
    void logBluetoothPresentationStateOnce() noexcept
    {
        static bool logged = false;

        if (logged) {
            return;
        }

        logged = true;

        try {
            const auto known = gamepadProbeVtables();

            if (known.gamepadHandler == 0) {
                pluginLog(
                    "Bluetooth presentation state: result=unavailable "
                    "reason=gamepad-handler-vtable-unresolved");
                return;
            }

            const auto module =
                reinterpret_cast<std::uintptr_t>(
                    GetModuleHandleW(nullptr));

            if (module == 0) {
                pluginLog(
                    "Bluetooth presentation state: result=unavailable "
                    "reason=module-base-unavailable");
                return;
            }

            const auto* dos =
                reinterpret_cast<const IMAGE_DOS_HEADER*>(module);

            if (!dos ||
                dos->e_magic != IMAGE_DOS_SIGNATURE) {
                pluginLog(
                    "Bluetooth presentation state: result=unavailable "
                    "reason=invalid-dos-header");
                return;
            }

            const auto* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    module +
                    static_cast<std::uintptr_t>(
                        dos->e_lfanew));

            if (!nt ||
                nt->Signature != IMAGE_NT_SIGNATURE) {
                pluginLog(
                    "Bluetooth presentation state: result=unavailable "
                    "reason=invalid-nt-header");
                return;
            }

            const auto* section =
                IMAGE_FIRST_SECTION(nt);

            for (unsigned i = 0;
                 i < nt->FileHeader.NumberOfSections;
                 ++i) {

                const auto characteristics =
                    section[i].Characteristics;

                if ((characteristics &
                        IMAGE_SCN_MEM_WRITE) == 0 ||
                    (characteristics &
                        IMAGE_SCN_MEM_DISCARDABLE) != 0) {
                    continue;
                }

                const auto start =
                    module + section[i].VirtualAddress;

                const auto size =
                    static_cast<std::size_t>(
                        section[i].Misc.VirtualSize != 0
                            ? section[i].Misc.VirtualSize
                            : section[i].SizeOfRawData);

                const auto end =
                    start + size;

                auto managerAddress =
                    (start + 7u) &
                    ~static_cast<std::uintptr_t>(7u);

                for (;
                     managerAddress +
                             sizeof(std::uintptr_t) <= end;
                     managerAddress +=
                         sizeof(std::uintptr_t)) {

                    std::uintptr_t managerVtable = 0;

                    if (!readGamepadProbePointer(
                            managerAddress,
                            managerVtable) ||
                        !isInputManagerVtable(
                            managerVtable,
                            known)) {
                        continue;
                    }

                    std::uintptr_t handlerAddress = 0;

                    if (!readGamepadProbePointer(
                            managerAddress + 0x78u,
                            handlerAddress) ||
                        handlerAddress < 0x10000u) {
                        continue;
                    }

                    std::uintptr_t handlerVtable = 0;

                    if (!readGamepadProbePointer(
                            handlerAddress,
                            handlerVtable) ||
                        handlerVtable !=
                            known.gamepadHandler) {
                        continue;
                    }

                    std::uintptr_t delegateAddress = 0;

                    if (!readGamepadProbePointer(
                            handlerAddress + 0xC0u,
                            delegateAddress)) {
                        continue;
                    }

                    std::uintptr_t delegateVtable = 0;

                    if (delegateAddress >= 0x10000u) {
                        (void)readGamepadProbePointer(
                            delegateAddress,
                            delegateVtable);
                    }

                    const auto kind =
                        classifyGamepadProbeVtable(
                            delegateVtable,
                            known);

                    g_bluetoothPresentationHandlerAddress =
                        handlerAddress;

                    std::ostringstream line;

                    line
                        << "Bluetooth presentation state:"
                        << " manager=0x"
                        << std::hex << std::uppercase
                        << managerAddress
                        << " handler=0x"
                        << handlerAddress
                        << " delegate=0x"
                        << delegateAddress
                        << " delegateVtable=0x"
                        << delegateVtable
                        << std::dec
                        << " kind="
                        << gamepadProbeKindName(kind)
                        << " behavior=read-only";

                    pluginLog(line.str());
                    return;
                }
            }

            pluginLog(
                "Bluetooth presentation state: "
                "result=no-live-gamepad-handler-found "
                "behavior=read-only");
        } catch (...) {
            pluginLog(
                "Bluetooth presentation state: "
                "result=exception "
                "behavior=read-only");
        }
    }
    void updateBluetoothGameplayInputBridge(
        std::chrono::steady_clock::time_point now) noexcept
    {
        static bool active = false;
        static bool activationLogged = false;
        static bool shadowlessFallbackLogged = false;
        static std::uint64_t lastGeneration = 0;
        static auto lastDispatch =
            std::chrono::steady_clock::time_point{};

        const bool bluetoothReady =
            g_controller &&
            g_gameState &&
            g_controller->connected() &&
            g_controller->bluetoothTransport();

        if (!bluetoothReady) {
            if (active && g_gameState) {
                g_gameState->resetBluetoothPhysicalInput();
                pluginLog(
                    "Bluetooth gameplay input bridge: RESET reason=transport-inactive");
            }

            active = false;
            shadowlessFallbackLogged = false;
            lastGeneration = 0;
            lastDispatch = {};
            return;
        }

        logBluetoothPresentationStateOnce();
        const auto snapshot =
            g_controller->latestInputSnapshot();

        if (!snapshot) {
            return;
        }

        float deltaSeconds = 0.0F;

        if (lastDispatch !=
            std::chrono::steady_clock::time_point{}) {
            deltaSeconds =
                std::chrono::duration<float>(
                    now - lastDispatch).count();
        }

        // When the SAD shadow owns Starfield slot 2, L3 and Circle keep
        // their native-poll transition timing. If Starfield retains a stale
        // native USB delegate after a USB -> Bluetooth handoff, do not lose
        // those buttons: fall back to the proven runtime physical-event path.
        const bool shadowTimingAvailable =
            g_bluetoothShadowDelegate != nullptr;

        auto replayState =
            snapshot->state;

        if (shadowTimingAvailable) {
            replayState.l3 =
                false;

            replayState.circle =
                false;

            // Square/Triangle now dispatch at native slot-2 time just like
            // L3/Circle. Only mask them when the SAD shadow actually exists:
            // shadowless fallback keeps the existing runtime bridge intact.
            replayState.square =
                false;
            replayState.triangle =
                false;
        }
        else if (!shadowlessFallbackLogged) {
            pluginLog(
                "Bluetooth gameplay input bridge: SHADOWLESS-FALLBACK "
                "reason=native-delegate-retained "
                "buttons=all-runtime sticks=runtime "
                "scanner=native-slot2-unavailable");
            shadowlessFallbackLogged = true;
        }

        g_gameState->dispatchBluetoothPhysicalInput(
            replayState,
            deltaSeconds,
            shadowTimingAvailable ?
                g_bluetoothShadowDelegate :
                nullptr);

        lastGeneration = snapshot->generation;
        lastDispatch = now;
        active = true;

        if (!activationLogged) {
            pluginLog(
                "Bluetooth gameplay input bridge: ACTIVE source=SAD-HID "
                "destination=Starfield-native-physical-events "
                "buttons=generic-gamepad triggers=analog-9/10 "
                "sticks=native-11/12 nativeBluetoothReselection=bypassed cadence=runtime-tick-replay");
            activationLogged = true;
        }
    }
    void runtimeTick()
    {
        if (g_runtimeShutdown) {
            return;
        }

        if (auto* main = RE::Main::GetSingleton(); main && main->quitGame) {
            shutdownRuntime();
            return;
        }

        // During NG+ world destruction/recreation, do not poll PlayerCharacter,
        // PlayerCamera, equipped inventory, health, ship, or vehicle state.
        // Reopen only after Starfield exposes the new gameplay HUD, with no
        // loading/credits/character-creation menu still displayed.
        if (g_ngPlusTransitionActive.load(std::memory_order_acquire)) {
            auto* ui = RE::UI::GetSingleton();
            const bool gameplayReady = ui &&
                ui->IsMenuOpen(RE::BSFixedString("HUDMenu")) &&
                !ui->IsMenuOpen(RE::BSFixedString("LoadingMenu")) &&
                !ui->IsMenuOpen(RE::BSFixedString("EndGameCreditsMenu")) &&
                !ui->IsMenuOpen(RE::BSFixedString("CharGenMenu")) &&
                !ui->IsMenuOpen(RE::BSFixedString("ChargenMenu"));
            if (!gameplayReady) {
                return;
            }
            g_ngPlusTransitionActive.store(false, std::memory_order_release);
            pluginLog("NG+ safety gate: gameplay HUD restored; runtime polling resumed");
        }

        updateBluetoothShadowDelegate();
        updateNativeDualSenseReselection(std::chrono::steady_clock::now());
        updateBluetoothGameplayInputBridge(std::chrono::steady_clock::now());

        if (g_controller) {
            const bool connected = g_controller->connected();
            const bool previous =
                g_boostpackProductionControllerConnected.exchange(
                    connected,
                    std::memory_order_acq_rel);
            if (previous != connected) {
                refreshBoostpackProductionContext(
                    connected ? "controller-reconnected" : "controller-disconnected");
            }
        }

        if (g_controller && g_gameState) {
            while (const auto action = g_controller->tryPopInputAction()) {
                applyBluetoothInputPresentationDevice(
                    sds::InputPresentationDevice::Gamepad);

                const bool queued = g_gameState->queueNativeInputAction(*action);
                if (queued && g_uiSpeakerPlayback) {
                    g_uiSpeakerPlayback->noteQuickMenuShortcut(*action);
                }
            }
        }

        bootstrapStartupEquippedWeaponIfReady();



        if (!g_shipPilotActive.load(std::memory_order_acquire) &&
            g_onFootRefreshPending.load(std::memory_order_acquire)) {
            bool loadingMenuOpen = false;
            if (auto* ui = RE::UI::GetSingleton()) {
                loadingMenuOpen = ui->IsMenuOpen(RE::BSFixedString("LoadingMenu"));
            }

            if (!loadingMenuOpen &&
                g_onFootRefreshPending.exchange(false, std::memory_order_acq_rel)) {
                const bool weaponRefreshed = refreshCurrentEquippedWeaponState();
                const bool healthRefreshed = g_gameState && g_gameState->refreshPlayerHealth();

                std::ostringstream diagnostic;
                diagnostic << "Ship pilot context: ON-FOOT-REFRESH weapon="
                           << (weaponRefreshed ? "fresh" : "unavailable")
                           << " health=" << (healthRefreshed ? "fresh" : "unavailable");
                pluginLog(diagnostic.str());
            }
        }

        const auto runtimeNow = std::chrono::steady_clock::now();
        if (g_haptics) {
            (void)g_haptics->tick(runtimeNow);
        }
        tickBoostpackProduction(runtimeNow);

        if (g_gameState) {
            g_gameState->pollNativeInputInjection();
            g_gameState->pollShipLightbarSeatAuthority();
            const bool pilotActiveForShipPoll = g_shipPilotActive.load(std::memory_order_acquire);
            const bool launchLandingReconEnabled =
                g_shipLaunchLandingReconEnabled.load(std::memory_order_acquire);
            const bool precisionTouchdownPoll = launchLandingReconEnabled &&
                g_shipLaunchLandingReconProbe.requiresPrecisionTouchdownPolling();

            if (precisionTouchdownPoll &&
                g_shipLaunchLandingRumblePhase.load(std::memory_order_acquire) == ShipLaunchLandingRumblePhase::None) {
                startShipLaunchLandingHaptics(ShipLaunchLandingRumblePhase::Landing);
            } else if (!precisionTouchdownPoll &&
                g_shipLaunchLandingRumblePhase.load(std::memory_order_acquire) == ShipLaunchLandingRumblePhase::Landing) {
                stopShipLaunchLandingHaptics("landing-cancelled");
            }
            maybeStartShipTakeoffRumble(pilotActiveForShipPoll, precisionTouchdownPoll);

            if (pilotActiveForShipPoll) {
                if (const auto propulsion = g_gameState->pollShipPropulsionState()) {
                    const auto propulsionWhen = std::chrono::steady_clock::now();
                    g_lastShipLanded.store(propulsion->landed, std::memory_order_release);
                    g_lastShipDocked.store(propulsion->docked, std::memory_order_release);
                    g_lastShipStateValid.store(true, std::memory_order_release);
                    if (launchLandingReconEnabled && !precisionTouchdownPoll) {
                        if (const auto boundary = g_shipLaunchLandingReconProbe.observeShipState(*propulsion, propulsionWhen)) {
                            logShipLaunchLandingBoundary(*boundary);
                            if (boundary->type == sds::ShipLaunchLandingTransition::Takeoff &&
                                g_shipLaunchLandingRumblePhase.load(std::memory_order_acquire) ==
                                    ShipLaunchLandingRumblePhase::Takeoff) {
                                const bool startTakeoffHaptics =
                                    !g_shipTakeoffBoundaryObserved.exchange(
                                        true,
                                        std::memory_order_acq_rel);

                                if (startTakeoffHaptics) {
                                    g_shipLaunchLandingStartedAtUs.store(
                                        steadyMicros(propulsionWhen),
                                        std::memory_order_release);

                                    if (g_haptics) {
                                        (void)g_haptics->setShipLaunchLandingRumble(
                                            true,
                                            "takeoff");
                                    }

                                    dispatchShipLaunchLandingTriggerSemantic(
                                        sds::GameEventType::ShipLaunchLandingHapticsStarted,
                                        ShipLaunchLandingRumblePhase::Takeoff,
                                        propulsionWhen);

                                    pluginLog(
                                        "Ship launch/landing haptics: stage=start phase=takeoff authority=landed-to-airborne-boundary");
                                }
                            }
                        }
                    }
                    if (g_haptics) {
                        (void)g_haptics->handleShipPropulsionState(*propulsion);
                    }
                }
            }

            if (precisionTouchdownPoll) {
                if (const auto landingPrecision = g_gameState->pollShipLandingReconStatePrecision()) {
                    if (const auto boundary =
                        g_shipLaunchLandingReconProbe.observeShipState(landingPrecision->state, landingPrecision->when)) {
                        logShipLaunchLandingBoundary(*boundary);
                        if (boundary->type == sds::ShipLaunchLandingTransition::Touchdown) {
                            pluginLog("Ship launch/landing haptics: landing-touchdown-observed action=landing-sequence-end");
                            stopShipLaunchLandingHaptics("landing-sequence-end");
                            dispatchShipTouchdownHaptic(*boundary, landingPrecision->when);
                        }
                    }
                    if (g_shipLaunchLandingRumblePhase.load(std::memory_order_acquire) == ShipLaunchLandingRumblePhase::Landing &&
                        !g_shipLaunchLandingReconProbe.requiresPrecisionTouchdownPolling()) {
                        stopShipLaunchLandingHaptics("landing-cancelled");
                    }
                }
            } else if (!pilotActiveForShipPoll && launchLandingReconEnabled &&
                g_shipLaunchLandingReconProbe.requiresWwiseCapture()) {
                if (const auto landingDiagnostic = g_gameState->pollShipLandingReconState()) {
                    const auto diagnosticWhen = std::chrono::steady_clock::now();
                    if (const auto boundary =
                        g_shipLaunchLandingReconProbe.observeShipState(*landingDiagnostic, diagnosticWhen)) {
                        logShipLaunchLandingBoundary(*boundary);
                    }
                }
            }

            if (g_shipLaunchLandingRumblePhase.load(std::memory_order_acquire) == ShipLaunchLandingRumblePhase::Takeoff) {
                const auto now = std::chrono::steady_clock::now();
                const auto startedAtUs = g_shipLaunchLandingStartedAtUs.load(std::memory_order_acquire);
                const bool timedOut = startedAtUs > 0 &&
                    steadyMicros(now) - startedAtUs >=
                        std::chrono::duration_cast<std::chrono::microseconds>(kShipTakeoffHapticsMax).count();
                if (!pilotActiveForShipPoll) {
                    stopShipLaunchLandingHaptics("takeoff-pilot-lost");
                } else if (g_shipBallisticBlockingMenuMask.load(std::memory_order_acquire) != 0) {
                    stopShipLaunchLandingHaptics("takeoff-blocked");
                } else if (timedOut) {
                    stopShipLaunchLandingHaptics("takeoff-timeout");
                }
            }
            (void)g_gameState->pollLandVehicleReconState();
            g_gameState->pollHealth();


        }

        if (g_audioCapture) {
            bool dialogueMenuActive = false;
            bool dataslateMenuActive = false;
            g_remoteVoBlocked = true;
            if (auto* ui = RE::UI::GetSingleton()) {
                dialogueMenuActive =
                    ui->IsMenuOpen(RE::BSFixedString("DialogueMenu"));
                dataslateMenuActive =
                    ui->IsMenuOpen(RE::BSFixedString("DataSlateMenu"));
                g_remoteVoBlocked = ui->IsMenuOpen(RE::BSFixedString("MainMenu")) ||
                    ui->IsMenuOpen(RE::BSFixedString("LoadingMenu"));
            }
            if (g_remoteVoAudioPipeline && (g_remoteVoBlocked || !g_speakerManager ||
                !g_speakerManager->categoryEnabled(sds::SpeakerCategory::Comms))) {
                g_remoteVoAudioPipeline->clear();
            }

            g_dataslateMenuActive.store(
                dataslateMenuActive,
                std::memory_order_release);

            const bool dataslateStopRequested =
                g_dataslatePauseRequested.load(std::memory_order_acquire);

            if (dataslateStopRequested &&
                g_dataslateControllerVoiceActive.load(std::memory_order_acquire) &&
                g_speakerManager &&
                g_speakerManager->active()) {

                sds::PreparedSpeakerPcm silence{};
                silence.frames.push_back(
                    { .left = 0.0F, .right = 0.0F });
                silence.gain = 1.0F;

                const std::uint32_t stopEventId =
                    kDataslatePauseEventId;

                const sds::CapturedSoundIdentity identity{
                    .id =
                        (static_cast<std::uint64_t>(stopEventId) << 32u) |
                        0x24DB9834u,
                    .controllerOnlySafe = false,
                };

                const bool stopped =
                    g_speakerManager->submitCaptured(
                        std::move(silence),
                        sds::SpeakerCategory::Comms,
                        identity,
                        true);

                if (stopped) {
                    g_dataslateControllerVoiceActive.store(
                        false,
                        std::memory_order_release);

                    {
                        std::scoped_lock lock(
                            g_dataslateVoiceQueueMutex);
                        g_dataslateVoiceQueue.clear();
                        g_dataslatePlaybackDeadline = {};
                    }

                    pluginLog(
                        "Dataslate controller voice: STOP reason=pause-runtime");
                }
            }

            if (dataslateStopRequested) {
                std::scoped_lock lock(
                    g_dataslateVoiceQueueMutex);
                g_dataslateVoiceQueue.clear();
                g_dataslatePlaybackDeadline = {};
            }

            if (!dataslateStopRequested &&
                g_dataslateControllerVoiceActive.load(std::memory_order_acquire)) {

                DataslateQueuedVoiceChunk nextChunk{};
                bool haveNextChunk = false;
                std::size_t remainingQueued = 0;
                bool sequenceFinished = false;

                const auto dataslateNow =
                    std::chrono::steady_clock::now();

                {
                    std::scoped_lock lock(
                        g_dataslateVoiceQueueMutex);

                    if (g_dataslatePlaybackDeadline.time_since_epoch().count() != 0 &&
                        dataslateNow >= g_dataslatePlaybackDeadline) {

                        if (!g_dataslateVoiceQueue.empty()) {
                            nextChunk =
                                std::move(
                                    g_dataslateVoiceQueue.front());

                            g_dataslateVoiceQueue.erase(
                                g_dataslateVoiceQueue.begin());

                            remainingQueued =
                                g_dataslateVoiceQueue.size();

                            haveNextChunk = true;
                        } else {
                            g_dataslatePlaybackDeadline = {};
                        }
                    }

                    if (!haveNextChunk &&
                        g_dataslatePlaybackDeadline.time_since_epoch().count() == 0 &&
                        g_dataslateContinuationDeadline.time_since_epoch().count() != 0 &&
                        dataslateNow >= g_dataslateContinuationDeadline) {

                        g_dataslateContinuationDeadline = {};
                        sequenceFinished = true;
                    }
                }

                if (sequenceFinished) {
                    g_dataslateControllerVoiceActive.store(
                        false,
                        std::memory_order_release);

                    pluginLog(
                        "Dataslate controller voice: sequence complete");
                }

                if (haveNextChunk) {
                    bool nextSubmitted = false;

                    if (g_speakerManager &&
                        g_speakerManager->active() &&
                        g_speakerManager->categoryEnabled(
                            sds::SpeakerCategory::Comms)) {

                        const sds::CapturedSoundIdentity identity{
                            .id =
                                (static_cast<std::uint64_t>(
                                    sds::kDataslateVoEventId) << 32u) |
                                static_cast<std::uint64_t>(
                                    nextChunk.sequence & 0xFFFFFFFFu),
                            .controllerOnlySafe = false,
                        };

                        nextSubmitted =
                            g_speakerManager->submitCaptured(
                                std::move(nextChunk.pcm),
                                sds::SpeakerCategory::Comms,
                                identity,
                                true);
                    }

                    if (nextSubmitted) {
                        {
                            std::scoped_lock lock(
                                g_dataslateVoiceQueueMutex);

                            g_dataslatePlaybackDeadline =
                                dataslateNow +
                                std::chrono::milliseconds(
                                    nextChunk.durationMs);

                            g_dataslateContinuationDeadline =
                                g_dataslatePlaybackDeadline +
                                kDataslateContinuationGrace;
                        }

                        char nextLog[256]{};
                        std::snprintf(
                            nextLog,
                            sizeof(nextLog),
                            "Dataslate controller voice: NEXT seq=%llu durationMs=%llu pending=%zu",
                            static_cast<unsigned long long>(
                                nextChunk.sequence),
                            static_cast<unsigned long long>(
                                nextChunk.durationMs),
                            remainingQueued);
                        pluginLog(nextLog);
                    } else {
                        g_dataslateControllerVoiceActive.store(
                            false,
                            std::memory_order_release);

                        {
                            std::scoped_lock lock(
                                g_dataslateVoiceQueueMutex);
                            g_dataslateVoiceQueue.clear();
                            g_dataslatePlaybackDeadline = {};
                        }

                        pluginLog(
                            "Dataslate controller voice: queued submit rejected; sequence stopped");
                    }
                }
            }

            g_audioCapture->setDialogueMenuActive(dialogueMenuActive);
            const bool pilotActive = g_shipPilotActive.load(std::memory_order_acquire);
            const bool launchLandingReconCaptureArmed =
                g_shipLaunchLandingReconEnabled.load(std::memory_order_acquire) &&
                (pilotActive || g_shipLaunchLandingReconProbe.requiresWwiseCapture());
            const bool landVehicleReconCaptureArmed =
                g_gameState && g_gameState->landVehicleReconCorrelationArmed();
            const bool boostpackProductionArmed = boostpackProductionCaptureArmed();
            const bool shipCaptureArmed =
                launchLandingReconCaptureArmed || landVehicleReconCaptureArmed ||
                boostpackProductionArmed ||
                (pilotActive && g_shipWeaponSemanticCache && g_shipWeaponSemanticCache->ready());
            g_audioCapture->setShipWeaponObservationArmed(shipCaptureArmed);
            g_audioCapture->drainDiagnostics();
            drainRemoteVoPlayback();
            if (g_musicRecon) {
                try {
                    g_musicRecon->noteDroppedWwise(g_audioCapture->takeMusicReconDropped());
                    for (auto& request : g_musicRecon->takeResolveRequests(32u)) {
                        if (!g_weaponAudioPipeline ||
                            !g_weaponAudioPipeline->tryEnqueueMusicRecon(std::move(request))) {
                            // Diagnostic resolution loss is non-fatal and never changes gameplay/controller behavior.
                        }
                    }
                } catch (...) {
                    pluginLog("Music recon: runtime handoff exception ignored; controller behavior unchanged");
                }
            }
            if (g_weaponSfxDiscovery) {
                g_weaponSfxDiscovery->noteDroppedWwise(g_audioCapture->takeWeaponSfxDropped());
                for (auto& report : g_weaponSfxDiscovery->takeReadyReports(std::chrono::steady_clock::now())) {
                    if (g_weaponAudioPipeline) {
                        (void)g_weaponAudioPipeline->tryEnqueueDiscovery(std::move(report));
                    }
                }
            }
            if (g_uiAudioDiscovery) {
                g_uiAudioDiscovery->noteDroppedWwise(g_audioCapture->takeUiAudioDropped());
                for (auto& line : g_uiAudioDiscovery->takeReadyDiagnostics(std::chrono::steady_clock::now())) {
                    pluginLog(line);
                }
                g_audioCapture->setUiAudioDiscoveryArmed(g_uiAudioDiscovery->captureArmed());
            }
            g_audioCapture->setUiAudioPlaybackArmed(
                (g_uiSpeakerPlayback && g_uiSpeakerPlayback->armed()) ||
                g_haptics != nullptr);
        }



        if (g_weaponAudioPipeline && g_musicHapticsEnabled.load(std::memory_order_acquire)) {
            try {
                for (auto& result : g_weaponAudioPipeline->tryTakeMusicHapticsResults(32u)) {
                    if (!result.ready || !result.pcm || result.pcm->frames.empty() ||
                        musicHapticsBlocked() || !musicHapticsPlayingIdActive(result.playingId)) {
                        continue;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    const auto ageMicros = (std::max)(
                        std::int64_t{ 0 },
                        std::chrono::duration_cast<std::chrono::microseconds>(now - result.selectedAt).count());
                    const auto startFrame = static_cast<std::size_t>(
                        (static_cast<std::uint64_t>(ageMicros) * kMusicHapticsSampleRate) / 1000000u);
                    if (startFrame >= result.pcm->frames.size() || !g_audioTransport) {
                        continue;
                    }

                    (void)g_audioTransport->enqueueMusicHaptic(sds::MusicHapticVoice{
                        .playingId = result.playingId,
                        .eventId = result.eventId,
                        .mediaId = result.mediaId,
                        .pcm = std::move(result.pcm),
                        .startFrame = startFrame,
                        .gain = (g_musicHapticsStrength.load(std::memory_order_acquire)) * g_musicHapticsUserScale.load(std::memory_order_acquire),
                    });
                }
            } catch (...) {
                pluginLog("Music haptics: prepared-voice handoff exception ignored; gameplay haptics unchanged");
            }
        }

        if (g_musicRecon) {
            try {
                if (g_weaponAudioPipeline) {
                    for (auto& result : g_weaponAudioPipeline->tryTakeMusicReconResults(32u)) {
                        g_musicRecon->observeResolved(std::move(result));
                    }
                }
                for (auto& line : g_musicRecon->takeDiagnostics(64u)) {
                    pluginLog(line);
                }
            } catch (...) {
                pluginLog("Music recon: runtime handoff exception ignored; controller behavior unchanged");
            }
        }

        if (g_shipEmReconEnabled.load(std::memory_order_acquire)) {
            if (const auto summary = g_shipEmReconProbe.takeReadySummary(std::chrono::steady_clock::now())) {
                logShipEmReconSummary(*summary);
            }
        }



        if (g_shipLaunchLandingReconEnabled.load(std::memory_order_acquire)) {
            while (const auto report = g_shipLaunchLandingReconProbe.takeReadyReport(std::chrono::steady_clock::now())) {
                logShipLaunchLandingReconReport(*report);
            }
        }


        if (g_mainMenuUiAudioPipeline) {
            for (auto& line : g_mainMenuUiAudioPipeline->tryTakeDiagnostics(16u)) {
                pluginLog(std::string("Main-menu prewarm: ") + line);
            }
        }

        if (g_weaponAudioPipeline) {
            for (auto& line : g_weaponAudioPipeline->tryTakeDiagnostics(32u)) {
                pluginLog(line);
            }
        }


    }

    void initializeRuntime()
    {
        if (g_runtimeInitialized) {
            return;
        }
        g_runtimeInitialized = true;

        const auto config =
            g_startupConfig.has_value() ?
                *g_startupConfig :
                loadRuntimeConfig();

        g_nativeDualSenseReconnectFixEnabled.store(
            config.operatingMode == sds::OperatingMode::ReconnectFixOnly ||
                config.dualSenseReconnectFix,
            std::memory_order_release);

        if (config.operatingMode == sds::OperatingMode::ReconnectFixOnly) {
            auto nativeLog = [](std::string_view message) { pluginLog(message); };

            g_controller = std::make_unique<sds::ControllerManager>(
                config,
                [nativeLog] {
                    return std::make_unique<sds::NativeUsbBackend>(nativeLog, false, true);
                },
                nativeLog,
                std::chrono::milliseconds(250),
                std::chrono::milliseconds(50),
                sds::ControllerManager::RightTriggerObserver{},
                sds::ControllerRuntimeMode::PresenceOnly);
            g_controller->start();

            pluginLog(
                "Runtime mode: ReconnectFixOnly ACTIVE systems=passive-DualSense-presence+native-reselection "
                "adaptiveTriggers=off haptics=off audio=off speaker=off lightbar=off touchpad=off "
                "gameState=off Wwise=off weapon=off ship=off REV8=off boostpack=off");

            if (const auto* tasks = SFSE::GetTaskInterface()) {
                tasks->AddPermanentTask(reconnectFixOnlyRuntimeTick);
                pluginLog(
                    "Reconnect fix: SFSE permanent task ACTIVE policy=one-native-reselection-per-physical-reconnect");
            } else {
                pluginLog(
                    "Reconnect fix: INACTIVE reason=SFSE-task-interface-unavailable; stopping passive presence monitor");
                g_controller->stop();
            }
            return;
        }

        const bool musicReconEnabled = config.debugLogging;
        const bool musicHapticsEnabled = config.advancedHaptics && config.musicHapticsEnabled;
        const bool musicSelectionEnabled = musicReconEnabled || musicHapticsEnabled;
        g_musicHapticsEnabled.store(musicHapticsEnabled, std::memory_order_release);
        g_musicHapticsStrength.store(sds::effectiveHapticStrength(config.hapticStrength), std::memory_order_release);
        g_musicHapticsUserScale.store(config.musicHapticsStrength, std::memory_order_release);
        if (musicReconEnabled) {
            g_musicRecon = std::make_unique<sds::MusicReconProbe>();
        } else {
            g_musicRecon.reset();
        }
        // Keep producer/cache ownership stable across live speaker toggles.
        // ControllerSpeakerManager is the runtime policy authority.
        const bool uiSpeakerPlaybackEnabled = true;
        g_weaponAudioPipelineEnabled =
            config.operatingMode == sds::OperatingMode::Full;
        const bool uiAudioDiscoveryEnabled = config.debugLogging;
        const bool shipBallisticHapticsEnabled = config.adaptiveTriggers || config.advancedHaptics;
        const bool shipLaserHapticsEnabled = shipBallisticHapticsEnabled;
        const bool shipParticleHapticsEnabled = shipBallisticHapticsEnabled;
        const bool shipMissileHapticsEnabled = shipBallisticHapticsEnabled;
        const bool shipEMHapticsEnabled = shipBallisticHapticsEnabled;
        const bool shipTouchdownHapticsEnabled = config.advancedHaptics;
        const bool shipEmReconEnabled = false;
        const bool shipLaunchLandingReconEnabled = true;
        const bool landVehicleReconEnabled = true;
        g_shipEmReconEnabled.store(shipEmReconEnabled, std::memory_order_release);
        g_shipLaunchLandingReconEnabled.store(shipLaunchLandingReconEnabled, std::memory_order_release);
        g_shipEmReconProbe.setPilotActive(false);
        g_shipLaunchLandingReconProbe.setPilotActive(false);
        g_shipLaserFireGate.setPilotActive(false);
        g_shipParticleFireGate.setPilotActive(false);
        g_shipMissileFireGate.setPilotActive(false);
        g_shipEMFireGate.setPilotActive(false);
        if (shipBallisticHapticsEnabled) {
            g_shipWeaponSemanticCache = std::make_shared<sds::ShipWeaponSemanticCache>();
        } else {
            g_shipWeaponSemanticCache.reset();
        }
        g_shipBallisticFireGate.setPilotActive(false);
        const bool boostpackProductionEnabled =
            config.operatingMode == sds::OperatingMode::Full;
        const bool sharedAudioPreparationEnabled =
            uiSpeakerPlaybackEnabled || g_weaponAudioPipelineEnabled || uiAudioDiscoveryEnabled ||
            musicSelectionEnabled || shipBallisticHapticsEnabled || shipLaunchLandingReconEnabled ||
            landVehicleReconEnabled || boostpackProductionEnabled;
        pluginLog("CTD RESTORE: shared weapon/UI/music preparation worker ACTIVE");
        if (sharedAudioPreparationEnabled) {
            const auto executablePath = currentExecutablePath();
            if (!executablePath.empty()) {
                g_weaponAudioPipelineDataPath = executablePath.parent_path() / "Data";
            }
        }
        // Keep the remote-VO observation path registered for process lifetime.
        // The live Comms gate below rejects before any filesystem/decode work.
        const bool remoteVoCaptureEnabled = true;
        const bool weaponSpeakerCaptureEnabled = g_weaponAudioPipelineEnabled;
        const bool autoRivetChargeCaptureEnabled = config.advancedHaptics;
        g_autoRivetChargeCaptureEnabled.store(
            autoRivetChargeCaptureEnabled,
            std::memory_order_release);
        g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
        const bool weaponSfxDiscoveryEnabled =
            weaponSpeakerCaptureEnabled && !kWeaponSfxDiscoveryTargets.empty();
        if (weaponSfxDiscoveryEnabled) {
            g_weaponSfxDiscovery = std::make_unique<sds::WeaponSfxDiscoveryProbe>(kWeaponSfxDiscoveryTargets);
        } else {
            g_weaponSfxDiscovery.reset();
        }

        if (uiAudioDiscoveryEnabled) {
            g_uiAudioDiscovery = std::make_unique<sds::UiAudioDiscoveryProbe>();
        } else {
            g_uiAudioDiscovery.reset();
            if (config.debugLogging) {
                pluginLog("UI audio discovery: INACTIVE reason=DebugLogging-disabled broadCapture=no");
            }
        }

        REX::INFO(
            "StarfieldDualSense {} runtime start | SFSE {} | triggers={} lightbar={} touchpad={} debug={}",
            kVersion,
            SFSE::GetSFSEVersion(),
            config.adaptiveTriggers,
            config.lightbar,
            config.touchpad,
            config.debugLogging);

        if (!musicReconEnabled) {
            pluginLog("Music recon: INACTIVE reason=DebugLogging-disabled");
        }
        pluginLog("Land vehicle state: ACTIVE source=AIProcess.occupiedFurniture authority=tier-a-hardware-validated camera=kVehicle-corroboration-only positionTelemetry=reference-world-finite-difference physics=boost-armed-airborne+descent+touchdown touchdownAuthority=accepted-vertical-boost+vertical-motion WwiseContact=0xDC42D80F,0xA3BB6A5E gunEvent=0x3DD3DADD verticalBoostEvent=0xF6A67354 controllerOutput=haptics+triggers+health-lightbar");
        pluginLog("REV-8 controller feel: ACTIVE authority=tier-a-occupiedFurniture motion=telemetry-load-biased boost=0xF6A67354 gun=VehicleFireWeapon+0x3DD3DADD aim=VehicleAim touchdown=boost-armed-physics haptics=chassis+boost+gun+touchdown triggers=R2-gun+L2-aim speaker=none lightbar=player-health");
        pluginLog("Controller indicators: ACTIVE MainMenu=white-blue-white-fade-4s batteryLeds=5-step-solid USBnativePlayerLedArbitration=filtered transports=USB+Bluetooth");
        resetLandVehicleWwiseRecon(std::chrono::steady_clock::now());

        auto nativeLog = [](std::string_view message) { pluginLog(message); };
        if (!g_landVehicleSpeakerCache) {
            g_landVehicleSpeakerCache = std::make_shared<sds::LandVehicleSpeakerCache>();
        }
        g_audioTransport = std::make_shared<sds::DualSenseAudioTransport>(
            nativeLog,
            config.debugLogging,
            config.speakerVolume);
        g_haptics = std::make_unique<sds::HapticsManager>(
            config,
            [transport = g_audioTransport, nativeLog] {
                auto wiredBackend =
                    std::make_unique<sds::DualSenseAudioHapticsClient>(
                        transport);

                return std::make_unique<sds::DualModeHapticsBackend>(
                    std::move(wiredBackend),
                    [] {
                        return g_controller &&
                            g_controller->connected() &&
                            g_controller->bluetoothTransport();
                    },
                    [](
                        std::uint8_t left,
                        std::uint8_t right,
                        std::chrono::milliseconds duration) {
                        return g_controller &&
                            g_controller->pulseBluetoothRumble(
                                left,
                                right,
                                duration);
                    },
                    [](
                        std::uint8_t left,
                        std::uint8_t right) {
                        return g_controller &&
                            g_controller->setBluetoothContinuousRumble(
                                left,
                                right);
                    },
                    nativeLog);
            },
            nativeLog);
        g_haptics->start();
        initializeMusicHapticsMenuMask();
        if (musicHapticsEnabled) {
            pluginLog("Music haptics: ACTIVE authority=Starfield_MUS-selected-media callback=AK_Duration exactMedia=yes workerDecode=lazy transport=shared-WASAPI fadeMs=350 peakCap=0.65 stereoCrossfeed=85/15 gameplayPriority=sidechain menuMute=MainMenu,DataMenu,PauseMenu,LoadingMenu,FaderMenu wholeGameMix=no");
        } else {
            pluginLog("Music haptics: INACTIVE reason=AdvancedHaptics-or-MusicHapticsEnabled-disabled wholeGameMix=no");
        }

        auto wiredSpeakerBackend =
            std::make_unique<sds::DualSenseAudioSpeakerClient>(
                g_audioTransport);

        auto bluetoothSpeakerBackend =
            std::make_unique<sds::BluetoothSpeakerBackend>(
                config.speakerVolume,
                nativeLog);

        auto speakerBackend =
            std::make_unique<sds::DualModeSpeakerBackend>(
                std::move(wiredSpeakerBackend),
                std::move(bluetoothSpeakerBackend),
                [] {
                    return
                        g_controller &&
                        g_controller->connected() &&
                        g_controller->bluetoothTransport();
                },
                nativeLog);

        g_speakerManager =
            std::make_unique<sds::ControllerSpeakerManager>(
                config,
                std::move(speakerBackend),
                nativeLog);

        // Start is deferred until the primary controller worker
        // has established the active USB/Bluetooth transport.

        if (boostpackProductionEnabled) {
            {
                std::scoped_lock lock(g_boostpackFeedbackMutex);
                g_boostpackFeedbackAuthority =
                    std::make_unique<sds::BoostpackFeedbackAuthority>();
            }
            if (!g_boostpackSpeakerPreparedCache) {
                g_boostpackSpeakerPreparedCache =
                    std::make_shared<sds::BoostpackSpeakerPreparedCache>();
            }
            g_boostpackSpeakerPlayback =
                std::make_unique<sds::BoostpackSpeakerPlayback>(
                    [](const sds::PreparedSpeakerPcm& pcm,
                       std::uint32_t eventId,
                       std::uint32_t mediaId) {
                        if (!g_speakerManager) {
                            return false;
                        }
                        return g_speakerManager->submitCaptured(
                            pcm,
                            sds::SpeakerCategory::Boostpack,
                            {
                                .id = (static_cast<std::uint64_t>(eventId) << 32u) |
                                    static_cast<std::uint64_t>(mediaId),
                                .controllerOnlySafe = false,
                            },
                            false);
                    },
                    g_boostpackSpeakerPreparedCache);
            g_boostpackProductionControllerConnected.store(
                g_controller && g_controller->connected(),
                std::memory_order_release);
            g_boostpackProductionLandVehicleActive.store(false, std::memory_order_release);
            initializeBoostpackProductionMenuMask();
            pluginLog(
                "Boostpack feedback: ACTIVE authority=exact-Wwise-0x1BE06B49 playerObject=0x2 "
                "zeroExternal=yes haptics=ignition+thrust speaker=real-Starfield-WEM");
        } else {
            g_boostpackSpeakerPlayback.reset();
            g_boostpackSpeakerPreparedCache.reset();
            {
                std::scoped_lock lock(g_boostpackFeedbackMutex);
                g_boostpackFeedbackAuthority.reset();
            }
            g_boostpackProductionContextEligible.store(false, std::memory_order_release);
            g_boostpackProductionControllerConnected.store(false, std::memory_order_release);
            g_boostpackProductionLandVehicleActive.store(false, std::memory_order_release);
            g_boostpackProductionBlockingMenuMask.store(0u, std::memory_order_release);
            pluginLog("Boostpack feedback: INACTIVE reason=OperatingMode-ReconnectFixOnly");
        }
        refreshBoostpackSemanticArming();

        if (config.debugLogging) {
            std::ostringstream speakerConfigLine;
            speakerConfigLine << "Controller speaker config: masterVolume=" << std::fixed << std::setprecision(2)
                              << config.speakerVolume
                              << " weapons=" << (config.speakerWeapons ? "enabled" : "disabled")
                              << " weaponsVolume=" << config.speakerWeaponsVolume;
            pluginLog(speakerConfigLine.str());
        }

        if (uiSpeakerPlaybackEnabled) {
            if (!g_uiSpeakerPreparedCache) {
                g_uiSpeakerPreparedCache = std::make_shared<sds::UiSpeakerPreparedCache>();
            }
            g_uiSpeakerPlayback = std::make_unique<sds::UiSpeakerPlayback>(
                [](const sds::PreparedSpeakerPcm& pcm,
                    std::uint32_t eventId,
                    std::uint32_t mediaId,
                    sds::SpeakerCategory category) {
                    if (!g_speakerManager) {
                        return false;
                    }
                    const sds::CapturedSoundIdentity identity{
                        .id = (static_cast<std::uint64_t>(eventId) << 32u) |
                            static_cast<std::uint64_t>(mediaId),
                        .controllerOnlySafe = false,
                    };
                    return g_speakerManager->submitCaptured(
                        pcm,
                        category,
                        identity,
                        false);
                },
                g_uiSpeakerPreparedCache,
                config,
                nativeLog);
            {
                std::ostringstream line;
                line << "UI/digipick/crafting/comms speaker: ACTIVE cues=" << sds::uiSpeakerCueDefinitions().size()
                     << " categories=ScannerUI+Digipick+Crafting+Comms gameObject=per-cue"
                     << " cadence=native-event variantPlayback=real-WEM-bounded-sequence preparation=background"
                     << " playback=additive normalGameAudio=untouched extraction=disabled";
                pluginLog(line.str());
            }
        } else {
            g_uiSpeakerPlayback.reset();
            g_uiSpeakerPreparedCache.reset();
            if (config.debugLogging) {
                pluginLog("UI/scanner speaker: INACTIVE reason=SpeakerScannerUI-or-master-disabled");
            }
        }

        if (weaponSpeakerCaptureEnabled) {
            // Retain cache populated by the early SFSE-load worker.
            if (!g_weaponSpeakerPreparedCache) {
                g_weaponSpeakerPreparedCache = std::make_shared<sds::WeaponSpeakerPreparedCache>();
            }
            g_weaponSpeakerPlayback = std::make_unique<sds::WeaponSpeakerPlayback>(
                [](const sds::PreparedSpeakerPcm& pcm,
                    std::string_view,
                    std::string_view action,
                    std::uint32_t eventId,
                    std::uint32_t mediaId,
                    std::uint8_t) {
                    if (!g_speakerManager) {
                        return false;
                    }
                    const sds::CapturedSoundIdentity identity{
                        .id = (static_cast<std::uint64_t>(eventId) << 32u) |
                            static_cast<std::uint64_t>(mediaId),
                        .controllerOnlySafe = false,
                    };
                    const bool submitted = g_speakerManager->submitCaptured(
                        pcm,
                        sds::SpeakerCategory::Weapons,
                        identity,
                        false);
                    // Use the SAME real Wwise-synchronized handling cue
                    // as controller-speaker audio; never synthesize reload
                    // feedback from button presses or guessed timing.
                    // A custom ReloadHapticsProfile runs directly from the
                    // original player Wwise event. Suppress this audio-driven
                    // path for that weapon to prevent duplicate vibration.
                    const auto customIdentity = g_customReloadEquippedIdentity.load(
                        std::memory_order_acquire);
                    const bool customReloadMapped = customIdentity &&
                        sds::findCustomWeaponReloadHapticsProfile(*customIdentity);
                    if (submitted && g_haptics && !customReloadMapped) {
                        (void)g_haptics->emitWeaponReloadCue(
                            action, std::chrono::steady_clock::now());
                    }
                    return submitted;
                },
                [](sds::PersistentPreparedSpeakerPcm voice,
                    std::string_view,
                    std::uint32_t,
                    std::uint32_t,
                    std::uint8_t) {
                    return g_speakerManager && g_speakerManager->setPersistentCaptured(
                        std::move(voice),
                        sds::SpeakerCategory::Weapons);
                },
                [](std::uint64_t owner, bool force) {
                    return g_speakerManager && g_speakerManager->clearPersistentCaptured(owner, force);
                },
                nativeLog,
                g_weaponSpeakerPreparedCache,
                true);

            g_audioTransport->setSpeakerPersistentInvalidationCallback(
                [](sds::SpeakerPersistentInvalidationReason reason) {
                    if (g_weaponSpeakerPlayback) {
                        g_weaponSpeakerPlayback->observeBackendInvalidation(reason);
                    }
                });
        }

        sds::ControllerManager::RightTriggerObserver rightTriggerObserver{};
        if (config.advancedHaptics || config.adaptiveTriggers || g_weaponSpeakerPlayback) {
            rightTriggerObserver = [](std::uint8_t r2, std::chrono::steady_clock::time_point when) {
                if (g_gameState && g_gameState->landVehicleReconCorrelationArmed()) {
                    std::ostringstream line;
                    line << "Land vehicle recon: input control=R2 raw=" << static_cast<unsigned>(r2)
                         << " state=" << (r2 > 12u ? "active" : "release")
                         << " role=annotation-only authority=no controllerOutput=none";
                    pluginLog(line.str());
                }
                const auto deferredFire = g_shipBallisticFireGate.observeRightTrigger(r2, when);
                const auto laserObservation = g_shipLaserFireGate.observeRightTrigger(r2, when);
                const auto particleObservation = g_shipParticleFireGate.observeRightTrigger(r2, when);
                const auto missileObservation = g_shipMissileFireGate.observeRightTrigger(r2, when);
                const auto emObservation = g_shipEMFireGate.observeRightTrigger(r2, when);
                if (g_haptics) {
                    (void)g_haptics->handleRightTriggerInput(r2, when);
                }
                if (deferredFire.authorized && g_eventRouter) {
                    sds::GameEvent semantic{};
                    semantic.type = sds::GameEventType::ShipBallisticWeaponFired;
                    // A forward-correlated first shot cannot be delivered in the past.
                    // Timestamp the finite effect at the real R2 proof that releases it.
                    semantic.when = when;
                    (void)g_eventRouter->dispatch(std::move(semantic));

                    std::ostringstream line;
                    line << "Ship ballistic fire: event=0x" << std::uppercase << std::hex
                         << std::setw(8) << std::setfill('0') << deferredFire.eventId
                         << " gameObject=0x" << deferredFire.gameObjectId
                         << std::dec << std::setfill(' ')
                         << " family=ballistic control=R2 source=player-Wwise+first-shot-forward-correlation"
                         << " semanticSource=r6-first-shot-forward-correlation"
                         << " ageUs=" << deferredFire.ageMicros;
                    pluginLog(line.str());
                }
                if (laserObservation.deferredFire.authorized && g_eventRouter) {
                    sds::GameEvent semantic{};
                    semantic.type = sds::GameEventType::ShipLaserWeaponFired;
                    semantic.when = when;
                    (void)g_eventRouter->dispatch(std::move(semantic));

                    std::ostringstream line;
                    line << "Ship laser fire: event=0x" << std::uppercase << std::hex
                         << std::setw(8) << std::setfill('0') << laserObservation.deferredFire.eventId
                         << " gameObject=0x" << laserObservation.deferredFire.gameObjectId
                         << std::dec << std::setfill(' ')
                         << " family=laser control=R2 source=player-Wwise+first-shot-forward-correlation"
                         << " semanticSource=v0365-first-shot-forward-correlation"
                         << " ageUs=" << laserObservation.deferredFire.ageMicros;
                    pluginLog(line.str());
                }
                if (laserObservation.stopped && g_eventRouter) {
                    sds::GameEvent semantic{};
                    semantic.type = sds::GameEventType::ShipLaserWeaponStopped;
                    semantic.when = when;
                    (void)g_eventRouter->dispatch(std::move(semantic));
                    pluginLog("Ship laser fire: STOP source=physical-R2-release semanticSource=v0365-stream-revoke");
                }
                if (particleObservation.authorized && g_eventRouter) {
                    sds::GameEvent semantic{};
                    semantic.type = sds::GameEventType::ShipParticleWeaponFired;
                    semantic.when = when;
                    (void)g_eventRouter->dispatch(std::move(semantic));

                    std::ostringstream line;
                    line << "Ship particle fire: event=0x" << std::uppercase << std::hex
                         << std::setw(8) << std::setfill('0') << particleObservation.eventId
                         << " gameObject=0x" << particleObservation.gameObjectId
                         << std::dec << std::setfill(' ')
                         << " family=particle control=R2 source=player-Wwise+first-shot-forward-correlation"
                         << " semanticSource=v0367-first-shot-forward-correlation"
                         << " ageUs=" << particleObservation.ageMicros;
                    pluginLog(line.str());
                }
                if (missileObservation.authorized && g_eventRouter) {
                    sds::GameEvent semantic{};
                    semantic.type = sds::GameEventType::ShipMissileWeaponFired;
                    semantic.when = when;
                    (void)g_eventRouter->dispatch(std::move(semantic));

                    std::ostringstream line;
                    line << "Ship missile fire: event=0x" << std::uppercase << std::hex
                         << std::setw(8) << std::setfill('0') << missileObservation.eventId
                         << " gameObject=0x" << missileObservation.gameObjectId
                         << std::dec << std::setfill(' ')
                         << " family=missile control=R2 source=player-Wwise+first-shot-forward-correlation"
                         << " semanticSource=v0369-first-shot-forward-correlation"
                         << " ageUs=" << missileObservation.ageMicros;
                    pluginLog(line.str());
                }
                if (emObservation.authorized && g_eventRouter) {
                    sds::GameEvent semantic{};
                    semantic.type = sds::GameEventType::ShipEMWeaponFired;
                    semantic.when = when;
                    (void)g_eventRouter->dispatch(std::move(semantic));

                    std::ostringstream line;
                    line << "Ship EM fire: event=0x" << std::uppercase << std::hex
                         << std::setw(8) << std::setfill('0') << emObservation.eventId
                         << " gameObject=0x" << emObservation.gameObjectId
                         << std::dec << std::setfill(' ')
                         << " family=em control=R2 source=player-Wwise+first-shot-forward-correlation"
                         << " semanticSource=v0371-first-shot-forward-correlation"
                         << " ageUs=" << emObservation.ageMicros;
                    pluginLog(line.str());
                }
                if (g_shipEmReconEnabled.load(std::memory_order_acquire)) {
                    const auto reconTrigger = g_shipEmReconProbe.observeRightTrigger(r2, when);
                    if (reconTrigger.pressed || reconTrigger.released) {
                        std::ostringstream line;
                        line << "Ship EM recon: trigger burst=" << reconTrigger.burstId
                             << " action=" << (reconTrigger.pressed ? "press" : "release")
                             << " r2=" << static_cast<unsigned>(r2)
                             << " recoveredPrePress=" << reconTrigger.prePressSamples.size()
                             << " diagnosticOnly=yes";
                        pluginLog(line.str());
                    }
                    for (const auto& sample : reconTrigger.prePressSamples) {
                        logShipEmReconSample(sample);
                    }
                }
                if (g_weaponSpeakerPlayback) {
                    g_weaponSpeakerPlayback->observeRightTrigger(r2, when);
                }
            };
        }

        g_controller = std::make_unique<sds::ControllerManager>(
            config,
            [nativeLog] { return std::make_unique<sds::NativeDualSenseBackend>(nativeLog, false); },
            nativeLog,
            std::chrono::milliseconds(2000),
            std::chrono::milliseconds(8),
            std::move(rightTriggerObserver));
        g_controller->start();

        if (g_speakerManager) {
            g_speakerManager->start();
        }

        pluginLog(
            "Controller speaker transport: AUTO "
            "USB=WASAPI Bluetooth=Opus-HID");

        g_eventRouter = std::make_unique<sds::RuntimeEventRouter>(
            [](sds::GameEvent event) {
                return g_controller && g_controller->enqueue(std::move(event));
            },
            [](sds::GameEvent event) {
                if (g_weaponSpeakerPlayback && event.type != sds::GameEventType::WeaponEquipped) {
                    (void)g_weaponSpeakerPlayback->observeGameEvent(event);
                }
                if (g_weaponSfxDiscovery && event.type != sds::GameEventType::WeaponEquipped) {
                    g_weaponSfxDiscovery->observeGameEvent(event);
                }
                auto speakerCopy = event;
                const bool hapticsAccepted = !g_haptics || g_haptics->handle(std::move(event));
                if (g_speakerManager) {
                    (void)g_speakerManager->handle(std::move(speakerCopy));
                }
                return hapticsAccepted;
            },
            nativeLog);

        auto runtimeEmit = [](sds::GameEvent event) {
            if (event.type == sds::GameEventType::MenuOpened &&
                std::string_view(event.text.data()) == "EndGameCreditsMenu") {
                g_ngPlusTransitionActive.store(true, std::memory_order_release);
                pluginLog("NG+ safety gate: end credits opened; game-world polling suspended");
            }
            if (g_musicRecon) {
                try {
                    g_musicRecon->observeGameEvent(event);
                } catch (...) {
                    pluginLog("Music recon: game-event observer exception ignored; runtime routing unchanged");
                }
            }
            if (event.type == sds::GameEventType::MenuOpened ||
                event.type == sds::GameEventType::MenuClosed) {
                if (g_uiSpeakerPlayback) {
                    g_uiSpeakerPlayback->observeGameEvent(event);
                }
                const auto menu = std::string_view(event.text.data());
                if (event.type == sds::GameEventType::MenuOpened &&
                    menu == "LoadingMenu" &&
                    g_startupWeaponBootstrapComplete.exchange(
                        false,
                        std::memory_order_acq_rel)) {

                    pluginLog(
                        "Game state: equipped weapon bootstrap rearmed reason=LoadingMenu-open");
                }

                if (event.type == sds::GameEventType::MenuOpened &&
                    menu == "LoadingMenu" &&
                    g_fireMarkerBridge) {

                    g_fireMarkerBridge->suspendPlayerGraphsForWorldTeardown();
                }
                syncLandVehicleActionGateForMenu(
                    menu, event.type == sds::GameEventType::MenuOpened);
                syncMusicHapticsForMenu(
                    menu, event.type == sds::GameEventType::MenuOpened);
                syncBoostpackProductionForMenu(
                    menu, event.type == sds::GameEventType::MenuOpened);
                if (g_shipLaunchLandingReconEnabled.load(std::memory_order_acquire)) {
                    g_shipLaunchLandingReconProbe.observeMenu(
                        menu, event.type == sds::GameEventType::MenuOpened, event.when);
                }
                if (g_shipLaunchLandingRumblePhase.load(std::memory_order_acquire) ==
                    ShipLaunchLandingRumblePhase::Takeoff) {
                    if (event.type == sds::GameEventType::MenuClosed &&
                        eventIdentity(event) == "FaderMenu") {

                        if (!g_shipTakeoffFirstFaderClosed.exchange(
                                true,
                                std::memory_order_acq_rel)) {

                            pluginLog(
                                "Ship launch/landing haptics: takeoff-first-fader-closed action=continue");
                        } else if (g_shipTakeoffBoundaryObserved.load(
                                       std::memory_order_acquire)) {

                            stopShipLaunchLandingHaptics(
                                "takeoff-final-fader-close");
                        }
                    } else if (event.type == sds::GameEventType::MenuOpened &&
                               eventIdentity(event) == "FaderMenu" &&
                               g_shipTakeoffFirstFaderClosed.load(
                                   std::memory_order_acquire)) {

                        pluginLog(
                            "Ship launch/landing haptics: takeoff-second-fader-open action=continue-until-final-fader-close");
                    } else if (event.type == sds::GameEventType::MenuOpened &&
                               eventIdentity(event) == "LoadingMenu" &&
                               g_shipTakeoffFirstFaderClosed.load(
                                   std::memory_order_acquire)) {

                        pluginLog(
                            "Ship launch/landing haptics: takeoff-loading-open action=continue-until-final-fader-close");
                    }
                }
                std::uint8_t bit = 0;
                if (menu == "DataMenu") {
                    bit = 0x01u;
                } else if (menu == "PauseMenu") {
                    bit = 0x02u;
                }
                if (bit != 0) {
                    std::uint8_t mask = 0;
                    if (event.type == sds::GameEventType::MenuOpened) {
                        mask = static_cast<std::uint8_t>(
                            g_shipBallisticBlockingMenuMask.fetch_or(bit, std::memory_order_acq_rel) | bit);
                    } else {
                        mask = static_cast<std::uint8_t>(
                            g_shipBallisticBlockingMenuMask.fetch_and(
                                static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel) &
                            static_cast<std::uint8_t>(~bit));
                    }
                    g_shipBallisticFireGate.setMenuBlocked(mask != 0);
                    g_shipLaserFireGate.setMenuBlocked(mask != 0);
                    g_shipParticleFireGate.setMenuBlocked(mask != 0);
                    g_shipMissileFireGate.setMenuBlocked(mask != 0);
                    g_shipEMFireGate.setMenuBlocked(mask != 0);
                    if (g_shipEmReconEnabled.load(std::memory_order_acquire)) {
                        g_shipEmReconProbe.setMenuBlocked(mask != 0);
                    }
                    g_shipLaunchLandingReconProbe.setMenuBlocked(mask != 0);
                }
            }

            switch (event.type) {
            case sds::GameEventType::ShipPilotEntered:
                resetLandVehicleActionGate();
                g_shipPilotActive.store(true, std::memory_order_release);
                refreshBoostpackProductionContext("ship-pilot-enter");
                g_lastShipStateValid.store(false, std::memory_order_release);
                g_shipBallisticFireGate.setPilotActive(true);
                g_shipLaserFireGate.setPilotActive(true);
                g_shipParticleFireGate.setPilotActive(true);
                g_shipMissileFireGate.setPilotActive(true);
                g_shipEMFireGate.setPilotActive(true);
                g_shipEmReconProbe.setPilotActive(true);
                g_shipLaunchLandingReconProbe.setPilotActive(true);
                g_shipWeaponCaptureProbeCount.store(0, std::memory_order_release);
                g_shipEmReconLogCount.store(0, std::memory_order_release);
                g_onFootRefreshPending.store(false, std::memory_order_release);
                g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
                pluginLog("Ship pilot context: ENTER source=SpaceshipHudMenu authority=menu");
                break;

            case sds::GameEventType::ShipPilotExited:
                g_shipPilotActive.store(false, std::memory_order_release);
                refreshBoostpackProductionContext("ship-pilot-exit");
                g_lastShipStateValid.store(false, std::memory_order_release);
                g_shipBallisticFireGate.setPilotActive(false);
                g_shipLaserFireGate.setPilotActive(false);
                g_shipParticleFireGate.setPilotActive(false);
                g_shipMissileFireGate.setPilotActive(false);
                g_shipEMFireGate.setPilotActive(false);
                g_shipEmReconProbe.setPilotActive(false);
                g_shipLaunchLandingReconProbe.setPilotActive(false);
                g_onFootRefreshPending.store(true, std::memory_order_release);
                g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
                if (std::string_view(event.text.data()) == "LoadingMenu") {
                    pluginLog("Ship pilot context: EXIT source=LoadingMenu-close reason=SpaceshipHudMenu-closed-during-load refresh=fresh-on-foot");
                } else {
                    pluginLog("Ship pilot context: EXIT source=SpaceshipHudMenu refresh=fresh-on-foot");
                }
                break;

            case sds::GameEventType::ShipPilotInvalidated:
                resetLandVehicleActionGate();
                g_shipPilotActive.store(false, std::memory_order_release);
                refreshBoostpackProductionContext("ship-pilot-invalidated");
                g_lastShipStateValid.store(false, std::memory_order_release);
                g_shipBallisticFireGate.setPilotActive(false);
                g_shipLaserFireGate.setPilotActive(false);
                g_shipParticleFireGate.setPilotActive(false);
                g_shipMissileFireGate.setPilotActive(false);
                g_shipEMFireGate.setPilotActive(false);
                g_shipEmReconProbe.setPilotActive(false);
                g_shipLaunchLandingReconProbe.setPilotActive(false);
                g_onFootRefreshPending.store(false, std::memory_order_release);
                g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
                pluginLog("Ship pilot context: INVALIDATE source=LoadingMenu refresh=deferred-until-fresh-context");
                break;

            case sds::GameEventType::ShipPilotResumed:
                resetLandVehicleActionGate();
                g_shipPilotActive.store(true, std::memory_order_release);
                refreshBoostpackProductionContext("ship-pilot-resumed");
                g_lastShipStateValid.store(false, std::memory_order_release);
                g_shipBallisticFireGate.setPilotActive(true);
                g_shipLaserFireGate.setPilotActive(true);
                g_shipParticleFireGate.setPilotActive(true);
                g_shipMissileFireGate.setPilotActive(true);
                g_shipEMFireGate.setPilotActive(true);
                g_shipEmReconProbe.setPilotActive(true);
                g_shipLaunchLandingReconProbe.setPilotActive(true);
                g_shipWeaponCaptureProbeCount.store(0, std::memory_order_release);
                g_shipEmReconLogCount.store(0, std::memory_order_release);
                g_onFootRefreshPending.store(false, std::memory_order_release);
                g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
                pluginLog("Ship pilot context: RESUME source=LoadingMenu-close evidence=SpaceshipHudMenu-still-open");
                break;

            case sds::GameEventType::LandVehicleAuthorityAcquired:
                if (g_gameState) {
                    const auto physics = g_gameState->latestLandVehiclePhysicsSnapshot();
                    setLandVehicleActionGateAuthority(physics.authorityActive, physics.authorityEpoch);
                }
                break;

            case sds::GameEventType::LandVehicleAuthorityReleased:
                setLandVehicleActionGateAuthority(false, 0);
                break;

            case sds::GameEventType::LandVehicleContextEntered:
                g_boostpackProductionLandVehicleActive.store(true, std::memory_order_release);
                refreshBoostpackProductionContext("land-vehicle-enter");
                resetLandVehicleActionGate();
                resetLandVehicleWwiseRecon(event.when);
                g_onFootRefreshPending.store(false, std::memory_order_release);
                g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
                pluginLog("Land vehicle context: ENTER suppression=handheld-output-clear authority=kVehicle-suppression-only");
                break;

            case sds::GameEventType::LandVehicleContextExited:
                stopLandVehicleEngineSpeaker();
                g_boostpackProductionLandVehicleActive.store(false, std::memory_order_release);
                refreshBoostpackProductionContext("land-vehicle-exit");
                resetLandVehicleActionGate();
                flushLandVehicleWwiseRecon("vehicle-exit", event.when);
                g_onFootRefreshPending.store(true, std::memory_order_release);
                g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
                pluginLog("Land vehicle context: EXIT suppression=release refresh=fresh-on-foot");
                break;

            case sds::GameEventType::Shutdown:
                stopLandVehicleEngineSpeaker();
                setBoostpackProductionContextEligible(false, "shutdown-event");
                resetLandVehicleActionGate();
                g_lastShipStateValid.store(false, std::memory_order_release);
                stopShipLaunchLandingHaptics("shutdown-event");
                g_shipBallisticFireGate.setPilotActive(false);
                g_shipLaserFireGate.setPilotActive(false);
                g_shipParticleFireGate.setPilotActive(false);
                g_shipMissileFireGate.setPilotActive(false);
                g_shipEMFireGate.setPilotActive(false);
                g_shipEmReconProbe.setPilotActive(false);
                g_shipLaunchLandingReconProbe.setPilotActive(false);
                g_shipLaunchLandingReconProbe.setMenuBlocked(true);
                if (g_audioCapture) {
                    g_audioCapture->setShipWeaponObservationArmed(false);
                }
                break;

            default:
                break;
            }

            if (g_audioCapture) {
                const bool pilotActive = g_shipPilotActive.load(std::memory_order_acquire);
                const bool launchLandingReconCaptureArmed =
                    g_shipLaunchLandingReconEnabled.load(std::memory_order_acquire) &&
                    (pilotActive || g_shipLaunchLandingReconProbe.requiresWwiseCapture());
                const bool landVehicleReconCaptureArmed =
                    event.type != sds::GameEventType::Shutdown &&
                    g_gameState && g_gameState->landVehicleReconCorrelationArmed();
                const bool boostpackProductionArmed =
                    event.type != sds::GameEventType::Shutdown && boostpackProductionCaptureArmed();
                const bool shipCaptureArmed =
                    launchLandingReconCaptureArmed || landVehicleReconCaptureArmed ||
                    boostpackProductionArmed ||
                    (pilotActive && g_shipWeaponSemanticCache && g_shipWeaponSemanticCache->ready());
                g_audioCapture->setShipWeaponObservationArmed(shipCaptureArmed);
            }

            if (g_uiAudioDiscovery &&
                (event.type == sds::GameEventType::MenuOpened ||
                 event.type == sds::GameEventType::MenuClosed)) {
                try {
                    g_uiAudioDiscovery->observeGameEvent(event);
                } catch (...) {
                    pluginLog("UI audio discovery: menu observer exception ignored; runtime routing unchanged");
                }
            }
            if (g_audioCapture) {
                g_audioCapture->setUiAudioDiscoveryArmed(
                    g_uiAudioDiscovery && g_uiAudioDiscovery->captureArmed());
            }
            if (g_weaponSpeakerPlayback && event.type == sds::GameEventType::WeaponEquipped) {
                try {
                    (void)g_weaponSpeakerPlayback->observeGameEvent(event);
                } catch (...) {
                    pluginLog("Weapon speaker: equip observer exception ignored; runtime routing unchanged");
                }
            }
            if (g_weaponSfxDiscovery && event.type == sds::GameEventType::WeaponEquipped) {
                try {
                    g_weaponSfxDiscovery->observeGameEvent(event);
                } catch (...) {
                    pluginLog("Weapon SFX discovery: equip observer exception ignored; runtime routing unchanged");
                }
            }
            if (event.type == sds::GameEventType::WeaponEquipped) {
                try {
                    g_customReloadEquippedIdentity.store(
                        std::make_shared<const std::string>(event.text.data()),
                        std::memory_order_release);
                } catch (...) {
                    g_customReloadEquippedIdentity.store({}, std::memory_order_release);
                }
                const bool autoRivetEquipped =
                    g_autoRivetChargeCaptureEnabled.load(std::memory_order_acquire) &&
                    std::string_view(event.text.data()) == "Auto-Rivet";
                g_autoRivetChargeCaptureArmed.store(
                    autoRivetEquipped,
                    std::memory_order_release);
            } else if (event.type == sds::GameEventType::Shutdown) {
                g_customReloadEquippedIdentity.store({}, std::memory_order_release);
                g_autoRivetChargeCaptureArmed.store(false, std::memory_order_release);
            }
            if (g_audioCapture) {
                const bool captureArmed =
                    g_autoRivetChargeCaptureArmed.load(std::memory_order_acquire) ||
                    (g_weaponSpeakerPlayback && g_weaponSpeakerPlayback->armed()) ||
                    (g_weaponSfxDiscovery && g_weaponSfxDiscovery->armed()) ||
                    ([&] {
                        const auto equipped = g_customReloadEquippedIdentity.load(std::memory_order_acquire);
                        return equipped && sds::findCustomWeaponReloadHapticsProfile(*equipped);
                    }());
                g_audioCapture->setWeaponSfxDiscoveryArmed(captureArmed);
            }
            return g_eventRouter && g_eventRouter->dispatch(std::move(event));
        };

        g_gameState = std::make_unique<sds::GameStateAdapter>(
            runtimeEmit,
            nativeLog,
            false,
            config.advancedHaptics);
        g_gameState->setLandVehicleMotionCallback([](const sds::LandVehicleMotionState& state) {
            updateLandVehicleEngineSpeaker(state);
            if (g_haptics) {
                g_haptics->handleLandVehicleMotionState(state);
            }
        });
        g_gameState->setLandVehicleSemanticCallback([](
            std::string_view semantic,
            bool active,
            std::chrono::steady_clock::time_point when) {
            observeLandVehicleProductionSemantic(semantic, active, when);
        });
        sds::setBoostpackSemanticObserver([](
            std::string_view semantic,
            bool active,
            std::chrono::steady_clock::time_point when) {
            observeBoostpackProductionSemantic(semantic, active, when);
        });
        refreshBoostpackSemanticArming();
        if (!g_gameState->registerSinks()) {
            pluginLog("Game state: event registration incomplete; controller worker remains active");
        }

        sds::FireMarkerBridge::AnimationMarkerObserver animationMarkerObserver{};
        if (g_weaponSfxDiscovery) {
            animationMarkerObserver = [](
                std::string_view tag,
                std::string_view payload,
                std::chrono::steady_clock::time_point when) {
                if (!g_weaponSfxDiscovery) {
                    return false;
                }
                try {
                    return g_weaponSfxDiscovery->observeAnimationMarker(tag, payload, when);
                } catch (...) {
                    pluginLog("Weapon SFX discovery: animation marker observer exception ignored; runtime routing unchanged");
                    return false;
                }
            };
        }

        g_fireMarkerBridge = std::make_unique<sds::FireMarkerBridge>(
            runtimeEmit,
            nativeLog,
            config.debugLogging,
            std::move(animationMarkerObserver));
        if (!g_fireMarkerBridge->start()) {
            pluginLog("Fire marker bridge: registration incomplete; persistent weapon walls remain active but live cadence is unavailable");
        } else {
            pluginLog("Fire marker bridge: confirmed player animation markers are wired to live weapon effects");
        }
        pluginLog("CTD RESTORE: FireMarkerBridge ACTIVE semanticHook=on shipHook=on runtimeTick=on");
        pluginLog("FIREMARKER LIFECYCLE FIX: LoadingMenu-open graph detach ACTIVE diagnostic=main-menu-ctd");

        pluginLog("CTD RESTORE: Starfield PostEvent capture block ACTIVE");
        if (true) {
            const auto executablePath = currentExecutablePath();
            sds::StarfieldAudioCapture::RemoteVoSourceCallback sourceProbe{};
            if (remoteVoCaptureEnabled) {
                g_remoteVoAudioPipeline = std::make_unique<sds::RemoteVoAudioPipeline>(
                    [nativeLog, executablePath](const sds::RemoteVoAudioRequest& request) {
                        return sds::prepareRemoteVoAudio(
                            request.filePath, request.codecId, request.language, executablePath, nativeLog);
                    });
                sourceProbe = [nativeLog, executablePath](const sds::RemoteVoMirrorRequest& request) {
                    if (g_runtimeShutdown || !g_speakerManager ||
                        !g_speakerManager->categoryEnabled(sds::SpeakerCategory::Comms)) {
                        // RemoteVoSourceCallback is a notification callback.
                        // Exit before any filesystem/decode work without
                        // changing the lambda's return type from void.
                        return;
                    }
                    const bool dataslateVoice =
                        request.eventId == sds::kDataslateVoEventId;
                    if (!dataslateVoice) {
                        if (!g_remoteVoBlocked) {
                            g_remoteVoAudioPipeline->enqueue({
                                .filePath = std::wstring(request.filePath),
                                .codecId = request.codecId,
                                .language = resolvedSpeakerVoiceLanguage(),
                                .identity = {
                                    .id = (static_cast<std::uint64_t>(request.eventId) << 32u) | request.externalCookie,
                                    .controllerOnlySafe = false,
                                },
                                .sequence = request.sequence,
                                .captureSteadyMicros = request.captureSteadyMicros,
                                .originalPlayingId = request.originalPlayingId,
                            });
                        }
                        return;
                    }

                    if (!g_dataslateMenuActive.load(std::memory_order_acquire)) {

                        bool continuationAllowed = false;
                        {
                            std::scoped_lock lock(
                                g_dataslateVoiceQueueMutex);

                            const auto now =
                                std::chrono::steady_clock::now();

                            continuationAllowed =
                                g_dataslateContinuationDeadline.time_since_epoch().count() != 0 &&
                                now <= g_dataslateContinuationDeadline;
                        }

                        if (!continuationAllowed) {
                            nativeLog(
                                "Dataslate controller voice: SKIP reason=DataSlateMenu-closed-no-active-recording");
                            return;
                        }

                        nativeLog(
                            "Dataslate controller voice: CONTINUE reason=native-recording-after-menu-exit");
                    }

                    // A fresh exact dataslate VO post is authoritative playback.
                    // This also handles Starfield posting the voice immediately
                    // before its UI_Dataslate_Audio_Play click.
                    g_remoteVoAudioPipeline->clear();
                    g_dataslatePauseRequested.store(false, std::memory_order_release);

                    const auto latencyMs = [&request]() noexcept -> long long {
                        if (request.captureSteadyMicros <= 0) {
                            return -1;
                        }
                        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        return (now - request.captureSteadyMicros) / 1000;
                    };
                    auto playback = sds::prepareRemoteVoAudio(
                        request.filePath, request.codecId, resolvedSpeakerVoiceLanguage(),
                        executablePath, nativeLog);
                    const auto captureToDecodeMs = latencyMs();
                    if (!playback.prepared) {
                        return;
                    }
                    nativeLog(sds::formatRemoteVoControllerPlaybackPreparation(playback));

                    bool submitted = false;
                    bool dataslateQueued = false;
                    std::size_t dataslatePendingCount = 0;
                    const bool speakerManagerPresent = static_cast<bool>(g_speakerManager);
                    const bool speakerActive = g_speakerManager && g_speakerManager->active();
                    const auto submitNow = std::chrono::steady_clock::now();

                    if (g_speakerManager) {
                        const sds::CapturedSoundIdentity identity{
                            .id = (static_cast<std::uint64_t>(request.eventId) << 32u) |
                                static_cast<std::uint64_t>(request.externalCookie),
                            .controllerOnlySafe = false,
                        };

                        if (g_dataslateControllerVoiceActive.load(std::memory_order_acquire)) {

                            try {
                                std::scoped_lock lock(g_dataslateVoiceQueueMutex);

                                const bool activeWindow =
                                    g_dataslatePlaybackDeadline.time_since_epoch().count() != 0 &&
                                    submitNow < g_dataslatePlaybackDeadline;

                                if (activeWindow) {
                                    DataslateQueuedVoiceChunk chunk{};
                                    chunk.pcm = std::move(playback.pcm);
                                    chunk.durationMs = playback.outputDurationMs;
                                    chunk.sequence = request.sequence;

                                    g_dataslateVoiceQueue.push_back(
                                        std::move(chunk));

                                    dataslatePendingCount =
                                        g_dataslateVoiceQueue.size();

                                    submitted = true;
                                    dataslateQueued = true;
                                }
                            } catch (...) {
                                submitted = false;
                                dataslateQueued = false;
                            }
                        }

                        if (!dataslateQueued) {
                            submitted = g_speakerManager->submitCaptured(
                                std::move(playback.pcm),
                                sds::SpeakerCategory::Comms,
                                identity,
                                true);
                        }
                    }

                    if (submitted) {
                        g_dataslateControllerVoiceActive.store(
                            true,
                            std::memory_order_release);

                        if (dataslateQueued) {
                            char queueLog[256]{};
                            std::snprintf(
                                queueLog,
                                sizeof(queueLog),
                                "Dataslate controller voice: QUEUED seq=%llu durationMs=%llu pending=%zu reason=native-sequence-advanced-early",
                                static_cast<unsigned long long>(request.sequence),
                                static_cast<unsigned long long>(playback.outputDurationMs),
                                dataslatePendingCount);
                            nativeLog(queueLog);
                        } else {
                            {
                                std::scoped_lock lock(
                                    g_dataslateVoiceQueueMutex);

                                g_dataslatePlaybackDeadline =
                                    submitNow +
                                    std::chrono::milliseconds(
                                        playback.outputDurationMs);

                                g_dataslateContinuationDeadline =
                                    g_dataslatePlaybackDeadline +
                                    kDataslateContinuationGrace;
                            }

                            g_dataslatePauseRequested.store(
                                false,
                                std::memory_order_release);

                            nativeLog(
                                "Dataslate controller voice: ACTIVE exact-event=0xBDB11FAA");
                        }
                    }

                    const auto speakerOutputMode =
                        g_speakerManager ?
                            g_speakerManager->outputMode() :
                            sds::SpeakerOutputMode::Both;

                    const char* outputModeName = speakerOutputMode == sds::SpeakerOutputMode::ControllerOnly
                        ? "ControllerOnly"
                        : "Both";
                    const char* originalOutput =
                        speakerOutputMode == sds::SpeakerOutputMode::ControllerOnly &&
                        g_audioCapture && g_audioCapture->dataslateControllerOnlyRoutingActive()
                            ? "silent-emitter-dataslate-native-lifecycle"
                            : "passthrough-dataslate-native-lifecycle";

                    char playbackLog[704]{};
                    std::snprintf(
                        playbackLog,
                        sizeof(playbackLog),
                        "Voice controller playback: seq=%llu submit=%s speakerManager=%s speakerActive=%s replacedActive=%s sourceFrames=%u sourceRate=%u outputFrames=%zu outputRate=%u durationMs=%llu captureToDecodeMs=%lld captureToSubmitMs=%lld category=Comms continuous=yes speakerOutputMode=%s originalPlayingId=%u originalOutput=%s",
                        static_cast<unsigned long long>(request.sequence),
                        submitted ? "accepted" : "rejected",
                        speakerManagerPresent ? "yes" : "no",
                        speakerActive ? "yes" : "no",
                        "no",
                        playback.sourceFrames,
                        playback.sourceSampleRate,
                        playback.outputFrames,
                        playback.outputSampleRate,
                        static_cast<unsigned long long>(playback.outputDurationMs),
                        captureToDecodeMs,
                        latencyMs(),
                        outputModeName,
                        request.originalPlayingId,
                        originalOutput);
                    nativeLog(playbackLog);
                };
            }

            sds::StarfieldAudioCapture::WeaponSfxObservationCallback weaponSfxObservation{};
            if (autoRivetChargeCaptureEnabled || g_weaponSpeakerPlayback || g_weaponSfxDiscovery || g_shipWeaponSemanticCache ||
                shipMissileHapticsEnabled || shipEMHapticsEnabled || shipLaunchLandingReconEnabled || landVehicleReconEnabled ||
                g_boostpackFeedbackAuthority != nullptr || g_boostpackSpeakerPlayback != nullptr ||
                sds::customWeaponReloadHapticsProfileCount() != 0u ||
                g_shipEmReconEnabled.load(std::memory_order_acquire)) {
                weaponSfxObservation = [autoRivetChargeCaptureEnabled](const sds::WeaponSfxWwiseObservation& observation) {
                    observeShipBallisticWwise(observation);
                    observeShipLaserWwise(observation);
                    observeShipParticleWwise(observation);
                    observeShipMissileWwise(observation);
                    observeShipEMWwise(observation);
                    observeShipLaunchLandingReconWwise(observation);
                    observeLandVehicleReconWwise(observation);
                    observeBoostpackProductionWwise(observation);
                    observeOnFootLandingWwise(observation);
                    if (g_shipEmReconEnabled.load(std::memory_order_acquire)) {
                        observeShipEmReconWwise(observation);
                    }
                    if (autoRivetChargeCaptureEnabled) {
                        observeAutoRivetChargeSemantic(observation);
                    }
                    // Custom reload vibration is independent of whether
                    // controller-speaker audio is enabled or a clip was queued.
                    // The real player weapon Wwise post is the sole authority.
                    if (g_haptics && observation.gameObjectId == 0x2u &&
                        observation.externalCount == 0u && !observation.hasExternalSources) {
                        const auto equipped = g_customReloadEquippedIdentity.load(
                            std::memory_order_acquire);
                        if (equipped) {
                            if (const auto* profile =
                                    sds::findCustomWeaponReloadHapticsProfile(*equipped)) {
                                const auto action = sds::customWeaponReloadHapticStage(
                                    *profile, observation.eventId);
                                if (action) {
                                    (void)g_haptics->emitWeaponReloadCue(
                                        *action, observation.when,
                                        profile->weaponIdentity, true);
                                }
                            }
                        }
                    }
                    if (g_weaponSpeakerPlayback) {
                        (void)g_weaponSpeakerPlayback->observeWwise(observation);
                    }
                    if (g_weaponSfxDiscovery) {
                        (void)g_weaponSfxDiscovery->observeWwise(observation);
                    }
                };
            }

            sds::StarfieldAudioCapture::UiAudioObservationCallback uiAudioObservation{};
            if (g_uiSpeakerPlayback || g_uiAudioDiscovery || g_haptics || remoteVoCaptureEnabled) {
                uiAudioObservation = [](const sds::UiAudioWwiseObservation& observation) {
                    if (observation.externalCount == 0u &&
                        !observation.hasExternalSources) {

                        if (observation.eventId == kDataslatePauseEventId) {
                            g_dataslatePauseRequested.store(
                                true,
                                std::memory_order_release);

                            {
                                std::scoped_lock lock(
                                    g_dataslateVoiceQueueMutex);
                                g_dataslateVoiceQueue.clear();
                                g_dataslatePlaybackDeadline = {};
                                g_dataslateContinuationDeadline = {};
                            }

                            if (g_dataslateControllerVoiceActive.load(std::memory_order_acquire) &&
                                g_speakerManager &&
                                g_speakerManager->active()) {

                                sds::PreparedSpeakerPcm silence{};
                                silence.frames.push_back(
                                    { .left = 0.0F, .right = 0.0F });
                                silence.gain = 1.0F;

                                const sds::CapturedSoundIdentity identity{
                                    .id =
                                        (static_cast<std::uint64_t>(kDataslatePauseEventId) << 32u) |
                                        0x24DB9834u,
                                    .controllerOnlySafe = false,
                                };

                                const bool stopped =
                                    g_speakerManager->submitCaptured(
                                        std::move(silence),
                                        sds::SpeakerCategory::Comms,
                                        identity,
                                        true);

                                if (stopped) {
                                    g_dataslateControllerVoiceActive.store(
                                        false,
                                        std::memory_order_release);
                                    {
                                        std::scoped_lock lock(
                                            g_dataslateVoiceQueueMutex);
                                        g_dataslatePlaybackDeadline = {};
                                        g_dataslateContinuationDeadline = {};
                                    }
                                    pluginLog(
                                        "Dataslate controller voice: STOP reason=pause");
                                } else {
                                    pluginLog(
                                        "Dataslate controller voice: pause stop deferred reason=controller-submit-rejected");
                                }
                            }
                        } else if (observation.eventId == kDataslatePlayEventId) {
                            g_dataslatePauseRequested.store(
                                false,
                                std::memory_order_release);
                            pluginLog(
                                "Dataslate controller voice: PLAY observed; awaiting exact external VO post");
                        }
                    }

                    if (g_haptics &&
                        observation.externalCount == 0u &&
                        !observation.hasExternalSources) {
                        (void)g_haptics->emitDigipickWwiseEvent(
                            observation.eventId,
                            observation.when);
                    }
                    if (g_uiSpeakerPlayback) {
                        const bool submitted =
                            g_uiSpeakerPlayback->observeWwise(observation);

                        if (submitted &&
                            observation.eventId == 0x27A3CE98u &&
                            g_speakerManager &&
                            g_speakerManager->outputMode() ==
                                sds::SpeakerOutputMode::ControllerOnly &&
                            observation.returnedPlayingId != 0u) {
                            (void)sds::stopWwisePlayingId(
                                observation.returnedPlayingId);
                        }
                    }
                    if (g_uiAudioDiscovery) {
                        (void)g_uiAudioDiscovery->observeWwise(observation);
                    }
                };
            }

            sds::StarfieldAudioCapture::MusicReconObservationCallback musicReconObservation{};
            if (g_musicRecon) {
                musicReconObservation = [](const sds::MusicReconWwiseObservation& observation) {
                    if (g_musicRecon) {
                        (void)g_musicRecon->observeWwise(observation);
                    }
                };
            }

            sds::StarfieldAudioCapture::MusicSelectionObservationCallback musicSelectionObservation{};
            if (musicHapticsEnabled) {
                musicSelectionObservation = [](const sds::MusicSelectionObservation& observation) {
                    if (!g_musicHapticsEnabled.load(std::memory_order_acquire) || observation.playingId == 0u) {
                        return;
                    }

                    if (observation.kind == sds::MusicSelectionObservationKind::Ended) {
                        musicHapticsErasePlayingId(observation.playingId);
                        if (g_audioTransport) {
                            (void)g_audioTransport->stopMusicHapticsPlayingId(observation.playingId);
                        }
                        return;
                    }

                    if (observation.kind != sds::MusicSelectionObservationKind::Selected ||
                        observation.eventId == 0u || observation.mediaId == 0u || musicHapticsBlocked()) {
                        return;
                    }
                    if (!musicHapticsMarkPlayingIdActive(observation.playingId)) {
                        return;
                    }

                    if (!g_weaponAudioPipeline || !g_weaponAudioPipeline->tryEnqueueMusicHaptics(
                            sds::MusicHapticsPrepareRequest{
                                .eventId = observation.eventId,
                                .mediaId = observation.mediaId,
                                .playingId = observation.playingId,
                                .selectedAt = observation.capturedAt,
                            })) {
                        // A later selected segment for the same Wwise playing ID may retry.
                    }
                };
            }

            g_audioCapture = std::make_unique<sds::StarfieldAudioCapture>(
                nativeLog,
                sds::StarfieldAudioCapture::RemoteVoMirrorCallback{},
                sds::kRemoteCommsVoMirrorProfile,
                std::move(sourceProbe),
                std::move(weaponSfxObservation),
                std::move(uiAudioObservation),
                std::move(musicReconObservation),
                std::move(musicSelectionObservation));
            if (!g_audioCapture->start()) {
                pluginLog("Starfield audio capture: PostEvent capture did not activate; controller voice/weapon/UI capture unavailable");
                if (g_musicRecon) {
                    pluginLog("Music recon: INACTIVE reason=existing-PostEvent-hook-unavailable output=none");
                }
            } else {
                g_audioCapture->setDataslateControllerOnlyRoutingEnabled(
                    g_speakerManager &&
                    g_speakerManager->categoryEnabled(
                        sds::SpeakerCategory::Comms) &&
                    g_speakerManager->outputMode() ==
                        sds::SpeakerOutputMode::ControllerOnly);

                const bool captureArmed =
                    g_autoRivetChargeCaptureArmed.load(std::memory_order_acquire) ||
                    (g_weaponSpeakerPlayback && g_weaponSpeakerPlayback->armed()) ||
                    (g_weaponSfxDiscovery && g_weaponSfxDiscovery->armed()) ||
                    ([&] {
                        const auto equipped = g_customReloadEquippedIdentity.load(std::memory_order_acquire);
                        return equipped && sds::findCustomWeaponReloadHapticsProfile(*equipped);
                    }());
                g_audioCapture->setWeaponSfxDiscoveryArmed(captureArmed);
                g_audioCapture->setShipWeaponObservationArmed(boostpackProductionCaptureArmed());
                g_audioCapture->setUiAudioDiscoveryArmed(false);
                g_audioCapture->setUiAudioPlaybackArmed(
                    (g_uiSpeakerPlayback && g_uiSpeakerPlayback->armed()) ||
                    g_haptics != nullptr);
                g_audioCapture->setMusicReconArmed(g_musicRecon != nullptr);
                g_audioCapture->setMusicSelectionArmed(musicSelectionEnabled);
                if (uiAudioDiscoveryEnabled) {
                    {
                        std::ostringstream line;
                        line << "UI audio discovery: ACTIVE diagnostic-only trigger=first-target-menu"
                             << " end=DataMenu-close maxDurationMs=300000 hudFollowupMs=60000"
                             << " correlationMs=150 queue=1024 aggregates=512 transitions=128"
                             << " unpromotedOnly=yes existingPromotedPlayback=" << sds::uiSpeakerCueDefinitions().size()
                             << " playback=production-promoted targetedResolution="
                             << sds::v0359UiAudioResolutionTargets().size()
                             << " extraction=disabled normalGameAudio=untouched";
                        pluginLog(line.str());
                    }
                }
                if (autoRivetChargeCaptureEnabled) {
                    pluginLog("Auto-Rivet tension gate: rawR2=no-authority chargeSource=player-Wwise blockingMenus=DataMenu,PauseMenu,LoadingMenu resumeRequiresFreshChargeStart=yes");
                }
                if (shipBallisticHapticsEnabled) {
                    pluginLog("Ship ballistic haptics: ACTIVE control=R2 baselineWall=pilot-context authority=first-player-Wwise+nearby-R2 forwardFirstShot=hardware-alias+180ms-real-R2 automaticHeartbeat=same-event+same-gameObject+held-R2 catalog=SoundBanksInfo+hardware-alias family=ballistic confirmedShotKick=yes laser=promoted-v0365 otherFamilies=inert menuMute=DataMenu,PauseMenu");
                }
                if (shipLaserHapticsEnabled) {
                    pluginLog("Ship laser haptics: ACTIVE control=R2 authority=exact-player-Wwise+nearby-R2 forwardFirstShot=hardware-alias+180ms-real-R2 automaticHeartbeat=same-event+same-gameObject+held-R2 event=0xCE7B2EB1 leaseMs=250 haptic=continuous-energy+native-pulse-crest tactileRetune=r2 crestMs=40 crestGain=0.55 overlayGain=0.55 trigger=smooth-resistance menuMute=DataMenu,PauseMenu ballisticBaseline=r6-frozen");
                }
                if (shipParticleHapticsEnabled) {
                    pluginLog("Ship particle haptics: ACTIVE subtype=ProtonBeam control=R2 authority=exact-player-Wwise+nearby-R2 forwardFirstShot=hardware-alias+180ms-real-R2 cadence=native-heartbeat sameObject=yes event=0xC8BBBCEA haptic=discrete-particle-pulse pulseMs=60 pulseGain=0.70 continuousParticle=no trigger=finite-energy-break triggerMs=60 menuMute=DataMenu,PauseMenu ballisticBaseline=r6-frozen laserBaseline=v0365-r2-frozen reconEvidence=v0366");
                }
                if (shipMissileHapticsEnabled) {
                    pluginLog("Ship missile haptics: ACTIVE control=R2 authority=exact-player-Wwise+nearby-R2 forwardFirstShot=hardware-alias+180ms-real-R2 cadence=native-heartbeat sameObject=yes event=0x6846C9EC haptic=discrete-launch-thump launchMs=105 launchGain=0.90 continuousMissile=no trigger=heavy-finite-break triggerMs=105 menuMute=DataMenu,PauseMenu ballisticBaseline=r6-frozen laserBaseline=v0365-r2-frozen particleBaseline=v0367-frozen reconEvidence=v0368");
                }
                if (shipEMHapticsEnabled) {
                    pluginLog("Ship EM haptics: ACTIVE control=R2 authority=exact-player-Wwise+nearby-R2 forwardFirstShot=hardware-alias+180ms-real-R2 cadence=native-heartbeat sameObject=yes event=0x7A1A570C haptic=discrete-electrical-pulse pulseMs=75 pulseGain=0.70 continuousEM=no trigger=light-electrical-break triggerStart=72 triggerMs=90 tactileRetune=r1 triggerRearm=r2 partialRelease=48 neutralRefresh=one-cycle menuMute=DataMenu,PauseMenu ballisticBaseline=r6-frozen laserBaseline=v0365-r2-frozen particleBaseline=v0367-frozen missileBaseline=v0369-frozen reconEvidence=v0370");
                }
                if (shipLaunchLandingReconEnabled) {
                    pluginLog("Ship launch/landing recon: ACTIVE reconDiagnosticOnly=yes authority=pilot-landed-state+menu-lifecycle transitions=takeoff:true-to-false,touchdown:false-to-true landingLifecycle=first-fader-load-hud-close-load-close-fader-close+post-load-touchdown-state+bounded-tail docked=suppressed Wwise=zero-external takeoffPreMs=2000 takeoffPostMs=2000 touchdownPostMs=2000 landingMaxMs=30000 cinematicTailWaitMs=15000 maxSamples=256 postLoadTouchdownState=fresh-player-pilot-read-only touchdownPrecisionPoll=runtime-tick-after-first-fader-close precisionEnds=touchdown-or-cancel crossLoadingCapture=diagnostic-only controllerOutput=launch-landing-rumble+touchdown-haptic propulsion=unchanged shipWeaponBaselines=ballistic-r6,laser-r2,particle-v0367,missile-v0369,em-v0371-r2");
                    if (shipTouchdownHapticsEnabled) {
                        pluginLog("Ship touchdown haptics: ACTIVE authority=landing-sequence-landed-false-to-true haptic=ShipTouchdownThump durationMs=100 gain=0.90 adaptiveTrigger=none speaker=none lightbar=white-amber-flash WwiseAuthority=none precisionPoll=runtime-tick-until-touchdown");
                    }
                    if (shipBallisticHapticsEnabled) {
                        pluginLog("Ship launch/landing haptics: ACTIVE body=ShipBoost gain=1.00 level=1.00 triggers=both-EffectEx-sustained start=64 forces=220/255/210 frequency=24 takeoffStart=landed+non-docked+FaderMenu-without-TakeoffMenu takeoffStop=next-FaderMenu-open-after-first-close loadingFallback=yes timeout=30s landingStart=precision-touchdown-tail landingStop=landing-sequence-end touchdownThump=unchanged-100ms-0.90 speaker=none lightbar=takeoff-blue-cyan+landing-amber+touchdown-white");
                    }
                }
                if (remoteVoCaptureEnabled) {
                    const auto startupSpeakerOutputMode =
                        g_speakerManager ?
                            g_speakerManager->outputMode() :
                            sds::SpeakerOutputMode::Both;
                    const char* outputModeName = startupSpeakerOutputMode == sds::SpeakerOutputMode::ControllerOnly
                        ? "ControllerOnly"
                        : "Both";

                    const auto configuredVoiceLanguage =
                        g_speakerManager ?
                            g_speakerManager->voiceLanguage() :
                            sds::SpeakerVoiceLanguage::English;

                    const auto resolvedVoiceLanguage =
                        resolvedSpeakerVoiceLanguage();

                    std::ostringstream voiceLanguageLine;
                    voiceLanguageLine
                        << "Voice archive language: configured="
                        << sds::speakerVoiceLanguageName(
                            configuredVoiceLanguage)
                        << " resolved="
                        << sds::speakerVoiceLanguageName(
                            resolvedVoiceLanguage);

                    pluginLog(
                        voiceLanguageLine.str());

                    char activationLog[768]{};
                    std::snprintf(
                        activationLog,
                        sizeof(activationLog),
                        "Voice Wwise Vorbis decode: v0.3.11 ACTIVE event=0x89E658E8 looseCandidates=raw,data-root archiveConfig=language-aware ba2HeaderBytes=32 gnrlRecordBytes=36 nameTableLookup=enabled payloadRead=matched-uncompressed riffChunkScan=enabled fmtExtendedParse=enabled vorbisNewFmt30=enabled packetHeaderBytes=2 packetRebuild=enabled codebooks=aoTuV-6.03 decoder=stb_vorbis pcm=16bit continuous=yes decompression=disabled resample=44100-to-48000 playback=continuous-controller-speaker replaceActive=yes speakerOutputMode=%s controllerOnlyStop=post-submit-failsafe commsMakeup=+6dB commsLimiter=soft hardwareSpeakerVolume=0x64 hardwarePreamp=0x05 speakerVolumeScale=0.8-v0310max-1.0-plus25pct replay=disabled extraction=disabled loopback=disabled; trigger=remote/radio-voice-only",
                        outputModeName);
                    pluginLog(activationLog);
                }
                if (weaponSpeakerCaptureEnabled) {
                    pluginLog("Weapon sustained speaker: ACTIVE weapons=Arc Welder,Cutter,Va'ruun Starstorm authority=player-Wwise startStop=yes loopMode=layered-stems+authored-smpl rawR2=start-no-authority release-stop=yes blockingMenus=DataMenu,PauseMenu,LoadingMenu");
                    std::ostringstream weaponCaptureLine;
                    weaponCaptureLine << "Weapon speaker: internal Wwise PostEvent capture ACTIVE profiles="
                                      << sds::weaponSpeakerProfiles().size()
                                      << " cues=";
                    bool firstCue = true;
                    for (const auto& profile : sds::weaponSpeakerProfiles()) {
                        for (const auto& cue : profile.cues) {
                            if (cue.trigger != sds::WeaponSpeakerTrigger::WwisePost) {
                                continue;
                            }
                            if (!firstCue) {
                                weaponCaptureLine << ',';
                            }
                            firstCue = false;
                            weaponCaptureLine << profile.weaponIdentity << ':' << cue.action
                                              << "@0x" << std::uppercase << std::hex << std::setw(8)
                                              << std::setfill('0') << cue.liveWwiseEventId << std::dec
                                              << std::setfill(' ') << "/obj0x" << std::uppercase << std::hex
                                              << cue.requiredGameObjectId << std::dec;
                        }
                    }
                    weaponCaptureLine << " normalGameAudio=untouched";
                    pluginLog(weaponCaptureLine.str());
                }
                if (g_weaponSfxDiscovery) {
                    pluginLog(
                        "Weapon SFX discovery: ACTIVE diagnostic-only mode=exact-target-set batch=6-shattered-space-wem-capture operatorTargets=2 startupNameScan=disabled onePassResolution=live-event workerResolution=background wemCapture=targeted-v0347 captureRoot=Data/SFSE/Plugins/StarfieldDualSenseDiagnostics/ShatteredSpaceWemCapture "
                        "internalWwise=zero-external fireWindowMs=-150/+350 "
                        "reloadWindowMs=-2500/+250 drawHolsterWindowMs=-250/+750 "
                        "markerLogging=candidates-only playback=no repost=no stopOriginal=no synthetic=no");
                } else if (weaponSpeakerCaptureEnabled) {
                    pluginLog("Weapon SFX discovery: INACTIVE reason=shattered-space-speaker-promotion playback=no normalGameAudio=untouched");
                } else {
                    pluginLog("Weapon SFX discovery: INACTIVE reason=weapon-speaker-capture-disabled playback=no normalGameAudio=untouched");
                }
            }
        }

        if (const auto* tasks = SFSE::GetTaskInterface()) {
            // TEMP DIAGNOSTIC:
            // Full-mode runtimeTick SKIPPED diagnostic=main-menu-lag.
            // This isolates recurring Full-mode work from startup-installed hooks/workers.
            tasks->AddPermanentTask(runtimeTick);
            pluginLog("CTD RESTORE: runtimeTick ACTIVE semanticHook=on shipHook=on FireMarkerBridge=on");

            if (sharedAudioPreparationEnabled && !g_weaponAudioPipelineDataPath.empty() &&
                (!g_weaponAudioPipelineEnabled || g_weaponSpeakerPreparedCache)) {
                const sds::AudioPipelineStartupOptions startupOptions{
                    .prepareUi = uiSpeakerPlaybackEnabled,
                    .prepareMainMenuUiOnly = false,
                    .prepareWeapons = g_weaponAudioPipelineEnabled,
                    .resolveUiDiagnostics = uiAudioDiscoveryEnabled,
                    .prepareShipWeaponSemantics = shipBallisticHapticsEnabled,
                    .prepareMusicRecon = musicSelectionEnabled,
                    .prepareBoostpackSpeaker = boostpackProductionEnabled,
                    .prepareLandVehicleSpeaker = true,
                    .existingWeaponCache = g_weaponSpeakerPreparedCache,
                };
                g_weaponAudioPipeline = std::make_unique<sds::WeaponAudioPipeline>(
                    g_weaponSpeakerPreparedCache,
                    sds::makeRealWeaponAudioPipelineBackend(g_weaponAudioPipelineDataPath, startupOptions),
                    g_uiSpeakerPreparedCache,
                    g_shipWeaponSemanticCache,
                    g_boostpackSpeakerPreparedCache,
                    g_landVehicleSpeakerCache);
                g_weaponAudioPipeline->start();
                if (g_musicRecon && g_audioCapture && g_audioCapture->active()) {
                    pluginLog("Music recon: ACTIVE diagnostic-only source=existing-PostEvent-hook externalSources=zero resolver=shared-background-worker decode=music-name-candidates output=none wholeGameMix=no");
                    pluginLog("Music selection recon: ACTIVE diagnostic-only targetSource=SoundBanksInfo-Starfield_MUS-multi-media callback=AK_Duration existingCallbackMode=chain-preserve-cookie retirement=AK_EndOfEvent pool=128 output=none");
                } else if (g_musicRecon) {
                    pluginLog("Music recon: INACTIVE reason=capture-unavailable output=none");
                }

                std::ostringstream pipelineLine;
                pipelineLine << "Weapon audio pipeline: worker started families="
                             << sds::weaponSpeakerAudioFamilyCount()
                             << " profiles=" << sds::weaponSpeakerProfiles().size()
                             << " uiPreparation=" << (uiSpeakerPlaybackEnabled ? "yes" : "no")
                             << " uiDiagnosticResolution=" << (uiAudioDiscoveryEnabled ? "yes" : "no")
                             << " uiDiagnosticTargets=" << sds::v0359UiAudioResolutionTargets().size()
                             << " weaponPreparation=" << (g_weaponAudioPipelineEnabled ? "yes" : "no")
                             << " shipWeaponSemantics=" << (shipBallisticHapticsEnabled ? "yes" : "no")
                             << " musicReconPreparation=" << (musicSelectionEnabled ? "yes" : "no")
                             << " discoveryTargets=" << kWeaponSfxDiscoveryTargets.size()
                             << " discoveryQueue=64 startupMode=ui-first workerResolution=background";
                pluginLog(pipelineLine.str());
                if (g_weaponAudioPipelineEnabled) {
                    pluginLog("Weapon speaker family: profile=XM-2311 family=Old Earth Pistol mode=explicit-shared");
                    pluginLog("Weapon speaker family: profile=Va'ruun Quickstrike family=Solstice mode=explicit-shared");
                    pluginLog("Weapon speaker family: profile=Va'ruun Longfang family=Orion mode=explicit-shared");
                    pluginLog("Weapon speaker family: profile=Va'ruun Starlash family=Equinox mode=explicit-shared");
                }
            } else if (sharedAudioPreparationEnabled) {
                pluginLog("Weapon audio pipeline: inactive reason=data-path-or-prepared-cache-unavailable; controller runtime remains active");
                if (g_musicRecon) {
                    pluginLog("Music recon: INACTIVE reason=shared-background-worker-unavailable output=none");
                }
            }
        } else {
            pluginLog("Game state: SFSE task interface unavailable; health polling disabled");
            pluginLog("Weapon audio pipeline: inactive reason=SFSE-task-interface-unavailable; controller runtime remains active");
            if (g_musicRecon) {
                pluginLog("Music recon: INACTIVE reason=SFSE-task-interface-unavailable output=none");
            }
        }
    }

    void onSfseMessage(SFSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }

        if (message->type == SFSE::MessagingInterface::kPostLoad) {
            sds::registerSettingsMenu(std::filesystem::path(kConfigPath), pluginLog, applyImmediateLiveSettings);
            return;
        }

        if (message->type == SFSE::MessagingInterface::kPostDataLoad) {
            initializeRuntime();
        }
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
    SFSE::Init(sfse, { .logName = "StarfieldDualSense" });
    REX::INFO("StarfieldDualSense {} loading | Starfield 1.16.244.0 | fireMarkerABI=validated-live", kVersion);

    g_startupConfig = loadRuntimeConfig();
    g_nativeDualSenseReconnectFixEnabled.store(
        g_startupConfig->operatingMode == sds::OperatingMode::ReconnectFixOnly ||
            g_startupConfig->dualSenseReconnectFix,
        std::memory_order_release);

    if (g_startupConfig->operatingMode == sds::OperatingMode::Full) {
        startEarlyMainMenuUiPreparation();
    } else {
        pluginLog(
            "Main-menu UI speaker prewarm: SKIPPED reason=OperatingMode-ReconnectFixOnly");
    }

    const auto* messaging = SFSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(onSfseMessage)) {
        pluginLog("StarfieldDualSense: failed to register SFSE messaging listener");
        return false;
    }

    return true;
}
