#include <StarfieldDualSense/HapticsManager.h>
#include <StarfieldDualSense/HapticStrength.h>
#include <StarfieldDualSense/WeaponReloadHaptics.h>
#include <StarfieldDualSense/ShipPropulsionHaptics.h>
#include <StarfieldDualSense/SustainedFireLiveness.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace
{
    constexpr auto kCutterStartCatchupWindow = 250ms;
    constexpr auto kShipLaserHeartbeatLease = 250ms;
    constexpr float kShipLaunchLandingRumbleGain = 1.00F;
    constexpr float kShipLaunchLandingRumbleLevel = 1.00F;

    std::string_view eventText(const sds::GameEvent& event) noexcept
    {
        std::size_t length = 0;
        while (length < event.text.size() && event.text[length] != '\0') {
            ++length;
        }
        return { event.text.data(), length };
    }

}

sds::HapticsManager::HapticsManager(Config config, BackendFactory backendFactory, LogCallback log) :
    _config(config),
    _advancedHapticsEnabled(config.advancedHaptics),
    _hapticStrength(sds::effectiveHapticStrength(config.hapticStrength)),
    _boostpackHapticsEnabled(config.boostpackHaptics),
    _boostpackHapticsStrength(std::clamp(config.boostpackHapticsStrength, 0.0F, 3.0F)),
    _backendFactory(std::move(backendFactory)),
    _log(std::move(log)),
    _engine(config.hapticStrength)
{
    (void)_engine.setWeaponHapticsConfig(config);
}

sds::HapticsManager::~HapticsManager()
{
    stop();
}

void sds::HapticsManager::start()
{
    std::scoped_lock lock(_engineMutex);
    if (_started) {
        return;
    }
    _started = true;

    try {
        if (!_backend) {
            _backend = _backendFactory ? _backendFactory() : nullptr;
        }
        if (!_backend) {
            log("Haptics: backend unavailable; HID features unaffected");
            return;
        }

        if (!_advancedHapticsEnabled.load(std::memory_order_acquire)) {
            log("Haptics: output muted by AdvancedHaptics=false");
            return;
        }

        if (!_backend->active()) {
            _backend->start();
        }
    } catch (...) {
        log("Haptics: backend startup failed; HID features unaffected");
        _backend.reset();
    }
}

void sds::HapticsManager::applyLiveSettings(
    GameplayHapticsLiveSettings settings) noexcept
{
    try {
        std::unique_lock lock(_engineMutex);
        settings.hapticStrength =
            sds::clampHapticStrengthSetting(settings.hapticStrength);
        settings.boostpackHapticsStrength =
            std::clamp(settings.boostpackHapticsStrength, 0.0F, 3.0F);

        const float effectiveStrength =
            sds::effectiveHapticStrength(settings.hapticStrength);

        const bool boostpackEnabledChanged =
            _boostpackHapticsEnabled.exchange(settings.boostpackHaptics, std::memory_order_acq_rel) !=
            settings.boostpackHaptics;
        const float previousBoostpackStrength =
            _boostpackHapticsStrength.exchange(
                settings.boostpackHapticsStrength,
                std::memory_order_acq_rel);
        const bool boostpackStrengthChanged =
            previousBoostpackStrength != settings.boostpackHapticsStrength;

        const bool wasEnabled =
            _advancedHapticsEnabled.load(std::memory_order_acquire);
        const float previousStrength =
            _hapticStrength.exchange(effectiveStrength, std::memory_order_acq_rel);
        const bool strengthChanged = previousStrength != effectiveStrength;

        _engine.setHapticStrength(settings.hapticStrength);
        const bool weaponSettingsChanged =
            _engine.setWeaponHapticsConfig(settings.weaponHapticsConfig);

        if (!settings.advancedHaptics) {
            _advancedHapticsEnabled.store(false, std::memory_order_release);
            if (wasEnabled && _backend) {
                (void)_backend->setContinuous({});
            }
            return;
        }

        bool backendStartedNow = false;
        if (!_backend) {
            _advancedHapticsEnabled.store(false, std::memory_order_release);
            log("Haptics: live enable requested but startup backend is unavailable; HID features unaffected");
            return;
        }
        if (!_backend->active()) {
            _backend->start();
            backendStartedNow = true;
        }

        _advancedHapticsEnabled.store(true, std::memory_order_release);

        if (wasEnabled && !strengthChanged && !boostpackEnabledChanged &&
            !boostpackStrengthChanged && !weaponSettingsChanged &&
            !backendStartedNow) {
            return;
        }

        (void)submitContinuousLocked();
    } catch (...) {
        if (settings.advancedHaptics) {
            _advancedHapticsEnabled.store(false, std::memory_order_release);
        }
        log("Haptics: live settings apply failed; existing controller features unaffected");
    }
}
void sds::HapticsManager::clearSustainedFireLocked() noexcept
{
    _cutterStartPending = false;
    _cutterStartWhen = {};
    _cutterStartDeadline = {};
    _cutterHeartbeatArmed = false;
    _cutterHeartbeatDeadline = {};
}

void sds::HapticsManager::clearOnFootContextStateLocked() noexcept
{
    GameEvent reset{};
    reset.type = GameEventType::WeaponEquipped;
    (void)_engine.handle(reset);
    (void)_engine.handleRightTriggerInput(0);
    _lastR2 = 0;
    _lastR2When = {};
    _boostpackThrustActive = false;
    _cutterHeartbeatEligible = false;
    _arcWelderFireEndEligible = false;
    clearSustainedFireLocked();
}


sds::HapticContinuousState sds::HapticsManager::composeContinuousLocked() noexcept
{
    if (_shipPilotActive || _shipLaunchLandingRumbleActive) {
        return composeShipContinuousLocked();
    }
    if (_engine.gameplayBlocked()) return {};
    if (_landVehicleContextActive) {
        return composeLandVehicleContinuousLocked();
    }
    if (_shipContextSuppressed) {
        return {};
    }
    return composeOnFootContinuousLocked();
}

bool sds::HapticsManager::submitContinuousLocked() noexcept
{
    // State changes, publication and backend teardown share the same lock.
    if (!_advancedHapticsEnabled.load(std::memory_order_acquire) || !_backend) {
        return true;
    }
    return _backend->setContinuous(composeContinuousLocked());
}

sds::HapticContinuousState sds::HapticsManager::composeOnFootContinuousLocked() noexcept
{
    if (_boostpackThrustActive &&
        _boostpackHapticsEnabled.load(std::memory_order_acquire)) {
        const float effectiveGain = std::clamp(
            0.30F *
                _hapticStrength.load(std::memory_order_acquire) *
                _boostpackHapticsStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax);
        if (effectiveGain > 0.0F) {
            return {
                .kind = HapticContinuousKind::BoostpackThrust,
                .gain = effectiveGain,
                .level = 0.55F,
                .shipLaserGain = 0.0F,
            };
        }
    }

    return _cutterStartPending ? HapticContinuousState{} : _engine.handleRightTriggerInput(_lastR2);
}
sds::HapticContinuousState sds::HapticsManager::composeShipContinuousLocked() const noexcept
{
    const float hapticStrength =
        _hapticStrength.load(std::memory_order_acquire);

    if (_shipLaunchLandingRumbleActive) {
        if (_engine.shipTransitionBlocked()) return {};
        return {
            .kind = HapticContinuousKind::ShipBoost,
            .gain = std::clamp(kShipLaunchLandingRumbleGain * hapticStrength, 0.0F, 1.0F),
            .level = kShipLaunchLandingRumbleLevel,
            .shipLaserGain = 0.0F,
        };
    }

    if (!_shipPilotActive || _engine.gameplayBlocked()) {
        return {};
    }

    auto state = _shipPropulsionContinuous;
    state.gain = std::clamp(state.gain * hapticStrength, 0.0F, 1.0F);
    state.shipLaserGain = _shipLaserActive ?
        std::clamp(0.55F * hapticStrength, 0.0F, 1.0F) : 0.0F;
    return state;
}
sds::HapticContinuousState sds::HapticsManager::composeLandVehicleContinuousLocked() const noexcept
{
    if (!_landVehicleProductionActive || !_landVehicleContextActive || _shipPilotActive ||
        _engine.gameplayBlocked() || !_landVehicleMotion.authorityActive ||
        _landVehicleProductionEpoch == 0 ||
        _landVehicleMotion.authorityEpoch != _landVehicleProductionEpoch) {
        return {};
    }

    auto state = _landVehicleContinuous;

    // Normal REV-8 chassis feedback is intentionally softer.
    // Apply this before boost/thrust overlays so their tuned
    // minimum strengths remain unchanged.
    state.gain =
        std::clamp(
            state.gain * 0.25F,
            0.0F,
            1.0F);

    if (_landVehicleForwardBoostActive ||
        _landVehicleVerticalThrustActive) {

        state.kind =
            HapticContinuousKind::LandVehicleChassis;

        state.shipLaserGain = 0.0F;
    }

    if (_landVehicleForwardBoostActive) {
        state.gain =
            std::max(
                state.gain,
                0.20F);

        state.level =
            std::max(
                state.level,
                0.62F);
    }

    if (_landVehicleVerticalThrustActive) {
        state.gain =
            std::max(
                state.gain,
                0.24F);

        state.level =
            std::max(
                state.level,
                0.88F);
    }

    state.gain =
        std::clamp(
            state.gain *
                _hapticStrength.load(
                    std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax);

    return state;
}
void sds::HapticsManager::clearLandVehicleProductionLocked() noexcept
{
    _landVehicleProductionActive = false;
    _landVehicleProductionEpoch = 0;
    _landVehicleMotion = {};
    _landVehicleContinuous = {};
    _landVehicleForwardBoostActive = false;
    _landVehicleVerticalThrustActive = false;
    _landVehicleForwardBoostDeadline = {};
    _landVehicleVerticalThrustDeadline = {};
    _landVehicleFeel.reset();
}

void sds::HapticsManager::clearShipLaserLocked() noexcept
{
    _shipLaserActive = false;
    _shipLaserLeaseDeadline = {};
}

void sds::HapticsManager::stop() noexcept
{
    std::scoped_lock lock(_engineMutex);
    if (_backend) {
        try {
            (void)_backend->setContinuous({});
            _backend->stop();
        } catch (...) {
            log("Haptics: backend stop failed; continuing shutdown");
        }
        _backend.reset();
    }
    {
        _shipPilotActive = false;
        _landVehicleContextActive = false;
        _shipContextSuppressed = false;
        _shipPropulsionContinuous = {};
        _shipLaunchLandingRumbleActive = false;
        clearShipLaserLocked();
        clearLandVehicleProductionLocked();
        clearOnFootContextStateLocked();
    }
    _started = false;
}

bool sds::HapticsManager::handle(GameEvent event) noexcept
{
    try {
        std::optional<HapticCommand> command;
        std::optional<HapticContinuousState> continuous;
        bool contextHandled = false;
        bool ok = true;
        bool continuousSubmitted = false;
        bool crestSubmitted = false;
        {
            std::scoped_lock lock(_engineMutex);
            const auto type = event.type;
            const auto marker = eventText(event);

            if (type == GameEventType::MenuOpened || type == GameEventType::MenuClosed ||
                type == GameEventType::GamePaused || type == GameEventType::GameUnpaused) {
                (void)_engine.handle(event);
                if (_engine.gameplayBlocked()) {
                    _lastR2 = 0;
                    _lastR2When = {};
                    clearSustainedFireLocked();
                    _boostpackThrustActive = false;
                    _shipPropulsionContinuous = {};
                    clearShipLaserLocked();
                    _landVehicleMotion = {};
                    _landVehicleContinuous = {};
                    _landVehicleForwardBoostActive = false;
                    _landVehicleVerticalThrustActive = false;
                    _landVehicleForwardBoostDeadline = {};
                    _landVehicleVerticalThrustDeadline = {};
                    _landVehicleFeel.reset();
                }
                if (type == GameEventType::MenuOpened && marker == "LoadingMenu") {
                    clearLandVehicleProductionLocked();
                }
                if (type == GameEventType::MenuOpened && marker == "MainMenu") {
                    _shipLaunchLandingRumbleActive = false;
                }
                if (type == GameEventType::MenuClosed && marker == "LoadingMenu" &&
                    !_shipPilotActive && !_landVehicleContextActive) {
                    _shipContextSuppressed = false;
                }
                continuous = HapticContinuousState{};
                contextHandled = true;
            } else if (type == GameEventType::ShipPilotEntered ||
                type == GameEventType::ShipPilotResumed) {
                _shipPilotActive = true;
                _landVehicleContextActive = false;
                _shipContextSuppressed = true;
                _shipPropulsionContinuous = {};
                clearShipLaserLocked();
                clearLandVehicleProductionLocked();
                clearOnFootContextStateLocked();
                continuous = _shipLaunchLandingRumbleActive ?
                    composeShipContinuousLocked() : HapticContinuousState{};
                contextHandled = true;
            } else if (type == GameEventType::ShipPilotInvalidated) {
                _shipPilotActive = false;
                _landVehicleContextActive = false;
                _shipContextSuppressed = true;
                _shipPropulsionContinuous = {};
                clearShipLaserLocked();
                clearLandVehicleProductionLocked();
                clearOnFootContextStateLocked();
                continuous = HapticContinuousState{};
                contextHandled = true;
            } else if (type == GameEventType::ShipPilotExited) {
                if (_shipPilotActive) {
                    _shipPilotActive = false;
                    _shipPropulsionContinuous = {};
                    clearShipLaserLocked();
                    clearOnFootContextStateLocked();
                    _shipContextSuppressed = _landVehicleContextActive;
                    continuous = HapticContinuousState{};
                }
                contextHandled = true;
            } else if (type == GameEventType::LandVehicleContextEntered) {
                if (!_shipPilotActive) {
                    clearLandVehicleProductionLocked();
                    _landVehicleContextActive = true;
                    _shipContextSuppressed = true;
                    clearOnFootContextStateLocked();
                    continuous = HapticContinuousState{};
                }
                contextHandled = true;
            } else if (type == GameEventType::LandVehicleContextExited) {
                if (_landVehicleContextActive) {
                    clearLandVehicleProductionLocked();
                    _landVehicleContextActive = false;
                    clearOnFootContextStateLocked();
                    if (!_shipPilotActive) {
                        _shipContextSuppressed = false;
                    }
                    continuous = HapticContinuousState{};
                }
                contextHandled = true;
            } else if (type == GameEventType::LandVehicleAuthorityAcquired) {
                if (_landVehicleContextActive && !_shipPilotActive) {
                    _landVehicleFeel.reset();
                    _landVehicleMotion = {};
                    _landVehicleContinuous = {};
                    _landVehicleProductionActive = true;
                    _landVehicleProductionEpoch = 0;
                    continuous = HapticContinuousState{};
                }
                contextHandled = true;
            } else if (type == GameEventType::LandVehicleAuthorityReleased) {
                clearLandVehicleProductionLocked();
                continuous = HapticContinuousState{};
                contextHandled = true;
            } else if (_engine.gameplayBlocked() && type != GameEventType::Shutdown &&
                       type != GameEventType::WeaponEquipped && type != GameEventType::ShipTouchdown) {
                contextHandled = true;
            } else if (type == GameEventType::LandVehicleTouchdown) {
                if (_landVehicleProductionActive && _landVehicleContextActive && !_shipPilotActive &&
                    !_engine.gameplayBlocked() && _landVehicleProductionEpoch != 0) {
                    command = HapticCommand{
                        .kind = HapticEffectKind::LandVehicleTouchdownThump,
                        .gain = std::clamp(
                            LandVehicleControllerFeel::touchdownGain(event.value) * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                }
                contextHandled = true;
            } else if (type == GameEventType::LandVehicleGunFired) {
                if (_landVehicleProductionActive && _landVehicleContextActive && !_shipPilotActive &&
                    !_engine.gameplayBlocked() && _landVehicleProductionEpoch != 0) {
                    command = HapticCommand{
                        .kind = HapticEffectKind::LandVehicleGunRecoil,
                        .gain = std::clamp(0.72F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                }
                contextHandled = true;
            } else if (type == GameEventType::LandVehicleBoostStarted) {
                if (_landVehicleProductionActive && _landVehicleContextActive && !_shipPilotActive &&
                    !_engine.gameplayBlocked() && _landVehicleProductionEpoch != 0) {
                    command = HapticCommand{
                        .kind = HapticEffectKind::LandVehicleBoostKick,
                        .gain = std::clamp(0.78F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                }
                contextHandled = true;
            } else if (type == GameEventType::ShipBallisticWeaponFired) {
                if (_shipPilotActive && _shipContextSuppressed && !_engine.gameplayBlocked()) {
                    clearShipLaserLocked();
                    command = HapticCommand{
                        .kind = HapticEffectKind::ShipBallisticCannonKick,
                        .gain = std::clamp(0.82F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                    continuous = composeShipContinuousLocked();
                }
                contextHandled = true;
            } else if (type == GameEventType::ShipLaserWeaponFired) {
                if (_shipPilotActive && _shipContextSuppressed && !_engine.gameplayBlocked()) {
                    _shipLaserActive = true;
                    _shipLaserLeaseDeadline = event.when + kShipLaserHeartbeatLease;
                    command = HapticCommand{
                        .kind = HapticEffectKind::ShipLaserPulseCrest,
                        .gain = std::clamp(0.55F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                    continuous = composeShipContinuousLocked();
                }
                contextHandled = true;
            } else if (type == GameEventType::ShipLaserWeaponStopped) {
                if (_shipPilotActive && _shipContextSuppressed && !_engine.gameplayBlocked()) {
                    clearShipLaserLocked();
                    continuous = composeShipContinuousLocked();
                }
                contextHandled = true;
            } else if (type == GameEventType::ShipParticleWeaponFired) {
                if (_shipPilotActive && _shipContextSuppressed && !_engine.gameplayBlocked()) {
                    clearShipLaserLocked();
                    command = HapticCommand{
                        .kind = HapticEffectKind::ShipParticlePulse,
                        .gain = std::clamp(0.70F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                    continuous = composeShipContinuousLocked();
                }
                contextHandled = true;
            } else if (type == GameEventType::ShipMissileWeaponFired) {
                if (_shipPilotActive && _shipContextSuppressed && !_engine.gameplayBlocked()) {
                    clearShipLaserLocked();
                    command = HapticCommand{
                        .kind = HapticEffectKind::ShipMissileLaunchThump,
                        .gain = std::clamp(0.90F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                    continuous = composeShipContinuousLocked();
                }
                contextHandled = true;
            } else if (type == GameEventType::ShipEMWeaponFired) {
                if (_shipPilotActive && _shipContextSuppressed && !_engine.gameplayBlocked()) {
                    clearShipLaserLocked();
                    command = HapticCommand{
                        .kind = HapticEffectKind::ShipEMPulse,
                        .gain = std::clamp(0.70F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = event.when,
                    };
                    continuous = composeShipContinuousLocked();
                }
                contextHandled = true;
            } else if (type == GameEventType::ShipTouchdown) {
                _shipLaunchLandingRumbleActive = false;
                continuous = composeShipContinuousLocked();
                if (!_engine.shipTransitionBlocked()) command = HapticCommand{
                    .kind = HapticEffectKind::ShipTouchdownThump,
                    .gain = std::clamp(0.90F * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                    .when = event.when,
                };
                contextHandled = true;
            } else if ((_shipContextSuppressed || _shipLaunchLandingRumbleActive) && type != GameEventType::Shutdown) {
                contextHandled = true;
            }

            if (!contextHandled) {
                command = _engine.handle(event);
            }

            if (!contextHandled && (type == GameEventType::WeaponEquipped ||
                type == GameEventType::Shutdown)) {
                if (type == GameEventType::WeaponEquipped) {
                    const auto* profile = findWeaponProfile(marker);
                    _cutterHeartbeatEligible = profile && profile->name == "Cutter";
                    _arcWelderFireEndEligible = profile && profile->name == "Arc Welder";
                } else {
                    _cutterHeartbeatEligible = false;
                    _arcWelderFireEndEligible = false;
                }
                clearSustainedFireLocked();
            }

            if (!contextHandled && type == GameEventType::WeaponFired && marker == "weaponFireStart") {
                _cutterHeartbeatArmed = _cutterHeartbeatEligible;
                _cutterHeartbeatDeadline = _cutterHeartbeatEligible ?
                    event.when + kSustainedEnergyHeartbeatTimeout :
                    std::chrono::steady_clock::time_point{};
                // Fire-marker delivery and USB input polling happen on different
                // threads. If the marker wins the race, _lastR2 may still be a
                // pre-pull zero. Feeding that stale zero straight back into the
                // engine would authorize and immediately cancel the Cutter beam.
                // A held value is safe to consume immediately; a low value that
                // was sampled after the marker is a real release. Otherwise keep
                // the confirmed start pending briefly for the observer to catch up.
                if (_lastR2 > 12 ||
                    (_lastR2When != std::chrono::steady_clock::time_point{} &&
                     _lastR2When >= event.when)) {
                    _cutterStartPending = false;
                    continuous = _engine.handleRightTriggerInput(_lastR2);
                } else {
                    _cutterStartPending = true;
                    _cutterStartWhen = event.when;
                    _cutterStartDeadline = event.when + kCutterStartCatchupWindow;
                }
            } else if (type == GameEventType::WeaponFired && marker == "weaponFireEnd" &&
                       _arcWelderFireEndEligible) {
                // The Arc Welder animation graph emits a confirmed weaponFireEnd
                // when the live arc stops for auto-reload or true ammo exhaustion.
                // Retire any marker-first catchup window as well, so a late held-R2
                // sample cannot resurrect a firing session that has already ended.
                _cutterStartPending = false;
                _cutterStartWhen = {};
                _cutterStartDeadline = {};
                continuous = _engine.handleRightTriggerInput(_lastR2);
            } else if (type == GameEventType::WeaponFired && marker == "WeaponFire" &&
                       _cutterHeartbeatArmed) {
                _cutterHeartbeatDeadline = event.when + kSustainedEnergyHeartbeatTimeout;
            } else if (type == GameEventType::WeaponFired && marker == "WeaponFire" &&
                       _cutterStartPending) {
                // Sustained-energy WeaponFire heartbeats can arrive immediately
                // after weaponFireStart, before USB R2 polling catches up. While
                // that confirmed start is pending, do not feed the stale pre-pull
                // R2 value back into the engine and revoke its authorization.
                // The first post-marker R2 sample resolves the pending start.
            } else if (!contextHandled &&
                       (type == GameEventType::WeaponFired ||
                        type == GameEventType::WeaponEquipped ||
                        type == GameEventType::GamePaused ||
                        type == GameEventType::MenuOpened ||
                        type == GameEventType::MenuClosed ||
                        type == GameEventType::Shutdown)) {
                continuous = _engine.handleRightTriggerInput(
                    type == GameEventType::Shutdown ? 0 : _lastR2);
            }

            if (!contextHandled && type == GameEventType::Shutdown) {
                _shipPilotActive = false;
                _landVehicleContextActive = false;
                // No gameplay owner remains to authorize late native cues.
                _shipContextSuppressed = true;
                clearLandVehicleProductionLocked();
                _shipLaunchLandingRumbleActive = false;
                _boostpackThrustActive = false;
                continuous = HapticContinuousState{};
                _lastR2 = 0;
                _lastR2When = {};
            }
            if (_advancedHapticsEnabled.load(std::memory_order_acquire) && _backend) {
                if (continuous) {
                    continuous = composeContinuousLocked();
                    continuousSubmitted = _backend->setContinuous(*continuous);
                    ok = continuousSubmitted;
                }
                if (command) {
                    crestSubmitted = _backend->enqueue(*command);
                    ok = crestSubmitted && ok;
                }
            }
        }

        if (event.type == GameEventType::ShipBallisticWeaponFired) {
            char delivery[256]{};
            if (command) {
                std::snprintf(
                    delivery, sizeof(delivery),
                    "Ship ballistic haptics: stage=confirmed-fire kind=ShipBallisticCannonKick gain=%.3f eventWhenUs=%lld",
                    static_cast<double>(command->gain),
                    static_cast<long long>(hapticEventTimestampMicros(command->when)));
            } else {
                std::snprintf(
                    delivery, sizeof(delivery),
                    "Ship ballistic haptics: stage=rejected reason=no-pilot-authority eventWhenUs=%lld",
                    static_cast<long long>(hapticEventTimestampMicros(event.when)));
            }
            log(delivery);
        }

        if (event.type == GameEventType::IncomingDamage) {
            char delivery[384]{};
            if (command) {
                const auto kindName = hapticEffectKindName(command->kind);
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Incoming damage delivery: stage=engine-produced eventWhenUs=%lld healthLoss=%.4f kind=%.*s gain=%.3f centered=yes",
                    static_cast<long long>(hapticEventTimestampMicros(command->when)),
                    static_cast<double>(event.value),
                    static_cast<int>(kindName.size()),
                    kindName.data(),
                    static_cast<double>(command->gain));
            } else {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Incoming damage delivery: stage=engine-rejected eventWhenUs=%lld healthLoss=%.4f",
                    static_cast<long long>(hapticEventTimestampMicros(event.when)),
                    static_cast<double>(event.value));
            }
            log(delivery);
        }

        if (event.type == GameEventType::MeleeImpact) {
            char delivery[384]{};
            if (command) {
                const auto kindName = hapticEffectKindName(command->kind);
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Melee impact delivery: stage=engine-produced eventWhenUs=%lld form=0x%08X kind=%.*s gain=%.3f",
                    static_cast<long long>(hapticEventTimestampMicros(command->when)),
                    static_cast<unsigned int>(event.formId),
                    static_cast<int>(kindName.size()),
                    kindName.data(),
                    static_cast<double>(command->gain));
            } else {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Melee impact delivery: stage=engine-rejected eventWhenUs=%lld form=0x%08X",
                    static_cast<long long>(hapticEventTimestampMicros(event.when)),
                    static_cast<unsigned int>(event.formId));
            }
            log(delivery);
        }

        if (!ok) {
            log("Haptics: state or command submission failed; HID features unaffected");
        }
        if (event.type == GameEventType::ShipLaserWeaponFired &&
            command && continuous && continuousSubmitted && crestSubmitted) {
            char delivery[256]{};
            std::snprintf(
                delivery,
                sizeof(delivery),
                "Ship laser haptics: stage=submitted continuousSubmitted=yes crestSubmitted=yes overlayGain=%.3f crestGain=%.3f eventWhenUs=%lld",
                static_cast<double>(continuous->shipLaserGain),
                static_cast<double>(command->gain),
                static_cast<long long>(hapticEventTimestampMicros(command->when)));
            log(delivery);
        }
        if (event.type == GameEventType::ShipTouchdown) {
            char delivery[256]{};
            if (command && crestSubmitted) {
                std::snprintf(
                    delivery, sizeof(delivery),
                    "Ship touchdown haptics: stage=submitted kind=ShipTouchdownThump gain=%.3f eventWhenUs=%lld",
                    static_cast<double>(command->gain),
                    static_cast<long long>(hapticEventTimestampMicros(command->when)));
            } else {
                std::snprintf(
                    delivery, sizeof(delivery),
                    "Ship touchdown haptics: stage=rejected reason=no-command-or-submit-failure eventWhenUs=%lld",
                    static_cast<long long>(hapticEventTimestampMicros(event.when)));
            }
            log(delivery);
        }
        if (event.type == GameEventType::ShipParticleWeaponFired) {
            char delivery[256]{};
            if (command && crestSubmitted) {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Ship particle haptics: stage=submitted pulseSubmitted=yes kind=ShipParticlePulse gain=%.3f continuousParticle=no eventWhenUs=%lld",
                    static_cast<double>(command->gain),
                    static_cast<long long>(hapticEventTimestampMicros(command->when)));
            } else {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Ship particle haptics: stage=rejected reason=no-pilot-authority-or-submit-failure eventWhenUs=%lld",
                    static_cast<long long>(hapticEventTimestampMicros(event.when)));
            }
            log(delivery);
        }
        if (event.type == GameEventType::ShipMissileWeaponFired) {
            char delivery[256]{};
            if (command && crestSubmitted) {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Ship missile haptics: stage=submitted launchSubmitted=yes kind=ShipMissileLaunchThump gain=%.3f continuousBed=no eventWhenUs=%lld",
                    static_cast<double>(command->gain),
                    static_cast<long long>(hapticEventTimestampMicros(command->when)));
            } else {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Ship missile haptics: stage=rejected reason=no-pilot-authority-or-submit-failure eventWhenUs=%lld",
                    static_cast<long long>(hapticEventTimestampMicros(event.when)));
            }
            log(delivery);
        }
        if (event.type == GameEventType::ShipEMWeaponFired) {
            char delivery[256]{};
            if (command && crestSubmitted) {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Ship EM haptics: stage=submitted pulseSubmitted=yes kind=ShipEMPulse gain=%.3f continuousEM=no eventWhenUs=%lld",
                    static_cast<double>(command->gain),
                    static_cast<long long>(hapticEventTimestampMicros(command->when)));
            } else {
                std::snprintf(
                    delivery,
                    sizeof(delivery),
                    "Ship EM haptics: stage=rejected reason=no-pilot-authority-or-submit-failure eventWhenUs=%lld",
                    static_cast<long long>(hapticEventTimestampMicros(event.when)));
            }
            log(delivery);
        }
        return ok;
    } catch (...) {
        log("Haptics: semantic dispatch failed; HID features unaffected");
        return false;
    }
}

void sds::HapticsManager::handleLandVehicleMotionState(
    const LandVehicleMotionState& state) noexcept
{
    try {
        bool submit = false;
        bool submitted = true;
        {
            std::scoped_lock lock(_engineMutex);
            if (!_landVehicleProductionActive || !_landVehicleContextActive || _shipPilotActive ||
                _engine.gameplayBlocked() || !state.authorityActive || state.authorityEpoch == 0) {
                _landVehicleMotion = {};
                _landVehicleContinuous = {};
                _landVehicleForwardBoostActive = false;
                _landVehicleVerticalThrustActive = false;
                _landVehicleForwardBoostDeadline = {};
                _landVehicleVerticalThrustDeadline = {};
                _landVehicleFeel.reset();
                submit = _landVehicleContextActive;
            } else if (_landVehicleProductionEpoch != 0 &&
                       state.authorityEpoch != _landVehicleProductionEpoch) {
                clearLandVehicleProductionLocked();
                submit = true;
            } else {
                if (_landVehicleProductionEpoch == 0) {
                    _landVehicleProductionEpoch = state.authorityEpoch;
                }
                _landVehicleMotion = state;
                const auto target = _landVehicleFeel.observeMotion(state);
                if (target.bodyGain > 0.0F) {
                    _landVehicleContinuous = {
                        .kind = HapticContinuousKind::LandVehicleChassis,
                        .gain = std::clamp(target.bodyGain, 0.0F, 1.0F),
                        .level = std::clamp(target.textureLevel, 0.0F, 1.0F),
                        .shipLaserGain = 0.0F,
                    };
                } else {
                    _landVehicleContinuous = {};
                }
                const auto diagnosticNow = std::chrono::steady_clock::now();
                if (diagnosticNow >= _nextLandVehicleDiagnosticLog) {
                    _nextLandVehicleDiagnosticLog = diagnosticNow + std::chrono::seconds(1);
                    const bool diagnosticBlocked = !_landVehicleProductionActive ||
                        !_landVehicleContextActive || _shipPilotActive || _engine.gameplayBlocked() ||
                        !state.authorityActive || state.authorityEpoch != _landVehicleProductionEpoch;
                    const bool diagnosticOwner = _landVehicleContinuous.kind == HapticContinuousKind::LandVehicleChassis &&
                        _landVehicleContinuous.gain > 0.0F;
                    char diagnostic[384]{};
                    std::snprintf(
                        diagnostic,
                        sizeof(diagnostic),
                        "REV-8 chassis diagnostic: speed=%.3f acceleration=%.3f bodyGain=%.3f textureLevel=%.3f blocked=%s owner=%s authority=%s epoch=%llu productionEpoch=%llu airborne=%s",
                        static_cast<double>(state.speed),
                        static_cast<double>(state.acceleration),
                        static_cast<double>(_landVehicleContinuous.gain),
                        static_cast<double>(_landVehicleContinuous.level),
                        diagnosticBlocked ? "yes" : "no",
                        diagnosticOwner ? "chassis" : "none",
                        state.authorityActive ? "yes" : "no",
                        static_cast<unsigned long long>(state.authorityEpoch),
                        static_cast<unsigned long long>(_landVehicleProductionEpoch),
                        state.airborne ? "yes" : "no");
                    log(diagnostic);
                }
                submit = true;
            }
            if (submit) submitted = submitContinuousLocked();
        }

        if (!submitted) {
            log("Haptics: REV-8 continuous submission failed; HID features unaffected");
        }
    } catch (...) {
        log("Haptics: REV-8 motion dispatch failed; HID features unaffected");
    }
}

bool sds::HapticsManager::setLandVehicleBoostInput(
    bool vertical,
    bool active,
    std::chrono::steady_clock::time_point when) noexcept
{
    try {
        std::unique_lock lock(_engineMutex);
        HapticContinuousState continuous{};
        std::optional<HapticCommand> kick{};
        bool changed = false;

        {
            bool& inputState =
                vertical ?
                    _landVehicleVerticalThrustActive :
                    _landVehicleForwardBoostActive;

            auto& deadline =
                vertical ?
                    _landVehicleVerticalThrustDeadline :
                    _landVehicleForwardBoostDeadline;

            if (active) {
                if (!_landVehicleProductionActive ||
                    !_landVehicleContextActive ||
                    _shipPilotActive ||
                    _engine.gameplayBlocked() ||
                    _landVehicleProductionEpoch == 0 ||
                    !_landVehicleMotion.authorityActive ||
                    _landVehicleMotion.authorityEpoch !=
                        _landVehicleProductionEpoch) {

                    inputState = false;
                    deadline = {};
                    return true;
                }

                deadline =
                    when +
                    std::chrono::seconds(5);

                if (inputState) {
                    return true;
                }

                inputState = true;
                changed = true;

                if (!vertical) {
                    kick = HapticCommand{
                        .kind =
                            HapticEffectKind::LandVehicleBoostKick,
                        .gain =
                            std::clamp(
                                0.64F *
                                    _hapticStrength.load(
                                        std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax),
                        .when = when,
                    };
                }
            }
            else {
                deadline = {};

                if (!inputState) {
                    return true;
                }

                inputState = false;
                changed = true;
            }

            continuous =
                composeContinuousLocked();
        }

        if (!changed ||
            !_advancedHapticsEnabled.load(
                std::memory_order_acquire) ||
            !_backend) {

            return true;
        }

        bool submitted =
            _backend->setContinuous(
                continuous);

        if (kick) {
            submitted =
                _backend->enqueue(*kick) &&
                submitted;
        }

        lock.unlock();
        char diagnostic[256]{};

        std::snprintf(
            diagnostic,
            sizeof(diagnostic),
            "REV-8 sustained haptic: mode=%s action=%s gain=%.3f level=%.3f kick=%s submitted=%s",
            vertical ? "vertical-thrust" : "forward-boost",
            active ? "start" : "stop",
            static_cast<double>(continuous.gain),
            static_cast<double>(continuous.level),
            kick ? "yes" : "no",
            submitted ? "yes" : "no");

        log(diagnostic);

        return submitted;
    }
    catch (...) {
        log(
            "Haptics: REV-8 sustained boost/thrust dispatch failed; "
            "other controller features unaffected");

        return false;
    }
}
bool sds::HapticsManager::handleShipPropulsionState(
    const ShipPropulsionState& state) noexcept
{
    try {
        HapticContinuousState continuous{};
        bool submit = false;
        bool submitted = true;
        {
            std::scoped_lock lock(_engineMutex);
            if (_shipPilotActive && _shipContextSuppressed && !_engine.gameplayBlocked()) {
                _shipPropulsionContinuous = mapShipPropulsionHaptics(state, 1.0F);
                continuous = composeContinuousLocked();
                submit = true;
                submitted = submitContinuousLocked();
            }
        }

        if (!submit) {
            return true;
        }

        const char* kind = continuous.kind == HapticContinuousKind::ShipBoost ? "boost" :
            continuous.kind == HapticContinuousKind::ShipPropulsion ? "propulsion" : "clear";
        char diagnostic[192]{};
        std::snprintf(
            diagnostic,
            sizeof(diagnostic),
            "Ship propulsion haptics: kind=%s gain=%.3f level=%.3f",
            kind,
            static_cast<double>(continuous.gain),
            static_cast<double>(continuous.level));
        log(diagnostic);

        if (!submitted) {
            log("Haptics: ship propulsion continuous submission failed; HID features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: ship propulsion semantic dispatch failed; HID features unaffected");
        return false;
    }
}

bool sds::HapticsManager::setShipLaunchLandingRumble(
    bool active,
    std::string_view phase) noexcept
{
    try {
        HapticContinuousState continuous{};
        bool changed = false;
        bool submitted = true;
        {
            std::scoped_lock lock(_engineMutex);
            if (_shipLaunchLandingRumbleActive != active) {
                _shipLaunchLandingRumbleActive = active;
                continuous = composeContinuousLocked();
                submitted = submitContinuousLocked();
                changed = true;
            }
        }

        if (!changed) {
            return true;
        }

        char delivery[320]{};
        std::snprintf(
            delivery,
            sizeof(delivery),
            "Ship launch/landing rumble: stage=%s action=%s phase=%.*s kind=%s gain=%.3f level=%.3f",
            submitted ? "submitted" : "rejected",
            active ? "start" : "stop",
            static_cast<int>(phase.size()),
            phase.data(),
            continuous.kind == HapticContinuousKind::ShipBoost ? "ShipBoost" :
                continuous.kind == HapticContinuousKind::ShipPropulsion ? "ShipPropulsion" : "None",
            static_cast<double>(continuous.gain),
            static_cast<double>(continuous.level));
        log(delivery);

        if (!submitted) {
            log("Haptics: ship launch/landing rumble submission failed; HID features unaffected");
        }
        return submitted;
    } catch (...) {
        log("Haptics: ship launch/landing rumble dispatch failed; HID features unaffected");
        return false;
    }
}

bool sds::HapticsManager::handleRightTriggerInput(
    std::uint8_t r2,
    std::chrono::steady_clock::time_point when) noexcept
{
    try {
        bool submitState = true;
        bool submitted = true;
        {
            std::scoped_lock lock(_engineMutex);
            if (_engine.gameplayBlocked()) return true;
            // A pre-marker release must not revoke the newer firing watchdog.
            if (_cutterStartPending && r2 <= 12 && when < _cutterStartWhen) return true;
            _lastR2 = r2;
            _lastR2When = when;
            if (_shipContextSuppressed) {
                if (_shipPilotActive && _shipLaserActive && r2 <= 12 && !_engine.gameplayBlocked()) {
                    clearShipLaserLocked();
                    submitState = true;
                } else {
                    submitState = false;
                }
            } else {
                if (r2 <= 12) {
                    _cutterHeartbeatArmed = false;
                    _cutterHeartbeatDeadline = {};
                }
                {
                    if (_cutterStartPending) {
                        if (when > _cutterStartDeadline) {
                            // Do not let an old start marker authorize a later pull.
                            _cutterStartPending = false;
                            _cutterStartWhen = {};
                            _cutterStartDeadline = {};
                            (void)_engine.handleRightTriggerInput(0);
                        } else {
                            _cutterStartPending = false;
                            _cutterStartWhen = {};
                            _cutterStartDeadline = {};
                        }
                    }
                }
            }
            if (submitState) submitted = submitContinuousLocked();
        }

        if (!submitted) {
            log("Haptics: continuous state submission failed; HID features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: continuous semantic dispatch failed; HID features unaffected");
        return false;
    }
}

bool sds::HapticsManager::emitWeaponReloadCue(
    std::string_view action,
    std::chrono::steady_clock::time_point when,
    std::string_view borrowedReloadProfile,
    bool useCustomReloadProfile) noexcept
{
    try {
        std::scoped_lock lock(_engineMutex);
        // The stage has already been authorized by the native weapon Wwise
        // post and successfully handed to the controller-speaker queue.
        const auto cue = weaponReloadHapticForAction(action);
        if (!cue || !_advancedHapticsEnabled.load(std::memory_order_acquire) ||
            !_backend || _engine.gameplayBlocked()) {
            return true;
        }
        if (_shipContextSuppressed || _shipPilotActive || _landVehicleContextActive) {
            return true;
        }
        const float familyStrength = _engine.reloadHapticsFamilyScale(
            borrowedReloadProfile, useCustomReloadProfile);
        if (familyStrength <= 0.0F) {
            return true;
        }

        const float gain = std::clamp(
            cue->baseGain * _hapticStrength.load(std::memory_order_acquire) *
                familyStrength,
            0.0F, sds::kHapticOverdriveGainMax);
        if (gain <= 0.0F) {
            return true;
        }
        if (!_backend->enqueue(HapticCommand{
                .kind = cue->kind,
                .gain = gain,
                .when = when,
            })) {
            log("Haptics: reload stage enqueue failed; existing weapon feedback unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: reload stage dispatch failed; existing weapon feedback unaffected");
        return false;
    }
}

bool sds::HapticsManager::emitDigipickWwiseEvent(
    std::uint32_t eventId,
    std::chrono::steady_clock::time_point when) noexcept
{
    try {
        std::unique_lock lock(_engineMutex);
        HapticEffectKind kind{};
        float baseGain = 0.0F;
        switch (eventId) {
        case 0x0A9F7EB0u: // UI_Menu_Minigame_Security_Rotate
            kind = HapticEffectKind::DigipickRotateTick;
            baseGain = 0.12F;
            break;
        case 0xFFE19CA3u: // UI_Menu_Minigame_Security_Select_Shape
            kind = HapticEffectKind::DigipickSelectClick;
            baseGain = 0.16F;
            break;
        case 0xF53EFAB6u: // UI_Menu_Minigame_Security_Pick_Insert_Success
            kind = HapticEffectKind::DigipickInsertClunk;
            baseGain = 0.30F;
            break;
        case 0xCCAAD205u: // UI_Menu_Minigame_Security_Puzzle_Success
            kind = HapticEffectKind::DigipickSuccess;
            baseGain = 0.38F;
            break;
        default:
            return true;
        }

        if (!_advancedHapticsEnabled.load(std::memory_order_acquire) || !_backend) {
            return true;
        }

        const float effectiveGain = std::clamp(
            baseGain * _hapticStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax);
        if (effectiveGain <= 0.0F) {
            return true;
        }

        if (!_backend->enqueue(HapticCommand{
                .kind = kind,
                .gain = effectiveGain,
                .when = when,
            })) {
            log("Haptics: Digipick finite effect enqueue failed; other controller features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: Digipick Wwise haptic dispatch failed; other controller features unaffected");
        return false;
    }
}
bool sds::HapticsManager::emitBoostpackIgnition(
    std::chrono::steady_clock::time_point when) noexcept
{
    try {
        std::unique_lock lock(_engineMutex);
        std::optional<HapticCommand> command{};
        {
            if (_shipContextSuppressed || _shipPilotActive || _landVehicleContextActive ||
                _shipLaunchLandingRumbleActive || _engine.gameplayBlocked()) {
                return true;
            }

            if (_boostpackHapticsEnabled.load(std::memory_order_acquire)) {
                const float effectiveGain = std::clamp(
                    0.48F *
                        _hapticStrength.load(std::memory_order_acquire) *
                        _boostpackHapticsStrength.load(std::memory_order_acquire), 0.0F, sds::kHapticOverdriveGainMax);
                if (effectiveGain > 0.0F) {
                    command = HapticCommand{
                        .kind = HapticEffectKind::BoostpackIgnition,
                        .gain = effectiveGain,
                        .when = when,
                    };
                }
            }
        }

        if (!command ||
            !_advancedHapticsEnabled.load(std::memory_order_acquire) ||
            !_backend) {
            return true;
        }

        if (!_backend->enqueue(*command)) {
            log("Haptics: boostpack ignition enqueue failed; other controller features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: boostpack ignition dispatch failed; other controller features unaffected");
        return false;
    }
}

bool sds::HapticsManager::emitOnFootLanding(
    float baseGain,
    std::chrono::steady_clock::time_point when) noexcept
{
    try {
        std::scoped_lock lock(_engineMutex);
        if (!_advancedHapticsEnabled.load(std::memory_order_acquire) || !_backend) {
            return true;
        }
        if (_engine.gameplayBlocked() || _shipContextSuppressed || _shipPilotActive || _landVehicleContextActive) {
            return true;
        }
        const float gain = std::clamp(
            baseGain * _hapticStrength.load(std::memory_order_acquire),
            0.0F, sds::kHapticOverdriveGainMax);
        if (gain <= 0.0F) {
            return true;
        }
        if (!_backend->enqueue(HapticCommand{
                .kind = HapticEffectKind::OnFootLandingThump,
                .gain = gain,
                .when = when,
            })) {
            log("Haptics: on-foot landing enqueue failed; other controller features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: on-foot landing dispatch failed; other controller features unaffected");
        return false;
    }
}

bool sds::HapticsManager::startBoostpackThrust() noexcept
{
    try {
        std::unique_lock lock(_engineMutex);
        {
            if (_shipContextSuppressed || _shipPilotActive || _landVehicleContextActive ||
                _shipLaunchLandingRumbleActive || _engine.gameplayBlocked()) {
                _boostpackThrustActive = false;
                return true;
            }

            _boostpackThrustActive = true;
        }

        const bool submitted = submitContinuousLocked();
        lock.unlock();
        if (!submitted) {
            log("Haptics: boostpack thrust start failed; other controller features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: boostpack thrust start dispatch failed; other controller features unaffected");
        return false;
    }
}

bool sds::HapticsManager::refreshBoostpackThrust() noexcept
{
    return startBoostpackThrust();
}

bool sds::HapticsManager::stopBoostpackThrust() noexcept
{
    try {
        std::unique_lock lock(_engineMutex);
        _boostpackThrustActive = false;
        const bool submitted = submitContinuousLocked();
        lock.unlock();
        if (!submitted) {
            log("Haptics: boostpack thrust stop failed; other controller features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: boostpack thrust stop dispatch failed; other controller features unaffected");
        return false;
    }
}
bool sds::HapticsManager::tick(std::chrono::steady_clock::time_point now) noexcept
{
    try {
        std::unique_lock lock(_engineMutex);
        bool expired = false;
        bool shipLaserExpired = false;
        bool landVehicleBoostExpired = false;
        {
            const bool forwardBoostExpired =
                _landVehicleForwardBoostActive &&
                _landVehicleForwardBoostDeadline !=
                    std::chrono::steady_clock::time_point{} &&
                now >= _landVehicleForwardBoostDeadline;

            const bool verticalThrustExpired =
                _landVehicleVerticalThrustActive &&
                _landVehicleVerticalThrustDeadline !=
                    std::chrono::steady_clock::time_point{} &&
                now >= _landVehicleVerticalThrustDeadline;

            if (forwardBoostExpired || verticalThrustExpired) {
                if (forwardBoostExpired) {
                    _landVehicleForwardBoostActive = false;
                    _landVehicleForwardBoostDeadline = {};
                }

                if (verticalThrustExpired) {
                    _landVehicleVerticalThrustActive = false;
                    _landVehicleVerticalThrustDeadline = {};
                }

                expired = true;
                landVehicleBoostExpired = true;
            }

            if (_shipContextSuppressed) {
                if (_shipPilotActive && _shipLaserActive &&
                    _shipLaserLeaseDeadline != std::chrono::steady_clock::time_point{} &&
                    now >= _shipLaserLeaseDeadline) {
                    clearShipLaserLocked();
                    expired = true;
                    shipLaserExpired = true;
                }
            } else if (_cutterHeartbeatArmed &&
                       _cutterHeartbeatDeadline != std::chrono::steady_clock::time_point{} &&
                       now >= _cutterHeartbeatDeadline) {
                _cutterHeartbeatArmed = false;
                _cutterHeartbeatDeadline = {};
                _cutterStartPending = false;
                _cutterStartWhen = {};
                _cutterStartDeadline = {};
                (void)_engine.handleRightTriggerInput(0);
                expired = true;
            }
        }

        if (!expired) {
            return true;
        }
        const bool submitted = submitContinuousLocked();
        lock.unlock();
        log(
            landVehicleBoostExpired ?
                "Haptics: REV-8 boost/thrust lease expired; clearing stale continuous feedback" :
            shipLaserExpired ?
                "Ship laser haptics: 250 ms heartbeat lease expired; stopping stale energy texture" :
                "Haptics: Cutter beam heartbeat expired; stopping stale continuous feedback");
        if (!submitted) {
            log(
                landVehicleBoostExpired ?
                    "Haptics: REV-8 boost/thrust lease clear failed; HID features unaffected" :
                shipLaserExpired ?
                    "Haptics: ship laser heartbeat timeout clear failed; HID features unaffected" :
                    "Haptics: Cutter heartbeat timeout clear failed; HID features unaffected");
            return false;
        }
        return true;
    } catch (...) {
        log("Haptics: continuous heartbeat watchdog failed; HID features unaffected");
        return false;
    }
}

bool sds::HapticsManager::active() const noexcept
{
    std::scoped_lock lock(_engineMutex);
    return _advancedHapticsEnabled.load(std::memory_order_acquire) &&
        _backend && _backend->active();
}

void sds::HapticsManager::log(std::string_view message) const noexcept
{
    if (!_log) {
        return;
    }
    try {
        _log(message);
    } catch (...) {
    }
}
