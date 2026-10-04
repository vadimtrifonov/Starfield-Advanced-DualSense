#include <StarfieldDualSense/HapticsManager.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    int failures = 0;

    void check(bool condition, const char* name)
    {
        std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
        if (!condition) {
            ++failures;
        }
    }

    struct BackendState
    {
        std::vector<sds::HapticCommand> commands;
        std::vector<sds::HapticContinuousState> continuousHistory;
        sds::HapticContinuousState continuous{};
        bool active{ false };
    };

    class Backend final : public sds::IHapticsBackend
    {
    public:
        explicit Backend(std::shared_ptr<BackendState> state) : state_(std::move(state)) {}
        void start() override { state_->active = true; }
        void stop() noexcept override { state_->active = false; }
        bool enqueue(sds::HapticCommand command) noexcept override
        {
            state_->commands.push_back(command);
            return true;
        }
        bool setContinuous(sds::HapticContinuousState state) noexcept override
        {
            state_->continuous = state;
            state_->continuousHistory.push_back(state);
            return true;
        }
        bool active() const noexcept override { return state_->active; }

    private:
        std::shared_ptr<BackendState> state_;
    };

    sds::GameEvent event(sds::GameEventType type, std::chrono::steady_clock::time_point when)
    {
        sds::GameEvent result{};
        result.type = type;
        result.when = when;
        return result;
    }

    sds::GameEvent menuEvent(
        sds::GameEventType type,
        std::string_view menu,
        std::chrono::steady_clock::time_point when)
    {
        auto result = event(type, when);
        std::copy_n(menu.data(), std::min(menu.size(), result.text.size() - 1), result.text.begin());
        return result;
    }
}

int main()
{
    const auto t0 = std::chrono::steady_clock::time_point{ 500s };
    auto backend = std::make_shared<BackendState>();
    auto logs = std::make_shared<std::vector<std::string>>();
    auto config = sds::Config::defaults();
    config.advancedHaptics = true;

    sds::HapticsManager manager(config, [backend] {
        return std::make_unique<Backend>(backend);
    }, [logs](std::string_view line) {
        logs->emplace_back(line);
    });
    manager.start();

    check(manager.setShipLaunchLandingRumble(true, "takeoff"),
        "takeoff sequence can start the shared launch/landing rumble");
    check(backend->continuous.kind == sds::HapticContinuousKind::ShipBoost &&
          std::abs(backend->continuous.gain - 1.00F) < 0.01F &&
          std::abs(backend->continuous.level - 1.00F) < 0.01F,
        "takeoff uses full-strength ShipBoost transition body");

    check(manager.setShipLaunchLandingRumble(false, "takeoff-sequence-end"),
        "explicit takeoff sequence end can stop the transition body");
    check(backend->continuous == sds::HapticContinuousState{},
        "takeoff rumble stop returns to exact neutral when no propulsion bed exists");

    check(manager.handle(event(sds::GameEventType::ShipPilotEntered, t0)),
        "landing fixture enters ship context");
    check(manager.handle(event(sds::GameEventType::ShipPilotInvalidated, t0 + 100ms)),
        "landing load invalidates ordinary pilot haptic authority");
    check(manager.setShipLaunchLandingRumble(true, "landing"),
        "landing cinematic can start rumble after ordinary pilot authority is invalidated");
    check(backend->continuous.kind == sds::HapticContinuousKind::ShipBoost &&
          std::abs(backend->continuous.gain - 1.00F) < 0.01F &&
          std::abs(backend->continuous.level - 1.00F) < 0.01F,
        "landing uses the same full-strength ShipBoost body after pilot invalidation");

    check(manager.handle(event(sds::GameEventType::ShipPilotExited, t0 + 1200ms)),
        "landing cinematic pilot-exit lifecycle event is consumed after invalidation");
    check(backend->continuous.kind == sds::HapticContinuousKind::ShipBoost,
        "landing body survives pilot-exit lifecycle after authority was already invalidated");

    check(manager.handle(menuEvent(sds::GameEventType::MenuOpened, "DataMenu", t0 + 1500ms)),
        "blocking menu event is consumed while landing transition is active");
    check(backend->continuous == sds::HapticContinuousState{},
        "blocking menu hard-mutes landing body even after pilot invalidation");
    check(manager.handle(menuEvent(sds::GameEventType::MenuClosed, "DataMenu", t0 + 1600ms)),
        "blocking menu close is consumed while landing transition is active");
    check(backend->continuous.kind == sds::HapticContinuousKind::ShipBoost &&
          std::abs(backend->continuous.gain - 1.00F) < 0.01F,
        "closing the final blocking menu restores the active landing body");

    check(manager.handle(event(sds::GameEventType::ShipPilotResumed, t0 + 1700ms)),
        "pilot resume is consumed while landing transition remains active");
    check(backend->continuous.kind == sds::HapticContinuousKind::ShipBoost &&
          std::abs(backend->continuous.gain - 1.00F) < 0.01F,
        "pilot resume keeps the active landing body continuous");

    const auto commandCount = backend->commands.size();
    check(manager.handle(event(sds::GameEventType::ShipTouchdown, t0 + 2s)),
        "authoritative touchdown is consumed while landing rumble is active");
    check(backend->continuous == sds::HapticContinuousState{},
        "touchdown hard-stops landing rumble before finite thump completes");
    check(backend->commands.size() == commandCount + 1u &&
          backend->commands.back().kind == sds::HapticEffectKind::ShipTouchdownThump,
        "accepted touchdown thump remains intact after landing rumble stop");

    check(manager.setShipLaunchLandingRumble(true, "landing"),
        "rumble can be re-armed for cancellation fixture");
    check(manager.setShipLaunchLandingRumble(false, "landing-cancelled"),
        "cancelled landing sequence hard-clears transition rumble");
    check(backend->continuous == sds::HapticContinuousState{},
        "cancelled landing leaves no stuck continuous haptic state");

    for (const auto menu : {"FaderMenu", "PauseMenu"}) {
        check(manager.setShipLaunchLandingRumble(true, "landing"), "landing feedback starts");
        check(manager.handle(menuEvent(sds::GameEventType::MenuOpened, menu, t0 + 3s)),
            "transition menu opens");
        const auto beforeTouchdown = backend->commands.size();
        check(manager.handle(event(sds::GameEventType::ShipTouchdown, t0 + 4s)),
            "touchdown is consumed inside transition menu");
        const bool fade = std::string_view(menu) == "FaderMenu";
        check(backend->continuous == sds::HapticContinuousState{} &&
            backend->commands.size() == beforeTouchdown + (fade ? 1u : 0u),
            "touchdown stops rumble and preserves its impact during a fade, but not pause");
        if (fade) check(backend->commands.back().kind == sds::HapticEffectKind::ShipTouchdownThump &&
            std::abs(backend->commands.back().gain - 0.90F) < 0.01F, "fade does not retune touchdown");
        check(manager.handle(menuEvent(sds::GameEventType::MenuClosed, menu, t0 + 5s)),
            "transition menu closes");
        check(backend->continuous == sds::HapticContinuousState{},
            "menu close cannot restart a completed landing");
    }

    bool takeoffLog = false;
    bool landingLog = false;
    bool stopLog = false;
    for (const auto& line : *logs) {
        takeoffLog = takeoffLog || line.find("Ship launch/landing rumble: stage=submitted action=start phase=takeoff") != std::string::npos;
        landingLog = landingLog || line.find("Ship launch/landing rumble: stage=submitted action=start phase=landing") != std::string::npos;
        stopLog = stopLog || line.find("Ship launch/landing rumble: stage=submitted action=stop") != std::string::npos;
    }
    check(takeoffLog && landingLog && stopLog,
        "launch/landing rumble logs start and stop delivery for hardware verification");

    manager.stop();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
