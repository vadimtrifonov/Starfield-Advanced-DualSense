#pragma once

#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/ControllerLiveSettings.h>
#include <StarfieldDualSense/Types.h>
#include <StarfieldDualSense/GameplayFeedbackGate.h>
#include <StarfieldDualSense/WeaponProfiles.h>

#include <chrono>

namespace sds
{
    class EffectsEngine
    {
    public:
        explicit EffectsEngine(Config config = Config::defaults()) noexcept;
        [[nodiscard]] bool applyLiveSettings(const ControllerLiveSettings& settings) noexcept;

        [[nodiscard]] EffectState handle(const GameEvent& event) noexcept;
        [[nodiscard]] EffectState handleRightTriggerInput(
            std::uint8_t value,
            std::chrono::steady_clock::time_point now) noexcept;
        [[nodiscard]] EffectState tick(std::chrono::steady_clock::time_point now) noexcept;
        [[nodiscard]] const EffectState& state() const noexcept { return _state; }
        [[nodiscard]] const WeaponProfile* equippedWeaponProfile() const noexcept { return _weaponProfile; }
        [[nodiscard]] std::uint8_t rightTriggerInput() const noexcept { return _rightTriggerInput; }
        [[nodiscard]] bool rightTriggerPressed() const noexcept { return _rightTriggerPressed; }
        [[nodiscard]] bool sustainedFireActive() const noexcept { return _sustainedFireActive; }

    private:
        enum class EonWallPhase : std::uint8_t
        {
            Normal,
            AwaitDeepTravel,
            AwaitCurrentPullRelease
        };

        enum class ShipEMTriggerRefreshPhase : std::uint8_t
        {
            None,
            NeutralFrameQueued,
            RetriggerReady
        };

        [[nodiscard]] bool onFootWeaponTriggersEnabled() const noexcept;
        [[nodiscard]] std::uint8_t equippedR2Rating() const noexcept;
        [[nodiscard]] WeaponTriggerFamily equippedTriggerFamily() const noexcept;
        [[nodiscard]] WeaponCadenceClass equippedCadenceClass() const noexcept;
        [[nodiscard]] TriggerEffect equippedWeaponTrigger() const noexcept;
        [[nodiscard]] TriggerEffect chargePullTrigger() const noexcept;
        [[nodiscard]] TriggerEffect firePulseTrigger() const noexcept;
        [[nodiscard]] TriggerEffect sustainedFireTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipPrimaryFireWallTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipBallisticWallTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipBallisticFireTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipLaserFireTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipParticleFireTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipMissileFireTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipEMFireTrigger() const noexcept;
        [[nodiscard]] TriggerEffect shipLaunchLandingTrigger() const noexcept;
        [[nodiscard]] TriggerEffect landVehiclePrimaryFireWallTrigger() const noexcept;
        [[nodiscard]] TriggerEffect landVehicleGunFireTrigger() const noexcept;
        [[nodiscard]] TriggerEffect landVehicleAimTrigger() const noexcept;
        [[nodiscard]] std::chrono::milliseconds firePulseDuration() const noexcept;
        void restorePersistentTrigger() noexcept;
        void clearTransientTriggerState() noexcept;
        void clearOnFootPersistentState() noexcept;
        void clearLandVehicleProductionState() noexcept;

        enum class ShipLightbarPhase : std::uint8_t
        {
            None,
            Takeoff,
            Landing
        };

        GameplayFeedbackGate _feedbackGate;
        Config _config{};
        EffectState _state{};
        bool _weaponEquipped{ false };
        bool _customWeaponEquipped{ false };
        bool _shipPilotActive{ false };
        bool _shipLightbarSeatActive{ false };
        bool _playerHealthValid{ false };
        float _playerHealthRatio{ 1.0F };
        bool _shipHealthValid{ false };
        float _shipHealthRatio{ 1.0F };
        ShipLightbarPhase _shipLightbarPhase{ ShipLightbarPhase::None };
        std::chrono::steady_clock::time_point _shipLightbarPhaseStartedAt{};
        std::chrono::steady_clock::time_point _shipTouchdownFlashStartedAt{};
        std::chrono::steady_clock::time_point _shipTouchdownFlashUntil{};
        bool _landVehicleContextActive{ false };
        bool _landVehicleProductionActive{ false };
        bool _landVehicleAimActive{ false };
        std::uint64_t _landVehicleProductionEpoch{ 0 };
        std::chrono::steady_clock::time_point _landVehicleGunRecoilDeadline{};
        bool _persistentContextSuppressed{ false };
        bool _shipR2BallisticConfirmed{ false };
        bool _shipLaserActive{ false };
        std::chrono::steady_clock::time_point _shipLaserLeaseDeadline{};
        bool _shipEMTriggerRearmed{ true };
        bool _shipLaunchLandingHapticsActive{ false };
        ShipEMTriggerRefreshPhase _shipEMTriggerRefreshPhase{ ShipEMTriggerRefreshPhase::None };
        const WeaponProfile* _weaponProfile{ nullptr };
        std::uint8_t _rightTriggerInput{ 0 };
        bool _rightTriggerPressed{ false };
        bool _sustainedFireActive{ false };
        std::chrono::steady_clock::time_point _sustainedHeartbeatDeadline{};
        EonWallPhase _eonWallPhase{ EonWallPhase::Normal };
        bool _eonShotPullSeen{ false };
        std::chrono::steady_clock::time_point _eonPendingUntil{};
    };
}
