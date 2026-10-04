#include <StarfieldDualSense/EffectsEngine.h>
#include <StarfieldDualSense/GameplayFeedbackGate.h>
#include <StarfieldDualSense/HapticsManager.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

using namespace sds;
using namespace std::chrono_literals;

namespace
{
    constexpr auto t0 = std::chrono::steady_clock::time_point{100s};
    int failures{};
    void check(bool ok, std::string_view label)
    {
        if (!ok) { std::cerr << "FAIL " << label << '\n'; ++failures; }
    }
    GameEvent event(GameEventType type, std::string_view text = {})
    {
        GameEvent e{};
        e.type = type;
        e.when = t0;
        std::copy_n(text.data(), (std::min)(text.size(), e.text.size() - 1), e.text.begin());
        return e;
    }
    struct Output { HapticContinuousState continuous{}; int commands{}; };
    class Backend final : public IHapticsBackend
    {
    public:
        explicit Backend(std::shared_ptr<Output> output) : _output(std::move(output)) {}
        void start() override {}
        void stop() noexcept override {}
        bool active() const noexcept override { return true; }
        bool enqueue(HapticCommand) noexcept override { ++_output->commands; return true; }
        bool setContinuous(HapticContinuousState value) noexcept override { _output->continuous = value; return true; }
    private:
        std::shared_ptr<Output> _output;
    };
    struct Fixture
    {
        std::shared_ptr<Output> output = std::make_shared<Output>();
        HapticsManager manager{Config::defaults(), [this] { return std::make_unique<Backend>(output); }};
        EffectsEngine triggers;
        Fixture() { manager.start(); }
        void send(GameEventType type, std::string_view text = {})
        {
            check(manager.handle(event(type, text)), "event accepted");
            (void)triggers.handle(event(type, text));
        }
        bool silent() const
        {
            return output->continuous == HapticContinuousState{} &&
                triggers.state().output.leftTrigger.mode == TriggerEffectMode::Off &&
                triggers.state().output.rightTrigger.mode == TriggerEffectMode::Off;
        }
        void enter(int context)
        {
            if (context == 0) send(GameEventType::WeaponEquipped, "Arc Welder");
            if (context == 1) send(GameEventType::ShipPilotEntered);
            if (context == 2) {
                send(GameEventType::LandVehicleContextEntered);
                send(GameEventType::LandVehicleAuthorityAcquired);
            }
        }
        void feed(int context)
        {
            if (context == 0) {
                (void)manager.handleRightTriggerInput(255, t0);
                (void)triggers.handleRightTriggerInput(255, t0);
                send(GameEventType::WeaponFired, "weaponFireStart");
            } else if (context == 1) {
                ShipPropulsionState s{};
                s.throttleTargetReadable = s.effectiveThrottleReadable = s.velocityReadable = true;
                s.throttleTarget = s.effectiveThrottle = 0.5F;
                s.velocity = 90.0F;
                s.maxForwardSpeed = 180.0F;
                (void)manager.handleShipPropulsionState(s);
            } else {
                LandVehicleMotionState s{};
                s.authorityActive = true;
                s.authorityEpoch = 1;
                s.speed = 12;
                s.acceleration = 2;
                manager.handleLandVehicleMotionState(s);
            }
        }
    };
}

int main()
{
    // The same nested menu policy must survive a change of feedback owner.
    for (auto menu : kGameplayBlockingMenus) {
        for (int context = 0; context != 3; ++context) {
            Fixture f;
            f.enter(context);
            f.feed(context);
            check(f.output->continuous.kind != HapticContinuousKind::None, "feedback active before menu");
            const auto trigger = f.triggers.state().output.rightTrigger;
            f.send(GameEventType::MenuOpened, "MonocleMenu");
            check(f.triggers.state().output.rightTrigger == trigger, "unrelated menus preserve active triggers");
            f.send(GameEventType::MenuOpened, menu);
            f.feed(context);
            const auto commands = f.output->commands;
            f.send(GameEventType::ShipLaserWeaponFired);
            f.send(GameEventType::LandVehicleGunFired);
            (void)f.manager.startBoostpackThrust();
            (void)f.manager.emitBoostpackIgnition(t0);
            (void)f.triggers.tick(t0 + 10s);
            auto live = gameplayHapticsLiveSettings(Config::defaults());
            live.advancedHaptics = false; f.manager.applyLiveSettings(live);
            live.advancedHaptics = true; f.manager.applyLiveSettings(live);
            check(f.silent() && f.output->commands == commands, menu);
            if (context == 1) f.send(GameEventType::ShipPilotExited);
            if (context == 2) f.send(GameEventType::LandVehicleContextExited);
            const auto next = (context + 1) % 3;
            f.enter(next);
            f.feed(next);
            check(f.silent(), "context change cannot forget an open menu");
            f.send(GameEventType::GamePaused);
            f.send(GameEventType::MenuClosed, menu);
            f.feed(next);
            check(f.silent(), "closing menu cannot override pause");
            f.send(GameEventType::GameUnpaused);
            f.feed(next);
            check(f.output->continuous.kind != HapticContinuousKind::None, "fresh feedback resumes");
        }
    }
    for (auto weapon : {"Auto-Rivet", "Magsniper", "Va'ruun Penumbra", "Cutter", "Arc Welder", "Novablast Disruptor"}) {
        Fixture f;
        f.send(GameEventType::WeaponEquipped, weapon);
        (void)f.manager.handleRightTriggerInput(255, t0);
        const std::string_view name(weapon);
        const auto marker = name == "Auto-Rivet" ? "AutoRivetChargeStart" :
            name == "Novablast Disruptor" ? "NovablastChargeStart" : "weaponFireStart";
        f.send(GameEventType::WeaponFired, marker);
        check(f.output->continuous.kind != HapticContinuousKind::None, weapon);
        f.send(GameEventType::MenuOpened, "DataMenu");
        f.send(GameEventType::MenuOpened, "GalaxyStarMapMenu");
        f.send(GameEventType::MenuClosed, "DataMenu");
        (void)f.manager.handleRightTriggerInput(255, t0 + 1ms);
        f.send(GameEventType::WeaponFired, marker);
        check(f.silent(), "nested menus block every sustained weapon");
    }
    {
        Fixture f;
        f.enter(0);
        f.send(GameEventType::GamePaused);
        const auto commands = f.output->commands;
        (void)f.manager.emitDigipickWwiseEvent(0x0A9F7EB0u, t0);
        check(f.silent() && f.output->commands == commands + 1, "intentional UI haptics remain available");
    }
    for (auto menu : kGameplayBlockingMenus) {
        Fixture f;
        f.send(GameEventType::WeaponEquipped, "Eon");
        const auto emit = [&] {
            check(f.manager.emitWeaponReloadCue("mag-in", t0), "reload cue accepted");
            check(f.manager.emitOnFootLanding(0.5F, t0), "landing cue accepted");
        };
        emit();
        check(f.output->commands == 2, "reload and landing cues reach the backend");
        f.send(GameEventType::MenuOpened, menu);
        emit();
        check(f.output->commands == 2, "menus block delayed reload and landing feedback");
        f.send(GameEventType::GamePaused);
        f.send(GameEventType::MenuClosed, menu);
        emit();
        check(f.output->commands == 2, "pause retains finite feedback suspension after menu closes");
        f.send(GameEventType::GameUnpaused);
        emit();
        check(f.output->commands == 4, "fresh reload and landing feedback resumes");
        f.send(GameEventType::Shutdown);
        emit();
        check(f.output->commands == 4, "shutdown cannot admit delayed finite feedback");
    }
    std::cout << "Gameplay feedback lifecycle: " << failures << " failure(s)\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
