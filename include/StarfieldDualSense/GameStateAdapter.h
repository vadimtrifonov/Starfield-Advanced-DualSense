#pragma once

#include <StarfieldDualSense/Types.h>
#include <StarfieldDualSense/GameWorldGate.h>
#include <StarfieldDualSense/FireMarkerTrace.h>
#include <StarfieldDualSense/InputDiagnostics.h>
#include <StarfieldDualSense/IncomingDamageDiagnostic.h>
#include <StarfieldDualSense/LandVehicleDriverEventRecon.h>
#include <StarfieldDualSense/LandVehicleReconProbe.h>
#include <StarfieldDualSense/LandVehicleTelemetryProbe.h>
#include <StarfieldDualSense/LandVehiclePhysicsProbe.h>
#include <StarfieldDualSense/LandVehicleMotionState.h>
#include <StarfieldDualSense/NativeInputInjection.h>
#include <StarfieldDualSense/ShipPilotContext.h>
#include <StarfieldDualSense/ShipPropulsionProbe.h>
#include <StarfieldDualSense/ShipPropulsionState.h>

#include <RE/Starfield.h>

#include <StarfieldDualSense/AnimationGraphEvent.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

namespace sds
{
    struct LandVehicleDriverEnterExitRawEvent
    {
        std::byte opaque{};
    };

    struct ShipLandingReconStateObservation
    {
        ShipPropulsionState state{};
        std::chrono::steady_clock::time_point when{};
    };
    using BoostpackSemanticObserver = std::function<void(
        std::string_view semantic,
        bool active,
        std::chrono::steady_clock::time_point when)>;

    void setBoostpackSemanticObserver(BoostpackSemanticObserver observer);
    void setBoostpackSemanticObservationArmed(bool armed) noexcept;
    [[nodiscard]] bool boostpackSemanticObservationArmed() noexcept;

    enum class InputPresentationDevice : std::uint8_t
    {
        KeyboardMouse = 0,
        Gamepad = 1
    };

    using InputPresentationObserver =
        void (*)(InputPresentationDevice) noexcept;

    void setInputPresentationObserver(
        InputPresentationObserver observer) noexcept;

    class GameStateAdapter final :
        public RE::BSTEventSink<RE::ActorItemEquipped::Event>,
        public RE::BSTEventSink<RE::BSAnimationGraphEvent>,
        public RE::BSTEventSink<RE::TargetHitEvent>,
        public RE::BSTEventSink<RE::TESHitEvent>,
        public RE::BSTEventSink<LandVehicleDriverEnterExitRawEvent>,
        public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
        public RE::BSTEventSink<RE::BGSAppPausedEvent>
    {
    public:
        using EmitCallback = std::function<bool(GameEvent)>;
        using LogCallback = std::function<void(std::string_view)>;
        using LandVehicleMotionCallback = std::function<void(const LandVehicleMotionState&)>;
        using LandVehicleSemanticCallback = std::function<void(
            std::string_view semantic,
            bool active,
            std::chrono::steady_clock::time_point when)>;

        explicit GameStateAdapter(
            EmitCallback emit,
            LogCallback log = {},
            bool fireMarkerTraceEnabled = false,
            bool incomingDamageDiagnosticEnabled = false);
        ~GameStateAdapter() override;

        GameStateAdapter(const GameStateAdapter&) = delete;
        GameStateAdapter& operator=(const GameStateAdapter&) = delete;

        bool registerSinks();
        void unregisterSinks() noexcept;
        [[nodiscard]] bool worldAvailable() const noexcept { return _worldGate.available(); }
        void pollHealth();
        [[nodiscard]] bool refreshPlayerHealth();
        void pollShipLightbarSeatAuthority();
        [[nodiscard]] std::optional<ShipPropulsionState> pollShipPropulsionState();
        [[nodiscard]] std::optional<ShipPropulsionState> pollShipLandingReconState();
        [[nodiscard]] std::optional<ShipLandingReconStateObservation> pollShipLandingReconStatePrecision();
        [[nodiscard]] std::optional<LandVehicleReconResult> pollLandVehicleReconState();
        [[nodiscard]] LandVehicleTelemetryResult latestLandVehicleTelemetrySnapshot() noexcept;
        [[nodiscard]] LandVehiclePhysicsResult latestLandVehiclePhysicsSnapshot() noexcept;
        [[nodiscard]] bool armLandVehicleVerticalBoost() noexcept;
        void setLandVehicleMotionCallback(LandVehicleMotionCallback callback);
        void setLandVehicleSemanticCallback(LandVehicleSemanticCallback callback);
        [[nodiscard]] bool landVehicleReconCorrelationArmed() const noexcept { return _landVehicleCorrelationArmed.load(std::memory_order_acquire); }
        [[nodiscard]] bool mainMenuOpen() const noexcept { return _worldGate.mainMenuOpen(); }
        [[nodiscard]] bool queueNativeInputAction(InputAction action) noexcept;
        void pollNativeInputInjection() noexcept;
        void dispatchBluetoothPhysicalInput(
            const TouchState& state,
            float deltaSeconds,
            void* gamepadDevice = nullptr) noexcept;
        void dispatchBluetoothScannerSticksAtNativePoll(
            const TouchState& state,
            float deltaSeconds,
            void* gamepadDevice) noexcept;
        void resetBluetoothPhysicalInput() noexcept;

        RE::BSEventNotifyControl ProcessEvent(
            const RE::ActorItemEquipped::Event& event,
            RE::BSTEventSource<RE::ActorItemEquipped::Event>* source) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::BSAnimationGraphEvent& event,
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* source) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TargetHitEvent& event,
            RE::BSTEventSource<RE::TargetHitEvent>* source) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESHitEvent& event,
            RE::BSTEventSource<RE::TESHitEvent>* source) override;
        RE::BSEventNotifyControl ProcessEvent(
            const LandVehicleDriverEnterExitRawEvent& event,
            RE::BSTEventSource<LandVehicleDriverEnterExitRawEvent>* source) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>* source) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::BGSAppPausedEvent& event,
            RE::BSTEventSource<RE::BGSAppPausedEvent>* source) override;

    private:
        bool installSemanticBroadcasterDiagnosticHook();
        bool installShipFlightControlDiagnosticHook();
        bool registerLandVehicleDriverEventReconSink();
        void unregisterLandVehicleDriverEventReconSink() noexcept;
        bool installNativeEnqueueOriginHook();
        bool installButtonEventConstructorOriginHook();
        static void semanticBroadcasterDiagnosticThunk(void* source, void* event);
        static std::uintptr_t nativeEnqueueOriginThunk(
            void* manager, void* event, std::uintptr_t arg3, std::uintptr_t arg4);
        static void* buttonEventConstructorOriginThunk(void* eventObject);
        void observeSemanticButton(void* source, void* event, std::uintptr_t callerReturnAddress) const noexcept;
        [[nodiscard]] bool dispatchNativeInputFrame(const NativeInputEmission& emission) noexcept;
        void refreshFireMarkerSinks() noexcept;
        void clearFireMarkerSinks() noexcept;
        [[nodiscard]] bool ensureTargetHitSinkRegistered(RE::PlayerCharacter* player) noexcept;
        void unregisterTargetHitSink() noexcept;
        void armTargetHitDiagnostic(std::string_view weapon, std::chrono::steady_clock::time_point now) noexcept;
        void disarmTargetHitDiagnostic() noexcept;
        [[nodiscard]] bool ensureTESHitSinkRegistered(RE::PlayerCharacter* player, std::string_view weapon) noexcept;
        void unregisterTESHitSink() noexcept;
        void armTESHitDiagnostic(
            std::string_view weapon,
            std::uint32_t weaponFormId,
            std::chrono::steady_clock::time_point now) noexcept;
        void disarmTESHitDiagnostic() noexcept;
        void runTESHitSourceDiscovery(
            RE::PlayerCharacter* player,
            std::string_view weapon,
            std::uint32_t weaponFormId) noexcept;
        [[nodiscard]] std::optional<float> readPlayerHealthRatio(RE::PlayerCharacter* player) const noexcept;
        void drainIncomingDamageCorrelations(std::chrono::steady_clock::time_point now) noexcept;
        void clearLandVehicleProductionState(std::chrono::steady_clock::time_point when, bool emitRelease) noexcept;
        bool emit(GameEvent event, std::string_view diagnostic = {});
        void log(std::string_view message) const noexcept;

        EmitCallback _emit{};
        LogCallback _log{};
        bool _fireMarkerTraceEnabled{ false };
        bool _incomingDamageDiagnosticEnabled{ false };
        bool _tesHitSessionDiscoveryAttempted{ false };
        bool _registered{ false };
        GameWorldGate _worldGate{};
        static constexpr std::size_t kMaxFireMarkerSources = 8;
        std::mutex _fireMarkerGraphMutex{};
        std::array<RE::BSTSmartPointer<RE::BSAnimationGraph>, kMaxFireMarkerSources> _fireMarkerGraphs{};
        std::size_t _fireMarkerSourceCount{ 0 };
        FireMarkerCaptureState _fireMarkerCapture{};
        std::mutex _targetHitMutex{};
        RE::BSTEventSource<RE::TargetHitEvent>* _targetHitSource{ nullptr };
        bool _targetHitSinkRegistered{ false };
        std::uint32_t _targetHitSinkCountBeforeRegistration{ 0 };
        std::uint32_t _targetHitSinkIndex{ 0 };
        bool _targetHitDiagnosticActive{ false };
        std::uint64_t _targetHitSequence{ 0 };
        std::array<char, 121> _targetHitWeapon{};
        std::chrono::steady_clock::time_point _targetHitArmedAt{};
        std::mutex _tesHitMutex{};
        RE::BSTEventSource<RE::TESHitEvent>* _tesHitSource{ nullptr };
        bool _tesHitSinkRegistered{ false };
        std::uint32_t _tesHitSinkCountBeforeRegistration{ 0 };
        std::uint32_t _tesHitSinkIndex{ 0 };
        bool _tesHitDiagnosticActive{ false };
        std::uint64_t _tesHitSequence{ 0 };
        std::array<char, 121> _tesHitWeapon{};
        std::uint32_t _tesHitWeaponFormId{ 0 };
        std::chrono::steady_clock::time_point _tesHitArmedAt{};
        std::mutex _incomingDamageMutex{};
        IncomingDamageDiagnostic _incomingDamageDiagnostic{};
        IncomingDamageNullTargetFallback _incomingDamageNullTargetFallback{};
        std::uint64_t _incomingDamageSequence{ 0 };
        bool _lastPeriodicHealthAvailable{ false };
        float _lastPeriodicHealthRatio{ 0.0F };
        std::chrono::steady_clock::time_point _lastPeriodicHealthSampleAt{};
        std::uint64_t _lastIncomingDamageDropCountLogged{ 0 };
        float _lastShipHealthRatio{ -1.0F };
        std::uint32_t _lastShipHealthFormId{ 0 };
        ShipPilotContext _shipPilotContext{};
        LandVehicleReconProbe _landVehicleReconProbe{};
        LandVehicleTelemetryProbe _landVehicleTelemetryProbe{};
        LandVehicleTelemetryResult _landVehicleLatestTelemetry{};
        LandVehiclePhysicsProbe _landVehiclePhysicsProbe{};
        LandVehiclePhysicsResult _landVehicleLatestPhysics{};
        bool _landVehicleProductionAuthorityActive{ false };
        std::uint64_t _landVehicleProductionEpoch{ 0 };
        LandVehicleMotionCallback _landVehicleMotionCallback{};
        LandVehicleSemanticCallback _landVehicleSemanticCallback{};
        std::mutex _landVehicleReconMutex{};
        RE::BSTEventSource<LandVehicleDriverEnterExitRawEvent>* _landVehicleDriverEventSource{ nullptr };
        bool _landVehicleDriverEventSinkRegistered{ false };
        LandVehicleDriverEventSnapshot _landVehicleLastDriverEvent{};
        std::uint64_t _landVehicleDriverEventSequence{ 0 };
        std::uint64_t _landVehicleLastPolledDriverEventSequence{ 0 };
        bool _landVehicleLoading{ false };
        bool _lastLandVehicleCameraCorroboration{ false };
        bool _landVehicleSuppressionBoundaryActive{ false };
        std::chrono::steady_clock::time_point _landVehicleLastDriverEventAt{};
        std::chrono::steady_clock::time_point _nextLandVehicleReconPoll{};
        std::chrono::steady_clock::time_point _nextLandVehicleSummaryLog{};
        std::atomic_bool _landVehicleCorrelationArmed{ false };
        ShipPropulsionProbe _shipPropulsionProbe{};
        std::chrono::steady_clock::time_point _nextShipPropulsionPoll{};
        std::chrono::steady_clock::time_point _nextShipLightbarSeatPoll{};
        bool _shipLightbarSeatKnown{ false };
        bool _shipLightbarSeatActive{ false };
        bool _hudMenuOpen{ false };
        float _lastHealthRatio{ -1.0F };
        std::chrono::steady_clock::time_point _nextHealthPoll{};
        NativeInputSequencer _nativeInputSequencer{};
        bool _pendingPhotoMode{ false };
        bool _monocleOpen{ false };
        std::chrono::steady_clock::time_point _photoModeDeadline{};
    };
}
