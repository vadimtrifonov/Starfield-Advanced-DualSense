#include <StarfieldDualSense/EffectsEngine.h>
#include <StarfieldDualSense/SustainedFireLiveness.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace
{
    using namespace std::chrono_literals;
    constexpr auto kShipLaserHeartbeatLease = 250ms;

    sds::Color mixShipLightbarColor(
        sds::Color from,
        sds::Color to,
        float t) noexcept
    {
        t = std::clamp(t, 0.0F, 1.0F);

        const auto channel =
            [t](std::uint8_t a, std::uint8_t b) noexcept {
                const auto value =
                    std::lround(
                        static_cast<float>(a) +
                        (static_cast<float>(b) - static_cast<float>(a)) * t);

                return static_cast<std::uint8_t>(
                    std::clamp<long>(value, 0, 255));
            };

        return {
            channel(from.r, to.r),
            channel(from.g, to.g),
            channel(from.b, to.b)
        };
    }

    sds::Color playerHealthLightbarColor(float ratio) noexcept
    {
        ratio = std::clamp(ratio, 0.0F, 1.0F);

        if (ratio < 0.25F) {
            return { 255, 0, 0 };
        }
        if (ratio <= 0.50F) {
            return { 255, 160, 0 };
        }
        return { 0, 64, 255 };
    }
    sds::Color shipHealthLightbarColor(float ratio) noexcept
    {
        ratio = std::clamp(ratio, 0.0F, 1.0F);

        // Match the existing on-foot health lightbar exactly.
        if (ratio < 0.25F) {
            return { 255, 0, 0 };
        }

        if (ratio <= 0.50F) {
            return { 255, 160, 0 };
        }

        return { 0, 64, 255 };
    }

    std::uint8_t scaledByte(std::uint8_t value, float scale)
    {
        const auto scaled = std::lround(static_cast<float>(value) * std::clamp(scale, 0.0F, 1.0F));
        return static_cast<std::uint8_t>(std::clamp<long>(scaled, 0, 255));
    }

    std::uint8_t clampByte(int value)
    {
        return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
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

}

sds::EffectsEngine::EffectsEngine(Config config) noexcept :
    _config(config)
{}

bool sds::EffectsEngine::applyLiveSettings(
    const ControllerLiveSettings& settings) noexcept
{
    const float triggerStrength = std::clamp(settings.triggerStrength, 0.0F, 1.0F);
    const bool adaptiveChanged = _config.adaptiveTriggers != settings.adaptiveTriggers;
    const bool customAdaptiveChanged =
        _config.customWeaponAdaptiveTriggersEnabled !=
        settings.customWeaponAdaptiveTriggersEnabled;
    const bool strengthChanged = _config.triggerStrength != triggerStrength;
    const bool lightbarChanged = _config.lightbar != settings.lightbar;

    if (!adaptiveChanged && !customAdaptiveChanged &&
        !strengthChanged && !lightbarChanged) {
        return false;
    }

    _config.adaptiveTriggers = settings.adaptiveTriggers;
    _config.customWeaponAdaptiveTriggersEnabled =
        settings.customWeaponAdaptiveTriggersEnabled;
    _config.triggerStrength = triggerStrength;
    _config.lightbar = settings.lightbar;

    if (!settings.lightbar) {
        _state.output.lightbar = {};
    }

    if (!settings.adaptiveTriggers ||
        (_customWeaponEquipped && !_shipPilotActive &&
         !_landVehicleContextActive &&
         !settings.customWeaponAdaptiveTriggersEnabled)) {
        _state.output.leftTrigger = {};
        _state.output.rightTrigger = {};
        _state.transientTriggerActive = false;
        _state.transientUntil = {};
        _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
    } else if (adaptiveChanged || customAdaptiveChanged || strengthChanged) {
        restorePersistentTrigger();
    }

    return true;
}

bool sds::EffectsEngine::onFootWeaponTriggersEnabled() const noexcept
{
    return _config.adaptiveTriggers &&
        (!_customWeaponEquipped || _config.customWeaponAdaptiveTriggersEnabled);
}

std::uint8_t sds::EffectsEngine::equippedR2Rating() const noexcept
{
    return _weaponProfile ? _weaponProfile->r2Rating : 4;
}

sds::WeaponTriggerFamily sds::EffectsEngine::equippedTriggerFamily() const noexcept
{
    return _weaponProfile ? _weaponProfile->triggerFamily : WeaponTriggerFamily::BallisticRifle;
}

sds::WeaponCadenceClass sds::EffectsEngine::equippedCadenceClass() const noexcept
{
    return _weaponProfile ? _weaponProfile->cadenceClass : WeaponCadenceClass::Single;
}

sds::TriggerEffect sds::EffectsEngine::equippedWeaponTrigger() const noexcept
{
    if (!onFootWeaponTriggersEnabled()) {
        return {};
    }

    // The matrix's R2 1-10 score is the primary tuning input. A higher rating
    // begins resistance earlier in the pull and raises the force wall.
    const int rating = std::clamp<int>(equippedR2Rating(), 1, 10);
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::ContinuousResistance;
    effect.startPosition = clampByte(164 - rating * 10);
    effect.force = scaledByte(clampByte(34 + rating * 24), _config.triggerStrength);
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::chargePullTrigger() const noexcept
{
    auto effect = equippedWeaponTrigger();
    if (effect.mode == TriggerEffectMode::Off) {
        return effect;
    }

    // Charge weapons tighten slightly as the player commits to the pull. The
    // input value is quantized by the controller worker, so this does not turn
    // into a HID-write storm while the trigger is held.
    const int pull = static_cast<int>(_rightTriggerInput);
    const int bonus = (pull * 42) / 255;
    const int scaledBonus = static_cast<int>(scaledByte(clampByte(bonus), _config.triggerStrength));
    effect.force = clampByte(static_cast<int>(effect.force) + scaledBonus);
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::firePulseTrigger() const noexcept
{
    if (!onFootWeaponTriggersEnabled()) {
        return {};
    }

    // v0.2.37 locks the hardware-proven Eon pistol mechanism: deep-travel
    // delivery followed by the 48 ms return to its normal resistance wall.
    // Only the pulse envelope is modestly stronger than the v0.2.36 diagnostic.
    if (_weaponProfile && _weaponProfile->name == "Eon") {
        TriggerEffect effect{};
        effect.mode = TriggerEffectMode::EffectEx;
        effect.startPosition = 108;
        effect.keepEffect = false;
        effect.beginForce = scaledByte(195, _config.triggerStrength);
        effect.middleForce = scaledByte(255, _config.triggerStrength);
        effect.endForce = scaledByte(175, _config.triggerStrength);
        effect.frequency = 76;
        return effect;
    }

    // Maelstrom's real 14-16 Hz WeaponFire cadence is already correct. Keep
    // cadence entirely game-driven and make each individual rifle pulse hit
    // harder and sharper instead of manufacturing extra events.
    if (_weaponProfile && _weaponProfile->name == "Maelstrom") {
        TriggerEffect effect{};
        effect.mode = TriggerEffectMode::EffectEx;
        effect.startPosition = 108;
        effect.keepEffect = false;
        effect.beginForce = scaledByte(180, _config.triggerStrength);
        effect.middleForce = scaledByte(245, _config.triggerStrength);
        effect.endForce = scaledByte(160, _config.triggerStrength);
        effect.frequency = 76;
        return effect;
    }

    // v0.2.39 keeps the hardware-approved 16 ms Microgun retrigger cadence
    // from v0.2.38 and changes only the per-kick force envelope. The first
    // recoil still comes from a real WeaponFire marker after the game's
    // natural spin-up delay; no synthetic shots are introduced.
    if (_weaponProfile && _weaponProfile->name == "Microgun") {
        TriggerEffect effect{};
        effect.mode = TriggerEffectMode::EffectEx;
        effect.startPosition = 90;
        effect.keepEffect = false;
        effect.beginForce = scaledByte(210, _config.triggerStrength);
        effect.middleForce = scaledByte(255, _config.triggerStrength);
        effect.endForce = scaledByte(190, _config.triggerStrength);
        effect.frequency = 40;
        return effect;
    }

    // v0.2.42 extends the hardware-approved Negotiator launcher feel to the
    // Bridger after hardware testing confirmed its shot timing/cadence are clean
    // but its generic launcher kick is much too weak. Both named launchers use
    // the same heavy EffectEx envelope; other launchers stay generic.
    if (_weaponProfile &&
        (_weaponProfile->name == "Negotiator" || _weaponProfile->name == "Bridger")) {
        TriggerEffect effect{};
        effect.mode = TriggerEffectMode::EffectEx;
        effect.startPosition = 74;
        effect.keepEffect = false;
        effect.beginForce = scaledByte(240, _config.triggerStrength);
        effect.middleForce = scaledByte(255, _config.triggerStrength);
        effect.endForce = scaledByte(220, _config.triggerStrength);
        effect.frequency = 28;
        return effect;
    }

    const int rating = std::clamp<int>(equippedR2Rating(), 1, 10);
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = clampByte(146 - rating * 8);
    effect.keepEffect = false;
    effect.beginForce = scaledByte(clampByte(72 + rating * 13), _config.triggerStrength);
    effect.middleForce = scaledByte(clampByte(100 + rating * 16), _config.triggerStrength);
    effect.endForce = scaledByte(clampByte(58 + rating * 10), _config.triggerStrength);

    switch (equippedTriggerFamily()) {
    case WeaponTriggerFamily::BallisticHandgun:
        effect.frequency = 54;
        break;
    case WeaponTriggerFamily::BallisticRapid:
        effect.frequency = 76;
        break;
    case WeaponTriggerFamily::BallisticRifle:
        effect.frequency = 50;
        break;
    case WeaponTriggerFamily::PrecisionBallistic:
        effect.frequency = 38;
        break;
    case WeaponTriggerFamily::Shotgun:
        effect.frequency = 32;
        break;
    case WeaponTriggerFamily::HeavyBallistic:
        effect.frequency = 40;
        break;
    case WeaponTriggerFamily::Launcher:
        effect.frequency = 28;
        break;
    case WeaponTriggerFamily::Magnetic:
        effect.frequency = 62;
        break;
    case WeaponTriggerFamily::Laser:
        effect.frequency = 80;
        break;
    case WeaponTriggerFamily::Particle:
        effect.frequency = 48;
        break;
    case WeaponTriggerFamily::SustainedEnergy:
        effect.frequency = 92;
        break;
    case WeaponTriggerFamily::EM:
        effect.frequency = 44;
        break;
    case WeaponTriggerFamily::Melee:
        effect.frequency = 34;
        break;
    }
    return effect;
}


sds::TriggerEffect sds::EffectsEngine::shipPrimaryFireWallTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::ContinuousResistance;
    effect.startPosition = 72;
    effect.force = scaledByte(184, _config.triggerStrength);
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::shipBallisticWallTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::ContinuousResistance;
    effect.startPosition = 86;
    effect.force = scaledByte(148, _config.triggerStrength);
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::shipBallisticFireTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = 78;
    effect.keepEffect = false;
    effect.beginForce = scaledByte(180, _config.triggerStrength);
    effect.middleForce = scaledByte(245, _config.triggerStrength);
    effect.endForce = scaledByte(165, _config.triggerStrength);
    effect.frequency = 46;
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::shipLaserFireTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    // Pulse lasers should feel energized and deliberate, not like a ballistic
    // mechanism cycling. Keep one smooth resistance wall live only while
    // native laser heartbeats maintain the bounded firing lease.
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::ContinuousResistance;
    effect.startPosition = 82;
    effect.force = scaledByte(168, _config.triggerStrength);
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::shipParticleFireTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    // Proton Beam fire is a discrete native heartbeat, not a sustained laser
    // stream. Give each confirmed shot a fast energetic snap, lighter than the
    // ballistic cannon break, then return to the generic cockpit wall.
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = 80;
    effect.keepEffect = false;
    effect.beginForce = scaledByte(158, _config.triggerStrength);
    effect.middleForce = scaledByte(218, _config.triggerStrength);
    effect.endForce = scaledByte(138, _config.triggerStrength);
    effect.frequency = 68;
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::shipMissileFireTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    // Each hardware-confirmed missile launch gets the heaviest ship-weapon
    // trigger break. It is still finite: native Wwise launch heartbeats own
    // cadence and the generic cockpit wall returns between launches.
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = 74;
    effect.keepEffect = false;
    effect.beginForce = scaledByte(205, _config.triggerStrength);
    effect.middleForce = scaledByte(255, _config.triggerStrength);
    effect.endForce = scaledByte(180, _config.triggerStrength);
    effect.frequency = 34;
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::shipEMFireTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    // EM fire is a discrete native heartbeat. Keep the break lighter than the
    // Proton Beam and push its frequency upward so it reads as an electrical
    // buzz/zap rather than a mechanical recoil impulse.
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = 72;
    effect.keepEffect = false;
    effect.beginForce = scaledByte(132, _config.triggerStrength);
    effect.middleForce = scaledByte(196, _config.triggerStrength);
    effect.endForce = scaledByte(112, _config.triggerStrength);
    effect.frequency = 92;
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::shipLaunchLandingTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    // v0.3.79 deliberately makes launch/landing feel mechanical and forceful
    // across both hands. keepEffect=true lets the DualSense hardware sustain
    // the authored pulse until the authoritative transition-stop semantic.
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = 64;
    effect.keepEffect = true;
    effect.beginForce = scaledByte(220, _config.triggerStrength);
    effect.middleForce = scaledByte(255, _config.triggerStrength);
    effect.endForce = scaledByte(210, _config.triggerStrength);
    effect.frequency = 24;
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::landVehiclePrimaryFireWallTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::ContinuousResistance;
    effect.startPosition = 88;
    effect.force = scaledByte(150, _config.triggerStrength);
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::landVehicleGunFireTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = 68;
    effect.keepEffect = false;
    effect.beginForce = scaledByte(180, _config.triggerStrength);
    effect.middleForce = scaledByte(228, _config.triggerStrength);
    effect.endForce = scaledByte(166, _config.triggerStrength);
    effect.frequency = 46;
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::landVehicleAimTrigger() const noexcept
{
    if (!_config.adaptiveTriggers) {
        return {};
    }

    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = 72;
    effect.keepEffect = true;
    effect.beginForce = scaledByte(92, _config.triggerStrength);
    effect.middleForce = scaledByte(154, _config.triggerStrength);
    effect.endForce = scaledByte(138, _config.triggerStrength);
    effect.frequency = 18;
    return effect;
}

sds::TriggerEffect sds::EffectsEngine::sustainedFireTrigger() const noexcept
{
    if (!onFootWeaponTriggersEnabled()) {
        return {};
    }

    // Continuous-beam tools should feel like heavy powered equipment rather
    // than a rifle cycling. EffectEx with keepEffect gives the controller a
    // sustained low-frequency mechanical throb until the real R2 release.
    const int rating = std::clamp<int>(equippedR2Rating(), 1, 10);
    TriggerEffect effect{};
    effect.mode = TriggerEffectMode::EffectEx;
    effect.startPosition = clampByte(132 - rating * 5);
    effect.keepEffect = true;
    effect.beginForce = scaledByte(clampByte(110 + rating * 10), _config.triggerStrength);
    effect.middleForce = scaledByte(clampByte(170 + rating * 10), _config.triggerStrength);
    effect.endForce = scaledByte(clampByte(130 + rating * 8), _config.triggerStrength);
    effect.frequency = 18;
    return effect;
}

std::chrono::milliseconds sds::EffectsEngine::firePulseDuration() const noexcept
{
    using namespace std::chrono_literals;

    // Negotiator and Bridger use the hardware-approved launcher hard snap:
    // one real WeaponFire starts one EffectEx kick, then the normal launcher
    // wall returns at 55 ms. Other launchers keep the 115 ms family lifetime.
    if (_weaponProfile &&
        (_weaponProfile->name == "Negotiator" || _weaponProfile->name == "Bridger")) {
        return 55ms;
    }

    // Microgun needs a deliberately short kick so its real high-rate
    // WeaponFire markers can be separated by a return to the heavy trigger
    // wall instead of continuously refreshing one EffectEx command.
    if (_weaponProfile && _weaponProfile->name == "Microgun") {
        return 16ms;
    }

    // Eon's hardware-proven pistol snap uses the same 48 ms pulse lifetime as
    // Maelstrom before returning to its own normal resistance wall.
    if (_weaponProfile &&
        (_weaponProfile->name == "Eon" || _weaponProfile->name == "Maelstrom")) {
        return 48ms;
    }

    switch (equippedCadenceClass()) {
    case WeaponCadenceClass::Rapid:
        return 45ms;
    case WeaponCadenceClass::ReceiverAware:
        return 65ms;
    case WeaponCadenceClass::Precision:
        return 95ms;
    case WeaponCadenceClass::Shotgun:
        return 105ms;
    case WeaponCadenceClass::Launcher:
        return 115ms;
    case WeaponCadenceClass::Charge:
        return 100ms;
    case WeaponCadenceClass::Sustained:
        return 55ms;
    case WeaponCadenceClass::Melee:
        return 90ms;
    case WeaponCadenceClass::Single:
    default:
        return 80ms;
    }
}

void sds::EffectsEngine::clearOnFootPersistentState() noexcept
{
    _weaponEquipped = false;
    _customWeaponEquipped = false;
    _weaponProfile = nullptr;
    _rightTriggerInput = 0;
    _rightTriggerPressed = false;
    _sustainedFireActive = false;
    _sustainedHeartbeatDeadline = {};
    _eonWallPhase = EonWallPhase::Normal;
    _eonShotPullSeen = false;
    _eonPendingUntil = {};
    _state.output.leftTrigger = {};
    _state.output.rightTrigger = {};
    _state.output.lightbar = {};
    _state.transientTriggerActive = false;
    _state.transientUntil = {};
}

void sds::EffectsEngine::clearLandVehicleProductionState() noexcept
{
    _landVehicleProductionActive = false;
    _landVehicleAimActive = false;
    _landVehicleProductionEpoch = 0;
    _landVehicleGunRecoilDeadline = {};
    _state.output.leftTrigger = {};
    _state.output.rightTrigger = {};
    _state.transientTriggerActive = false;
    _state.transientUntil = {};
}

void sds::EffectsEngine::clearTransientTriggerState() noexcept
{
    _rightTriggerInput = 0;
    _rightTriggerPressed = false;
    _sustainedFireActive = false;
    _sustainedHeartbeatDeadline = {};
    _eonWallPhase = EonWallPhase::Normal;
    _eonShotPullSeen = false;
    _eonPendingUntil = {};
    _shipLaserActive = false;
    _shipLaserLeaseDeadline = {};
    _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
    _landVehicleAimActive = false;
    _landVehicleGunRecoilDeadline = {};
    _state.transientTriggerActive = false;
    _state.transientUntil = {};
    _state.output.leftTrigger = {};
    _state.output.rightTrigger = {};
}

void sds::EffectsEngine::restorePersistentTrigger() noexcept
{
    if (_shipLaunchLandingHapticsActive) {
        if (_feedbackGate.shipTransitionBlocked()) {
            _state.output.leftTrigger = {};
            _state.output.rightTrigger = {};
        } else {
            _state.output.leftTrigger = shipLaunchLandingTrigger();
            _state.output.rightTrigger = shipLaunchLandingTrigger();
        }
        return;
    }

    _state.output.leftTrigger = {};
    if (_feedbackGate.blocked()) {
        _state.output.rightTrigger = {};
        return;
    }
    if (_shipPilotActive) {
        if (_shipLaserActive) {
            _state.output.rightTrigger = shipLaserFireTrigger();
        } else {
            _state.output.rightTrigger = _shipR2BallisticConfirmed ?
                shipBallisticWallTrigger() : shipPrimaryFireWallTrigger();
        }
        return;
    }
    if (_landVehicleProductionActive && _landVehicleContextActive) {
        _state.output.leftTrigger = _landVehicleAimActive ? landVehicleAimTrigger() : TriggerEffect{};
        _state.output.rightTrigger = landVehiclePrimaryFireWallTrigger();
        return;
    }
    if (_persistentContextSuppressed || !_weaponEquipped) {
        _state.output.rightTrigger = {};
        return;
    }

    // Once Eon's synchronized one-shot recoil has actually been sent, do not
    // overwrite it before its 48 ms deadline. An early physical release may
    // still restore the wall sooner; shallow pending travel keeps the wall armed.
    if (_weaponProfile && _weaponProfile->name == "Eon" &&
        _eonWallPhase == EonWallPhase::AwaitCurrentPullRelease) {
        return;
    }

    if (equippedCadenceClass() == WeaponCadenceClass::Charge && _rightTriggerPressed) {
        _state.output.rightTrigger = chargePullTrigger();
    } else {
        _state.output.rightTrigger = equippedWeaponTrigger();
    }
}

sds::EffectState sds::EffectsEngine::handle(const GameEvent& event) noexcept
{
    if (_feedbackGate.handle(event)) {
        if (_feedbackGate.blocked()) clearTransientTriggerState();
        restorePersistentTrigger();
    }
    switch (event.type) {
    case GameEventType::PlayerHealthChanged:
        _playerHealthRatio = std::clamp(event.value, 0.0F, 1.0F);
        _playerHealthValid = true;

        // Physical pilot-seat authority owns the lightbar independently of
        // the menu-based ship trigger/haptic context. Keep the fresh player
        // health cached, but do not paint it over ship/cinematic output.
        if (_shipLightbarSeatActive) {
            break;
        }

        // Ship context owns/suppresses the normal health lightbar,
        // but the REV-8 deliberately keeps player health visible.
        if (_persistentContextSuppressed &&
            !_landVehicleContextActive) {
            break;
        }

        if (_config.lightbar) {
            _state.output.lightbar =
                playerHealthLightbarColor(_playerHealthRatio);
        }
        break;

    case GameEventType::ShipHealthChanged:
        _shipHealthRatio = std::clamp(event.value, 0.0F, 1.0F);
        _shipHealthValid = true;
        if (_config.lightbar &&
            _shipLightbarSeatActive &&
            _shipLightbarPhase == ShipLightbarPhase::None &&
            _shipTouchdownFlashUntil == std::chrono::steady_clock::time_point{}) {
            _state.output.lightbar =
                shipHealthLightbarColor(_shipHealthRatio);
        }
        break;

    case GameEventType::ShipLightbarSeatEntered:
        // Lightbar-only authority. Do not touch ship trigger, haptic, speaker,
        // propulsion, or weapon ownership.
        _shipLightbarSeatActive = true;
        _shipHealthValid = false;
        break;

    case GameEventType::ShipLightbarSeatExited:
        // Actual physical pilot-seat release is the only normal path that
        // releases persistent ship/cinematic lightbar ownership.
        _shipLightbarSeatActive = false;
        _shipHealthValid = false;
        _shipLightbarPhase = ShipLightbarPhase::None;
        _shipLightbarPhaseStartedAt = {};
        _shipTouchdownFlashStartedAt = {};
        _shipTouchdownFlashUntil = {};

        if (_config.lightbar) {
            if (_persistentContextSuppressed &&
                !_landVehicleContextActive) {
                _state.output.lightbar = {};
            } else if (_playerHealthValid) {
                _state.output.lightbar =
                    playerHealthLightbarColor(_playerHealthRatio);
            }
        }
        break;

    case GameEventType::WeaponEquipped:
        if (_persistentContextSuppressed || _shipLaunchLandingHapticsActive) {
            break;
        }
        _weaponEquipped = true;
        _customWeaponEquipped = isCustomWeaponProfileMatch(eventIdentity(event));
        _weaponProfile = findWeaponProfile(eventIdentity(event));
        // A weapon swap must always terminate any previous recoil/beam effect
        // so the newly equipped weapon immediately owns its matrix wall.
        _sustainedFireActive = false;
        _sustainedHeartbeatDeadline = {};
        _state.transientTriggerActive = false;
        _eonWallPhase = EonWallPhase::Normal;
        _eonShotPullSeen = false;
        _eonPendingUntil = {};
        restorePersistentTrigger();
        break;

    case GameEventType::WeaponFired:
        // This event reaches the engine only from a confirmed Starfield
        // animation marker. R2 input alone never fabricates recoil.
        if (onFootWeaponTriggersEnabled() && _weaponEquipped &&
            !_feedbackGate.blocked() && !_shipLaunchLandingHapticsActive) {
            const auto markerText = eventIdentity(event);
            const bool sustainedEnergy = equippedTriggerFamily() == WeaponTriggerFamily::SustainedEnergy;
            const bool cutterHeartbeatWatchdog = _weaponProfile && _weaponProfile->name == "Cutter";

            if (sustainedEnergy) {
                // h7 proved weaponFireStart precedes the Cutter's repeated
                // WeaponFire markers. The start marker owns one continuous
                // construction-equipment texture; repeated beam markers do
                // not become gun-like individual kicks. Empty text remains
                // accepted for the pre-bridge test/API path.
                if (markerText == "weaponFireEnd" && _weaponProfile &&
                    _weaponProfile->name == "Arc Welder") {
                    // Starfield emits weaponFireEnd when the Arc Welder stops
                    // producing its arc, including an automatic reload or
                    // running completely dry while R2 is still held. End the
                    // powered trigger cadence from that authoritative marker;
                    // a later R2 sample alone must not revive it.
                    _sustainedFireActive = false;
                    _sustainedHeartbeatDeadline = {};
                    _state.transientTriggerActive = false;
                    _state.transientUntil = {};
                    restorePersistentTrigger();
                    break;
                }
                if (!markerText.empty() && markerText != "weaponFireStart") {
                    if (markerText == "WeaponFire" && _sustainedFireActive && cutterHeartbeatWatchdog) {
                        _sustainedHeartbeatDeadline = event.when + kSustainedEnergyHeartbeatTimeout;
                    }
                    break;
                }
                _state.output.rightTrigger = sustainedFireTrigger();
                _sustainedFireActive = true;
                _sustainedHeartbeatDeadline = cutterHeartbeatWatchdog
                    ? event.when + kSustainedEnergyHeartbeatTimeout
                    : std::chrono::steady_clock::time_point{};
                _state.transientTriggerActive = true;
                _state.transientUntil = std::chrono::steady_clock::time_point::max();
            } else {
                // Ballistic/shotgun/rapid-fire cadence comes from Starfield's real
                // repeated WeaponFire markers. Ignore unrelated animation tags.
                if (!markerText.empty() && markerText != "WeaponFire") {
                    break;
                }
                _sustainedFireActive = false;

                // Microgun fires so quickly that a normal 55 ms Sustained
                // pulse is continuously refreshed and becomes one flat effect.
                // Once a kick starts, overlapping real WeaponFire markers may
                // not extend it; tick() must get the controller back to the
                // Microgun wall before a later shot can retrigger the kick.
                if (_weaponProfile && _weaponProfile->name == "Microgun" &&
                    _state.transientTriggerActive) {
                    break;
                }

                if (_weaponProfile && _weaponProfile->name == "Eon") {
                    using namespace std::chrono_literals;
                    constexpr auto kEonPendingRecoilWindow = 200ms;
                    constexpr std::uint8_t kEonRecoilDepth = 160;

                    // The marker authorizes exactly one recoil. Eon waits for
                    // deep trigger travel, holds the final pistol pulse for
                    // 48 ms, then returns its normal resistance wall.
                    if (_rightTriggerInput >= kEonRecoilDepth) {
                        _state.output.rightTrigger = firePulseTrigger();
                        _state.transientTriggerActive = true;
                        _state.transientUntil = event.when + firePulseDuration();
                        _eonWallPhase = EonWallPhase::AwaitCurrentPullRelease;
                        _eonShotPullSeen = true;
                        _eonPendingUntil = {};
                    } else {
                        _state.transientTriggerActive = false;
                        _eonWallPhase = EonWallPhase::AwaitDeepTravel;
                        _eonShotPullSeen = _rightTriggerPressed;
                        _eonPendingUntil = event.when + kEonPendingRecoilWindow;
                        restorePersistentTrigger();
                    }
                } else {
                    _state.output.rightTrigger = firePulseTrigger();
                    _state.transientTriggerActive = true;
                    _state.transientUntil = event.when + firePulseDuration();
                }
            }
        }
        break;

    case GameEventType::ShipLaunchLandingLightbarStarted: {
        const auto phase = eventIdentity(event);

        if (phase == "takeoff") {
            _shipLightbarPhase = ShipLightbarPhase::Takeoff;
        } else if (phase == "landing") {
            _shipLightbarPhase = ShipLightbarPhase::Landing;
        } else {
            _shipLightbarPhase = ShipLightbarPhase::None;
        }

        _shipLightbarPhaseStartedAt = event.when;
        _shipTouchdownFlashStartedAt = {};
        _shipTouchdownFlashUntil = {};
        break;
    }

    case GameEventType::ShipLaunchLandingLightbarStopped:
        _shipLightbarPhase = ShipLightbarPhase::None;
        _shipLightbarPhaseStartedAt = {};
        break;

    case GameEventType::ShipTouchdown:
        _shipLightbarPhase = ShipLightbarPhase::None;
        _shipLightbarPhaseStartedAt = {};
        _shipTouchdownFlashStartedAt = event.when;
        _shipTouchdownFlashUntil =
            event.when + std::chrono::milliseconds(350);
        break;

    case GameEventType::ShipLaunchLandingHapticsStarted:
        if (_config.adaptiveTriggers) {
            _shipLaunchLandingHapticsActive = true;
            _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};
            _state.transientTriggerActive = false;
            _state.transientUntil = {};
            restorePersistentTrigger();
        }
        break;

    case GameEventType::ShipLaunchLandingHapticsStopped:
        _shipLaunchLandingHapticsActive = false;
        _state.output.leftTrigger = {};
        _state.transientTriggerActive = false;
        _state.transientUntil = {};
        restorePersistentTrigger();
        break;

    case GameEventType::ShipBallisticWeaponFired:
        if (_shipPilotActive && !_shipLaunchLandingHapticsActive &&
            !_feedbackGate.blocked() && _config.adaptiveTriggers) {
            _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};
            _shipR2BallisticConfirmed = true;
            _state.output.rightTrigger = shipBallisticFireTrigger();
            _state.transientTriggerActive = true;
            _state.transientUntil = event.when + std::chrono::milliseconds(45);
        }
        break;

    case GameEventType::ShipLaserWeaponFired:
        if (_shipPilotActive && !_shipLaunchLandingHapticsActive &&
            !_feedbackGate.blocked() && _config.adaptiveTriggers) {
            _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
            _shipR2BallisticConfirmed = false;
            _shipLaserActive = true;
            _shipLaserLeaseDeadline = event.when + kShipLaserHeartbeatLease;
            _state.output.rightTrigger = shipLaserFireTrigger();
            _state.transientTriggerActive = false;
            _state.transientUntil = {};
        }
        break;

    case GameEventType::ShipLaserWeaponStopped:
        if (_shipPilotActive) {
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};
            restorePersistentTrigger();
        }
        break;

    case GameEventType::ShipParticleWeaponFired:
        if (_shipPilotActive && !_shipLaunchLandingHapticsActive &&
            !_feedbackGate.blocked() && _config.adaptiveTriggers) {
            _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
            _shipR2BallisticConfirmed = false;
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};
            _state.output.rightTrigger = shipParticleFireTrigger();
            _state.transientTriggerActive = true;
            _state.transientUntil = event.when + std::chrono::milliseconds(60);
        }
        break;

    case GameEventType::ShipMissileWeaponFired:
        if (_shipPilotActive && !_shipLaunchLandingHapticsActive &&
            !_feedbackGate.blocked() && _config.adaptiveTriggers) {
            _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
            _shipR2BallisticConfirmed = false;
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};
            _state.output.rightTrigger = shipMissileFireTrigger();
            _state.transientTriggerActive = true;
            _state.transientUntil = event.when + std::chrono::milliseconds(105);
        }
        break;

    case GameEventType::ShipEMWeaponFired:
        if (_shipPilotActive && !_shipLaunchLandingHapticsActive &&
            !_feedbackGate.blocked() && _config.adaptiveTriggers) {
            _shipR2BallisticConfirmed = false;
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};

            if (_shipEMTriggerRearmed) {
                _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
                _state.output.rightTrigger = shipEMFireTrigger();
                _state.transientTriggerActive = true;
                _state.transientUntil = event.when + std::chrono::milliseconds(90);
            } else {
                // A rapid partial re-pull can leave the DualSense actuator
                // mechanically inside the previous EffectEx region even after
                // our 90 ms transient has expired. Force one neutral controller
                // presentation before re-applying the same native-authorized EM
                // break so the hardware gets a real re-arm edge without asking
                // the player for a full physical release.
                _state.output.rightTrigger = {};
                _state.transientTriggerActive = false;
                _state.transientUntil = {};
                _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::NeutralFrameQueued;
            }

            _shipEMTriggerRearmed = false;
        }
        break;

    case GameEventType::ShipPilotEntered:
    case GameEventType::ShipPilotResumed:
        clearLandVehicleProductionState();
        _shipPilotActive = true;
        _shipLightbarSeatActive = true;
        _shipHealthValid = false;
        _shipHealthRatio = 1.0F;
        _landVehicleContextActive = false;
        _persistentContextSuppressed = true;
        _shipR2BallisticConfirmed = false;
        _shipLaserActive = false;
        _shipLaserLeaseDeadline = {};
        _shipEMTriggerRearmed = true;
        _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
        clearOnFootPersistentState();
        restorePersistentTrigger();
        break;

    case GameEventType::ShipPilotExited:
        if (_shipPilotActive) {
            _shipPilotActive = false;
            _shipR2BallisticConfirmed = false;
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};
            _shipEMTriggerRearmed = true;
            _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
            clearOnFootPersistentState();
            _persistentContextSuppressed = _landVehicleContextActive;
        }
        break;

    case GameEventType::LandVehicleContextEntered: {
        if (!_shipPilotActive) {
            const auto retainedHealthLightbar =
                _state.output.lightbar;

            clearLandVehicleProductionState();
            _landVehicleContextActive = true;
            _persistentContextSuppressed = true;
            clearOnFootPersistentState();

            if (_config.lightbar) {
                _state.output.lightbar =
                    retainedHealthLightbar;
            }
        }
        break;
    }

    case GameEventType::LandVehicleContextExited: {
        if (_landVehicleContextActive) {
            const auto retainedHealthLightbar =
                _state.output.lightbar;

            clearLandVehicleProductionState();
            _landVehicleContextActive = false;
            clearOnFootPersistentState();

            if (_config.lightbar) {
                _state.output.lightbar =
                    retainedHealthLightbar;
            }

            if (!_shipPilotActive) {
                _persistentContextSuppressed = false;
            }
        }
        break;
    }

    case GameEventType::LandVehicleAuthorityAcquired:
        if (_landVehicleContextActive && !_shipPilotActive) {
            _landVehicleProductionActive = true;
            _landVehicleAimActive = false;
            ++_landVehicleProductionEpoch;
            _landVehicleGunRecoilDeadline = {};
            _state.transientTriggerActive = false;
            _state.transientUntil = {};
            restorePersistentTrigger();
        }
        break;

    case GameEventType::LandVehicleAuthorityReleased:
        clearLandVehicleProductionState();
        break;

    case GameEventType::LandVehicleAimStarted:
        if (_landVehicleProductionActive && _landVehicleContextActive &&
            !_shipPilotActive && !_feedbackGate.blocked()) {
            _landVehicleAimActive = true;
            _state.output.leftTrigger = landVehicleAimTrigger();
        }
        break;

    case GameEventType::LandVehicleAimStopped:
        if (_landVehicleProductionActive && _landVehicleContextActive && !_shipPilotActive) {
            _landVehicleAimActive = false;
            _state.output.leftTrigger = {};
        }
        break;

    case GameEventType::LandVehicleGunFired:
        if (_landVehicleProductionActive && _landVehicleContextActive && !_shipPilotActive &&
            !_feedbackGate.blocked() && _config.adaptiveTriggers) {
            _state.output.rightTrigger = landVehicleGunFireTrigger();
            _landVehicleGunRecoilDeadline = event.when + std::chrono::milliseconds(70);
            _state.transientTriggerActive = true;
            _state.transientUntil = _landVehicleGunRecoilDeadline;
        }
        break;

    case GameEventType::ShipPilotInvalidated:
        clearLandVehicleProductionState();
        _shipPilotActive = false;
        _landVehicleContextActive = false;
        _shipLaunchLandingHapticsActive = false;
        _shipR2BallisticConfirmed = false;
        _shipLaserActive = false;
        _shipLaserLeaseDeadline = {};
        _shipEMTriggerRearmed = true;
        _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
        _persistentContextSuppressed = true;
        clearOnFootPersistentState();
        break;

    case GameEventType::MenuOpened: {
        const auto identity = eventIdentity(event);

        if (identity == "MainMenu") {
            _shipLightbarSeatActive = false;
            _shipHealthValid = false;
            _shipLightbarPhase = ShipLightbarPhase::None;
            _shipLightbarPhaseStartedAt = {};
            _shipTouchdownFlashStartedAt = {};
            _shipTouchdownFlashUntil = {};
            if (_config.lightbar) {
                _state.output.lightbar = {};
            }
        }

        if (identity == "MainMenu") {
            _shipLaunchLandingHapticsActive = false;
            restorePersistentTrigger();
        }
        if (identity == "LoadingMenu" && _landVehicleContextActive && !_shipPilotActive) {
            _landVehicleProductionActive = false;
        }
        break;
    }

    case GameEventType::MenuClosed: {
        const auto identity = eventIdentity(event);
        if (!_shipPilotActive && !_landVehicleContextActive &&
            _persistentContextSuppressed && identity == "LoadingMenu") {
            // Loading ending only releases the context gate. It does not
            // reconstruct any cached on-foot output; fresh gameplay events
            // must rebuild weapon and health ownership afterward.
            _persistentContextSuppressed = false;
        }
        break;
    }

    case GameEventType::Shutdown:
        _feedbackGate = {};
        clearLandVehicleProductionState();
        _landVehicleProductionEpoch = 0;
        _shipPilotActive = false;
        _landVehicleContextActive = false;
        _shipLaunchLandingHapticsActive = false;
        _shipR2BallisticConfirmed = false;
        _shipLaserActive = false;
        _shipLaserLeaseDeadline = {};
        _shipEMTriggerRearmed = true;
        _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
        _persistentContextSuppressed = false;
        _weaponEquipped = false;
        _customWeaponEquipped = false;
        _weaponProfile = nullptr;
        _rightTriggerInput = 0;
        _rightTriggerPressed = false;
        _sustainedFireActive = false;
        _sustainedHeartbeatDeadline = {};
        _eonWallPhase = EonWallPhase::Normal;
        _eonShotPullSeen = false;
        _eonPendingUntil = {};
        _state = {};
        break;

    default:
        break;
    }

    return _state;
}

sds::EffectState sds::EffectsEngine::handleRightTriggerInput(
    std::uint8_t value,
    std::chrono::steady_clock::time_point now) noexcept
{
    if (_feedbackGate.blocked() || _shipLaunchLandingHapticsActive) return _state;

    constexpr std::uint8_t kPressThreshold = 24;
    constexpr std::uint8_t kReleaseThreshold = 12;

    const bool wasPressed = _rightTriggerPressed;
    _rightTriggerInput = value;
    if (!_rightTriggerPressed && value >= kPressThreshold) {
        _rightTriggerPressed = true;
    } else if (_rightTriggerPressed && value <= kReleaseThreshold) {
        _rightTriggerPressed = false;
    }

    if (_shipPilotActive) {
        // Raw R2 is never firing authority. For EM it may only re-arm the
        // already-authorized trigger presentation once the finger backs out of
        // the electrical break region. This deliberately does not require the
        // normal full-release threshold (12).
        constexpr std::uint8_t kShipEMPartialRearmThreshold = 48;
        if (value <= kShipEMPartialRearmThreshold) {
            _shipEMTriggerRearmed = true;
        }

        // Raw R2 may also revoke a laser stream that was already authorized by
        // native Wwise evidence.
        if (wasPressed && !_rightTriggerPressed && _shipLaserActive) {
            _shipLaserActive = false;
            _shipLaserLeaseDeadline = {};
            restorePersistentTrigger();
        }
        return _state;
    }

    if (_landVehicleContextActive || _landVehicleProductionActive) {
        // Physical R2 is annotation only in land-vehicle context. Native
        // VehicleFireWeapon + Wwise correlation is the only gun authority.
        return _state;
    }

    if (_persistentContextSuppressed || !_weaponEquipped ||
        !onFootWeaponTriggersEnabled()) {
        return _state;
    }

    if (_weaponProfile && _weaponProfile->name == "Eon" &&
        _eonWallPhase == EonWallPhase::AwaitDeepTravel) {
        constexpr std::uint8_t kEonRecoilDepth = 160;

        if (now >= _eonPendingUntil) {
            _eonWallPhase = EonWallPhase::Normal;
            _eonShotPullSeen = false;
            _eonPendingUntil = {};
            restorePersistentTrigger();
        } else {
            if (_rightTriggerPressed) {
                _eonShotPullSeen = true;
            }

            if (value >= kEonRecoilDepth) {
                _state.output.rightTrigger = firePulseTrigger();
                _state.transientTriggerActive = true;
                _state.transientUntil = now + firePulseDuration();
                _eonWallPhase = EonWallPhase::AwaitCurrentPullRelease;
                _eonShotPullSeen = true;
                _eonPendingUntil = {};
                return _state;
            }

            // If the authorized shot pull is released before it ever reaches
            // deep travel, retire the marker so a later pull cannot inherit it.
            if (_eonShotPullSeen && wasPressed && !_rightTriggerPressed) {
                _eonWallPhase = EonWallPhase::Normal;
                _eonShotPullSeen = false;
                _eonPendingUntil = {};
                _state.transientTriggerActive = false;
                restorePersistentTrigger();
                return _state;
            }
        }
    }

    if (_weaponProfile && _weaponProfile->name == "Eon" &&
        _eonWallPhase == EonWallPhase::AwaitCurrentPullRelease) {
        if (_rightTriggerPressed) {
            _eonShotPullSeen = true;
        }
        if (_eonShotPullSeen && wasPressed && !_rightTriggerPressed) {
            _eonWallPhase = EonWallPhase::Normal;
            _eonShotPullSeen = false;
            _eonPendingUntil = {};
            _state.transientTriggerActive = false;
            restorePersistentTrigger();
        }
        return _state;
    }

    if (wasPressed && !_rightTriggerPressed && _sustainedFireActive) {
        _sustainedFireActive = false;
        _sustainedHeartbeatDeadline = {};
        _state.transientTriggerActive = false;
        restorePersistentTrigger();
        return _state;
    }

    if (!_state.transientTriggerActive &&
        equippedCadenceClass() == WeaponCadenceClass::Charge &&
        _rightTriggerPressed) {
        restorePersistentTrigger();
    }

    if (wasPressed && !_rightTriggerPressed && !_state.transientTriggerActive) {
        restorePersistentTrigger();
    }
    return _state;
}

sds::EffectState sds::EffectsEngine::tick(std::chrono::steady_clock::time_point now) noexcept
{
    if (_config.lightbar) {
        const auto emptyTime =
            std::chrono::steady_clock::time_point{};

        if (_shipTouchdownFlashUntil != emptyTime) {
            if (now < _shipTouchdownFlashUntil) {
                const auto elapsedMs =
                    std::max<std::int64_t>(
                        0,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - _shipTouchdownFlashStartedAt).count());

                constexpr Color white{ 255, 255, 255 };
                constexpr Color amber{ 255, 120, 8 };

                if (elapsedMs < 100) {
                    _state.output.lightbar = white;
                } else {
                    const auto step =
                        std::clamp<std::int64_t>(
                            (elapsedMs - 100) / 50,
                            0,
                            5);

                    _state.output.lightbar =
                        mixShipLightbarColor(
                            white,
                            amber,
                            static_cast<float>(step) / 5.0F);
                }
            } else {
                _shipTouchdownFlashStartedAt = {};
                _shipTouchdownFlashUntil = {};
            }
        }

        if (_shipTouchdownFlashUntil == emptyTime) {
            if (_shipLightbarPhase != ShipLightbarPhase::None) {
                const auto elapsedMs =
                    std::max<std::int64_t>(
                        0,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - _shipLightbarPhaseStartedAt).count());

                if (_shipLightbarPhase == ShipLightbarPhase::Takeoff) {
                    constexpr Color ignitionBase{ 0, 24, 150 };
                    constexpr Color ignitionPeak{ 190, 245, 255 };
                    constexpr Color engineLow{ 0, 40, 170 };
                    constexpr Color engineHigh{ 70, 225, 255 };

                    if (elapsedMs < 250) {
                        const auto step =
                            std::clamp<std::int64_t>(
                                elapsedMs / 50,
                                0,
                                5);

                        _state.output.lightbar =
                            mixShipLightbarColor(
                                ignitionBase,
                                ignitionPeak,
                                static_cast<float>(step) / 5.0F);
                    } else {
                        const auto step =
                            static_cast<int>(
                                ((elapsedMs - 250) / 50) % 20);

                        const float pulse =
                            step <= 10 ?
                                static_cast<float>(step) / 10.0F :
                                static_cast<float>(20 - step) / 10.0F;

                        _state.output.lightbar =
                            mixShipLightbarColor(
                                engineLow,
                                engineHigh,
                                pulse);
                    }
                } else {
                    constexpr Color landingLow{ 100, 20, 0 };
                    constexpr Color landingHigh{ 255, 155, 20 };

                    const auto step =
                        static_cast<int>(
                            (elapsedMs / 50) % 30);

                    const float pulse =
                        step <= 15 ?
                            static_cast<float>(step) / 15.0F :
                            static_cast<float>(30 - step) / 15.0F;

                    _state.output.lightbar =
                        mixShipLightbarColor(
                            landingLow,
                            landingHigh,
                            pulse);
                }
            } else if (_shipLightbarSeatActive) {
                _state.output.lightbar =
                    _shipHealthValid ?
                        shipHealthLightbarColor(_shipHealthRatio) :
                        Color{};
            } else if (_persistentContextSuppressed &&
                       !_landVehicleContextActive) {
                _state.output.lightbar = {};
            } else if (_playerHealthValid) {
                _state.output.lightbar =
                    playerHealthLightbarColor(_playerHealthRatio);
            }
        }
    }

    if (_feedbackGate.blocked()) {
        restorePersistentTrigger();
        return _state;
    }

    if (_shipEMTriggerRefreshPhase == ShipEMTriggerRefreshPhase::NeutralFrameQueued) {
        // Keep the trigger neutral for the remainder of this controller cycle.
        // ControllerManager applies outputs after tick(), so the next loop is
        // guaranteed to observe a real Off -> EffectEx transition.
        _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::RetriggerReady;
    } else if (_shipEMTriggerRefreshPhase == ShipEMTriggerRefreshPhase::RetriggerReady) {
        if (_shipPilotActive && !_feedbackGate.blocked() && _config.adaptiveTriggers) {
            _state.output.rightTrigger = shipEMFireTrigger();
            _state.transientTriggerActive = true;
            _state.transientUntil = now + std::chrono::milliseconds(90);
        }
        _shipEMTriggerRefreshPhase = ShipEMTriggerRefreshPhase::None;
    }

    if (_shipPilotActive && _shipLaserActive &&
        _shipLaserLeaseDeadline != std::chrono::steady_clock::time_point{} &&
        now >= _shipLaserLeaseDeadline) {
        _shipLaserActive = false;
        _shipLaserLeaseDeadline = {};
        restorePersistentTrigger();
    }

    if (_weaponProfile && _weaponProfile->name == "Eon" &&
        _eonWallPhase == EonWallPhase::AwaitDeepTravel && now >= _eonPendingUntil) {
        _eonWallPhase = EonWallPhase::Normal;
        _eonPendingUntil = {};
        restorePersistentTrigger();
    }

    if (_weaponProfile && _weaponProfile->name == "Eon" &&
        _eonWallPhase == EonWallPhase::AwaitCurrentPullRelease &&
        _state.transientTriggerActive && now >= _state.transientUntil) {
        // End Eon's final pistol pulse after 48 ms and put its normal
        // resistance wall back under the still-held trigger.
        _eonWallPhase = EonWallPhase::Normal;
        _eonShotPullSeen = false;
        _eonPendingUntil = {};
    }

    if (_sustainedFireActive &&
        _sustainedHeartbeatDeadline != std::chrono::steady_clock::time_point{} &&
        now >= _sustainedHeartbeatDeadline) {
        _sustainedFireActive = false;
        _sustainedHeartbeatDeadline = {};
        _state.transientTriggerActive = false;
        restorePersistentTrigger();
    }

    if (_landVehicleGunRecoilDeadline != std::chrono::steady_clock::time_point{} &&
        now >= _landVehicleGunRecoilDeadline) {
        _landVehicleGunRecoilDeadline = {};
        _state.transientTriggerActive = false;
        _state.transientUntil = {};
        restorePersistentTrigger();
    }

    if (_state.transientTriggerActive && !_sustainedFireActive && now >= _state.transientUntil) {
        _state.transientTriggerActive = false;
        restorePersistentTrigger();
    }
    return _state;
}
