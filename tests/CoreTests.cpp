#include <StarfieldDualSense/Types.h>
#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/DeviceClassifier.h>
#include <StarfieldDualSense/EventQueue.h>
#include <StarfieldDualSense/EffectsEngine.h>
#include <StarfieldDualSense/InputDiagnostics.h>
#include <StarfieldDualSense/FireMarkerTrace.h>
#include <StarfieldDualSense/MeleeEventDiagnostic.h>
#include <StarfieldDualSense/MeleeHaptics.h>
#include <StarfieldDualSense/NativeInputInjection.h>
#include <StarfieldDualSense/SemanticInputInjection.h>
#include <StarfieldDualSense/Touchpad.h>
#include <StarfieldDualSense/TESHitSourceDiscovery.h>
#include <StarfieldDualSense/WeaponProfiles.h>
#include <StarfieldDualSense/DualSenseReports.h>
#include <StarfieldDualSense/ControllerManager.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <iostream>
#include <string_view>

namespace
{
    int failures = 0;

    void expect(bool condition, std::string_view name)
    {
        if (condition) {
            std::cout << "PASS " << name << '\n';
        } else {
            std::cerr << "FAIL " << name << '\n';
            ++failures;
        }
    }

    bool near(float lhs, float rhs)
    {
        return std::fabs(lhs - rhs) < 0.0001F;
    }

    template <class T>
    concept HasCreateButtonState = requires(T value) { value.create; };

    template <class T>
    concept HasTouchActionMapper = requires(T value) { mapTouchGestureToInputAction(value); };

    template <class T>
    concept HasInputActionQueue = requires(T& value) { value.tryPopInputAction(); };

    template <class T>
    concept HasNativeUserEventMapper = requires(T value) { nativeUserEventForInputAction(value); };

    template <class T>
    concept HasR2InputState = requires(T value) { value.r2; value.r2Button; };

    template <class T>
    concept HasLiveR2Handler = requires(T& value, std::uint8_t input, std::chrono::steady_clock::time_point now) {
        value.handleRightTriggerInput(input, now);
    };


    struct FakeBackendState
    {
        std::atomic<int> connectAttempts{ 0 };
        std::atomic<int> disconnectCalls{ 0 };
        std::atomic<int> lightbarCalls{ 0 };
        std::atomic<int> triggerCalls{ 0 };
        std::atomic<int> resetCalls{ 0 };
        std::atomic<int> pollTouchCalls{ 0 };
        std::atomic<int> failuresBeforeConnect{ 0 };
        std::atomic<bool> isConnected{ false };
        sds::Capabilities caps{};
        std::mutex touchMutex{};
        std::deque<sds::TouchState> touchSamples{};
        std::mutex outputMutex{};
        sds::TriggerEffect lastLeftTrigger{};
        sds::TriggerEffect lastRightTrigger{};
    };

    class FakeBackend final : public sds::IControllerBackend
    {
    public:
        explicit FakeBackend(std::shared_ptr<FakeBackendState> state) : _state(std::move(state)) {}

        bool connect() override
        {
            const int attempt = ++_state->connectAttempts;
            if (attempt <= _state->failuresBeforeConnect.load()) {
                _state->isConnected = false;
                return false;
            }
            _state->isConnected = true;
            return true;
        }

        void disconnect() noexcept override
        {
            ++_state->disconnectCalls;
            _state->isConnected = false;
        }

        bool connected() const noexcept override { return _state->isConnected.load(); }

        sds::DeviceIdentity identity() const override
        {
            auto id = sds::classifyDevice(0x054C, 0x0CE6, 64);
            return id;
        }

        sds::Capabilities capabilities() const noexcept override { return _state->caps; }
        std::optional<sds::TouchState> pollTouch() override
        {
            ++_state->pollTouchCalls;
            std::scoped_lock lock(_state->touchMutex);
            if (_state->touchSamples.empty()) {
                return std::nullopt;
            }
            auto sample = _state->touchSamples.front();
            _state->touchSamples.pop_front();
            return sample;
        }

        bool setLightbar(sds::Color) override
        {
            ++_state->lightbarCalls;
            return connected();
        }

        bool setTriggers(const sds::TriggerEffect& left, const sds::TriggerEffect& right) override
        {
            ++_state->triggerCalls;
            {
                std::scoped_lock lock(_state->outputMutex);
                _state->lastLeftTrigger = left;
                _state->lastRightTrigger = right;
            }
            return connected();
        }

        void resetOutputs() noexcept override { ++_state->resetCalls; }

    private:
        std::shared_ptr<FakeBackendState> _state;
    };

    void pushTouch(const std::shared_ptr<FakeBackendState>& state, sds::TouchState sample)
    {
        std::scoped_lock lock(state->touchMutex);
        state->touchSamples.push_back(sample);
    }

    bool waitUntil(std::chrono::milliseconds timeout, const std::function<bool()>& condition)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    }
}

int runCoreTests()
{
    {
        expect(sds::shouldArmMeleeEventDiagnostic(sds::WeaponTriggerFamily::Melee, 1),
            "melee event diagnostic arms only when a melee profile has a registered player graph");
        expect(!sds::shouldArmMeleeEventDiagnostic(sds::WeaponTriggerFamily::BallisticHandgun, 1),
            "melee event diagnostic stays disarmed for ranged weapon families");
        expect(!sds::shouldArmMeleeEventDiagnostic(sds::WeaponTriggerFamily::Melee, 0),
            "melee event diagnostic stays disarmed when no player animation graph is registered");
        expect(sds::kMeleeEventDiagnosticWindow == std::chrono::seconds(20),
            "melee event diagnostic uses the approved bounded 20-second window");
        expect(sds::kTargetHitSourceDocumentedOffset == 0x5D0,
            "TargetHit source probe preserves CommonLibSF documented PlayerCharacter offset");
        expect(sds::targetHitSourceOffset(0x100000, 0x1005D0) == 0x5D0,
            "TargetHit source probe reports compiler-adjusted source offset");
        expect(sds::targetHitSourceOffsetMatchesDocumented(0x100000, 0x1005D0),
            "TargetHit source probe accepts documented compiler offset");
        expect(!sds::targetHitSourceOffsetMatchesDocumented(0x100000, 0x1005C8),
            "TargetHit source probe flags a compiler offset mismatch without registering a sink");
        expect(sds::targetHitDocumentedSourceAddress(0x100000) == 0x1005D0,
            "TargetHit documented-offset probe derives the runtime candidate directly from PlayerCharacter plus 0x5D0");
        expect(sds::targetHitSourceCandidatesDiffer(0x100000, 0x100618),
            "TargetHit documented-offset probe flags the observed 0x618 compiler candidate as distinct from 0x5D0");
        expect(!sds::targetHitSourceCandidatesDiffer(0x100000, 0x1005D0),
            "TargetHit documented-offset probe recognizes a compiler candidate already matching 0x5D0");
        expect(sds::targetHitSourceRegistrationEligible(
                   true, true, true, true, true, 17, 32, 0x200000),
            "TargetHit corrected registration accepts the hardware-validated documented source shape");
        expect(!sds::targetHitSourceRegistrationEligible(
                   true, true, false, true, true, 17, 32, 0x200000),
            "TargetHit corrected registration rejects a vtable outside Starfield");
        expect(!sds::targetHitSourceRegistrationEligible(
                   true, true, true, true, false, 17, 32, 0x200000),
            "TargetHit corrected registration rejects a first virtual slot outside Starfield");
        expect(!sds::targetHitSourceRegistrationEligible(
                   true, true, true, true, true, 33, 32, 0x200000),
            "TargetHit corrected registration rejects impossible sink counts");
        expect(!sds::targetHitSourceRegistrationEligible(
                   true, true, true, true, true, 1, 32, 0),
            "TargetHit corrected registration rejects a populated source with null sink storage");
        expect(sds::targetHitSourceRegistrationEligible(
                   true, true, true, true, true, 0, 0, 0),
            "TargetHit corrected registration permits an empty event source with no allocated sink storage");
        expect(sds::targetHitRegistrationVerified(17, 18, true, true),
            "TargetHit registration verification accepts one-count growth with readable exact sink membership");
        expect(!sds::targetHitRegistrationVerified(17, 17, true, true),
            "TargetHit registration verification rejects unchanged sink count");
        expect(!sds::targetHitRegistrationVerified(17, 18, true, false),
            "TargetHit registration verification rejects a missing exact sink pointer");
        expect(!sds::targetHitRegistrationVerified(17, 18, false, true),
            "TargetHit registration verification rejects unreadable sink storage");
        expect(sds::targetHitUnregistrationVerified(17, 17, true, false),
            "TargetHit unregistration verification accepts restoration to pre-registration count with sink absent");
        expect(!sds::targetHitUnregistrationVerified(17, 18, true, false),
            "TargetHit unregistration verification rejects a retained extra sink entry");
        expect(!sds::targetHitUnregistrationVerified(17, 17, true, true),
            "TargetHit unregistration verification rejects the exact sink still present");
        expect(sds::kPlayerTESHitSinkDocumentedOffset == 0x620,
            "TESHit discovery preserves CommonLibSF documented PlayerCharacter sink offset");
        expect(sds::tesHitDocumentedPlayerSinkAddress(0x100000) == 0x100620,
            "TESHit discovery derives the player's documented TESHit sink without a compiler base cast");
        expect(sds::tesHitSourceShapeEligible(true, true, 17, 32, 0x200000),
            "TESHit discovery accepts an exact source-vtable match with sane populated sink storage");
        expect(!sds::tesHitSourceShapeEligible(false, true, 17, 32, 0x200000),
            "TESHit discovery rejects a source whose vtable does not match BSTEventSource<TESHitEvent>");
        expect(!sds::tesHitSourceShapeEligible(true, true, 33, 32, 0x200000),
            "TESHit discovery rejects impossible sink counts");
        expect(!sds::tesHitSourceShapeEligible(true, true, 1, 32, 0),
            "TESHit discovery rejects populated sources with null sink storage");
        expect(sds::tesHitSourceCandidateVerified(true, true, true),
            "TESHit discovery verifies a candidate only when the exact PlayerCharacter TESHit sink is present");
        expect(!sds::tesHitSourceCandidateVerified(true, true, false),
            "TESHit discovery rejects candidates that do not contain the exact player TESHit sink");
        expect(!sds::tesHitSourceCandidateVerified(true, false, true),
            "TESHit discovery rejects unreadable sink arrays even if other evidence looks correct");
        expect(sds::tesHitUniqueSourceRegistrationEligible(1, 1, 0x12345678),
            "TESHit registration accepts exactly one sane source-vtable match");
        expect(!sds::tesHitUniqueSourceRegistrationEligible(2, 1, 0x12345678),
            "TESHit registration rejects multiple exact source-vtable matches");
        expect(!sds::tesHitUniqueSourceRegistrationEligible(1, 0, 0),
            "TESHit registration rejects a source scan with no sane candidate");
        expect(sds::tesHitRegistrationVerified(7, 8, true, true),
            "TESHit registration verification accepts one-count growth with exact sink membership");
        expect(!sds::tesHitRegistrationVerified(7, 7, true, true),
            "TESHit registration verification rejects unchanged sink count");
        expect(!sds::tesHitRegistrationVerified(7, 8, true, false),
            "TESHit registration verification rejects missing exact sink membership");
        expect(sds::tesHitUnregistrationVerified(8, 7, true, false),
            "TESHit unregistration verification accepts an exact one-count drop at shutdown");
        expect(!sds::tesHitUnregistrationVerified(8, 8, true, false),
            "TESHit unregistration verification rejects an unchanged shutdown sink count");
        expect(!sds::tesHitUnregistrationVerified(8, 7, true, true),
            "TESHit unregistration verification rejects the exact sink still present");
        expect(!sds::tesHitUnregistrationVerified(0, UINT32_MAX, true, false),
            "TESHit unregistration verification cannot underflow its one-count-drop check");
        expect(sds::confirmedPlayerMeleeImpact(true, false, true, 0x35A48, 0, 0x35A48),
            "player melee impact accepts a non-player target with player cause and matching zero-projectile weapon source");
        expect(!sds::confirmedPlayerMeleeImpact(true, true, true, 0x35A48, 0, 0x35A48),
            "player melee impact rejects the player as target");
        expect(!sds::confirmedPlayerMeleeImpact(true, false, false, 0x35A48, 0, 0x35A48),
            "player melee impact rejects a non-player cause");
        expect(!sds::confirmedPlayerMeleeImpact(true, false, true, 0x4F760, 0, 0x35A48),
            "player melee impact rejects a source FormID that does not match the equipped melee weapon");
        expect(!sds::confirmedPlayerMeleeImpact(true, false, true, 0x35A48, 0x6218A, 0x35A48),
            "player melee impact rejects projectile-backed hits");
        expect(!sds::confirmedPlayerMeleeImpact(false, false, true, 0x35A48, 0, 0x35A48),
            "player melee impact requires a real target");
        expect(sds::meleeHapticTierForWeapon("Combat Knife") == sds::MeleeHapticTier::Light,
            "Combat Knife selects the light melee tuning tier");
        expect(sds::meleeHapticTierForWeapon("Rescue Axe") == sds::MeleeHapticTier::Heavy,
            "Rescue Axe selects the heavy melee tuning tier");
        expect(sds::meleeHapticTierForWeapon("Mauling Axe") == sds::MeleeHapticTier::VeryHeavy,
            "Mauling Axe selects the very-heavy melee tuning tier");
        expect(sds::meleeHapticTierForWeapon("Tanto") == sds::MeleeHapticTier::None,
            "v0.2.66 leaves non-representative melee weapons unmapped until tuning is approved");

        sds::FireMarkerCaptureState meleeCapture;
        const auto now = std::chrono::steady_clock::now();
        meleeCapture.arm("Combat Knife", now, sds::kMeleeEventDiagnosticWindow);
        const auto swing = meleeCapture.tryCapture(true, now + std::chrono::milliseconds(25),
            "MeleeSwing", "<unavailable>", 0x1234);
        expect(swing && swing->kind == sds::FireMarkerRecordKind::Marker &&
                   std::string_view(swing->weapon.data()) == "Combat Knife" &&
                   std::string_view(swing->tag.data()) == "MeleeSwing",
            "melee event diagnostic records weapon, timing context, and animation tag without emitting gameplay events");
        expect(!meleeCapture.tryCapture(true, now + std::chrono::seconds(20),
                   "LateSwing", "", 0x1234),
            "melee event diagnostic drops events at the 20-second boundary");
    }
    expect(sds::ControllerType::DualSense != sds::ControllerType::Unknown,
        "shared controller type smoke test");

    {
        using namespace std::chrono_literals;
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::time_point{} + 1000ms;

        sds::SemanticPulseSequencer pulse{};
        expect(pulse.queue(start), "semantic pulse accepts first inventory request");
        expect(!pulse.queue(start), "semantic pulse rejects overlapping inventory request");
        expect(pulse.active(), "semantic pulse reports active after queue");

        const auto first = pulse.poll(start);
        expect(first.has_value(), "semantic pulse emits press-transition edge immediately");
        if (first) {
            expect(first->edge == sds::SemanticPulseEdge::Held && near(first->value, 1.0F) &&
                       near(first->heldDownSecs, 0.000F) && near(first->previousHeldDownSecs, 0.000F) &&
                       first->status == 0 && first->stepIndex == 1,
                "semantic pulse first edge matches native zero-held press transition");
        }

        expect(!pulse.poll(start + 14ms).has_value(),
            "semantic pulse does not emit second edge before 15ms");
        const auto second = pulse.poll(start + 15ms);
        expect(second.has_value() && second->edge == sds::SemanticPulseEdge::Held &&
                   near(second->heldDownSecs, 0.015F) && near(second->previousHeldDownSecs, 0.000F) &&
                   second->stepIndex == 2,
            "semantic pulse emits first held edge at 15ms");
        const auto third = pulse.poll(start + 30ms);
        expect(third.has_value() && third->edge == sds::SemanticPulseEdge::Held &&
                   near(third->heldDownSecs, 0.030F) && near(third->previousHeldDownSecs, 0.015F) &&
                   third->stepIndex == 3,
            "semantic pulse emits second held edge at 30ms");
        const auto fourth = pulse.poll(start + 45ms);
        expect(fourth.has_value() && fourth->edge == sds::SemanticPulseEdge::Held &&
                   near(fourth->heldDownSecs, 0.045F) && near(fourth->previousHeldDownSecs, 0.030F) &&
                   fourth->stepIndex == 4,
            "semantic pulse emits third held edge at 45ms");
        const auto release = pulse.poll(start + 60ms);
        expect(release.has_value() && release->edge == sds::SemanticPulseEdge::Release &&
                   near(release->value, 0.0F) && near(release->heldDownSecs, 0.045F) &&
                   near(release->previousHeldDownSecs, 0.045F) &&
                   release->status == 2 && release->stepIndex == 5,
            "semantic pulse emits release edge at 60ms");
        expect(!pulse.active(), "semantic pulse becomes inactive after release");
        expect(pulse.queue(start + 61ms), "semantic pulse can be re-armed after release");

        constexpr std::uintptr_t moduleBase = 0x10000000ULL;
        constexpr std::uintptr_t actionPointer = 0x123456789ABCDEF0ULL;
        sds::SemanticPulseStep packetStep{
            .edge = sds::SemanticPulseEdge::Held,
            .value = 1.0F,
            .heldDownSecs = 0.030F,
            .previousHeldDownSecs = 0.015F,
            .status = 0,
            .stepIndex = 2,
        };
        const auto packet = sds::buildQuickInventorySemanticPacket(
            moduleBase, actionPointer, 0x11223344U, packetStep);

        auto readU32 = [&packet](std::size_t offset) {
            std::uint32_t value{};
            std::memcpy(&value, packet.data() + offset, sizeof(value));
            return value;
        };
        auto readI32 = [&packet](std::size_t offset) {
            std::int32_t value{};
            std::memcpy(&value, packet.data() + offset, sizeof(value));
            return value;
        };
        auto readPtr = [&packet](std::size_t offset) {
            std::uintptr_t value{};
            std::memcpy(&value, packet.data() + offset, sizeof(value));
            return value;
        };
        auto readFloat = [&packet](std::size_t offset) {
            float value{};
            std::memcpy(&value, packet.data() + offset, sizeof(value));
            return value;
        };

        expect(readPtr(0x00) == moduleBase + sds::kQuickInventoryPrimaryVtableRva,
            "semantic packet uses native ButtonEvent primary vtable");
        expect(readU32(0x08) == 0 && readU32(0x0C) == 0 && readU32(0x10) == 0,
            "semantic packet uses keyboard device zero and button event type");
        expect(readPtr(0x18) == 0 && readU32(0x20) == 0x11223344U && readU32(0x24) == 0,
            "semantic packet sets null next pointer, timeCode, and held status");
        expect(readPtr(0x28) == actionPointer && readI32(0x30) == 73 && packet[0x34] == 0,
            "semantic packet carries QuickInventory action pointer and keyboard I idCode");
        expect(readPtr(0x38) == moduleBase + sds::kQuickInventoryIdVtableRva &&
                   readPtr(0x40) == moduleBase + sds::kQuickInventoryUserVtableRva,
            "semantic packet uses native secondary vtables");
        expect(near(readFloat(0x48), 1.0F) && near(readFloat(0x4C), 0.030F),
            "semantic packet stores value and held duration");
        expect(readPtr(0x50) == 0 && readPtr(0x58) == 0,
            "baseline semantic packet leaves downstream hidden debounce pointers zero");

        std::array<std::uint8_t, sds::kQuickInventorySemanticPacketSize> nativeTemplate{};
        constexpr std::uintptr_t nativeNextPointer = 0x9999888877776666ULL;
        constexpr std::uintptr_t nativeActionPointer = 0x1111222233334444ULL;
        constexpr std::uintptr_t nativeDebounceManager = 0xAAAABBBBCCCCDDDDULL;
        constexpr std::uint32_t nativeReferenceHigh = 73U;
        std::memcpy(nativeTemplate.data() + 0x18, &nativeNextPointer, sizeof(nativeNextPointer));
        std::memcpy(nativeTemplate.data() + 0x28, &nativeActionPointer, sizeof(nativeActionPointer));
        std::memcpy(nativeTemplate.data() + 0x58, &nativeDebounceManager, sizeof(nativeDebounceManager));
        std::uint64_t nativeUnk50 = static_cast<std::uint64_t>(nativeReferenceHigh) << 32U;
        std::memcpy(nativeTemplate.data() + 0x50, &nativeUnk50, sizeof(nativeUnk50));
        nativeTemplate[0x14] = 0xA5;

        const auto replayPacket = sds::buildQuickInventorySemanticPacketFromTemplate(
            nativeTemplate, 0x55667788U, packetStep);
        auto replayReadU32 = [&replayPacket](std::size_t offset) {
            std::uint32_t value{};
            std::memcpy(&value, replayPacket.data() + offset, sizeof(value));
            return value;
        };
        auto replayReadPtr = [&replayPacket](std::size_t offset) {
            std::uintptr_t value{};
            std::memcpy(&value, replayPacket.data() + offset, sizeof(value));
            return value;
        };
        auto replayReadFloat = [&replayPacket](std::size_t offset) {
            float value{};
            std::memcpy(&value, replayPacket.data() + offset, sizeof(value));
            return value;
        };
        expect(replayPacket[0x14] == 0xA5 && replayReadPtr(0x28) == nativeActionPointer &&
                   replayReadPtr(0x58) == nativeDebounceManager,
            "template replay preserves engine-owned packet bytes and debounce manager");
        expect(replayReadPtr(0x18) == 0,
            "template replay clears stale native event-list next pointer");
        expect(replayReadU32(0x20) == 0x55667788U && replayReadU32(0x24) == 0 &&
                   near(replayReadFloat(0x48), 1.0F) && near(replayReadFloat(0x4C), 0.030F),
            "template replay replaces dynamic time, status, value, and held fields");
        expect(near(replayReadFloat(0x50), 0.015F) && replayReadU32(0x54) == 73U,
            "template replay mirrors native previous-held and idCode packing in unk50");

        packetStep.edge = sds::SemanticPulseEdge::Release;
        packetStep.value = 0.0F;
        packetStep.status = 2;
        const auto releasedPacket = sds::buildQuickInventorySemanticPacket(
            moduleBase, actionPointer, 0x11223345U, packetStep);
        std::uint32_t releasedStatus{};
        std::memcpy(&releasedStatus, releasedPacket.data() + 0x24, sizeof(releasedStatus));
        expect(releasedStatus == 2,
            "semantic packet carries native release status");
    }

    {
        sds::QuickInventoryNativeTemplate cache{};
        std::array<std::uint8_t, sds::kQuickInventorySemanticPacketSize> seedPacket{};
        seedPacket[0x58] = 0x5A;
        expect(!sds::captureQuickInventoryNativeTemplate(
                   cache, 0x11110000ULL, seedPacket, false, 0.0F, 900U),
            "semantic template cache rejects release edge as seed");
        expect(sds::captureQuickInventoryNativeTemplate(
                   cache, 0x22220000ULL, seedPacket, true, 0.015F, 901U),
            "semantic template cache accepts first native held frame");
        expect(cache.ready && cache.sourceAddress == 0x22220000ULL &&
                   cache.packet[0x58] == 0x5A && cache.lastObservedTimeCode == 901U,
            "semantic template cache preserves native source, packet, and timeCode");
        auto replacementPacket = seedPacket;
        replacementPacket[0x58] = 0xA5;
        expect(!sds::captureQuickInventoryNativeTemplate(
                   cache, 0x33330000ULL, replacementPacket, true, 0.030F, 902U),
            "semantic template cache does not overwrite seeded packet");
        expect(cache.sourceAddress == 0x22220000ULL && cache.packet[0x58] == 0x5A &&
                   cache.lastObservedTimeCode == 902U,
            "semantic template cache still advances observed timeCode after seed");
        sds::recordNativeSemanticTimeCode(cache, 905U);
        sds::recordNativeSemanticTimeCode(cache, 903U);
        expect(cache.lastObservedTimeCode == 905U,
            "semantic template cache tracks the newest native semantic timeCode");

        expect(sds::isQuickInventorySemanticAction("QuickInventory"),
            "semantic diagnostic accepts QuickInventory");
        expect(!sds::isQuickInventorySemanticAction("QuickMap"),
            "semantic diagnostic rejects other quick actions");
        expect(!sds::isQuickInventorySemanticAction(""),
            "semantic diagnostic rejects empty action");

        sds::SemanticButtonDiagnostic diagnostic{};
        diagnostic.edge = sds::InputButtonEdge::Press;
        diagnostic.sourceAddress = 0x1111;
        diagnostic.sourceVtable = 0x2222;
        diagnostic.sourceVtableExpected = true;
        diagnostic.eventAddress = 0x3333;
        diagnostic.primaryVtable = 0x4444;
        diagnostic.primaryVtableExpected = true;
        diagnostic.deviceType = 0;
        diagnostic.deviceId = 2;
        diagnostic.eventType = 0;
        diagnostic.status = 0;
        diagnostic.timeCode = 54321;
        diagnostic.idCode = 23;
        diagnostic.userEvent = "QuickInventory";
        diagnostic.disabled = false;
        diagnostic.idVtable = 0x5555;
        diagnostic.userVtable = 0x6666;
        diagnostic.value = 1.0F;
        diagnostic.heldDownSecs = 0.0F;
        diagnostic.unk50 = 0x7777;
        diagnostic.debounceManager = 0x8888;
        diagnostic.rawBytes.front() = 0x48;
        diagnostic.rawBytes.back() = 0xAA;

        const auto text = sds::formatSemanticButtonDiagnostic(diagnostic);
        expect(text.find("Semantic observer: action='QuickInventory' edge=press") != std::string::npos,
            "semantic diagnostic formats action and edge");
        expect(text.find("source=0x0000000000001111") != std::string::npos &&
                   text.find("sourceVtable=0x0000000000002222") != std::string::npos &&
                   text.find("sourceExpected=true") != std::string::npos,
            "semantic diagnostic formats source identity");
        expect(text.find("event=0x0000000000003333") != std::string::npos &&
                   text.find("vtable=0x0000000000004444") != std::string::npos &&
                   text.find("eventExpected=true") != std::string::npos,
            "semantic diagnostic formats event identity");
        expect(text.find("idCode=23") != std::string::npos &&
                   text.find("value=1.000") != std::string::npos &&
                   text.find("held=0.000") != std::string::npos,
            "semantic diagnostic formats button payload");
        expect(text.find("idVtable=0x0000000000005555") != std::string::npos &&
                   text.find("userVtable=0x0000000000006666") != std::string::npos,
            "semantic diagnostic formats secondary vtables");
        expect(text.find("raw60=48 00") != std::string::npos && text.ends_with("aa"),
            "semantic diagnostic includes raw event bytes");

        const auto patch = sds::buildSemanticBroadcasterEntryPatch(0x10000000, 0x10001000);
        expect(patch.has_value(), "semantic hook builds in-range near jump patch");
        if (patch) {
            expect((*patch)[0] == 0xE9 && (*patch)[5] == 0x90 && (*patch)[6] == 0x90,
                "semantic hook patch uses JMP rel32 plus two NOPs");
            const std::array<std::uint8_t, 7> expected{ 0xE9, 0xFB, 0x0F, 0x00, 0x00, 0x90, 0x90 };
            expect(*patch == expected, "semantic hook patch encodes displacement from source plus five");
        }
        expect(!sds::buildSemanticBroadcasterEntryPatch(0x1000, 0x900000000ULL).has_value(),
            "semantic hook rejects out-of-range relative jump");

        const std::array<std::uint8_t, 7> validPrologue{ 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10 };
        expect(sds::matchesSemanticBroadcasterPrologue(validPrologue),
            "semantic hook accepts validated broadcaster prologue");
        auto invalidPrologue = validPrologue;
        invalidPrologue[6] = 0x11;
        expect(!sds::matchesSemanticBroadcasterPrologue(invalidPrologue),
            "semantic hook rejects changed broadcaster prologue");
    }

    {
        sds::SemanticCallerTraceDiagnostic trace{};
        trace.edge = sds::InputButtonEdge::Press;
        trace.returnAddress = 0x0000000140123456ULL;
        trace.moduleBase = 0x0000000140000000ULL;
        trace.moduleSize = 0x01000000ULL;
        trace.stackFrames[0] = 0x00007FF600001111ULL;
        trace.stackFrames[1] = 0x0000000140101000ULL;
        trace.stackFrames[2] = 0x0000000140202000ULL;
        trace.stackFrames[3] = 0x00007FF700002222ULL;
        trace.stackFrameCount = 4;

        const auto text = sds::formatSemanticCallerTrace(trace);
        expect(text.find("Semantic caller trace: edge=press") != std::string::npos,
            "semantic caller trace formats edge");
        expect(text.find("return=0x0000000140123456") != std::string::npos &&
                   text.find("returnRva=Starfield+0x123456") != std::string::npos,
            "semantic caller trace formats absolute and relative return address");
        expect(text.find("stack=Starfield+0x101000 -> Starfield+0x202000") != std::string::npos,
            "semantic caller trace keeps only Starfield stack frames in order");
        expect(text.find("00007ff600001111") == std::string::npos &&
                   text.find("00007ff700002222") == std::string::npos,
            "semantic caller trace excludes non-Starfield stack frames");

        trace.edge = sds::InputButtonEdge::Release;
        trace.returnAddress = 0x00007FF600003333ULL;
        trace.stackFrameCount = 0;
        const auto foreign = sds::formatSemanticCallerTrace(trace);
        expect(foreign.find("edge=release") != std::string::npos &&
                   foreign.find("returnRva=outside-Starfield") != std::string::npos &&
                   foreign.find("stack=(no Starfield frames)") != std::string::npos,
            "semantic caller trace handles foreign caller and empty Starfield stack");
    }

    {
        constexpr std::uintptr_t callAddress = 0x00000001422DAD40ULL;
        constexpr std::uintptr_t targetAddress = 0x0000000142541B10ULL;
        std::array<std::uint8_t, 5> callBytes{ 0xE8, 0, 0, 0, 0 };
        const auto displacement = static_cast<std::int32_t>(
            static_cast<std::int64_t>(targetAddress) -
            static_cast<std::int64_t>(callAddress + callBytes.size()));
        std::memcpy(callBytes.data() + 1, &displacement, sizeof(displacement));

        const auto decoded = sds::decodeDirectRel32CallTarget(callAddress, callBytes);
        expect(decoded.has_value() && *decoded == targetAddress,
            "upstream probe decodes direct rel32 call target");

        auto notCall = callBytes;
        notCall[0] = 0xE9;
        expect(!sds::decodeDirectRel32CallTarget(callAddress, notCall).has_value(),
            "upstream probe rejects non-call instruction");

        sds::NativeCodeProbeDiagnostic probe{};
        probe.frameAddress = 0x00000001422DAD45ULL;
        probe.moduleBase = 0x0000000140000000ULL;
        probe.moduleSize = 0x06000000ULL;
        probe.functionStart = 0x00000001422DAC80ULL;
        probe.functionEnd = 0x00000001422DAE20ULL;
        probe.callAddress = callAddress;
        probe.callTarget = targetAddress;
        probe.callTargetDecoded = true;
        probe.callKind = sds::NativeCallKind::DirectRel32;
        probe.callFound = true;
        probe.windowStart = 0x00000001422DACE0ULL;
        probe.codeSize = 4;
        probe.codeBytes[0] = 0x48;
        probe.codeBytes[1] = 0x8B;
        probe.codeBytes[2] = 0xD9;
        probe.codeBytes[3] = 0xE8;

        const auto text = sds::formatNativeCodeProbe(probe, 1);
        expect(text.find("Semantic upstream probe: frame=1 frameRva=Starfield+0x22dad45") != std::string::npos,
            "upstream probe formats frame rva");
        expect(text.find("function=Starfield+0x22dac80..0x22dae20") != std::string::npos &&
                   text.find("functionOffset=+0xc5") != std::string::npos,
            "upstream probe formats unwind function bounds and frame offset");
        expect(text.find("callsite=Starfield+0x22dad40 callKind=direct-rel32") != std::string::npos &&
                   text.find("callTarget=Starfield+0x2541b10") != std::string::npos,
            "upstream probe formats decoded callsite kind and target");
        expect(text.find("codeStart=Starfield+0x22dace0") != std::string::npos &&
                   text.ends_with("48 8b d9 e8"),
            "upstream probe formats code window bytes");
    }

    {
        const std::uintptr_t windowStart = 0x00000001422DAD37ULL;
        const std::vector<std::uint8_t> bytes{
            0x48, 0x8B, 0x5C, 0x24, 0x40,
            0x48, 0x8B, 0xD7,
            0x48, 0x8B, 0xCE,
            0x41, 0xFF, 0xD7,
            0x90,
            0x48, 0x85, 0xDB
        };
        const auto decoded = sds::findNearestCallBefore(
            windowStart, bytes, windowStart + bytes.size());
        expect(decoded.has_value(),
            "upstream call scanner finds indirect register call before frame");
        expect(decoded && decoded->kind == sds::NativeCallKind::IndirectRegister &&
                   decoded->instructionAddress == 0x00000001422DAD42ULL &&
                   decoded->instructionSize == 3 && !decoded->targetDecoded,
            "upstream call scanner identifies rex indirect-register call without inventing target");
    }

    {
        constexpr std::uintptr_t windowStart = 0x0000000141890D90ULL;
        constexpr std::uintptr_t targetAddress = 0x00000001422DAC90ULL;
        std::vector<std::uint8_t> bytes{ 0x48, 0x8B, 0xCE, 0xE8, 0, 0, 0, 0, 0x90 };
        const auto callAddress = windowStart + 3;
        const auto displacement = static_cast<std::int32_t>(
            static_cast<std::int64_t>(targetAddress) -
            static_cast<std::int64_t>(callAddress + 5));
        std::memcpy(bytes.data() + 4, &displacement, sizeof(displacement));
        const auto decoded = sds::findNearestCallBefore(
            windowStart, bytes, windowStart + bytes.size());
        expect(decoded && decoded->kind == sds::NativeCallKind::DirectRel32 &&
                   decoded->instructionAddress == callAddress &&
                   decoded->targetDecoded && decoded->targetAddress == targetAddress,
            "upstream call scanner decodes nearest direct rel32 call");
    }

    {
        constexpr std::uintptr_t windowStart = 0x0000000140001000ULL;
        constexpr std::uintptr_t vtableTarget = 0x0000000144D59F50ULL;
        constexpr std::uintptr_t queueGlobalTarget = 0x00000001461EEA20ULL;

        std::vector<std::uint8_t> bytes{
            0x48, 0x8D, 0x05, 0, 0, 0, 0,
            0x90,
            0x48, 0x8B, 0x0D, 0, 0, 0, 0,
            0x90,
            0x48, 0x89, 0x15, 0, 0, 0, 0
        };

        const auto writeDisp = [&](std::size_t instructionOffset, std::uintptr_t target) {
            const auto nextInstruction = windowStart + instructionOffset + 7;
            const auto displacement = static_cast<std::int32_t>(
                static_cast<std::int64_t>(target) - static_cast<std::int64_t>(nextInstruction));
            std::memcpy(bytes.data() + instructionOffset + 3, &displacement, sizeof(displacement));
        };
        writeDisp(0, vtableTarget);
        writeDisp(8, queueGlobalTarget);
        writeDisp(16, queueGlobalTarget);

        const auto vtableRefs = sds::findRipRelativeReferences(windowStart, bytes, vtableTarget);
        expect(vtableRefs.size() == 1 &&
                   vtableRefs[0].kind == sds::NativeRipReferenceKind::Lea &&
                   vtableRefs[0].instructionAddress == windowStart &&
                   vtableRefs[0].instructionSize == 7 &&
                   vtableRefs[0].targetAddress == vtableTarget,
            "native xref scanner finds rex RIP-relative lea to ButtonEvent vtable");

        const auto queueRefs = sds::findRipRelativeReferences(windowStart, bytes, queueGlobalTarget);
        expect(queueRefs.size() == 2 &&
                   queueRefs[0].kind == sds::NativeRipReferenceKind::MovLoad &&
                   queueRefs[0].instructionAddress == windowStart + 8 &&
                   queueRefs[1].kind == sds::NativeRipReferenceKind::MovStore &&
                   queueRefs[1].instructionAddress == windowStart + 16,
            "native xref scanner distinguishes RIP-relative global load and store");

        expect(sds::findRipRelativeReferences(windowStart, bytes, windowStart + 1).empty(),
            "native xref scanner ignores unrelated RIP-relative targets");
    }

    {
        sds::NativeFunctionChunkDiagnostic chunk{};
        chunk.moduleBase = 0x0000000140000000ULL;
        chunk.moduleSize = 0x06000000ULL;
        chunk.functionStart = 0x00000001422DAC90ULL;
        chunk.functionEnd = 0x00000001422DAD86ULL;
        chunk.chunkStart = 0x00000001422DAC90ULL;
        chunk.chunkIndex = 0;
        chunk.chunkCount = 2;
        chunk.byteCount = 4;
        chunk.bytes[0] = 0x48;
        chunk.bytes[1] = 0x89;
        chunk.bytes[2] = 0x5C;
        chunk.bytes[3] = 0x24;
        const auto text = sds::formatNativeFunctionChunk(chunk);
        expect(text.find("Semantic pump dump: function=Starfield+0x22dac90..0x22dad86") != std::string::npos &&
                   text.find("chunk=1/2 start=Starfield+0x22dac90 bytes4=") != std::string::npos &&
                   text.ends_with("48 89 5c 24"),
            "upstream full-function chunk formatter preserves boundaries and bytes");
    }

    {
        sds::NativeSourceSnapshotDiagnostic snapshot{};
        snapshot.moduleBase = 0x0000000140000000ULL;
        snapshot.moduleSize = 0x06000000ULL;
        snapshot.managerAddress = 0x000000018761D880ULL;
        snapshot.observedSourceAddress = 0x000000018761D890ULL;
        snapshot.expectedSourceAddress = 0x000000018761D890ULL;
        snapshot.sourceVtable = 0x0000000144D7E408ULL;
        snapshot.sourceMatchesManager = true;
        snapshot.byteCount = 4;
        snapshot.bytes[0] = 0x08;
        snapshot.bytes[1] = 0xE4;
        snapshot.bytes[2] = 0xD7;
        snapshot.bytes[3] = 0x44;
        const auto text = sds::formatNativeSourceSnapshot(snapshot);
        expect(text.find("Semantic source snapshot:") != std::string::npos &&
                   text.find("sourceMatchesManager=true") != std::string::npos &&
                   text.ends_with("08 e4 d7 44"),
            "upstream source snapshot formatter reports manager relationship and bytes");
    }


    {
        const std::uintptr_t start = 0x00000001422D7000ULL;
        const std::vector<std::uint8_t> bytes{
            0x89, 0x4B, 0x20,                         // mov [rbx+20], ecx
            0x48, 0x89, 0x53, 0x28,                   // mov [rbx+28], rdx
            0xC7, 0x43, 0x30, 0x49, 0x00, 0x00, 0x00, // mov dword [rbx+30], 73
            0xF3, 0x0F, 0x11, 0x43, 0x48,             // movss [rbx+48], xmm0
            0xF3, 0x0F, 0x11, 0x4B, 0x4C              // movss [rbx+4c], xmm1
        };
        const auto writes = sds::findButtonEventFieldWrites(start, bytes);
        std::uint32_t mask = 0;
        for (const auto& write : writes) {
            mask |= write.fieldMask;
        }
        expect((mask & sds::kNativeButtonFieldTimeCode) != 0,
            "recycled producer scan detects ButtonEvent timeCode writes");
        expect((mask & sds::kNativeButtonFieldAction) != 0,
            "recycled producer scan detects ButtonEvent action writes");
        expect((mask & sds::kNativeButtonFieldIdCode) != 0,
            "recycled producer scan detects ButtonEvent idCode writes");
        expect((mask & sds::kNativeButtonFieldValue) != 0,
            "recycled producer scan detects ButtonEvent value writes");
        expect((mask & sds::kNativeButtonFieldHeld) != 0,
            "recycled producer scan detects ButtonEvent held writes");
        expect(writes.size() == 5,
            "recycled producer scan reports one reference per field write");
    }

    {
        const std::uintptr_t start = 0x00000001422D7100ULL;
        const std::vector<std::uint8_t> bytes{
            0xC5, 0xFA, 0x11, 0x43, 0x48,             // vmovss [rbx+48], xmm0 (VEX2)
            0xC4, 0xE1, 0x72, 0x11, 0x4B, 0x4C        // vmovss [rbx+4c], xmm1 (VEX3)
        };
        const auto writes = sds::findButtonEventFieldWrites(start, bytes);
        std::uint32_t mask = 0;
        for (const auto& write : writes) {
            mask |= write.fieldMask;
        }
        expect((mask & sds::kNativeButtonFieldValue) != 0 &&
                   (mask & sds::kNativeButtonFieldHeld) != 0,
            "recycled producer scan detects VEX vmovss value and held writes");
    }

    {
        const std::uintptr_t start = 0x00000001422D7700ULL;
        const std::uintptr_t target = 0x00000001422DABC0ULL;
        const std::vector<std::uint8_t> bytes{
            0x90,
            0xE8, 0xBA, 0x34, 0x00, 0x00, // start+1 -> target
            0x90,
            0xE8, 0x00, 0x00, 0x00, 0x00  // another direct call, different target
        };
        const auto calls = sds::findDirectRel32CallsToTarget(start, bytes, target);
        expect(calls.size() == 1 && calls[0].instructionAddress == start + 1 &&
                   calls[0].targetDecoded && calls[0].targetAddress == target,
            "enqueue-origin scan finds only direct calls to the requested target");
    }

    {
        const auto patch = sds::buildRel32CallPatch(
            0x00000001422D7780ULL,
            0x0000000142290000ULL);
        expect(patch.has_value() && (*patch)[0] == 0xE8,
            "enqueue-origin callsite patch uses a direct CALL rel32");
        if (patch) {
            std::array<std::uint8_t, 5> instruction{};
            std::copy(patch->begin(), patch->end(), instruction.begin());
            const auto decoded = sds::decodeDirectRel32CallTarget(
                0x00000001422D7780ULL, instruction);
            expect(decoded && *decoded == 0x0000000142290000ULL,
                "enqueue-origin callsite patch decodes back to the branch island");
        }
        expect(!sds::buildRel32CallPatch(0x1000ULL, 0x1000000000ULL).has_value(),
            "enqueue-origin callsite patch rejects out-of-range branch islands");
    }

    {
        sds::NativeEnqueueOriginHistory history{};
        std::array<std::uintptr_t, sds::kSemanticCallerTraceMaxFrames> stack{};
        stack[0] = 0x00000001422D7785ULL;
        stack[1] = 0x0000000141881234ULL;
        history.record(
            0x0000000189720038ULL,
            1273,
            0x00000001422D7785ULL,
            0x0000000140000000ULL,
            0x06000000ULL,
            stack,
            2);

        expect(!history.findNewest(0x0000000189720038ULL, 1272).has_value(),
            "enqueue-origin history requires the native timeCode for an exact match");
        const auto pointerMatch = history.findNewest(0x0000000189720038ULL);
        expect(pointerMatch && pointerMatch->timeCode == 1273,
            "enqueue-origin history can fall back to the newest exact event pointer");
        const auto match = history.findNewest(0x0000000189720038ULL, 1273);
        expect(match && match->eventAddress == 0x0000000189720038ULL &&
                   match->timeCode == 1273 &&
                   match->callerReturnAddress == 0x00000001422D7785ULL,
            "enqueue-origin history correlates an exact event pointer and timeCode");
        if (match) {
            const auto text = sds::formatNativeEnqueueOrigin(*match);
            expect(text.find("Native enqueue origin: seq=1") != std::string::npos &&
                       text.find("event=0x0000000189720038") != std::string::npos &&
                       text.find("timeCode=1273") != std::string::npos &&
                       text.find("caller=Starfield+0x22d7785") != std::string::npos,
                "enqueue-origin formatter reports event, timeCode, and Starfield caller");
        }
    }

    {
        const std::uintptr_t start = 0x00000001422D7000ULL;
        const std::vector<std::uint8_t> bytes{
            0xE8, 0x10, 0x00, 0x00, 0x00, // direct rel32
            0x41, 0xFF, 0xD7,             // call r15
            0xFF, 0x53, 0x18              // call qword ptr [rbx+18]
        };
        const auto calls = sds::findNativeCallSites(start, bytes);
        expect(calls.size() == 3,
            "recycled producer call scan finds direct and indirect calls");
        expect(calls.size() >= 1 && calls[0].kind == sds::NativeCallKind::DirectRel32 &&
                   calls[0].targetDecoded && calls[0].targetAddress == start + 0x15,
            "recycled producer call scan decodes direct rel32 target");
        expect(calls.size() >= 2 && calls[1].kind == sds::NativeCallKind::IndirectRegister &&
                   !calls[1].targetDecoded,
            "recycled producer call scan identifies register-indirect call");
        expect(calls.size() >= 3 && calls[2].kind == sds::NativeCallKind::IndirectMemory &&
                   !calls[2].targetDecoded,
            "recycled producer call scan identifies memory-indirect call");
    }

    {
        sds::NativeButtonConstructorHistory history{};
        std::array<std::uintptr_t, sds::kSemanticCallerTraceMaxFrames> stackA{};
        stackA[0] = 0x0000000141111111ULL;
        stackA[1] = 0x0000000142222222ULL;
        history.record(
            0x0000000180001000ULL,
            0x00000001422DA555ULL,
            0x0000000140000000ULL,
            0x06000000ULL,
            stackA,
            2);

        std::array<std::uintptr_t, sds::kSemanticCallerTraceMaxFrames> stackB{};
        stackB[0] = 0x0000000143333333ULL;
        history.record(
            0x0000000180002000ULL,
            0x00000001422DA666ULL,
            0x0000000140000000ULL,
            0x06000000ULL,
            stackB,
            1);

        const auto match = history.findNewest(0x0000000180001000ULL);
        expect(match.has_value() && match->objectAddress == 0x0000000180001000ULL,
            "constructor history matches an exact ButtonEvent object pointer");
        expect(match && match->callerReturnAddress == 0x00000001422DA555ULL &&
                   match->stackFrameCount == 2 && match->stackFrames[1] == 0x0000000142222222ULL,
            "constructor history preserves caller and stack provenance");
        expect(!history.findNewest(0x0000000180003000ULL).has_value(),
            "constructor history rejects unknown ButtonEvent pointers");

        for (std::size_t i = 0; i < sds::kNativeButtonConstructorHistoryCapacity + 2; ++i) {
            std::array<std::uintptr_t, sds::kSemanticCallerTraceMaxFrames> stack{};
            history.record(
                0x0000000190000000ULL + i,
                0x0000000141000000ULL + i,
                0x0000000140000000ULL,
                0x06000000ULL,
                stack,
                0);
        }
        expect(!history.findNewest(0x0000000180001000ULL).has_value(),
            "constructor history evicts records after fixed-capacity rollover");

        sds::NativeButtonConstructorOriginDiagnostic diagnostic{};
        diagnostic.sequence = 42;
        diagnostic.objectAddress = 0x0000000187654321ULL;
        diagnostic.callerReturnAddress = 0x00000001422DA555ULL;
        diagnostic.moduleBase = 0x0000000140000000ULL;
        diagnostic.moduleSize = 0x06000000ULL;
        diagnostic.stackFrames[0] = 0x00007FF700001111ULL;
        diagnostic.stackFrames[1] = 0x0000000141890C60ULL;
        diagnostic.stackFrames[2] = 0x00000001417C7751ULL;
        diagnostic.stackFrameCount = 3;
        const auto text = sds::formatNativeButtonConstructorOrigin(diagnostic);
        expect(text.find("ButtonEvent constructor origin: seq=42 object=0x0000000187654321") != std::string::npos,
            "constructor origin formatter includes sequence and object pointer");
        expect(text.find("caller=Starfield+0x22da555") != std::string::npos &&
                   text.find("stack=Starfield+0x1890c60 -> Starfield+0x17c7751") != std::string::npos,
            "constructor origin formatter reports Starfield caller and filtered stack");
        expect(text.find("00007ff700001111") == std::string::npos,
            "constructor origin formatter excludes non-Starfield stack frames");

        const std::array<std::uint8_t, 10> validCtorPrologue{
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18
        };
        expect(sds::matchesButtonEventConstructorPrologue(validCtorPrologue),
            "constructor hook accepts validated 10-byte ButtonEvent constructor prologue");
        auto invalidCtorPrologue = validCtorPrologue;
        invalidCtorPrologue[9] = 0x19;
        expect(!sds::matchesButtonEventConstructorPrologue(invalidCtorPrologue),
            "constructor hook rejects changed ButtonEvent constructor prologue");

        const auto ctorPatch = sds::buildButtonEventConstructorEntryPatch(0x10000000, 0x10001000);
        expect(ctorPatch.has_value(), "constructor hook builds in-range 10-byte entry patch");
        if (ctorPatch) {
            expect((*ctorPatch)[0] == 0xE9 && (*ctorPatch)[5] == 0x90 && (*ctorPatch)[9] == 0x90,
                "constructor hook patch uses JMP rel32 followed by five NOPs");
        }
        expect(!sds::buildButtonEventConstructorEntryPatch(0x1000, 0x900000000ULL).has_value(),
            "constructor hook rejects out-of-range branch island");
    }

    {
        sds::InputButtonDiagnostic diagnostic{};
        diagnostic.edge = sds::InputButtonEdge::Press;
        diagnostic.deviceType = 0;
        diagnostic.deviceId = 2;
        diagnostic.eventType = 0;
        diagnostic.status = 0;
        diagnostic.timeCode = 12345;
        diagnostic.idCode = 23;
        diagnostic.userEvent = "QuickInventory";
        diagnostic.disabled = false;
        diagnostic.value = 1.0F;
        diagnostic.heldDownSecs = 0.0F;
        diagnostic.objectAddress = 0x1234;
        diagnostic.primaryVtable = 0x5678;
        diagnostic.chordState = 0x1111222233334444ULL;
        diagnostic.debounceState = 0x5555666677778888ULL;
        diagnostic.unk50 = 0x9ABC;
        diagnostic.debounceManager = 0xDEF0;
        diagnostic.rawBytes.front() = 0xAB;
        diagnostic.rawBytes.back() = 0xCD;
        const auto text = sds::formatInputButtonDiagnostic(diagnostic);
        expect(text.find("edge=press") != std::string::npos, "input diagnostic formats press edge");
        expect(text.find("device=0") != std::string::npos && text.find("deviceId=2") != std::string::npos,
            "input diagnostic formats device identity");
        expect(text.find("idCode=23") != std::string::npos && text.find("userEvent='QuickInventory'") != std::string::npos,
            "input diagnostic formats binding identity");
        expect(text.find("disabled=false") != std::string::npos, "input diagnostic formats disabled state");
        expect(text.find("value=1.000") != std::string::npos && text.find("held=0.000") != std::string::npos,
            "input diagnostic formats analog values");
        expect(text.find("timeCode=12345") != std::string::npos && text.find("status=0") != std::string::npos,
            "input diagnostic formats timing/status");

        diagnostic.edge = sds::InputButtonEdge::Release;
        diagnostic.value = 0.0F;
        diagnostic.heldDownSecs = 0.084F;
        const auto released = sds::formatInputButtonDiagnostic(diagnostic);
        expect(released.find("edge=release") != std::string::npos, "input diagnostic formats release edge");
        expect(text.find("object=0x0000000000001234") != std::string::npos &&
                   text.find("vtable=0x0000000000005678") != std::string::npos,
            "input diagnostic formats object and primary vtable addresses");
        expect(text.find("chord=0x1111222233334444") != std::string::npos &&
                   text.find("debounce=0x5555666677778888") != std::string::npos,
            "input diagnostic formats secondary-base state words");
        expect(text.find("unk50=0x0000000000009abc") != std::string::npos &&
                   text.find("debounceManager=0x000000000000def0") != std::string::npos,
            "input diagnostic formats opaque ButtonEvent pointers");
        expect(text.find("raw60=ab 00") != std::string::npos && text.ends_with("cd"),
            "input diagnostic includes full raw ButtonEvent dump");
    }


    {
        sds::InputTraceBuffer trace(750, 1000, 4);

        sds::NativeInputDiagnostic oldSample{};
        oldSample.observedMs = 1000;
        oldSample.deviceType = 2;
        oldSample.eventType = 4;
        trace.push(oldSample);

        sds::NativeInputDiagnostic recentButton{};
        recentButton.observedMs = 1600;
        recentButton.deviceType = 2;
        recentButton.deviceId = 1;
        recentButton.eventType = 0;
        recentButton.status = 0;
        recentButton.timeCode = 77;
        recentButton.idCode = 9;
        recentButton.userEvent = "SomeGamepadEvent";
        recentButton.disabled = false;
        recentButton.value = 1.0F;
        recentButton.heldDownSecs = 0.0F;
        trace.push(recentButton);

        const auto started = trace.beginCapture(2000);
        expect(started.captureId == 1, "input trace assigns first capture id");
        expect(started.buffered.size() == 1 && started.buffered.front().observedMs == 1600,
            "input trace replays only events inside pre-swipe window");
        expect(trace.activeCaptureId(2500).value_or(0) == 1,
            "input trace remains active during post-swipe window");

        sds::NativeInputDiagnostic forward{};
        forward.observedMs = 2300;
        forward.deviceType = 0;
        forward.eventType = 0;
        forward.idCode = 73;
        forward.userEvent = "QuickInventory";
        trace.push(forward);
        expect(trace.observedCount() == 2,
            "input trace counts buffered and forward events for active capture");

        const auto finished = trace.finishCapture(3001);
        expect(finished.has_value() && finished->captureId == 1 && finished->observedCount == 2,
            "input trace finishes capture after post-swipe deadline");
        expect(!trace.activeCaptureId(3001).has_value(),
            "input trace clears active capture after finish");

        const auto text = sds::formatNativeInputDiagnostic(recentButton, 7, -400);
        expect(text.find("capture=7") != std::string::npos && text.find("relativeMs=-400") != std::string::npos,
            "native input trace formats capture timing");
        expect(text.find("device=2") != std::string::npos && text.find("eventType=0") != std::string::npos,
            "native input trace formats base event identity");
        expect(text.find("idCode=9") != std::string::npos && text.find("userEvent='SomeGamepadEvent'") != std::string::npos,
            "native input trace formats button details when present");
    }

    const auto defaults = sds::Config::defaults();
    expect(defaults.adaptiveTriggers, "config defaults adaptive triggers on");
    expect(near(defaults.triggerStrength, 1.0F), "config defaults trigger strength");
    expect(defaults.advancedHaptics, "config defaults advanced haptics on");
    expect(near(defaults.hapticStrength, 1.0F), "config defaults haptic strength");
    expect(defaults.controllerSpeaker, "config defaults controller speaker on");
    expect(near(defaults.speakerVolume, 0.8F), "config defaults speaker volume");
    expect(defaults.speakerOutputMode == sds::SpeakerOutputMode::Both, "config defaults speaker output mode to Both");
    expect(defaults.speakerComms && defaults.speakerScannerUI && defaults.speakerWeapons && defaults.speakerDigipick && defaults.speakerCrafting && defaults.speakerShipSystems, "config defaults all speaker categories on");
    expect(defaults.lightbar && defaults.touchpad, "config defaults visual/touch on");
    expect(!defaults.debugLogging, "config defaults debug logging off");

    const auto parsed = sds::loadConfig(R"(
# comment
AdaptiveTriggers = false
TriggerStrength = 0.42
HapticStrength = 2.5
SpeakerVolume = -1.0
SpeakerOutputMode = "ControllerOnly"
SpeakerComms = false
SpeakerShipSystems = false
Lightbar = false
Touchpad = false
DebugLogging = true
UnknownFutureOption = 123
)");
    expect(!parsed.adaptiveTriggers, "config parses bool false");
    expect(near(parsed.triggerStrength, 0.42F), "config parses float");
    expect(near(parsed.hapticStrength, 1.0F), "config clamps high intensity");
    expect(near(parsed.speakerVolume, 0.0F), "config clamps low intensity");
    expect(parsed.speakerOutputMode == sds::SpeakerOutputMode::ControllerOnly, "config parses speaker output mode");
    expect(!sds::speakerCategoryEnabled(parsed, sds::SpeakerCategory::Comms) && !sds::speakerCategoryEnabled(parsed, sds::SpeakerCategory::ShipSystems), "config parses speaker category toggles");
    expect(!parsed.lightbar && !parsed.touchpad, "config parses feature booleans");
    expect(parsed.debugLogging, "config parses debug logging");

    const auto invalid = sds::loadConfig("TriggerStrength = nope\nAdaptiveTriggers = maybe\n");
    expect(near(invalid.triggerStrength, 1.0F), "invalid float preserves default");
    expect(invalid.adaptiveTriggers, "invalid bool preserves default");

    const auto ds = sds::classifyDevice(0x054C, 0x0CE6, 64);
    expect(ds.type == sds::ControllerType::DualSense, "classifies regular DualSense");
    expect(ds.connection == sds::ConnectionType::Usb, "classifies DualSense USB");

    const auto edge = sds::classifyDevice(0x054C, 0x0DF2, 64);
    expect(edge.type == sds::ControllerType::DualSenseEdge, "classifies DualSense Edge");
    expect(edge.connection == sds::ConnectionType::Usb, "classifies Edge USB");

    const auto bt = sds::classifyDevice(0x054C, 0x0CE6, 78);
    expect(bt.type == sds::ControllerType::DualSense, "recognizes supported Bluetooth device identity");
    expect(bt.connection == sds::ConnectionType::Bluetooth, "recognizes Bluetooth report length");

    const auto unknownProduct = sds::classifyDevice(0x054C, 0xFFFF, 64);
    expect(!unknownProduct.supported(), "rejects unknown Sony product");

    const auto wrongVendor = sds::classifyDevice(0x1234, 0x0CE6, 64);
    expect(!wrongVendor.supported(), "rejects wrong vendor");


    {
        sds::EventQueue<2> queue;
        sds::GameEvent a{}; a.type = sds::GameEventType::MenuOpened;
        sds::GameEvent b{}; b.type = sds::GameEventType::WeaponFired;
        sds::GameEvent c{}; c.type = sds::GameEventType::GamePaused;
        expect(queue.push(a), "event queue accepts first event");
        expect(queue.push(b), "event queue accepts second event");
        expect(!queue.push(c), "event queue drops newest on overflow");
        sds::GameEvent closed{}; closed.type = sds::GameEventType::MenuClosed;
        expect(queue.push(closed), "event queue keeps menu close on overflow");
        expect(queue.push(a), "event queue keeps menu open on overflow");
        expect(!queue.push(c), "event queue drops other events above capacity");
        const auto first = queue.tryPop();
        const auto second = queue.tryPop();
        const auto third = queue.tryPop();
        const auto fourth = queue.tryPop();
        expect(first && first->type == sds::GameEventType::MenuOpened, "event queue preserves FIFO first");
        expect(second && second->type == sds::GameEventType::WeaponFired, "event queue preserves FIFO second");
        expect(third && third->type == sds::GameEventType::MenuClosed, "event queue preserves FIFO for kept menu close");
        expect(fourth && fourth->type == sds::GameEventType::MenuOpened, "event queue preserves FIFO for kept menu open");
        expect(!queue.tryPop().has_value(), "event queue empty returns nullopt");
        queue.stop();
        expect(queue.stopped(), "event queue reports stopped");
        expect(!queue.push(a), "event queue rejects pushes after stop");
    }

    {
        sds::Config effectConfig = sds::Config::defaults();
        effectConfig.triggerStrength = 1.0F;
        sds::EffectsEngine engine(effectConfig);

        sds::GameEvent healthy{};
        healthy.type = sds::GameEventType::PlayerHealthChanged;
        healthy.value = 0.75F;
        auto state = engine.handle(healthy);
        expect(state.output.lightbar == sds::Color{ 0, 64, 255 }, "effects health normal lightbar");

        sds::GameEvent warning = healthy;
        warning.value = 0.40F;
        state = engine.handle(warning);
        expect(state.output.lightbar == sds::Color{ 255, 160, 0 }, "effects health warning lightbar");

        sds::GameEvent critical = healthy;
        critical.value = 0.10F;
        state = engine.handle(critical);
        expect(state.output.lightbar == sds::Color{ 255, 0, 0 }, "effects health critical lightbar");

        sds::GameEvent equipped{};
        equipped.type = sds::GameEventType::WeaponEquipped;
        state = engine.handle(equipped);
        expect(state.output.rightTrigger.mode == sds::TriggerEffectMode::ContinuousResistance,
            "effects equipped weapon enables R2 resistance");
        expect(state.output.rightTrigger.force > 0, "effects equipped weapon has R2 force");

        sds::GameEvent fired{};
        fired.type = sds::GameEventType::WeaponFired;
        fired.when = std::chrono::steady_clock::now();
        state = engine.handle(fired);
        expect(state.transientTriggerActive, "effects weapon fire starts transient");
        expect(state.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "effects weapon fire uses trigger pulse");

        state = engine.tick(fired.when + std::chrono::milliseconds(100));
        expect(!state.transientTriggerActive, "effects transient expires");
        expect(state.output.rightTrigger.mode == sds::TriggerEffectMode::ContinuousResistance,
            "effects resumes equipped resistance after transient");
    }



    {
        const auto profiles = sds::weaponProfiles();
        expect(profiles.size() == 71, "weapon matrix exposes all 71 archetypes");

        bool everyProfileSelfMatches = true;
        for (const auto& profile : profiles) {
            const auto* found = sds::findWeaponProfile(profile.name);
            if (!found || found->name != profile.name) {
                everyProfileSelfMatches = false;
                break;
            }
        }
        expect(everyProfileSelfMatches, "every weapon archetype self-identifies by normalized name");

        const auto* eon = sds::findWeaponProfile("weap_pistol_eon|Eon");
        const auto* razorback = sds::findWeaponProfile("WEAP_Razorback|Razorback");
        const auto* breach = sds::findWeaponProfile("Breach");
        const auto* cutter = sds::findWeaponProfile("Cutter");
        const auto* maulingAxe = sds::findWeaponProfile("weap_mauling_axe|Mauling Axe");
        const auto* antiquePistol = sds::findWeaponProfile("Custom Antique Pistol");

        expect(eon && eon->r2Rating == 3 && eon->intensity == sds::WeaponIntensity::Light,
            "matrix maps Eon to Light R2 3");
        expect(razorback && razorback->r2Rating == 7 && razorback->intensity == sds::WeaponIntensity::VeryHeavy,
            "matrix maps Razorback to Very Heavy R2 7");
        expect(breach && breach->r2Rating == 8 && breach->triggerFamily == sds::WeaponTriggerFamily::Shotgun,
            "matrix maps Breach to Very Heavy shotgun R2 8");
        expect(cutter && cutter->r2Rating == 4 && cutter->triggerFamily == sds::WeaponTriggerFamily::SustainedEnergy,
            "matrix maps Cutter to continuous-beam R2 4");
        expect(maulingAxe && maulingAxe->r2Rating == 7 && maulingAxe->triggerFamily == sds::WeaponTriggerFamily::Melee,
            "matrix maps Mauling Axe to melee R2 7");
        expect(antiquePistol && antiquePistol->source == "Terran Armada",
            "matrix includes Terran Armada weapon profiles");
        expect(!sds::findWeaponProfile("Definitely Not A Starfield Weapon"),
            "weapon matrix leaves unknown identities unclassified");
    }

    {
        sds::Config effectConfig = sds::Config::defaults();
        effectConfig.triggerStrength = 1.0F;

        auto equipNamed = [](sds::EffectsEngine& engine, const char* identity) {
            sds::GameEvent event{};
            event.type = sds::GameEventType::WeaponEquipped;
            std::strncpy(event.text.data(), identity, event.text.size() - 1);
            return engine.handle(event);
        };

        sds::EffectsEngine eonEngine(effectConfig);
        sds::EffectsEngine razorEngine(effectConfig);
        sds::EffectsEngine launcherEngine(effectConfig);
        const auto eonState = equipNamed(eonEngine, "weap_eon|Eon");
        const auto razorState = equipNamed(razorEngine, "weap_razorback|Razorback");
        const auto launcherState = equipNamed(launcherEngine, "weap_bridger|Bridger");

        expect(eonEngine.equippedWeaponProfile() && eonEngine.equippedWeaponProfile()->name == "Eon",
            "effects engine retains classified equipped archetype");
        expect(eonState.output.rightTrigger.mode == sds::TriggerEffectMode::ContinuousResistance &&
                   razorState.output.rightTrigger.mode == sds::TriggerEffectMode::ContinuousResistance,
            "matrix-driven weapons use persistent R2 resistance");
        expect(eonState.output.rightTrigger.force < razorState.output.rightTrigger.force &&
                   razorState.output.rightTrigger.force < launcherState.output.rightTrigger.force,
            "R2 wall strength follows matrix rating order");
        expect(eonState.output.rightTrigger.startPosition > razorState.output.rightTrigger.startPosition &&
                   razorState.output.rightTrigger.startPosition > launcherState.output.rightTrigger.startPosition,
            "heavier R2 ratings engage resistance earlier in the pull");

        sds::EffectsEngine laserEngine(effectConfig);
        sds::EffectsEngine shotgunEngine(effectConfig);
        (void)equipNamed(laserEngine, "Solstice");
        (void)equipNamed(shotgunEngine, "Breach");
        sds::GameEvent fired{};
        fired.type = sds::GameEventType::WeaponFired;
        fired.when = std::chrono::steady_clock::now();
        const auto laserFire = laserEngine.handle(fired);
        const auto shotgunFire = shotgunEngine.handle(fired);
        expect(laserFire.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx &&
                   shotgunFire.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "profiled weapon fire uses EffectEx transient when a fire event is available");
        expect(laserFire.output.rightTrigger.frequency != shotgunFire.output.rightTrigger.frequency,
            "weapon families produce distinct trigger-pulse texture");

        sds::EffectsEngine unknownEngine(effectConfig);
        const auto unknownState = equipNamed(unknownEngine, "UnknownModdedWeapon");
        expect(!unknownEngine.equippedWeaponProfile() &&
                   unknownState.output.rightTrigger.mode == sds::TriggerEffectMode::ContinuousResistance,
            "unknown weapons fail soft to generic R2 resistance");
    }

    expect(HasR2InputState<sds::TouchState>,
        "USB input state exposes analog/digital R2");
    expect(HasLiveR2Handler<sds::EffectsEngine>,
        "effects engine consumes live R2 state");

    {
        std::array<std::uint8_t, 64> triggerReport{};
        triggerReport[0] = 0x01;
        triggerReport[5] = 0x33;  // L2 analog
        triggerReport[6] = 0xCC;  // R2 analog
        triggerReport[9] = 0x08;  // R2 digital bit
        const auto parsedTrigger = sds::parseUsbInputReport(triggerReport);
        expect(parsedTrigger && parsedTrigger->l2 == 0x33 && parsedTrigger->r2 == 0xCC && parsedTrigger->r2Button,
            "USB parser decodes live L2/R2 axes and R2 digital state");
    }

    {
        using namespace std::chrono_literals;
        sds::Config effectConfig = sds::Config::defaults();
        effectConfig.triggerStrength = 1.0F;
        const auto now = std::chrono::steady_clock::now();

        auto equipNamed = [](sds::EffectsEngine& engine, const char* identity) {
            sds::GameEvent event{};
            event.type = sds::GameEventType::WeaponEquipped;
            std::strncpy(event.text.data(), identity, event.text.size() - 1);
            return engine.handle(event);
        };

        // v0.2.37 keeps the proven deep-travel delivery and 48 ms wall-return
        // mechanism while modestly strengthening Eon's final pistol envelope.
        sds::EffectsEngine earlyEonEngine(effectConfig);
        const auto earlyWall = equipNamed(earlyEonEngine, "Eon").output.rightTrigger;
        sds::GameEvent earlyShot{};
        earlyShot.type = sds::GameEventType::WeaponFired;
        earlyShot.when = now + 1ms;
        std::strncpy(earlyShot.text.data(), "WeaponFire", earlyShot.text.size() - 1);
        const auto earlyPending = earlyEonEngine.handle(earlyShot);
        expect(!earlyPending.transientTriggerActive && earlyPending.output.rightTrigger == earlyWall,
            "Eon early WeaponFire waits for deep R2 travel before recoil");
        const auto early24 = earlyEonEngine.handleRightTriggerInput(24, now + 50ms);
        expect(!early24.transientTriggerActive && early24.output.rightTrigger == earlyWall,
            "Eon generic press threshold does not consume pending recoil");
        const auto early159 = earlyEonEngine.handleRightTriggerInput(159, now + 100ms);
        expect(!early159.transientTriggerActive && early159.output.rightTrigger == earlyWall,
            "Eon pending recoil remains armed below deep-travel threshold");
        const auto earlySynchronized = earlyEonEngine.handleRightTriggerInput(160, now + 120ms);
        expect(earlySynchronized.transientTriggerActive &&
                   earlySynchronized.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Eon pending recoil synchronizes to deep physical R2 travel");
        const auto earlyStillPulsing = earlyEonEngine.tick(now + 167ms);
        expect(earlyStillPulsing.transientTriggerActive &&
                   earlyStillPulsing.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Eon deep-travel pulse remains active through 47 ms");
        const auto earlyWallReturn = earlyEonEngine.tick(now + 168ms);
        expect(!earlyWallReturn.transientTriggerActive &&
                   earlyWallReturn.output.rightTrigger == earlyWall && earlyEonEngine.rightTriggerPressed(),
            "Eon deep-travel pulse restores wall at 48 ms while trigger is held");
        const auto earlyRelease = earlyEonEngine.handleRightTriggerInput(0, now + 180ms);
        expect(!earlyRelease.transientTriggerActive && earlyRelease.output.rightTrigger == earlyWall,
            "Eon release leaves the 48 ms wall return unchanged");

        sds::EffectsEngine dryFireEngine(effectConfig);
        const auto persistent = equipNamed(dryFireEngine, "Eon").output.rightTrigger;
        const auto pulled = dryFireEngine.handleRightTriggerInput(220, now);
        expect(dryFireEngine.rightTriggerPressed() && !pulled.transientTriggerActive &&
                   pulled.output.rightTrigger == persistent,
            "R2 pull alone never fabricates weapon recoil");

        sds::GameEvent confirmedShot{};
        confirmedShot.type = sds::GameEventType::WeaponFired;
        confirmedShot.when = now + 1ms;
        std::strncpy(confirmedShot.text.data(), "WeaponFire", confirmedShot.text.size() - 1);
        const auto fired = dryFireEngine.handle(confirmedShot);
        expect(fired.transientTriggerActive && fired.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "confirmed WeaponFired event starts profiled recoil pulse");

        // If R2 is already deep before WeaponFire arrives, Eon should start
        // the same one clean snap immediately.
        expect(fired.output.rightTrigger.startPosition == 108 &&
                   fired.output.rightTrigger.beginForce == 195 &&
                   fired.output.rightTrigger.middleForce == 255 &&
                   fired.output.rightTrigger.endForce == 175 &&
                   fired.output.rightTrigger.frequency == 76,
            "Eon final pistol recoil uses the stronger approved envelope");
        const auto eonStillSnapping = dryFireEngine.tick(now + 25ms);
        expect(eonStillSnapping.transientTriggerActive,
            "Eon single-snap recoil remains active through its impact peak");
        const auto eonAfterLegacyTimeout = dryFireEngine.tick(now + 45ms);
        expect(eonAfterLegacyTimeout.transientTriggerActive &&
                   eonAfterLegacyTimeout.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Eon one-shot recoil survives the legacy 36 ms timeout");
        const auto eonAtWallReturn = dryFireEngine.tick(now + 49ms);
        expect(!eonAtWallReturn.transientTriggerActive &&
                   eonAtWallReturn.output.rightTrigger == persistent && dryFireEngine.rightTriggerPressed(),
            "Eon returns its normal wall after 48 ms without waiting for release");
        const auto eonDelayedSamePull = dryFireEngine.handleRightTriggerInput(58, now + 80ms);
        expect(!eonDelayedSamePull.transientTriggerActive &&
                   eonDelayedSamePull.output.rightTrigger == persistent,
            "Eon keeps its wall during the remainder of the same held pull");
        const auto eonPostShotRelease = dryFireEngine.handleRightTriggerInput(0, now + 120ms);
        expect(!eonPostShotRelease.transientTriggerActive &&
                   eonPostShotRelease.output.rightTrigger == persistent,
            "Eon release leaves the timed wall return unchanged");
        confirmedShot.when = now + 160ms;
        const auto eonSecondPending = dryFireEngine.handle(confirmedShot);
        expect(!eonSecondPending.transientTriggerActive &&
                   eonSecondPending.output.rightTrigger == persistent,
            "Eon next early WeaponFire waits for its matching R2 press");
        const auto eonSecondShallow = dryFireEngine.handleRightTriggerInput(58, now + 180ms);
        expect(!eonSecondShallow.transientTriggerActive &&
                   eonSecondShallow.output.rightTrigger == persistent,
            "Eon next early WeaponFire remains pending on shallow R2 press");
        const auto eonSecondShot = dryFireEngine.handleRightTriggerInput(160, now + 190ms);
        expect(eonSecondShot.transientTriggerActive &&
                   eonSecondShot.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Eon next early WeaponFire recoils at matching deep R2 travel");
        const auto eonSecondWallReturn = dryFireEngine.tick(now + 239ms);
        expect(!eonSecondWallReturn.transientTriggerActive &&
                   eonSecondWallReturn.output.rightTrigger == persistent && dryFireEngine.rightTriggerPressed(),
            "Eon synchronized recoil repeats the 48 ms wall return on consecutive shots");
        const auto eonSecondRelease = dryFireEngine.handleRightTriggerInput(0, now + 250ms);
        expect(!eonSecondRelease.transientTriggerActive &&
                   eonSecondRelease.output.rightTrigger == persistent,
            "Eon synchronized release leaves the returned wall unchanged");

        sds::EffectsEngine maelstromEngine(effectConfig);
        const auto maelstromWall = equipNamed(maelstromEngine, "Maelstrom").output.rightTrigger;
        sds::GameEvent maelstromShot{};
        maelstromShot.type = sds::GameEventType::WeaponFired;
        maelstromShot.when = now + 100ms;
        std::strncpy(maelstromShot.text.data(), "WeaponFire", maelstromShot.text.size() - 1);
        const auto maelstromPulse = maelstromEngine.handle(maelstromShot);
        expect(maelstromPulse.transientTriggerActive &&
                   maelstromPulse.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx &&
                   maelstromPulse.output.rightTrigger.beginForce >= 175 &&
                   maelstromPulse.output.rightTrigger.middleForce >= 235 &&
                   maelstromPulse.output.rightTrigger.endForce >= 150 &&
                   maelstromPulse.output.rightTrigger.frequency >= 70 &&
                   maelstromPulse.output.rightTrigger.startPosition < maelstromWall.startPosition,
            "Maelstrom recoil uses a harder more aggressive per-shot pulse");

        sds::EffectsEngine cutterEngine(effectConfig);
        const auto cutterWall = equipNamed(cutterEngine, "Cutter").output.rightTrigger;
        confirmedShot.when = now + 2ms;
        confirmedShot.text.fill('\0');
        std::strncpy(confirmedShot.text.data(), "weaponFireStart", confirmedShot.text.size() - 1);
        const auto sustained = cutterEngine.handle(confirmedShot);
        expect(cutterEngine.sustainedFireActive() && sustained.transientTriggerActive &&
                   sustained.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx &&
                   sustained.output.rightTrigger.keepEffect,
            "confirmed Cutter fire-start marker begins sustained trigger texture before USB R2 polling catches up");
        expect(sustained.output.rightTrigger.frequency <= 30 &&
                   sustained.output.rightTrigger.middleForce >= 190,
            "Cutter sustained texture is a slow heavy construction-equipment throb");
        confirmedShot.when = now + 3ms;
        confirmedShot.text.fill('\0');
        std::strncpy(confirmedShot.text.data(), "WeaponFire", confirmedShot.text.size() - 1);
        const auto repeatedBeamMarker = cutterEngine.handle(confirmedShot);
        expect(repeatedBeamMarker.output.rightTrigger == sustained.output.rightTrigger &&
                   cutterEngine.sustainedFireActive(),
            "repeated Cutter WeaponFire markers do not become gun-like recoil kicks");
        confirmedShot.when = now + 203ms;
        const auto laterBeamHeartbeat = cutterEngine.handle(confirmedShot);
        expect(laterBeamHeartbeat.output.rightTrigger == sustained.output.rightTrigger &&
                   cutterEngine.sustainedFireActive(),
            "repeated Cutter WeaponFire refreshes sustained-fire liveness without changing texture");
        const auto stillSustained = cutterEngine.tick(now + 450ms);
        expect(stillSustained.transientTriggerActive && cutterEngine.sustainedFireActive(),
            "recent Cutter WeaponFire heartbeat keeps sustained trigger texture alive");
        const auto depleted = cutterEngine.tick(now + 504ms);
        expect(!depleted.transientTriggerActive && !cutterEngine.sustainedFireActive() &&
                   depleted.output.rightTrigger == cutterWall,
            "missing Cutter heartbeat stops sustained trigger texture when beam energy is depleted");
        confirmedShot.when = now + 600ms;
        (void)cutterEngine.handle(confirmedShot);
        expect(!cutterEngine.sustainedFireActive(),
            "WeaponFire heartbeat alone cannot restart Cutter sustained texture after timeout");
        confirmedShot.when = now + 700ms;
        confirmedShot.text.fill('\0');
        std::strncpy(confirmedShot.text.data(), "weaponFireStart", confirmedShot.text.size() - 1);
        (void)cutterEngine.handle(confirmedShot);
        (void)cutterEngine.handleRightTriggerInput(230, now + 750ms);
        const auto released = cutterEngine.handleRightTriggerInput(0, now + 2100ms);
        expect(!cutterEngine.sustainedFireActive() && !released.transientTriggerActive &&
                   released.output.rightTrigger == cutterWall,
            "R2 release ends Cutter texture and restores matrix wall");

        sds::EffectsEngine arcWelderEngine(effectConfig);
        const auto arcWelderWall = equipNamed(arcWelderEngine, "Arc Welder").output.rightTrigger;
        sds::GameEvent arcWelderStart{};
        arcWelderStart.type = sds::GameEventType::WeaponFired;
        arcWelderStart.when = now + 800ms;
        std::strncpy(arcWelderStart.text.data(), "weaponFireStart", arcWelderStart.text.size() - 1);
        const auto arcWelderStarted = arcWelderEngine.handle(arcWelderStart);
        expect(arcWelderEngine.sustainedFireActive() && arcWelderStarted.transientTriggerActive &&
                   arcWelderStarted.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Arc Welder weaponFireStart begins sustained trigger cadence");
        (void)arcWelderEngine.handleRightTriggerInput(200, now + 850ms);

        sds::GameEvent arcWelderEnd{};
        arcWelderEnd.type = sds::GameEventType::WeaponFired;
        arcWelderEnd.when = now + 900ms;
        std::strncpy(arcWelderEnd.text.data(), "weaponFireEnd", arcWelderEnd.text.size() - 1);
        const auto arcWelderStopped = arcWelderEngine.handle(arcWelderEnd);
        expect(!arcWelderEngine.sustainedFireActive() && !arcWelderStopped.transientTriggerActive &&
                   arcWelderStopped.output.rightTrigger == arcWelderWall,
            "Arc Welder weaponFireEnd immediately restores ready trigger wall while R2 remains held");

        const auto arcWelderLateHeld = arcWelderEngine.handleRightTriggerInput(210, now + 950ms);
        expect(!arcWelderEngine.sustainedFireActive() && !arcWelderLateHeld.transientTriggerActive &&
                   arcWelderLateHeld.output.rightTrigger == arcWelderWall,
            "late held R2 cannot resurrect Arc Welder trigger cadence after weaponFireEnd");

        sds::EffectsEngine microgunEngine(effectConfig);
        const auto microgunWall = equipNamed(microgunEngine, "Microgun").output.rightTrigger;

        // Microgun is a delayed-spin-up heavy automatic. R2 alone must not
        // fabricate recoil. Once real WeaponFire markers begin, each short
        // kick must end at 16 ms even if another marker arrives during it, so
        // the resistance wall can mechanically retrigger between kicks.
        const auto microgunPressOnly = microgunEngine.handleRightTriggerInput(39, now + 1300ms);
        expect(!microgunPressOnly.transientTriggerActive && microgunPressOnly.output.rightTrigger == microgunWall,
            "Microgun trigger input alone does not fabricate recoil during spin-up");

        sds::GameEvent microgunShot{};
        microgunShot.type = sds::GameEventType::WeaponFired;
        microgunShot.when = now + 2200ms;
        std::strncpy(microgunShot.text.data(), "WeaponFire", microgunShot.text.size() - 1);
        const auto microgunPulse = microgunEngine.handle(microgunShot);
        expect(microgunPulse.transientTriggerActive && !microgunEngine.sustainedFireActive() &&
                   microgunPulse.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx &&
                   !microgunPulse.output.rightTrigger.keepEffect,
            "rapid ballistic weapons keep real per-shot cadence even when their matrix cadence class is Sustained");
        expect(microgunPulse.output.rightTrigger.startPosition == 90 &&
                   microgunPulse.output.rightTrigger.beginForce == 210 &&
                   microgunPulse.output.rightTrigger.middleForce == 255 &&
                   microgunPulse.output.rightTrigger.endForce == 190 &&
                   microgunPulse.output.rightTrigger.frequency == 40,
            "Microgun v0.2.39 uses the approved heavy-kick force envelope");

        auto overlappingMicrogunShot = microgunShot;
        overlappingMicrogunShot.when = now + 2208ms;
        (void)microgunEngine.handle(overlappingMicrogunShot);
        const auto microgunBeforeReturn = microgunEngine.tick(now + 2215ms);
        expect(microgunBeforeReturn.transientTriggerActive,
            "Microgun short kick remains active through 15 ms");
        const auto microgunReturnedWall = microgunEngine.tick(now + 2216ms);
        expect(!microgunReturnedWall.transientTriggerActive &&
                   microgunReturnedWall.output.rightTrigger == microgunWall,
            "Microgun overlapping marker cannot extend kick beyond 16 ms wall return");

        auto retriggerMicrogunShot = microgunShot;
        retriggerMicrogunShot.when = now + 2228ms;
        const auto microgunRetrigger = microgunEngine.handle(retriggerMicrogunShot);
        expect(microgunRetrigger.transientTriggerActive &&
                   microgunRetrigger.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Microgun next real shot retriggers after wall return");
        const auto microgunRetriggerReturn = microgunEngine.tick(now + 2244ms);
        expect(!microgunRetriggerReturn.transientTriggerActive &&
                   microgunRetriggerReturn.output.rightTrigger == microgunWall,
            "Microgun retriggered kick returns to wall after 16 ms");

        sds::EffectsEngine negotiatorEngine(effectConfig);
        const auto negotiatorWall = equipNamed(negotiatorEngine, "Negotiator").output.rightTrigger;
        sds::GameEvent negotiatorShot{};
        negotiatorShot.type = sds::GameEventType::WeaponFired;
        negotiatorShot.when = now + 2300ms;
        std::strncpy(negotiatorShot.text.data(), "WeaponFire", negotiatorShot.text.size() - 1);
        const auto negotiatorPulse = negotiatorEngine.handle(negotiatorShot);
        expect(negotiatorPulse.transientTriggerActive &&
                   negotiatorPulse.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Negotiator real WeaponFire starts one launcher kick");
        expect(negotiatorPulse.output.rightTrigger.startPosition == 74 &&
                   negotiatorPulse.output.rightTrigger.beginForce == 240 &&
                   negotiatorPulse.output.rightTrigger.middleForce == 255 &&
                   negotiatorPulse.output.rightTrigger.endForce == 220 &&
                   negotiatorPulse.output.rightTrigger.frequency == 28,
            "Negotiator v0.2.40 uses the approved much-heavier launcher envelope");
        const auto negotiatorBeforeReturn = negotiatorEngine.tick(now + 2354ms);
        expect(negotiatorBeforeReturn.transientTriggerActive &&
                   negotiatorBeforeReturn.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Negotiator hard-snap kick remains active through 54 ms");
        const auto negotiatorReturnedWall = negotiatorEngine.tick(now + 2355ms);
        expect(!negotiatorReturnedWall.transientTriggerActive &&
                   negotiatorReturnedWall.output.rightTrigger == negotiatorWall,
            "Negotiator hard-snap timing returns to wall at 55 ms");

        sds::EffectsEngine bridgerEngine(effectConfig);
        const auto bridgerWall = equipNamed(bridgerEngine, "Bridger").output.rightTrigger;
        sds::GameEvent bridgerShot{};
        bridgerShot.type = sds::GameEventType::WeaponFired;
        bridgerShot.when = now + 2400ms;
        std::strncpy(bridgerShot.text.data(), "WeaponFire", bridgerShot.text.size() - 1);
        const auto bridgerPulse = bridgerEngine.handle(bridgerShot);
        expect(bridgerPulse.transientTriggerActive &&
                   bridgerPulse.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Bridger real WeaponFire starts one launcher kick");
        expect(bridgerPulse.output.rightTrigger.startPosition == 74 &&
                   bridgerPulse.output.rightTrigger.beginForce == 240 &&
                   bridgerPulse.output.rightTrigger.middleForce == 255 &&
                   bridgerPulse.output.rightTrigger.endForce == 220 &&
                   bridgerPulse.output.rightTrigger.frequency == 28,
            "Bridger v0.2.42 matches the approved heavy launcher envelope");
        const auto bridgerBeforeReturn = bridgerEngine.tick(now + 2454ms);
        expect(bridgerBeforeReturn.transientTriggerActive &&
                   bridgerBeforeReturn.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
            "Bridger hard-snap kick remains active through 54 ms");
        const auto bridgerReturnedWall = bridgerEngine.tick(now + 2455ms);
        expect(!bridgerReturnedWall.transientTriggerActive &&
                   bridgerReturnedWall.output.rightTrigger == bridgerWall,
            "Bridger hard-snap returns to wall at 55 ms");

        sds::EffectsEngine swapEngine(effectConfig);
        (void)equipNamed(swapEngine, "Cutter");
        sds::GameEvent cutterStart{};
        cutterStart.type = sds::GameEventType::WeaponFired;
        cutterStart.when = now;
        std::strncpy(cutterStart.text.data(), "weaponFireStart", cutterStart.text.size() - 1);
        (void)swapEngine.handle(cutterStart);
        const auto swapped = equipNamed(swapEngine, "Maelstrom");
        expect(!swapEngine.sustainedFireActive() && !swapped.transientTriggerActive &&
                   swapped.output.rightTrigger.mode == sds::TriggerEffectMode::ContinuousResistance,
            "weapon swap terminates Cutter sustained texture and applies the new weapon wall");

        sds::EffectsEngine chargeEngine(effectConfig);
        const auto baseCharge = equipNamed(chargeEngine, "Magsniper").output.rightTrigger;
        const auto chargedPull = chargeEngine.handleRightTriggerInput(240, now);
        expect(chargedPull.output.rightTrigger.mode == sds::TriggerEffectMode::ContinuousResistance &&
                   chargedPull.output.rightTrigger.force >= baseCharge.force,
            "charged weapons tighten resistance as R2 pull increases");
    }

    expect(HasCreateButtonState<sds::TouchState>,
        "touch state exposes Create button state");
    expect(HasTouchActionMapper<sds::TouchGesture>,
        "touch gestures expose a shortcut-action mapper");
    expect(HasInputActionQueue<sds::ControllerManager>,
        "controller manager exposes detected shortcut actions to the game thread");
    expect(HasNativeUserEventMapper<sds::InputAction>,
        "shortcut actions expose Starfield native user events");

    {
        expect(sds::nativeUserEventForInputAction(sds::InputAction::OpenInventory) == "QuickInventory",
            "Inventory action uses native QuickInventory event");
        expect(sds::nativeUserEventForInputAction(sds::InputAction::OpenMissions) == "QuickMission",
            "Missions action uses native QuickMission event");
        expect(sds::nativeUserEventForInputAction(sds::InputAction::OpenDataMenu) == "DataMenu",
            "Data action uses native DataMenu event");
        expect(sds::nativeUserEventForInputAction(sds::InputAction::OpenSkills) == "QuickSkills",
            "Skills action uses native QuickSkills event");
        expect(sds::nativeUserEventForInputAction(sds::InputAction::OpenMap) == "QuickMap",
            "Map action uses native QuickMap event");
        expect(sds::nativeUserEventForInputAction(sds::InputAction::OpenPowers) == "QuickPowers",
            "Powers action uses native QuickPowers event");
        expect(sds::nativeUserEventForInputAction(sds::InputAction::OpenPhotoMode) == "Monocle",
            "Photo Mode action begins through native Monocle event");
    }

    {
        const auto up = sds::mapTouchGestureToInputAction(sds::TouchGesture::SwipeUp);
        const auto down = sds::mapTouchGestureToInputAction(sds::TouchGesture::SwipeDown);
        const auto left = sds::mapTouchGestureToInputAction(sds::TouchGesture::SwipeLeft);
        const auto right = sds::mapTouchGestureToInputAction(sds::TouchGesture::SwipeRight);
        const auto click = sds::mapTouchGestureToInputAction(sds::TouchGesture::RightClick);
        const auto hold = sds::mapTouchGestureToInputAction(sds::TouchGesture::RightHold);
        const auto create = sds::mapTouchGestureToInputAction(sds::TouchGesture::CreatePressed);
        expect(up && *up == sds::InputAction::OpenInventory, "swipe up maps to Inventory");
        expect(down && *down == sds::InputAction::OpenMissions, "swipe down maps to Missions");
        expect(left && *left == sds::InputAction::OpenPowers, "swipe left maps to Powers");
        expect(right && *right == sds::InputAction::OpenSkills, "swipe right maps to Skills");
        expect(click && *click == sds::InputAction::OpenMap, "right click maps to Map");
        expect(!hold.has_value(), "right hold has no shortcut mapping");
        expect(create && *create == sds::InputAction::OpenPhotoMode, "Create maps to Photo Mode");
        expect(!sds::mapTouchGestureToInputAction(sds::TouchGesture::None).has_value(),
            "non-action touch gesture maps to no shortcut");
    }

    {
        std::array<std::uint8_t, 64> report{};
        report[0] = 0x01;
        // buttonsA (Create) is payload offset 0x08/report byte 0x09.
        report[0x09] = 0x10;
        // buttonsB (touchpad click) is payload offset 0x09/report byte 0x0A.
        report[0x0A] = 0x02;

        auto encodeTouch = [&](std::size_t reportOffset, std::uint8_t id, bool down, std::uint16_t x, std::uint16_t y) {
            report[reportOffset + 0] = static_cast<std::uint8_t>((id & 0x7F) | (down ? 0x00 : 0x80));
            report[reportOffset + 1] = static_cast<std::uint8_t>(x & 0xFF);
            report[reportOffset + 2] = static_cast<std::uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
            report[reportOffset + 3] = static_cast<std::uint8_t>((y >> 4) & 0xFF);
        };

        // Payload 0x20/0x24 -> report 0x21/0x25.
        encodeTouch(0x21, 7, true, 1234, 567);
        encodeTouch(0x25, 9, false, 1800, 900);

        const auto touch = sds::parseUsbInputReport(report);
        expect(touch.has_value(), "touch parser accepts 64-byte USB report");
        expect(touch && touch->click, "touch parser reads touchpad click");
        expect(touch && touch->create, "touch parser reads Create button");
        expect(touch && touch->first.down && touch->first.id == 7, "touch parser reads first finger active/id");
        expect(touch && touch->first.x == 1234 && touch->first.y == 567, "touch parser decodes first finger coordinates");
        expect(touch && !touch->second.down && touch->second.id == 9, "touch parser reads second finger inactive/id");
        expect(touch && touch->second.x == 1800 && touch->second.y == 900, "touch parser decodes second finger coordinates");

        auto shortReport = std::span<const std::uint8_t>(report.data(), 20);
        expect(!sds::parseUsbInputReport(shortReport).has_value(), "touch parser rejects short report");
        report[0] = 0x31;
        expect(!sds::parseUsbInputReport(report).has_value(), "touch parser rejects non-USB report id");
    }

    {
        sds::TouchGestureTracker tracker(250);
        const auto t0 = std::chrono::steady_clock::now();
        sds::TouchState state{};

        // Left-side physical click is surfaced as LeftClick so the transport
        // layer can leave USB native while Bluetooth bridges TogglePOV.
        state.first = { 300, 500, 1, true };
        state.click = true;
        expect(tracker.update(state, t0) == sds::TouchGesture::LeftClick,
            "gesture tracker exposes left touchpad click for transport-aware POV handling");
        state.click = false;
        expect(tracker.update(state, t0 + std::chrono::milliseconds(10)) == sds::TouchGesture::None,
            "gesture tracker leaves left touchpad release untouched");

        constexpr auto expectedRightClick = sds::TouchGesture::RightClick;

        sds::TouchGestureTracker rightClickTracker(250);
        sds::TouchState rightClick{};
        rightClick.first = { 1500, 500, 2, true };
        rightClick.click = true;
        expect(rightClickTracker.update(rightClick, t0 + std::chrono::milliseconds(20)) == expectedRightClick,
            "right touchpad click emits immediately for Map");
        expect(rightClickTracker.update(rightClick, t0 + std::chrono::milliseconds(950)) == sds::TouchGesture::None,
            "holding right touchpad click emits no second gesture");
        rightClick.click = false;
        expect(rightClickTracker.update(rightClick, t0 + std::chrono::milliseconds(980)) == sds::TouchGesture::None,
            "right touchpad release emits no delayed action");

        constexpr auto expectedCreatePressed = sds::TouchGesture::CreatePressed;
        sds::TouchGestureTracker createTracker(250);
        sds::TouchState createState{};
        createState.create = true;
        expect(createTracker.update(createState, t0 + std::chrono::milliseconds(1000)) == expectedCreatePressed,
            "Create button press emits photo-mode gesture");
        expect(createTracker.update(createState, t0 + std::chrono::milliseconds(1010)) == sds::TouchGesture::None,
            "Create button hold does not repeat gesture");
        createState.create = false;
        expect(createTracker.update(createState, t0 + std::chrono::milliseconds(1020)) == sds::TouchGesture::None,
            "Create button release does not emit a second action");

        state = {};
        sds::TouchGestureTracker swipeTracker(250);
        state.first = { 300, 500, 1, true };
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(20)) == sds::TouchGesture::None,
            "gesture tracker starts touch without gesture");
        state.first = { 900, 520, 1, true };
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(100)) == sds::TouchGesture::None,
            "gesture tracker waits for touch release");
        state.first.down = false;
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(150)) == sds::TouchGesture::SwipeRight,
            "gesture tracker detects right swipe");

        state.first = { 1000, 800, 2, true };
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(200)) == sds::TouchGesture::None,
            "gesture tracker starts vertical swipe without gesture");
        state.first = { 990, 300, 2, true };
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(260)) == sds::TouchGesture::None,
            "gesture tracker tracks vertical swipe until release");
        state.first.down = false;
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(300)) == sds::TouchGesture::SwipeUp,
            "gesture tracker detects up swipe");

        state.first = { 500, 500, 3, true };
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(400)) == sds::TouchGesture::None,
            "gesture tracker starts tiny motion sample");
        state.first = { 560, 540, 3, true };
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(450)) == sds::TouchGesture::None,
            "gesture tracker holds tiny motion until release");
        state.first.down = false;
        expect(swipeTracker.update(state, t0 + std::chrono::milliseconds(500)) == sds::TouchGesture::None,
            "gesture tracker ignores tiny motion");
    }


    {
        sds::OutputState output{};
        output.lightbar = { 12, 34, 56 };
        output.playerLeds = 0x04;
        output.rightTrigger.mode = sds::TriggerEffectMode::ContinuousResistance;
        output.rightTrigger.startPosition = 81;
        output.rightTrigger.force = 123;
        output.leftTrigger.mode = sds::TriggerEffectMode::EffectEx;
        output.leftTrigger.startPosition = 100;
        output.leftTrigger.keepEffect = true;
        output.leftTrigger.beginForce = 11;
        output.leftTrigger.middleForce = 22;
        output.leftTrigger.endForce = 33;
        output.leftTrigger.frequency = 50;

        const auto report = sds::buildUsbOutputReport(output);
        expect(report.size() == 48, "USB output report is 48 bytes");
        expect(report[0] == 0x02, "USB output report id");
        expect(report[1] == 0x0C && report[2] == 0x14,
            "USB output owns adaptive triggers, lightbar, and player indicators");
        expect((report[0x2C] & 0x20) == 0,
            "steady USB output omits one-shot lightbar setup command");
        expect(report[0x0B] == 0x01 && report[0x0C] == 81 && report[0x0D] == 123,
            "USB output encodes R2 continuous resistance");
        expect(report[0x16] == 0x26, "USB output encodes L2 EffectEx mode");
        expect(report[0x17] == static_cast<std::uint8_t>(0xFF - 100), "USB output encodes EffectEx start");
        expect(report[0x18] == 0x02, "USB output encodes EffectEx keep flag");
        expect(report[0x1A] == 11 && report[0x1B] == 22 && report[0x1C] == 33,
            "USB output encodes EffectEx forces");
        expect(report[0x1F] == 25, "USB output encodes EffectEx frequency");
        expect((report[0x2C] & 0x1F) == 0x04,
            "USB output encodes requested player LED mask");
        expect(report[0x2D] == 12 && report[0x2E] == 34 && report[0x2F] == 56,
            "USB output encodes lightbar RGB");
    }

    {
        const auto report = sds::buildUsbOutputReport({});
        expect(report[0] == 0x02, "reset output retains USB report id");
        expect(report[0x0B] == 0x00 && report[0x16] == 0x00, "reset output disables triggers");
        expect(report[0x2D] == 0 && report[0x2E] == 0 && report[0x2F] == 0,
            "reset output clears lightbar");
    }


    {
        auto fake = std::make_shared<FakeBackendState>();
        fake->failuresBeforeConnect = 2;
        fake->caps = {
            .adaptiveTriggers = true,
            .lightbar = true,
            .touchpadInput = false,
            .advancedHaptics = false,
            .controllerSpeaker = false,
            .bluetoothTransport = false
        };

        sds::ControllerManager manager(
            sds::Config::defaults(),
            [fake] { return std::make_unique<FakeBackend>(fake); },
            {},
            std::chrono::milliseconds(40));

        manager.start();
        expect(manager.enqueue(sds::GameEvent{ .type = sds::GameEventType::PlayerHealthChanged, .value = 0.2F }),
            "controller manager accepts event while disconnected");
        expect(waitUntil(std::chrono::milliseconds(250), [&] { return manager.connected(); }),
            "controller manager reconnects after failed attempts");
        expect(fake->connectAttempts.load() >= 3, "controller manager retried connection");
        expect(fake->connectAttempts.load() < 8, "controller manager reconnect uses backoff");
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return fake->lightbarCalls.load() > 0; }),
            "controller manager applies persistent lightbar after reconnect");

        const auto beforeStop = std::chrono::steady_clock::now();
        manager.stop();
        const auto stopElapsed = std::chrono::steady_clock::now() - beforeStop;
        expect(stopElapsed < std::chrono::milliseconds(250), "controller manager stops promptly");
        expect(fake->resetCalls.load() > 0 && fake->disconnectCalls.load() > 0,
            "controller manager resets and disconnects backend on stop");
    }

    {
        auto fake = std::make_shared<FakeBackendState>();
        fake->caps = {
            .adaptiveTriggers = false,
            .lightbar = true,
            .touchpadInput = false,
            .advancedHaptics = false,
            .controllerSpeaker = false,
            .bluetoothTransport = false
        };
        sds::ControllerManager manager(
            sds::Config::defaults(),
            [fake] { return std::make_unique<FakeBackend>(fake); },
            {},
            std::chrono::milliseconds(20));
        manager.start();
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return manager.connected(); }),
            "controller manager connects fake backend");

        sds::GameEvent equipped{};
        equipped.type = sds::GameEventType::WeaponEquipped;
        expect(manager.enqueue(equipped), "controller manager accepts equipped event");
        sds::GameEvent health{};
        health.type = sds::GameEventType::PlayerHealthChanged;
        health.value = 0.4F;
        expect(manager.enqueue(health), "controller manager accepts health event");

        expect(waitUntil(std::chrono::milliseconds(100), [&] { return fake->lightbarCalls.load() > 0; }),
            "controller manager applies supported lightbar capability");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        expect(fake->triggerCalls.load() == 0,
            "controller manager gates unsupported adaptive triggers");
        manager.stop();
    }

    {
        auto fake = std::make_shared<FakeBackendState>();
        fake->caps = {
            .adaptiveTriggers = true,
            .lightbar = true,
            .touchpadInput = true,
            .advancedHaptics = false,
            .controllerSpeaker = false,
            .bluetoothTransport = false
        };
        std::atomic<bool> sawIdentityLog{ false };
        std::atomic<bool> sawCapabilityLog{ false };
        sds::ControllerManager manager(
            sds::Config::defaults(),
            [fake] { return std::make_unique<FakeBackend>(fake); },
            [&](std::string_view message) {
                if (message.find("DualSense") != std::string_view::npos) {
                    sawIdentityLog = true;
                }
                if (message.find("triggers=yes") != std::string_view::npos &&
                    message.find("touchpad=yes") != std::string_view::npos) {
                    sawCapabilityLog = true;
                }
            },
            std::chrono::milliseconds(20));
        manager.start();
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return manager.connected(); }),
            "controller manager connects for diagnostic logging");
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return sawIdentityLog.load(); }),
            "controller manager logs controller identity");
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return sawCapabilityLog.load(); }),
            "controller manager logs controller capabilities");
        manager.stop();
    }


    {
        auto fake = std::make_shared<FakeBackendState>();
        fake->caps = {
            .adaptiveTriggers = false,
            .lightbar = false,
            .touchpadInput = true,
            .advancedHaptics = false,
            .controllerSpeaker = false,
            .bluetoothTransport = false
        };
        sds::ControllerManager manager(
            sds::Config::defaults(),
            [fake] { return std::make_unique<FakeBackend>(fake); },
            {},
            std::chrono::milliseconds(20));
        manager.start();
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return manager.connected(); }),
            "controller manager connects for touch shortcut actions");

        sds::TouchState sample{};
        sample.first = { 1000, 800, 11, true };
        pushTouch(fake, sample);
        sample.first = { 990, 300, 11, true };
        pushTouch(fake, sample);
        sample.first.down = false;
        pushTouch(fake, sample);

        std::optional<sds::InputAction> action;
        expect(waitUntil(std::chrono::milliseconds(150), [&] {
            action = manager.tryPopInputAction();
            return action.has_value();
        }), "controller manager forwards detected touch shortcut to game thread");
        expect(action && *action == sds::InputAction::OpenInventory,
            "controller manager forwards swipe-up Inventory action");
        manager.stop();
    }

    {
        auto fake = std::make_shared<FakeBackendState>();
        fake->caps = {
            .adaptiveTriggers = true,
            .lightbar = true,
            .touchpadInput = false,
            .advancedHaptics = false,
            .controllerSpeaker = false,
            .bluetoothTransport = false
        };
        sds::ControllerManager manager(
            sds::Config::defaults(),
            [fake] { return std::make_unique<FakeBackend>(fake); },
            {},
            std::chrono::milliseconds(20),
            std::chrono::milliseconds(10));
        manager.start();
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return manager.connected(); }),
            "controller manager connects for stable output test");

        sds::GameEvent closeMainMenu{};
        closeMainMenu.type = sds::GameEventType::MenuClosed;
        constexpr std::string_view mainMenuName = "MainMenu";
        std::memcpy(
            closeMainMenu.text.data(),
            mainMenuName.data(),
            mainMenuName.size());
        expect(manager.enqueue(closeMainMenu),
            "stable output test exits MainMenu animation");

        sds::GameEvent health{};
        health.type = sds::GameEventType::PlayerHealthChanged;
        health.value = 0.75F;
        expect(manager.enqueue(health), "controller manager accepts stable-output health event");
        expect(waitUntil(std::chrono::milliseconds(100), [&] { return fake->lightbarCalls.load() > 0; }),
            "controller manager applies changed output");

        // v0.2.27b+ intentionally does not periodically rewrite an unchanged
        // DualSense output state. Give the worker time to settle, snapshot the
        // backend call counts, then verify they remain stable well beyond the
        // legacy 10 ms keepalive interval supplied above.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto stableLightbarCalls = fake->lightbarCalls.load();
        const auto stableTriggerCalls = fake->triggerCalls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        expect(fake->lightbarCalls.load() == stableLightbarCalls &&
                   fake->triggerCalls.load() == stableTriggerCalls,
            "controller manager does not periodically rewrite unchanged output");
        manager.stop();
    }


    {
        auto observerFake = std::make_shared<FakeBackendState>();
        observerFake->caps = {
            .adaptiveTriggers = false,
            .lightbar = false,
            .touchpadInput = true,
            .advancedHaptics = false,
            .controllerSpeaker = false,
            .bluetoothTransport = false
        };

        sds::TouchState first{};
        first.r2 = 31;
        pushTouch(observerFake, first);

        sds::TouchState sameBucket{};
        sameBucket.r2 = 30;
        pushTouch(observerFake, sameBucket);

        sds::TouchState nextBucket{};
        nextBucket.r2 = 48;
        pushTouch(observerFake, nextBucket);

        std::mutex observedMutex;
        std::vector<std::uint8_t> observedR2;
        sds::Config observerConfig = sds::Config::defaults();
        observerConfig.adaptiveTriggers = false;
        observerConfig.lightbar = false;
        observerConfig.touchpad = false;
        observerConfig.advancedHaptics = true;
        sds::ControllerManager observerManager(
            observerConfig,
            [observerFake] { return std::make_unique<FakeBackend>(observerFake); },
            {},
            std::chrono::milliseconds(1),
            std::chrono::milliseconds(8),
            [&](std::uint8_t r2, std::chrono::steady_clock::time_point) {
                std::scoped_lock lock(observedMutex);
                observedR2.push_back(r2);
            });

        observerManager.start();
        expect(waitUntil(std::chrono::milliseconds(250), [&] {
            return observerFake->pollTouchCalls.load() >= 3;
        }), "installed R2 observer causes input polling with touchpad/adaptive consumption disabled");

        expect(waitUntil(std::chrono::milliseconds(250), [&] {
            std::scoped_lock lock(observedMutex);
            return observedR2.size() >= 2;
        }), "R2 observer receives first sample and bucket change");

        {
            std::scoped_lock lock(observedMutex);
            expect(observedR2.size() >= 2 &&
                   observedR2[0] == 31 &&
                   observedR2[1] == 48,
                "R2 observer suppresses unchanged samples inside the same 16-value bucket");
        }
        observerManager.stop();
        {
            std::scoped_lock lock(observedMutex);
            expect(!observedR2.empty() && observedR2.back() == 0,
                "controller worker shutdown sends one R2 zero clear");
        }
    }

    {
        auto thresholdFake = std::make_shared<FakeBackendState>();
        thresholdFake->caps.touchpadInput = true;
        sds::TouchState belowThreshold{};
        belowThreshold.r2 = 20;
        pushTouch(thresholdFake, belowThreshold);
        sds::TouchState atThreshold{};
        atThreshold.r2 = 24;
        pushTouch(thresholdFake, atThreshold);
        sds::TouchState backBelow{};
        backBelow.r2 = 23;
        pushTouch(thresholdFake, backBelow);

        std::mutex thresholdMutex;
        std::vector<std::uint8_t> thresholdObserved;
        sds::Config thresholdConfig = sds::Config::defaults();
        thresholdConfig.adaptiveTriggers = false;
        thresholdConfig.lightbar = false;
        thresholdConfig.touchpad = false;
        thresholdConfig.advancedHaptics = true;
        sds::ControllerManager thresholdManager(
            thresholdConfig,
            [thresholdFake] { return std::make_unique<FakeBackend>(thresholdFake); },
            {}, std::chrono::milliseconds(1), std::chrono::milliseconds(8),
            [&](std::uint8_t r2, std::chrono::steady_clock::time_point) {
                std::scoped_lock lock(thresholdMutex);
                thresholdObserved.push_back(r2);
            });
        thresholdManager.start();
        expect(waitUntil(std::chrono::milliseconds(250), [&] {
            std::scoped_lock lock(thresholdMutex);
            return thresholdObserved.size() >= 3;
        }), "R2 observer delivers Novablast threshold crossings inside one bucket");
        {
            std::scoped_lock lock(thresholdMutex);
            expect(thresholdObserved.size() >= 3 &&
                   thresholdObserved[0] == 20 &&
                   thresholdObserved[1] == 24 &&
                   thresholdObserved[2] == 23,
                "R2 observer preserves exact Novablast start/stop threshold at raw 24");
        }
        thresholdManager.stop();
    }

    {
        auto releaseThresholdFake = std::make_shared<FakeBackendState>();
        releaseThresholdFake->caps.touchpadInput = true;
        sds::TouchState aboveRelease{};
        aboveRelease.r2 = 15;
        pushTouch(releaseThresholdFake, aboveRelease);
        sds::TouchState atRelease{};
        atRelease.r2 = 12;
        pushTouch(releaseThresholdFake, atRelease);

        std::mutex releaseThresholdMutex;
        std::vector<std::uint8_t> releaseThresholdObserved;
        sds::Config releaseThresholdConfig = sds::Config::defaults();
        releaseThresholdConfig.adaptiveTriggers = false;
        releaseThresholdConfig.lightbar = false;
        releaseThresholdConfig.touchpad = false;
        releaseThresholdConfig.advancedHaptics = true;
        sds::ControllerManager releaseThresholdManager(
            releaseThresholdConfig,
            [releaseThresholdFake] { return std::make_unique<FakeBackend>(releaseThresholdFake); },
            {}, std::chrono::milliseconds(1), std::chrono::milliseconds(8),
            [&](std::uint8_t r2, std::chrono::steady_clock::time_point) {
                std::scoped_lock lock(releaseThresholdMutex);
                releaseThresholdObserved.push_back(r2);
            });
        releaseThresholdManager.start();
        expect(waitUntil(std::chrono::milliseconds(250), [&] {
            std::scoped_lock lock(releaseThresholdMutex);
            return releaseThresholdObserved.size() >= 2;
        }), "R2 observer delivers Cutter release threshold crossing inside bucket zero");
        {
            std::scoped_lock lock(releaseThresholdMutex);
            expect(releaseThresholdObserved.size() >= 2 &&
                   releaseThresholdObserved[0] == 15 &&
                   releaseThresholdObserved[1] == 12,
                "R2 observer preserves exact Cutter release threshold at raw 12");
        }
        releaseThresholdManager.stop();
    }

    {
        auto throwingObserverFake = std::make_shared<FakeBackendState>();
        throwingObserverFake->caps.touchpadInput = true;
        sds::TouchState sample{};
        sample.r2 = 64;
        pushTouch(throwingObserverFake, sample);

        sds::ControllerManager throwingObserverManager(
            sds::Config::defaults(),
            [throwingObserverFake] {
                return std::make_unique<FakeBackend>(throwingObserverFake);
            },
            {},
            std::chrono::milliseconds(1),
            std::chrono::milliseconds(8),
            [](std::uint8_t, std::chrono::steady_clock::time_point) {
                throw 7;
            });
        throwingObserverManager.start();
        expect(waitUntil(std::chrono::milliseconds(250), [&] {
            return throwingObserverManager.connected();
        }), "throwing R2 observer does not stop controller processing");
        throwingObserverManager.stop();
    }

    {
        auto withoutObserver = std::make_shared<FakeBackendState>();
        auto withObserver = std::make_shared<FakeBackendState>();
        const sds::Capabilities caps{
            .adaptiveTriggers = true,
            .lightbar = false,
            .touchpadInput = true,
            .advancedHaptics = false,
            .controllerSpeaker = false,
            .bluetoothTransport = false
        };
        withoutObserver->caps = caps;
        withObserver->caps = caps;

        sds::Config config = sds::Config::defaults();
        config.lightbar = false;
        config.touchpad = false;
        config.adaptiveTriggers = true;

        sds::ControllerManager managerWithout(
            config,
            [withoutObserver] { return std::make_unique<FakeBackend>(withoutObserver); },
            {}, std::chrono::milliseconds(1), std::chrono::milliseconds(8));
        sds::ControllerManager managerWith(
            config,
            [withObserver] { return std::make_unique<FakeBackend>(withObserver); },
            {}, std::chrono::milliseconds(1), std::chrono::milliseconds(8),
            [](std::uint8_t, std::chrono::steady_clock::time_point) {});

        managerWithout.start();
        managerWith.start();
        expect(waitUntil(std::chrono::milliseconds(100), [&] {
            return managerWithout.connected() && managerWith.connected();
        }), "observer equivalence managers connect");

        sds::GameEvent equipped{};
        equipped.type = sds::GameEventType::WeaponEquipped;
        std::strncpy(equipped.text.data(), "Novablast Disruptor", equipped.text.size() - 1);
        expect(managerWithout.enqueue(equipped) && managerWith.enqueue(equipped),
            "observer equivalence managers accept same weapon event");
        sds::TouchState r2{};
        r2.r2 = 160;
        pushTouch(withoutObserver, r2);
        pushTouch(withObserver, r2);
        expect(waitUntil(std::chrono::milliseconds(250), [&] {
            return withoutObserver->pollTouchCalls.load() >= 1 &&
                   withObserver->pollTouchCalls.load() >= 1 &&
                   withoutObserver->triggerCalls.load() >= 1 &&
                   withObserver->triggerCalls.load() >= 1;
        }), "observer equivalence sequence is processed");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        sds::TriggerEffect outputWithoutObserver{};
        sds::TriggerEffect outputWithObserver{};
        {
            std::scoped_lock lock(withoutObserver->outputMutex);
            outputWithoutObserver = withoutObserver->lastRightTrigger;
        }
        {
            std::scoped_lock lock(withObserver->outputMutex);
            outputWithObserver = withObserver->lastRightTrigger;
        }
        expect(outputWithoutObserver == outputWithObserver,
            "installing R2 observer does not change adaptive-trigger output");
        managerWithout.stop();
        managerWith.stop();
    }

    {
        const auto* inventoryDefinition =
            sds::nativeInputDefinitionForAction(sds::InputAction::OpenInventory);
        expect(inventoryDefinition && inventoryDefinition->userEvent == "QuickInventory" &&
                   inventoryDefinition->deviceType == 2U && inventoryDefinition->deviceId == 0U &&
                   inventoryDefinition->eventType == 0U && inventoryDefinition->idCode == 73 &&
                   inventoryDefinition->pulse.size() == 5U,
            "native action table resolves Inventory to the proven QuickInventory gamepad definition");
        const auto* missionsDefinition =
            sds::nativeInputDefinitionForAction(sds::InputAction::OpenMissions);
        const auto* dataDefinition =
            sds::nativeInputDefinitionForAction(sds::InputAction::OpenDataMenu);
        const auto* skillsDefinition =
            sds::nativeInputDefinitionForAction(sds::InputAction::OpenSkills);
        const auto* mapDefinition =
            sds::nativeInputDefinitionForAction(sds::InputAction::OpenMap);
        const auto* powersDefinition =
            sds::nativeInputDefinitionForAction(sds::InputAction::OpenPowers);
        const auto* photoModeDefinition =
            sds::nativeInputDefinitionForAction(sds::InputAction::OpenPhotoMode);
        expect(missionsDefinition && missionsDefinition->userEvent == "QuickMission" &&
                   missionsDefinition->deviceType == 2U && missionsDefinition->idCode == 76 &&
                   missionsDefinition->pulse.size() == 5U,
            "native action table resolves Missions to QuickMission gamepad definition");
        expect(dataDefinition == nullptr,
            "native action table no longer enables redundant Data menu shortcut");
        expect(skillsDefinition && skillsDefinition->userEvent == "QuickSkills" &&
                   skillsDefinition->deviceType == 2U && skillsDefinition->idCode == 80 &&
                   skillsDefinition->pulse.size() == 5U,
            "native action table resolves Skills to QuickSkills gamepad definition");
        expect(mapDefinition && mapDefinition->userEvent == "QuickMap" &&
                   mapDefinition->deviceType == 2U && mapDefinition->idCode == 77 &&
                   mapDefinition->pulse.size() == 5U,
            "native action table resolves Map to QuickMap gamepad definition");
        expect(powersDefinition && powersDefinition->userEvent == "QuickPowers" &&
                   powersDefinition->deviceType == 2U && powersDefinition->idCode == 75 &&
                   powersDefinition->pulse.size() == 5U,
            "native action table resolves Powers to QuickPowers gamepad definition");
        expect(photoModeDefinition && photoModeDefinition->userEvent == "Monocle" &&
                   photoModeDefinition->deviceType == 2U && photoModeDefinition->idCode == 70 &&
                   photoModeDefinition->pulse.size() == 5U,
            "native action table stages Photo Mode through Monocle gamepad definition");
        expect(sds::isMappedNativeInputUserEvent("QuickInventory") &&
                   sds::isMappedNativeInputUserEvent("QuickMission") &&
                   sds::isMappedNativeInputUserEvent("QuickSkills") &&
                   sds::isMappedNativeInputUserEvent("QuickMap") &&
                   sds::isMappedNativeInputUserEvent("QuickPowers") &&
                   sds::isMappedNativeInputUserEvent("Monocle") &&
                   sds::isMappedNativeInputUserEvent("PhotoMode") &&
                   !sds::isMappedNativeInputUserEvent("DataMenu"),
            "native semantic observer filter covers the enabled v0.2.25 actions");

        using Clock = std::chrono::steady_clock;
        const auto start = Clock::time_point{};
        sds::NativeInputSequencer sequence{};
        expect(sequence.enqueue(sds::InputAction::OpenMissions),
            "native sequencer accepts Missions action");
        auto mappedFirst = sequence.poll(start);
        expect(mappedFirst && mappedFirst->userEvent == "QuickMission" &&
                   mappedFirst->deviceType == 2U && mappedFirst->idCode == 76,
            "Missions pulse emits QuickMission as gamepad input");
        sequence.reset();
        expect(!sequence.enqueue(sds::InputAction::OpenDataMenu),
            "native sequencer rejects redundant Data action");
        sequence.reset();
        expect(sequence.enqueue(sds::InputAction::OpenSkills),
            "native sequencer accepts Skills action");
        mappedFirst = sequence.poll(start);
        expect(mappedFirst && mappedFirst->userEvent == "QuickSkills" &&
                   mappedFirst->deviceType == 2U && mappedFirst->idCode == 80,
            "Skills pulse emits QuickSkills as gamepad input");
        sequence.reset();
        expect(sequence.enqueue(sds::InputAction::OpenPowers),
            "native sequencer accepts Powers action");
        mappedFirst = sequence.poll(start);
        expect(mappedFirst && mappedFirst->userEvent == "QuickPowers" &&
                   mappedFirst->deviceType == 2U && mappedFirst->idCode == 75,
            "Powers pulse emits QuickPowers as gamepad input");
        sequence.reset();
        expect(sequence.enqueue(sds::InputAction::OpenMap),
            "native sequencer accepts Map action");
        mappedFirst = sequence.poll(start);
        expect(mappedFirst && mappedFirst->userEvent == "QuickMap" &&
                   mappedFirst->deviceType == 2U && mappedFirst->idCode == 77,
            "Map pulse emits QuickMap as gamepad input");
        sequence.reset();
        expect(sequence.enqueue(sds::InputAction::OpenPhotoMode),
            "native sequencer accepts Create Photo Mode staging action");
        mappedFirst = sequence.poll(start);
        expect(mappedFirst && mappedFirst->userEvent == "Monocle" &&
                   mappedFirst->deviceType == 2U && mappedFirst->idCode == 70,
            "Create Photo Mode staging pulse emits Monocle as gamepad input");
        sequence.reset();
        expect(sequence.enqueueUserEvent("PhotoMode"),
            "native sequencer accepts verified PhotoMode follow-up semantic");
        mappedFirst = sequence.poll(start);
        expect(mappedFirst && mappedFirst->userEvent == "PhotoMode" &&
                   mappedFirst->deviceType == 2U && mappedFirst->idCode == 86,
            "PhotoMode follow-up pulse emits V binding identity as gamepad input");
        sequence.reset();
        expect(!sequence.enqueueUserEvent("NotAStarfieldAction"),
            "native sequencer rejects arbitrary unverified user-event strings");
        sequence.reset();
        expect(sequence.enqueue(sds::InputAction::OpenInventory),
            "native sequencer accepts Inventory action");
        expect(!sequence.enqueue(sds::InputAction::OpenInventory),
            "native sequencer rejects overlap");

        const auto first = sequence.poll(start);
        expect(first && first->edge == sds::NativeInputEdge::Press &&
                   near(first->value, 1.0F) && near(first->heldDownSecs, 0.000F) &&
                   near(first->previousHeldDownSecs, 0.000F) && first->status == 0U,
            "native QuickInventory first frame mirrors native unhandled press");
        expect(first && first->deviceType == 2U && first->idCode == 73 &&
                   first->userEvent == "QuickInventory",
            "every native Inventory pulse starts with the proven gamepad semantic identity");
        expect(!sequence.poll(start + std::chrono::milliseconds(14)).has_value(),
            "native QuickInventory waits for second frame");

        const auto second = sequence.poll(start + std::chrono::milliseconds(15));
        const auto third = sequence.poll(start + std::chrono::milliseconds(30));
        const auto fourth = sequence.poll(start + std::chrono::milliseconds(45));
        const auto release = sequence.poll(start + std::chrono::milliseconds(60));
        expect(second && second->deviceType == 2U && near(second->heldDownSecs, 0.015F) &&
                   near(second->previousHeldDownSecs, 0.000F) && second->status == 0U,
            "native QuickInventory second frame carries gamepad identity and held history");
        expect(third && third->deviceType == 2U && near(third->heldDownSecs, 0.030F) &&
                   near(third->previousHeldDownSecs, 0.015F) && third->status == 0U,
            "native QuickInventory third frame carries gamepad identity and held history");
        expect(fourth && fourth->deviceType == 2U && near(fourth->heldDownSecs, 0.045F) &&
                   near(fourth->previousHeldDownSecs, 0.030F) && fourth->status == 0U,
            "native QuickInventory fourth frame carries gamepad identity and held history");
        expect(release && release->deviceType == 2U &&
                   release->edge == sds::NativeInputEdge::Release &&
                   near(release->value, 0.0F) && near(release->heldDownSecs, 0.045F) &&
                   near(release->previousHeldDownSecs, 0.045F) && release->status == 0U,
            "native QuickInventory release retains gamepad identity and exact release history");
        expect(!sequence.poll(start + std::chrono::milliseconds(75)).has_value(),
            "native QuickInventory sequence finishes after release");
    }

    {
        constexpr std::uintptr_t primary = 0x11110000ULL;
        constexpr std::uintptr_t id = 0x22220000ULL;
        constexpr std::uintptr_t user = 0x33330000ULL;
        sds::NativeButtonSlotState slot{
            .primaryVtable = primary,
            .idVtable = id,
            .userVtable = user,
            .timeCode = 0xFFFFFFFFU,
        };
        expect(sds::isReusableNativeButtonSlot(slot, primary, id, user),
            "native input pool accepts a free initialized ButtonEvent slot");
        slot.timeCode = 42U;
        expect(!sds::isReusableNativeButtonSlot(slot, primary, id, user),
            "native input pool rejects an in-use ButtonEvent slot");
        slot.timeCode = 0xFFFFFFFFU;
        slot.primaryVtable ^= 0x10ULL;
        expect(!sds::isReusableNativeButtonSlot(slot, primary, id, user),
            "native input pool rejects a slot with unexpected vtable");

        std::array<sds::NativeButtonSlotState, 20> ring{};
        for (auto& candidate : ring) {
            candidate = {
                .primaryVtable = primary,
                .idVtable = id,
                .userVtable = user,
                .timeCode = 42U,
            };
        }
        ring[17].timeCode = 0xFFFFFFFFU;
        auto selected = sds::findReusableNativeButtonPoolSlot(
            ring, 0, primary, id, user);
        expect(selected && *selected == 17U,
            "native input pool selects the only Starfield-reclaimed slot");

        ring[17].timeCode = 42U;
        ring[2].timeCode = 0xFFFFFFFFU;
        selected = sds::findReusableNativeButtonPoolSlot(
            ring, 18, primary, id, user);
        expect(selected && *selected == 2U,
            "native input pool selection wraps around from the rotating search index");

        ring[2].timeCode = 42U;
        selected = sds::findReusableNativeButtonPoolSlot(
            ring, 0, primary, id, user);
        expect(!selected,
            "native input pool exhaustion returns no slot instead of overwriting a busy event");

        ring[9].timeCode = 0xFFFFFFFFU;
        selected = sds::findReusableNativeButtonPoolSlot(
            ring, 9, primary, id, user);
        expect(selected && *selected == 9U,
            "native input pool slot becomes selectable only after free sentinel returns");

        expect(sds::isNativeButtonPoolAddress(0x1000, 0x1000, 30, 0x60),
            "native pool accepts first engine-owned ButtonEvent address");
        expect(sds::isNativeButtonPoolAddress(0x1000 + 29 * 0x60, 0x1000, 30, 0x60),
            "native pool accepts last engine-owned ButtonEvent address");
        expect(!sds::isNativeButtonPoolAddress(0x1000 + 30 * 0x60, 0x1000, 30, 0x60),
            "native pool rejects address beyond 30 engine-owned ButtonEvents");
        expect(!sds::isNativeButtonPoolAddress(0x1001, 0x1000, 30, 0x60),
            "native pool rejects unaligned external ButtonEvent address");

        const auto headPlan = sds::planNativeButtonQueueRecycle({
            .head = 0x2000, .tail = 0x4000, .previous = 0, .selected = 0x2000, .selectedNext = 0x3000 });
        expect(headPlan && headPlan->newHead == 0x3000 && headPlan->newTail == 0x4000 &&
                   !headPlan->writePreviousNext,
            "native recycle unlinks queue head without writing a previous link");

        const auto middlePlan = sds::planNativeButtonQueueRecycle({
            .head = 0x2000, .tail = 0x4000, .previous = 0x2000, .selected = 0x3000, .selectedNext = 0x4000 });
        expect(middlePlan && middlePlan->newHead == 0x2000 && middlePlan->newTail == 0x4000 &&
                   middlePlan->writePreviousNext && middlePlan->previousNext == 0x4000,
            "native recycle unlinks middle ButtonEvent and preserves head/tail");

        const auto tailPlan = sds::planNativeButtonQueueRecycle({
            .head = 0x2000, .tail = 0x4000, .previous = 0x3000, .selected = 0x4000, .selectedNext = 0 });
        expect(tailPlan && tailPlan->newHead == 0x2000 && tailPlan->newTail == 0x3000 &&
                   tailPlan->writePreviousNext && tailPlan->previousNext == 0,
            "native recycle unlinks queue tail and moves tail to previous node");

        expect(!sds::planNativeButtonQueueRecycle({
                    .head = 0x2000, .tail = 0x4000, .previous = 0, .selected = 0x3000, .selectedNext = 0x4000 }),
            "native recycle rejects inconsistent head selection");

        const auto packed = sds::packNativeButtonDebounceState(73, 0.015F);
        std::uint32_t previousBits = 0;
        const float previousHeld = 0.015F;
        std::memcpy(&previousBits, &previousHeld, sizeof(previousBits));
        expect(static_cast<std::uint32_t>(packed) == previousBits &&
                   static_cast<std::uint32_t>(packed >> 32U) == 73U,
            "native input packs previous-held and idCode like native unk50");
    }

    return failures == 0 ? 0 : 1;
}
