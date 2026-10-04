#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/HapticsManager.h>
#include <StarfieldDualSense/ShipPropulsionHaptics.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition) {
            std::cerr << "FAIL " << message << '\n';
            std::exit(1);
        }
        std::cout << "PASS " << message << '\n';
    }

    sds::GameEvent event(sds::GameEventType type, std::string_view text = {})
    {
        sds::GameEvent out{};
        out.type = type;
        const auto count = (std::min)(text.size(), out.text.size() - 1);
        std::memcpy(out.text.data(), text.data(), count);
        out.when = std::chrono::steady_clock::now();
        return out;
    }

    struct State
    {
        std::atomic<bool> active{ false };
        std::atomic<int> continuousSets{ 0 };
        std::mutex mutex{};
        sds::HapticContinuousState continuous{};
        std::function<void()> beforePublish;
        std::function<void()> beforeEnqueue;
        std::atomic_bool enqueuedAfterStop{ false };
    };

    class Backend final : public sds::IHapticsBackend
    {
    public:
        explicit Backend(std::shared_ptr<State> state) : _state(std::move(state)) {}
        void start() override { _state->active = true; }
        void stop() noexcept override { _state->active = false; }
        bool enqueue(sds::HapticCommand) noexcept override
        {
            const auto state = _state;
            if (state->beforeEnqueue) state->beforeEnqueue();
            state->enqueuedAfterStop = !state->active.load();
            return true;
        }
        bool setContinuous(sds::HapticContinuousState continuous) noexcept override
        {
            if (_state->beforePublish) _state->beforePublish();
            {
                std::scoped_lock lock(_state->mutex);
                _state->continuous = continuous;
            }
            ++_state->continuousSets;
            return true;
        }
        bool active() const noexcept override { return _state->active.load(); }

    private:
        std::shared_ptr<State> _state;
    };

    sds::HapticContinuousState continuous(const std::shared_ptr<State>& state)
    {
        std::scoped_lock lock(state->mutex);
        return state->continuous;
    }

    sds::ShipPropulsionState normalState()
    {
        return {
            .throttleTargetReadable = true,
            .effectiveThrottleReadable = true,
            .velocityReadable = true,
            .throttleTarget = 0.50F,
            .effectiveThrottle = 0.50F,
            .velocity = 90.0F,
            .maxForwardSpeed = 180.0F,
            .boostFuelCurrent = 4.0F,
            .boostFuelPermanent = 4.0F,
            .boostSpeed = 4.0F,
        };
    }

    sds::ShipPropulsionState boostState()
    {
        auto out = normalState();
        out.throttleTarget = 2.0F;
        out.effectiveThrottle = 2.0F;
        out.velocity = 400.0F;
        out.boostFuelCurrent = 3.0F;
        return out;
    }
}

int main()
{
    auto backendState = std::make_shared<State>();
    auto config = sds::Config::defaults();
    config.advancedHaptics = true;
    config.hapticStrength = 1.0F;

    sds::HapticsManager manager(
        config,
        [backendState] { return std::make_unique<Backend>(backendState); });
    manager.start();

    require(manager.handleShipPropulsionState(normalState()),
        "pre-entry propulsion observation is safely consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "ship propulsion cannot acquire haptic ownership before pilot entry");

    require(manager.handle(event(sds::GameEventType::ShipPilotEntered)),
        "pilot entry is consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "pilot entry begins from exact neutral before the first fresh propulsion sample");

    require(manager.handleShipPropulsionState(normalState()),
        "fresh normal propulsion state is accepted while piloting");
    require(continuous(backendState).kind == sds::HapticContinuousKind::ShipPropulsion,
        "fresh pilot propulsion sample acquires ship continuous ownership");

    require(manager.handleShipPropulsionState(boostState()),
        "fresh boost state is accepted while piloting");
    require(continuous(backendState).kind == sds::HapticContinuousKind::ShipBoost,
        "fresh native boost evidence replaces normal propulsion with boost texture");

    require(manager.handle(event(sds::GameEventType::MenuOpened, "DataMenu")),
        "DataMenu open is consumed while piloting");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "DataMenu immediately hard-mutes ship propulsion haptics");
    require(manager.handleShipPropulsionState(normalState()),
        "propulsion sample during DataMenu is safely consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "DataMenu blocks fresh propulsion from reacquiring vibration");
    require(manager.handle(event(sds::GameEventType::MenuOpened, "PauseMenu")),
        "nested PauseMenu open is consumed while piloting");
    require(manager.handle(event(sds::GameEventType::MenuClosed, "DataMenu")),
        "DataMenu close is consumed while PauseMenu remains open");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "nested PauseMenu keeps propulsion muted after DataMenu closes");
    require(manager.handle(event(sds::GameEventType::MenuClosed, "PauseMenu")),
        "final blocking menu close is consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "closing final menu does not restore stale cached propulsion");
    require(manager.handleShipPropulsionState(normalState()),
        "fresh propulsion sample after menu close is accepted");
    require(continuous(backendState).kind == sds::HapticContinuousKind::ShipPropulsion,
        "fresh post-menu propulsion sample reacquires vibration");

    require(manager.handle(event(sds::GameEventType::ShipPilotInvalidated, "LoadingMenu")),
        "loading invalidation is consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "loading invalidation hard-clears ship propulsion immediately");

    require(manager.handleShipPropulsionState(normalState()),
        "propulsion observation during invalidated loading is safely consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "invalidated loading rejects stale ship propulsion ownership");

    require(manager.handle(event(sds::GameEventType::MenuClosed, "LoadingMenu")),
        "LoadingMenu close is consumed before normalized resume");
    require(manager.handle(event(sds::GameEventType::ShipPilotResumed, "LoadingMenu")),
        "pilot resume is consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "pilot resume remains neutral until a fresh post-load propulsion sample");

    require(manager.handleShipPropulsionState(normalState()),
        "fresh post-resume propulsion state is accepted");
    require(continuous(backendState).kind == sds::HapticContinuousKind::ShipPropulsion,
        "fresh post-resume sample reacquires propulsion ownership");

    require(manager.handle(event(sds::GameEventType::ShipPilotExited, "SpaceshipHudMenu")),
        "pilot exit is consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "pilot exit hard-clears propulsion haptics");

    require(manager.handleShipPropulsionState(boostState()),
        "post-exit propulsion observation is safely consumed");
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "post-exit ship samples cannot reacquire haptic ownership");

    // Stall an older publication at the backend, not at a diagnostic log statement.
    require(manager.handle(event(sds::GameEventType::ShipPilotEntered)), "ordering fixture enters ship");
    std::promise<void> entered, release, pauseStarted;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future().share();
    std::atomic_bool holdNext{true};
    backendState->beforePublish = [&] {
        if (holdNext.exchange(false)) {
            entered.set_value();
            require(released.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "stalled publication released");
        }
    };
    auto older = std::async(std::launch::async, [&] { return manager.handleShipPropulsionState(boostState()); });
    require(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "older propulsion publication entered backend");
    auto pause = std::async(std::launch::async, [&] {
        pauseStarted.set_value();
        return manager.handle(event(sds::GameEventType::MenuOpened, "PauseMenu"));
    });
    require(pauseStarted.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "newer menu stop started");
    const bool overtook = pause.wait_for(std::chrono::milliseconds(25)) == std::future_status::ready;
    release.set_value();
    require(older.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
        pause.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "both publications complete");
    require(older.get() && pause.get(), "ordered submissions succeed");
    backendState->beforePublish = {};
    require(!overtook && continuous(backendState).kind == sds::HapticContinuousKind::None,
        "an older update cannot overwrite a newer menu stop");

    manager.stop();
    require(continuous(backendState).kind == sds::HapticContinuousKind::None,
        "shutdown leaves propulsion haptics at exact neutral");
    for (bool reload : {false, true}) {
        auto state = std::make_shared<State>();
        sds::HapticsManager finite(config, [state] { return std::make_unique<Backend>(state); });
        finite.start();
        require(finite.handle(event(sds::GameEventType::WeaponEquipped, "Eon")), "finite cue fixture equips Eon");
        std::promise<void> enqueuing, finishEnqueue, stopStarted;
        auto waiting = enqueuing.get_future();
        auto finish = finishEnqueue.get_future().share();
        state->beforeEnqueue = [&] {
            enqueuing.set_value();
            require(finish.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "finite cue submission released");
        };
        auto submit = std::async(std::launch::async, [&] {
            return reload ? finite.emitWeaponReloadCue("mag-in") : finite.emitOnFootLanding(0.5F);
        });
        require(waiting.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "finite cue reached backend");
        auto stopping = std::async(std::launch::async, [&] { stopStarted.set_value(); finite.stop(); });
        require(stopStarted.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "concurrent backend stop started");
        const bool stoppedEarly = stopping.wait_for(std::chrono::milliseconds(25)) == std::future_status::ready;
        finishEnqueue.set_value();
        require(submit.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
            stopping.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "finite submission and stop complete");
        require(submit.get(), "finite cue submission succeeds");
        stopping.get();
        require(!stoppedEarly && !state->enqueuedAfterStop,
            reload ? "reload cue retains backend through submission" : "landing cue retains backend through submission");
    }
    return EXIT_SUCCESS;
}
