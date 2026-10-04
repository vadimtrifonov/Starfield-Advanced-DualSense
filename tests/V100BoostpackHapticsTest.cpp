#include <StarfieldDualSense/Config.h>
#include <StarfieldDualSense/GameplayHapticsLiveSettings.h>
#include <StarfieldDualSense/HapticMixer.h>
#include <StarfieldDualSense/HapticWaveforms.h>
#include <StarfieldDualSense/HapticsManager.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    int g_failures = 0;

    void check(bool condition, std::string_view label)
    {
        if (condition) {
            std::cout << "PASS " << label << '\n';
            return;
        }

        std::cerr << "FAIL " << label << '\n';
        ++g_failures;
    }

    bool near(float a, float b, float epsilon = 0.015F)
    {
        return std::abs(a - b) <= epsilon;
    }

    struct BackendState
    {
        int creates{};
        int starts{};
        int stops{};
        bool active{};
        std::vector<sds::HapticCommand> commands{};
        std::vector<sds::HapticContinuousState> continuous{};
    };

    class Backend final : public sds::IHapticsBackend
    {
    public:
        explicit Backend(std::shared_ptr<BackendState> state) :
            _state(std::move(state))
        {}

        void start() override
        {
            ++_state->starts;
            _state->active = true;
        }

        void stop() noexcept override
        {
            ++_state->stops;
            _state->active = false;
        }

        bool enqueue(sds::HapticCommand command) noexcept override
        {
            _state->commands.push_back(command);
            return true;
        }

        bool setContinuous(sds::HapticContinuousState state) noexcept override
        {
            _state->continuous.push_back(state);
            return true;
        }

        [[nodiscard]] bool active() const noexcept override
        {
            return _state->active;
        }

    private:
        std::shared_ptr<BackendState> _state;
    };

    bool waveformSafe(const sds::HapticWaveform& waveform)
    {
        return !waveform.empty() &&
            std::all_of(waveform.begin(), waveform.end(), [](const auto& frame) {
                return frame[0] == 0.0F && frame[1] == 0.0F &&
                    std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
                    std::abs(frame[2]) <= 1.0F && std::abs(frame[3]) <= 1.0F;
            }) &&
            waveform.back() == sds::HapticFrame{};
    }
}

int main()
{
    const auto t0 = std::chrono::steady_clock::time_point{} + 40s;

    auto config = sds::Config::defaults();
    config.advancedHaptics = true;
    config.hapticStrength = 0.50F;
    config.boostpackHaptics = true;
    config.boostpackHapticsStrength = 1.25F;

    auto projected = sds::gameplayHapticsLiveSettings(config);
    check(projected.boostpackHaptics,
        "live projection carries BoostpackHaptics");
    check(near(projected.boostpackHapticsStrength, 1.25F, 0.001F),
        "live projection carries BoostpackHapticsStrength");

    config.boostpackHapticsStrength = 99.0F;
    check(
        near(sds::gameplayHapticsLiveSettings(config).boostpackHapticsStrength, 3.0F, 0.001F),
        "live projection clamps BoostpackHapticsStrength high to 3.0");
    config.boostpackHapticsStrength = -3.0F;
    check(
        near(sds::gameplayHapticsLiveSettings(config).boostpackHapticsStrength, 0.0F, 0.001F),
        "live projection clamps BoostpackHapticsStrength low to zero");

    config.boostpackHapticsStrength = 1.25F;

    const auto ignitionWave = sds::synthesizeHapticEffect({
        .kind = sds::HapticEffectKind::BoostpackIgnition,
        .gain = 0.60F,
        .when = t0,
    });
    check(
        ignitionWave.size() == 3360u,
        "boostpack ignition waveform is exactly 70 ms at 48 kHz");
    check(
        waveformSafe(ignitionWave),
        "boostpack ignition waveform is bounded, channel-safe, and ends at zero");

    sds::HapticMixer boostMixer;
    boostMixer.setContinuous({
        .kind = sds::HapticContinuousKind::BoostpackThrust,
        .gain = 0.30F,
        .level = 0.55F,
    });
    std::vector<sds::HapticFrame> boostBlock(4800);
    boostMixer.render(boostBlock);
    const auto boostStats = sds::measureHapticBlock(boostBlock);
    check(
        boostStats.rmsCh3 > 0.020F && boostStats.rmsCh3 < 0.120F,
        "boostpack thrust body is tactile but remains below an overpowering RMS ceiling");
    check(
        std::all_of(boostBlock.begin(), boostBlock.end(), [](const auto& frame) {
            return frame[0] == 0.0F && frame[1] == 0.0F &&
                std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
                std::abs(frame[2]) <= 1.0F && std::abs(frame[3]) <= 1.0F;
        }),
        "boostpack thrust mixer output stays finite, bounded, and on haptic channels only");

    auto backend = std::make_shared<BackendState>();
    sds::HapticsManager manager(
        config,
        [backend] {
            ++backend->creates;
            return std::make_unique<Backend>(backend);
        });
    manager.start();

    check(backend->creates == 1 && backend->starts == 1,
        "boostpack haptics reuse the one stable gameplay haptics backend");

    check(manager.emitBoostpackIgnition(t0),
        "authoritative boost start can submit one ignition kick");
    check(
        backend->commands.size() == 1u &&
            backend->commands.back().kind == sds::HapticEffectKind::BoostpackIgnition,
        "ignition uses dedicated on-foot BoostpackIgnition effect kind");
    check(
        near(backend->commands.back().gain, 0.30F),
        "ignition gain multiplies 0.48 base by global and boostpack strengths");

    const auto commandCountBeforeBody = backend->commands.size();
    check(manager.startBoostpackThrust(),
        "authoritative boost start acquires sustained thrust body");
    check(
        backend->commands.size() == commandCountBeforeBody,
        "starting sustained thrust never duplicates the ignition kick");
    check(!backend->continuous.empty(),
        "boostpack start submits continuous state");
    if (!backend->continuous.empty()) {
        const auto& body = backend->continuous.back();
        check(
            body.kind == sds::HapticContinuousKind::BoostpackThrust,
            "boostpack body uses dedicated BoostpackThrust continuous kind");
        check(
            near(body.gain, 0.1875F),
            "thrust gain multiplies 0.30 base by global and boostpack strengths");
        check(
            near(body.level, 0.55F, 0.001F),
            "boostpack body retains fixed low-frequency thrust texture level");
    }

    sds::GameEvent equipped{};
    equipped.type = sds::GameEventType::WeaponEquipped;
    equipped.when = t0 + 10ms;
    std::string_view("Maelstrom").copy(equipped.text.data(), 9);
    check(manager.handle(equipped), "weapon equip during thrust is consumed");
    const auto commandsBeforeShot = backend->commands.size();
    sds::GameEvent shot{};
    shot.type = sds::GameEventType::WeaponFired;
    shot.when = t0 + 20ms;
    check(manager.handle(shot) && backend->commands.size() == commandsBeforeShot + 1,
        "weapon recoil still layers over active thrust");
    check(backend->continuous.back().kind == sds::HapticContinuousKind::BoostpackThrust,
        "weapon processing cannot displace the active thrust owner");
    sds::GameEvent menu{};
    menu.type = sds::GameEventType::MenuOpened;
    menu.when = t0 + 30ms;
    std::string_view("MonocleMenu").copy(menu.text.data(), 11);
    check(manager.handle(menu) &&
        backend->continuous.back().kind == sds::HapticContinuousKind::BoostpackThrust,
        "unrelated menu processing preserves active thrust");

    const auto commandCountBeforeRefresh = backend->commands.size();
    const auto continuousCountBeforeRefresh = backend->continuous.size();
    check(manager.refreshBoostpackThrust(),
        "repeated exact Wwise authority refreshes thrust body");
    check(
        backend->commands.size() == commandCountBeforeRefresh,
        "refresh does not emit a second ignition kick");
    check(
        backend->continuous.size() == continuousCountBeforeRefresh + 1u &&
            backend->continuous.back().kind == sds::HapticContinuousKind::BoostpackThrust,
        "refresh keeps the same sustained thrust body");

    auto live = sds::gameplayHapticsLiveSettings(config);
    live.boostpackHapticsStrength = 0.50F;
    manager.applyLiveSettings(live);
    check(
        !backend->continuous.empty() &&
            backend->continuous.back().kind == sds::HapticContinuousKind::BoostpackThrust &&
            near(backend->continuous.back().gain, 0.075F),
        "live BoostpackHapticsStrength retunes active body immediately");

    live.boostpackHaptics = false;
    manager.applyLiveSettings(live);
    check(
        !backend->continuous.empty() &&
            backend->continuous.back().kind != sds::HapticContinuousKind::BoostpackThrust,
        "live BoostpackHaptics=false clears boostpack delivery without manager reconstruction");

    const auto mutedCommands = backend->commands.size();
    check(manager.emitBoostpackIgnition(t0 + 100ms),
        "muted boostpack ignition semantic remains accepted");
    check(
        backend->commands.size() == mutedCommands,
        "BoostpackHaptics=false suppresses ignition delivery only");

    live.boostpackHaptics = true;
    manager.applyLiveSettings(live);
    check(
        backend->creates == 1 &&
            !backend->continuous.empty() &&
            backend->continuous.back().kind == sds::HapticContinuousKind::BoostpackThrust,
        "live boostpack re-enable restores active body on the same backend");

    live.advancedHaptics = false;
    manager.applyLiveSettings(live);
    check(
        !backend->continuous.empty() &&
            backend->continuous.back() == sds::HapticContinuousState{},
        "global AdvancedHaptics=false hard-clears active boostpack body");

    const auto globallyMutedCommands = backend->commands.size();
    check(manager.emitBoostpackIgnition(t0 + 150ms),
        "globally muted ignition semantic remains accepted");
    check(
        backend->commands.size() == globallyMutedCommands,
        "global AdvancedHaptics=false suppresses boostpack ignition delivery");

    live.advancedHaptics = true;
    manager.applyLiveSettings(live);
    check(
        backend->creates == 1 &&
            !backend->continuous.empty() &&
            backend->continuous.back().kind == sds::HapticContinuousKind::BoostpackThrust,
        "global live re-enable restores active boostpack body without reconstruction");

    std::fill(menu.text.begin(), menu.text.end(), '\0');
    std::string_view("GalaxyStarMapMenu").copy(menu.text.data(), 17);
    menu.type = sds::GameEventType::MenuOpened;
    check(manager.handle(menu), "star map interrupts boostpack thrust");
    menu.type = sds::GameEventType::MenuClosed;
    check(manager.handle(menu) &&
        backend->continuous.back().kind == sds::HapticContinuousKind::None,
        "closing a blocking menu does not revive old boostpack authority");
    check(manager.startBoostpackThrust() &&
        backend->continuous.back().kind == sds::HapticContinuousKind::BoostpackThrust,
        "fresh thrust authority can resume after the menu");

    check(manager.stopBoostpackThrust(),
        "production stop clears authoritative boostpack thrust body");
    check(
        !backend->continuous.empty() &&
            backend->continuous.back().kind != sds::HapticContinuousKind::BoostpackThrust,
        "boostpack stop returns continuous delivery away from boostpack body");

    sds::GameEvent ship{};
    ship.type = sds::GameEventType::ShipPilotEntered;
    ship.when = t0 + 1s;
    check(manager.handle(ship),
        "ship entry remains a consumed haptics context boundary");
    const auto shipContinuousCount = backend->continuous.size();
    check(manager.startBoostpackThrust(),
        "out-of-context boostpack start is safely consumed");
    check(
        backend->continuous.size() == shipContinuousCount ||
            backend->continuous.back().kind != sds::HapticContinuousKind::BoostpackThrust,
        "ship context cannot acquire on-foot boostpack body");

    manager.stop();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "PASS Task 5F production boostpack haptics contract\n";
    return EXIT_SUCCESS;
}