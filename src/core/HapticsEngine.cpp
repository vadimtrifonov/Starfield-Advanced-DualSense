#include <StarfieldDualSense/HapticsEngine.h>
#include <StarfieldDualSense/WeaponReloadHapticSettings.h>
#include <StarfieldDualSense/HapticStrength.h>
#include <StarfieldDualSense/MeleeHaptics.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace
{

    std::string_view eventText(const sds::GameEvent& event) noexcept
    {
        std::size_t length = 0;
        while (length < event.text.size() && event.text[length] != '\0') {
            ++length;
        }
        return { event.text.data(), length };
    }

    float incomingDamagePerceptualGain(float severity) noexcept
    {
        const float clamped = std::clamp(severity, 0.0F, 1.0F);
        if (clamped <= 0.02F) {
            return 0.45F + 5.0F * clamped;
        }
        if (clamped <= 0.10F) {
            return 0.55F + 1.625F * (clamped - 0.02F);
        }
        if (clamped < 0.23F) {
            constexpr float kSlope = 0.236F / 0.13F;
            return 0.68F + kSlope * (clamped - 0.10F);
        }
        return std::clamp(0.18F + 3.2F * clamped, 0.0F, 1.0F);
    }
}

sds::HapticsEngine::HapticsEngine(float hapticStrength) noexcept :
    _hapticStrength(sds::effectiveHapticStrength(hapticStrength))
{}

void sds::HapticsEngine::setHapticStrength(float strength) noexcept
{
    _hapticStrength = sds::effectiveHapticStrength(strength);
}

bool sds::HapticsEngine::setWeaponHapticsConfig(const Config& config) noexcept
{
    const bool changed =
        !sds::weaponHapticsSettingsEqual(_weaponHapticsConfig, config);

    _weaponHapticsConfig = config;

    if (_equipped &&
        ((_customWeaponEquipped && !_weaponHapticsConfig.customWeaponsEnabled) ||
         !sds::weaponHapticsFamilyEnabled(
             _weaponHapticsConfig,
             _equipped->triggerFamily))) {
        clearWeaponFeedback();
    }

    return changed;
}
float sds::HapticsEngine::reloadHapticsFamilyScale(
    std::string_view borrowedProfileIdentity,
    bool useCustomReloadProfile) const noexcept
{
    // Native/vanilla reloads use the currently equipped firing profile.
    // A mapped custom reload instead uses the selected ReloadHapticsProfile.
    // Its family must not be inherited from ControllerFeedbackProfile.
    const WeaponProfile* borrowedProfile = nullptr;
    if (useCustomReloadProfile && !borrowedProfileIdentity.empty()) {
        borrowedProfile = findWeaponProfile(borrowedProfileIdentity);
    }
    return sds::weaponReloadHapticsFamilyScale(
        _weaponHapticsConfig,
        _equipped,
        _customWeaponEquipped,
        borrowedProfile,
        useCustomReloadProfile);
}

void sds::HapticsEngine::clearWeaponFeedback() noexcept
{
    _cutterBeamAuthorized = false;
    _arcWelderAuthorized = false;
    _penumbraStressActive = false;
    _magsniperChargeActive = false;
    _novablastChargeAuthorized = false;
    _autoRivetChargeAuthorized = false;
}

std::optional<sds::HapticCommand> sds::HapticsEngine::handle(const GameEvent& event) noexcept
{
    if (_feedbackGate.handle(event)) {
        if (_feedbackGate.blocked()) clearWeaponFeedback();
        return std::nullopt;
    }
    if (event.type == GameEventType::WeaponEquipped) {
        clearWeaponFeedback();
        _customWeaponEquipped = isCustomWeaponProfileMatch(eventText(event));
        _equipped = findWeaponProfile(eventText(event));
        _equippedFormId = event.formId;
        return std::nullopt;
    }
    if (event.type == GameEventType::Shutdown) {
        clearWeaponFeedback();
        _equipped = nullptr;
        _customWeaponEquipped = false;
        _equippedFormId = 0;
        _feedbackGate = {};
        return std::nullopt;
    }
    if (_feedbackGate.blocked()) return std::nullopt;

    if (event.type == GameEventType::IncomingDamage) {
        if (!std::isfinite(event.value) || event.value <= 0.0F) {
            return std::nullopt;
        }
        const float severity = std::clamp(event.value, 0.0F, 1.0F);
        const float severityGain = incomingDamagePerceptualGain(severity);
        return HapticCommand{
            .kind = HapticEffectKind::IncomingDamageImpact,
            .gain = sds::clampHapticGain(severityGain * _hapticStrength),
            .when = event.when,
        };
    }
    if (!_equipped ||
        (_customWeaponEquipped && !_weaponHapticsConfig.customWeaponsEnabled)) {
        return std::nullopt;
    }

    if (!sds::weaponHapticsFamilyEnabled(
            _weaponHapticsConfig,
            _equipped->triggerFamily)) {
        return std::nullopt;
    }

    const float weaponStrength =
        sds::weaponHapticsFamilyStrength(
            _weaponHapticsConfig,
            _equipped->triggerFamily);

    if (event.type == GameEventType::MeleeSwing ||
        event.type == GameEventType::MeleeImpact) {
        if (_equipped->triggerFamily != WeaponTriggerFamily::Melee) {
            return std::nullopt;
        }
        const auto tier = meleeHapticTierForWeapon(_equipped->name);
        if (tier == MeleeHapticTier::None) {
            return std::nullopt;
        }
        if (event.type == GameEventType::MeleeImpact &&
            (_equippedFormId == 0 || event.formId != _equippedFormId)) {
            return std::nullopt;
        }

        HapticEffectKind meleeKind{};
        if (event.type == GameEventType::MeleeSwing) {
            meleeKind = tier == MeleeHapticTier::Light ? HapticEffectKind::MeleeLightSwing :
                tier == MeleeHapticTier::Heavy ? HapticEffectKind::MeleeHeavySwing :
                HapticEffectKind::MeleeVeryHeavySwing;
        } else {
            meleeKind = tier == MeleeHapticTier::Light ? HapticEffectKind::MeleeLightImpact :
                tier == MeleeHapticTier::Heavy ? HapticEffectKind::MeleeHeavyImpact :
                HapticEffectKind::MeleeVeryHeavyImpact;
        }

        const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
        return HapticCommand{
            .kind = meleeKind,
            .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
            .when = event.when,
        };
    }

    if (event.type != GameEventType::WeaponFired) {
        return std::nullopt;
    }

    if (_equipped->name == "Auto-Rivet") {
        const auto marker = eventText(event);
        if (marker == "AutoRivetChargeStart") {
            _autoRivetChargeAuthorized = true;
            return std::nullopt;
        }
        if (marker == "AutoRivetChargeStop") {
            _autoRivetChargeAuthorized = false;
            return std::nullopt;
        }
        if (marker != "WeaponFire") {
            return std::nullopt;
        }
        _autoRivetChargeAuthorized = false;
    }

    if (_equipped->name == "Novablast Disruptor") {
        const auto marker = eventText(event);
        if (marker == "NovablastChargeStart") {
            _novablastChargeAuthorized = true;
            return std::nullopt;
        }
        if (marker == "NovablastChargeStop") {
            _novablastChargeAuthorized = false;
            return std::nullopt;
        }
        if (marker != "WeaponFire") {
            return std::nullopt;
        }
        _novablastChargeAuthorized = false;
    }

    if (_equipped->name == "Cutter") {
        if (eventText(event) == "weaponFireStart") {
            _cutterBeamAuthorized = true;
        }
        return std::nullopt;
    }

    if (_equipped->name == "Arc Welder") {
        if (eventText(event) == "weaponFireStart") {
            _arcWelderAuthorized = true;
        } else if (eventText(event) == "weaponFireEnd") {
            _arcWelderAuthorized = false;
        }
        return std::nullopt;
    }

    HapticEffectKind kind{};
    if (_equipped->name == "Eon") {
        kind = HapticEffectKind::EonSnap;
    } else if (_equipped->name == "Bridger") {
        kind = HapticEffectKind::BridgerConcussion;
    } else if (_equipped->name == "Microgun") {
        kind = HapticEffectKind::MicrogunKick;
    } else if (_equipped->name == "Novablast Disruptor") {
        kind = HapticEffectKind::NovablastDischarge;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::BallisticHandgun) {
        kind = HapticEffectKind::BallisticHandgunKick;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::BallisticRapid) {
        kind = HapticEffectKind::BallisticRapidKick;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::BallisticRifle) {
        kind = HapticEffectKind::BallisticRifleKick;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::PrecisionBallistic ||
               _equipped->triggerFamily == WeaponTriggerFamily::HeavyBallistic) {
        kind = HapticEffectKind::PrecisionBallisticKick;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::Launcher) {
        kind = _equipped->familyLabel.starts_with("Particle") ?
            HapticEffectKind::ParticleLauncherConcussion :
            HapticEffectKind::LauncherConcussion;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::Magnetic) {
        if (_equipped->cadenceClass == WeaponCadenceClass::Charge) {
            kind = HapticEffectKind::MagneticPrecision;
        } else if (_equipped->cadenceClass == WeaponCadenceClass::Rapid ||
                   _equipped->cadenceClass == WeaponCadenceClass::Sustained) {
            kind = HapticEffectKind::MagneticRapid;
        } else {
            kind = HapticEffectKind::MagneticPulse;
        }
    } else if (_equipped->familyLabel.starts_with("Particle")) {
        kind = HapticEffectKind::ParticlePulse;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::Shotgun) {
        kind = HapticEffectKind::ShotgunBlast;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::Laser) {
        kind = HapticEffectKind::LaserPulse;
    } else if (_equipped->triggerFamily == WeaponTriggerFamily::Particle) {
        kind = HapticEffectKind::ParticlePulse;
    } else {
        return std::nullopt;
    }

    const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
    return HapticCommand{
        .kind = kind,
        .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
        .when = event.when,
    };
}


sds::HapticContinuousState sds::HapticsEngine::handleRightTriggerInput(std::uint8_t r2) noexcept
{
    if (_feedbackGate.blocked()) {
        return {};
    }

    if (!_equipped ||
        (_customWeaponEquipped && !_weaponHapticsConfig.customWeaponsEnabled)) {
        return {};
    }

    if (!sds::weaponHapticsFamilyEnabled(
            _weaponHapticsConfig,
            _equipped->triggerFamily)) {
        clearWeaponFeedback();
        return {};
    }

    const float weaponStrength =
        sds::weaponHapticsFamilyStrength(
            _weaponHapticsConfig,
            _equipped->triggerFamily);

    if (_equipped && _equipped->name == "Auto-Rivet") {
        if (!_autoRivetChargeAuthorized || r2 < 24) {
            return {};
        }

        const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
        return {
            .kind = HapticContinuousKind::AutoRivetTension,
            .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
            .level = std::clamp(
                (static_cast<float>(r2) - 24.0F) / 231.0F,
                0.0F,
                1.0F),
        };
    }

    if (_equipped && _equipped->name == "Va'ruun Penumbra") {
        if (r2 <= 12) {
            _penumbraStressActive = false;
            return {};
        }
        if (!_penumbraStressActive && r2 >= 24) {
            _penumbraStressActive = true;
        }
        if (!_penumbraStressActive) {
            return {};
        }

        const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
        return {
            .kind = HapticContinuousKind::PenumbraStress,
            .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
            .level = 1.0F,
        };
    }

    _penumbraStressActive = false;

    if (_equipped && _equipped->name == "Magsniper") {
        if (r2 <= 12) {
            _magsniperChargeActive = false;
            return {};
        }
        if (!_magsniperChargeActive && r2 >= 24) {
            _magsniperChargeActive = true;
        }
        if (!_magsniperChargeActive) {
            return {};
        }

        const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
        return {
            .kind = HapticContinuousKind::MagsniperCharge,
            .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
            .level = 1.0F,
        };
    }

    _magsniperChargeActive = false;

    if (_equipped && _equipped->name == "Cutter") {
        if (r2 <= 12) {
            _cutterBeamAuthorized = false;
            return {};
        }
        if (!_cutterBeamAuthorized) {
            return {};
        }

        const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
        return {
            .kind = HapticContinuousKind::CutterBeam,
            .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
            .level = std::clamp(
                (static_cast<float>(r2) - 24.0F) / 231.0F,
                0.0F,
                1.0F),
        };
    }

    if (_equipped && _equipped->name == "Arc Welder") {
        if (r2 <= 12) {
            _arcWelderAuthorized = false;
            return {};
        }
        if (!_arcWelderAuthorized) {
            return {};
        }

        const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
        return {
            .kind = HapticContinuousKind::ArcWelderArc,
            .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
            .level = 1.0F,
        };
    }

    if (!_equipped || _equipped->name != "Novablast Disruptor" ||
        !_novablastChargeAuthorized || r2 < 24) {
        return {};
    }

    const float rating = static_cast<float>(_equipped->hapticRating) / 10.0F;
    return {
        .kind = HapticContinuousKind::NovablastCharge,
        .gain = sds::clampHapticGain(rating * _hapticStrength * weaponStrength),
        .level = std::clamp(
            (static_cast<float>(r2) - 24.0F) / 231.0F,
            0.0F,
            1.0F),
    };
}
