#pragma once

#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/GameplayHapticsLiveSettings.h>
#include <StarfieldDualSense/HapticsEngine.h>
#include <StarfieldDualSense/IHapticsBackend.h>
#include <StarfieldDualSense/LandVehicleControllerFeel.h>
#include <StarfieldDualSense/LandVehicleMotionState.h>
#include <StarfieldDualSense/ShipPropulsionState.h>
#include <StarfieldDualSense/Types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

namespace sds
{
    class HapticsManager
    {
    public:
        using BackendFactory = std::function<std::unique_ptr<IHapticsBackend>()>;
        using LogCallback = std::function<void(std::string_view)>;

        HapticsManager(Config config, BackendFactory backendFactory, LogCallback log = {});
        ~HapticsManager();

        void start();
        void stop() noexcept;
        void applyLiveSettings(GameplayHapticsLiveSettings settings) noexcept;
        [[nodiscard]] bool handle(GameEvent event) noexcept;
        [[nodiscard]] bool handleRightTriggerInput(
            std::uint8_t r2,
            std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now()) noexcept;
        [[nodiscard]] bool handleShipPropulsionState(
            const ShipPropulsionState& state) noexcept;
        void handleLandVehicleMotionState(const LandVehicleMotionState& state) noexcept;
        [[nodiscard]] bool setLandVehicleBoostInput(
            bool vertical,
            bool active,
            std::chrono::steady_clock::time_point when =
                std::chrono::steady_clock::now()) noexcept;
        [[nodiscard]] bool setShipLaunchLandingRumble(
            bool active,
            std::string_view phase) noexcept;
        // Emitted only after an authored reload speaker stage is accepted.
        [[nodiscard]] bool emitWeaponReloadCue(
            std::string_view action,
            std::chrono::steady_clock::time_point when =
                std::chrono::steady_clock::now(),
            std::string_view borrowedReloadProfile = {},
            bool useCustomReloadProfile = false) noexcept;
        [[nodiscard]] bool emitDigipickWwiseEvent(
            std::uint32_t eventId,
            std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now()) noexcept;
        [[nodiscard]] bool emitBoostpackIgnition(
            std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now()) noexcept;
        [[nodiscard]] bool emitOnFootLanding(
            float baseGain,
            std::chrono::steady_clock::time_point when = std::chrono::steady_clock::now()) noexcept;
        [[nodiscard]] bool startBoostpackThrust() noexcept;
        [[nodiscard]] bool refreshBoostpackThrust() noexcept;
        [[nodiscard]] bool stopBoostpackThrust() noexcept;
        [[nodiscard]] bool tick(
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
        [[nodiscard]] bool active() const noexcept;

    private:
        void log(std::string_view message) const noexcept;
        void clearOnFootContextStateLocked() noexcept;
        [[nodiscard]] HapticContinuousState composeContinuousLocked() noexcept;
        [[nodiscard]] bool submitContinuousLocked() noexcept;
        [[nodiscard]] HapticContinuousState composeOnFootContinuousLocked() noexcept;
        [[nodiscard]] HapticContinuousState composeShipContinuousLocked() const noexcept;
        [[nodiscard]] HapticContinuousState composeLandVehicleContinuousLocked() const noexcept;
        void clearLandVehicleProductionLocked() noexcept;
        void clearShipLaserLocked() noexcept;

        Config _config{};
        std::atomic_bool _advancedHapticsEnabled{ true };
        std::atomic<float> _hapticStrength{ 1.0F };
        std::atomic_bool _boostpackHapticsEnabled{ true };
        std::atomic<float> _boostpackHapticsStrength{ 1.0F };
        BackendFactory _backendFactory{};
        LogCallback _log{};
        HapticsEngine _engine;
        mutable std::mutex _engineMutex{};
        std::unique_ptr<IHapticsBackend> _backend{};
        std::uint8_t _lastR2{ 0 };
        std::chrono::steady_clock::time_point _lastR2When{};
        bool _boostpackThrustActive{ false };
        bool _shipPilotActive{ false };
        bool _landVehicleContextActive{ false };
        bool _shipContextSuppressed{ false };
        std::uint8_t _shipBlockingMenuMask{ 0 };
        HapticContinuousState _shipPropulsionContinuous{};
        LandVehicleControllerFeel _landVehicleFeel{};
        LandVehicleMotionState _landVehicleMotion{};
        HapticContinuousState _landVehicleContinuous{};
        bool _landVehicleForwardBoostActive{ false };
        bool _landVehicleVerticalThrustActive{ false };
        std::chrono::steady_clock::time_point _landVehicleForwardBoostDeadline{};
        std::chrono::steady_clock::time_point _landVehicleVerticalThrustDeadline{};
        bool _landVehicleProductionActive{ false };
        std::uint64_t _landVehicleProductionEpoch{ 0 };
        std::chrono::steady_clock::time_point _nextLandVehicleDiagnosticLog{};
        bool _shipLaunchLandingRumbleActive{ false };
        bool _shipLaserActive{ false };
        std::chrono::steady_clock::time_point _shipLaserLeaseDeadline{};
        bool _cutterHeartbeatEligible{ false };
        bool _arcWelderFireEndEligible{ false };
        bool _cutterStartPending{ false };
        std::chrono::steady_clock::time_point _cutterStartWhen{};
        std::chrono::steady_clock::time_point _cutterStartDeadline{};
        bool _cutterHeartbeatArmed{ false };
        std::chrono::steady_clock::time_point _cutterHeartbeatDeadline{};
        std::atomic_bool _reloadPaused{ false };
        std::atomic<std::uint8_t> _reloadBlockingMenus{ 0u };
        bool _started{ false };
    };
}
