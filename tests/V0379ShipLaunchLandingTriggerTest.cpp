#include <StarfieldDualSense/EffectsEngine.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

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
    const auto t0 = std::chrono::steady_clock::time_point{ 400s };
    auto config = sds::Config::defaults();
    config.adaptiveTriggers = true;

    sds::EffectsEngine engine(config);
    const auto entered = engine.handle(event(sds::GameEventType::ShipPilotEntered, t0));
    const auto baseline = entered.output.rightTrigger;
    check(baseline.mode == sds::TriggerEffectMode::ContinuousResistance,
        "pilot entry retains accepted cockpit R2 wall before transition");
    check(entered.output.leftTrigger.mode == sds::TriggerEffectMode::Off,
        "pilot entry leaves L2 neutral before transition");

    const auto started = engine.handle(event(sds::GameEventType::ShipLaunchLandingHapticsStarted, t0 + 10ms));
    check(started.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx &&
          started.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
        "transition start installs sustained EffectEx on both triggers");
    check(started.output.leftTrigger == started.output.rightTrigger &&
          started.output.rightTrigger.keepEffect,
        "launch and landing use one symmetric sustained trigger texture");
    check(started.output.rightTrigger.startPosition == 64u &&
          started.output.rightTrigger.beginForce == 220u &&
          started.output.rightTrigger.middleForce == 255u &&
          started.output.rightTrigger.endForce == 210u &&
          started.output.rightTrigger.frequency == 24u,
        "aggressive transition trigger uses the v0.3.79 heavy pulsing envelope");

    const auto stillActive = engine.tick(t0 + 10s);
    check(stillActive.output.leftTrigger == started.output.leftTrigger &&
          stillActive.output.rightTrigger == started.output.rightTrigger,
        "transition trigger has no synthetic short lease or timeout inside EffectsEngine");

    const auto menuOpen = engine.handle(menuEvent(sds::GameEventType::MenuOpened, "DataMenu", t0 + 11s));
    check(menuOpen.output.leftTrigger.mode == sds::TriggerEffectMode::Off &&
          menuOpen.output.rightTrigger.mode == sds::TriggerEffectMode::Off,
        "DataMenu hard-mutes both transition triggers");
    const auto menuClosed = engine.handle(menuEvent(sds::GameEventType::MenuClosed, "DataMenu", t0 + 12s));
    check(menuClosed.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx &&
          menuClosed.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
        "closing final blocking menu restores active transition trigger texture");

    const auto stopped = engine.handle(event(sds::GameEventType::ShipLaunchLandingHapticsStopped, t0 + 13s));
    check(stopped.output.leftTrigger.mode == sds::TriggerEffectMode::Off,
        "transition stop releases L2 to neutral");
    check(stopped.output.rightTrigger == baseline,
        "transition stop restores accepted cockpit R2 wall when pilot authority remains");

    sds::EffectsEngine landing(config);
    const auto invalidated = landing.handle(event(sds::GameEventType::ShipPilotInvalidated, t0 + 20s));
    check(invalidated.output.leftTrigger.mode == sds::TriggerEffectMode::Off &&
          invalidated.output.rightTrigger.mode == sds::TriggerEffectMode::Off,
        "loading invalidation starts from neutral trigger ownership");
    const auto landingStarted = landing.handle(event(sds::GameEventType::ShipLaunchLandingHapticsStarted, t0 + 21s));
    check(landingStarted.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx &&
          landingStarted.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
        "authoritative landing cinematic may own both triggers after normal pilot invalidation");
    const auto cinematicExit = landing.handle(event(sds::GameEventType::ShipPilotExited, t0 + 21250ms));
    check(cinematicExit.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx &&
          cinematicExit.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
        "landing transition survives pilot-exit lifecycle after authority was already invalidated");
    const auto landingMenuOpen = landing.handle(menuEvent(sds::GameEventType::MenuOpened, "DataMenu", t0 + 21500ms));
    check(landingMenuOpen.output.leftTrigger.mode == sds::TriggerEffectMode::Off &&
          landingMenuOpen.output.rightTrigger.mode == sds::TriggerEffectMode::Off,
        "blocking menu mutes landing triggers even after ordinary pilot authority was invalidated");
    const auto landingMenuClose = landing.handle(menuEvent(sds::GameEventType::MenuClosed, "DataMenu", t0 + 21750ms));
    check(landingMenuClose.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx &&
          landingMenuClose.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
        "landing transition trigger restores after the final blocking menu closes");
    const auto landingResumed = landing.handle(event(sds::GameEventType::ShipPilotResumed, t0 + 21900ms));
    check(landingResumed.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx &&
          landingResumed.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
        "pilot resume retains active landing transition trigger ownership");
    const auto landingStopped = landing.handle(event(sds::GameEventType::ShipLaunchLandingHapticsStopped, t0 + 22s));
    check(landingStopped.output.leftTrigger.mode == sds::TriggerEffectMode::Off,
        "landing sequence end releases L2 after pilot authority resumed");
    check(landingStopped.output.rightTrigger == baseline,
        "landing sequence end restores cockpit R2 wall after pilot authority resumed");

    sds::EffectsEngine landingWithoutAuthority(config);
    (void)landingWithoutAuthority.handle(event(sds::GameEventType::ShipPilotInvalidated, t0 + 30s));
    const auto noAuthorityStarted = landingWithoutAuthority.handle(
        event(sds::GameEventType::ShipLaunchLandingHapticsStarted, t0 + 31s));
    check(noAuthorityStarted.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx &&
          noAuthorityStarted.output.rightTrigger.mode == sds::TriggerEffectMode::EffectEx,
        "landing transition may own both triggers without cockpit authority");
    const auto noAuthorityStopped = landingWithoutAuthority.handle(
        event(sds::GameEventType::ShipLaunchLandingHapticsStopped, t0 + 32s));
    check(noAuthorityStopped.output.leftTrigger.mode == sds::TriggerEffectMode::Off &&
          noAuthorityStopped.output.rightTrigger.mode == sds::TriggerEffectMode::Off,
        "landing sequence end releases both triggers when no cockpit authority is active");

    for (auto menu : {"GalaxyStarMapMenu", "LoadingMenu", "MainMenu", "FaderMenu"}) {
        sds::EffectsEngine transition(config);
        (void)transition.handle(event(sds::GameEventType::ShipLaunchLandingHapticsStarted, t0));
        (void)transition.handle(menuEvent(sds::GameEventType::MenuOpened, menu, t0 + 1s));
        (void)transition.handleRightTriggerInput(255, t0 + 2s);
        const auto during = transition.tick(t0 + 3s);
        const bool fade = std::string_view(menu) == "FaderMenu";
        check(during.output.leftTrigger == during.output.rightTrigger &&
            (during.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx) == fade,
            "only a fade preserves both transition triggers");
        (void)transition.handle(event(sds::GameEventType::GamePaused, t0 + 4s));
        (void)transition.handle(menuEvent(sds::GameEventType::MenuClosed, menu, t0 + 5s));
        check(transition.state().output.leftTrigger.mode == sds::TriggerEffectMode::Off &&
            transition.state().output.rightTrigger.mode == sds::TriggerEffectMode::Off,
            "menu close cannot override a paused transition");
        const auto resumed = transition.handle(event(sds::GameEventType::GameUnpaused, t0 + 6s));
        const bool ended = std::string_view(menu) == "MainMenu";
        check((resumed.output.leftTrigger.mode == sds::TriggerEffectMode::EffectEx) != ended,
            "MainMenu discards transition authority rather than resuming it");
    }

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
