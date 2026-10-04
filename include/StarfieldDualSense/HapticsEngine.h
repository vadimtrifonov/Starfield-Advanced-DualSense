#pragma once

#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/HapticTypes.h>
#include <StarfieldDualSense/GameplayFeedbackGate.h>
#include <StarfieldDualSense/Types.h>
#include <StarfieldDualSense/WeaponProfiles.h>

#include <optional>
#include <string_view>

namespace sds
{
    class HapticsEngine
    {
    public:
        explicit HapticsEngine(float hapticStrength = 1.0F) noexcept;
        void setHapticStrength(float strength) noexcept;
        [[nodiscard]] bool setWeaponHapticsConfig(const Config& config) noexcept;
        // Read under HapticsManager's existing _engineMutex.
        [[nodiscard]] float reloadHapticsFamilyScale(
            std::string_view borrowedProfileIdentity = {},
            bool useCustomReloadProfile = false) const noexcept;
        [[nodiscard]] std::optional<HapticCommand> handle(const GameEvent& event) noexcept;
        [[nodiscard]] HapticContinuousState handleRightTriggerInput(std::uint8_t r2) noexcept;

        [[nodiscard]] bool gameplayBlocked() const noexcept { return _feedbackGate.blocked(); }
        [[nodiscard]] bool shipTransitionBlocked() const noexcept { return _feedbackGate.shipTransitionBlocked(); }

    private:
        void clearWeaponFeedback() noexcept;
        GameplayFeedbackGate _feedbackGate;
        float _hapticStrength{ 1.0F };
        Config _weaponHapticsConfig{};
        const WeaponProfile* _equipped{ nullptr };
        bool _customWeaponEquipped{ false };
        std::uint32_t _equippedFormId{ 0 };
        bool _cutterBeamAuthorized{ false };
        bool _arcWelderAuthorized{ false };
        bool _penumbraStressActive{ false };
        bool _magsniperChargeActive{ false };
        bool _novablastChargeAuthorized{ false };
        bool _autoRivetChargeAuthorized{ false };
    };
}
