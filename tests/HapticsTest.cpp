#include <StarfieldDualSense/HapticsEngine.h>
#include <StarfieldDualSense/HapticsManager.h>
#include <StarfieldDualSense/RuntimeEventRouter.h>

#ifdef _WIN32
#include <StarfieldDualSense/DualSenseAudioHapticsClient.h>
#include <type_traits>
static_assert(std::is_base_of_v<sds::IHapticsBackend, sds::DualSenseAudioHapticsClient>);
#endif

#include <StarfieldDualSense/HapticCommandQueue.h>
#include <StarfieldDualSense/HapticEndpointSelection.h>
#include <StarfieldDualSense/HapticMixer.h>
#include <StarfieldDualSense/HapticWaveforms.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

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

    sds::GameEvent equip(std::string_view weapon)
    {
        sds::GameEvent event{};
        event.type = sds::GameEventType::WeaponEquipped;
        const auto count = (std::min)(weapon.size(), event.text.size() - 1);
        std::memcpy(event.text.data(), weapon.data(), count);
        event.text[count] = '\0';
        return event;
    }

    sds::GameEvent fire(std::chrono::steady_clock::time_point when)
    {
        sds::GameEvent event{};
        event.type = sds::GameEventType::WeaponFired;
        event.when = when;
        std::strncpy(event.text.data(), "WeaponFire", event.text.size() - 1);
        return event;
    }

    sds::GameEvent fireMarker(std::string_view marker, std::chrono::steady_clock::time_point when)
    {
        sds::GameEvent event{};
        event.type = sds::GameEventType::WeaponFired;
        event.when = when;
        const auto count = (std::min)(marker.size(), event.text.size() - 1);
        std::memcpy(event.text.data(), marker.data(), count);
        event.text[count] = '\0';
        return event;
    }

    sds::GameEvent meleeSwing(std::chrono::steady_clock::time_point when)
    {
        sds::GameEvent event{};
        event.type = sds::GameEventType::MeleeSwing;
        event.when = when;
        return event;
    }

    sds::GameEvent meleeImpact(std::uint32_t sourceFormId, std::chrono::steady_clock::time_point when)
    {
        sds::GameEvent event{};
        event.type = sds::GameEventType::MeleeImpact;
        event.formId = sourceFormId;
        event.when = when;
        return event;
    }

    float actuatorRms(const std::vector<sds::HapticFrame>& frames)
    {
        double sum = 0.0;
        for (const auto& frame : frames) {
            sum += static_cast<double>(frame[2]) * frame[2];
        }
        return frames.empty() ? 0.0F :
            static_cast<float>(std::sqrt(sum / frames.size()));
    }

    float actuatorRmsWindow(
        const std::vector<sds::HapticFrame>& frames,
        float startMs,
        float endMs,
        float sampleRate = 48000.0F)
    {
        if (frames.empty() || sampleRate <= 0.0F || endMs <= startMs) {
            return 0.0F;
        }

        const auto start = (std::min)(
            frames.size(),
            static_cast<std::size_t>(std::lround(startMs * sampleRate / 1000.0F)));
        const auto end = (std::min)(
            frames.size(),
            static_cast<std::size_t>(std::lround(endMs * sampleRate / 1000.0F)));
        if (end <= start) {
            return 0.0F;
        }

        double sum = 0.0;
        for (std::size_t i = start; i < end; ++i) {
            sum += static_cast<double>(frames[i][2]) * frames[i][2];
        }
        return static_cast<float>(std::sqrt(sum / static_cast<double>(end - start)));
    }


    float lowBandRms(const std::vector<sds::HapticFrame>& frames, float cutoffHz = 150.0F, float sampleRate = 48000.0F)
    {
        if (frames.empty() || cutoffHz <= 0.0F || sampleRate <= 0.0F) {
            return 0.0F;
        }
        const float dt = 1.0F / sampleRate;
        const float rc = 1.0F / (2.0F * std::numbers::pi_v<float> * cutoffHz);
        const float alpha = dt / (rc + dt);
        double sum = 0.0;
        float filtered = 0.0F;
        for (const auto& frame : frames) {
            filtered += alpha * (frame[2] - filtered);
            sum += static_cast<double>(filtered) * filtered;
        }
        return static_cast<float>(std::sqrt(sum / frames.size()));
    }
    std::size_t zeroCrossings(const std::vector<sds::HapticFrame>& frames)
    {
        std::size_t count = 0;
        for (std::size_t i = 1; i < frames.size(); ++i) {
            const bool a = frames[i - 1][2] >= 0.0F;
            const bool b = frames[i][2] >= 0.0F;
            if (a != b) {
                ++count;
            }
        }
        return count;
    }
}

struct FakeHapticsState
{
    std::atomic<int> starts{ 0 };
    std::atomic<int> stops{ 0 };
    std::atomic<int> enqueues{ 0 };
    std::atomic<bool> active{ false };
    std::atomic<bool> accept{ true };
    std::atomic<int> continuousSets{ 0 };
    std::mutex continuousMutex{};
    sds::HapticContinuousState continuous{};
    sds::HapticCommand last{};
};

class FakeHapticsBackend final : public sds::IHapticsBackend
{
public:
    explicit FakeHapticsBackend(std::shared_ptr<FakeHapticsState> state) : _state(std::move(state)) {}
    void start() override { ++_state->starts; _state->active = true; }
    void stop() noexcept override { ++_state->stops; _state->active = false; }
    bool enqueue(sds::HapticCommand command) noexcept override
    {
        ++_state->enqueues;
        _state->last = command;
        return _state->accept.load();
    }
    bool setContinuous(sds::HapticContinuousState state) noexcept override
    {
        {
            std::scoped_lock lock(_state->continuousMutex);
            _state->continuous = state;
        }
        ++_state->continuousSets;
        return true;
    }
    bool active() const noexcept override { return _state->active.load(); }
private:
    std::shared_ptr<FakeHapticsState> _state;
};

int main()
{
    const auto now = std::chrono::steady_clock::now();

    expect(sds::isMeleeImpactHapticEffect(sds::HapticEffectKind::MeleeLightImpact),
        "delivery trace classifies light melee impact as an impact command");
    expect(sds::isMeleeImpactHapticEffect(sds::HapticEffectKind::MeleeHeavyImpact),
        "delivery trace classifies heavy melee impact as an impact command");
    expect(sds::isMeleeImpactHapticEffect(sds::HapticEffectKind::MeleeVeryHeavyImpact),
        "delivery trace classifies very-heavy melee impact as an impact command");
    expect(!sds::isMeleeImpactHapticEffect(sds::HapticEffectKind::MeleeLightSwing),
        "delivery trace does not classify melee swing as impact");
    expect(sds::hapticEffectKindName(sds::HapticEffectKind::MeleeVeryHeavyImpact) ==
               std::string_view("MeleeVeryHeavyImpact"),
        "delivery trace exposes stable melee impact effect names");

    sds::HapticsEngine eon(1.0F);
    expect(!eon.handle(equip("Eon")), "Eon equip updates state without emitting haptics");
    const auto eonShot = eon.handle(fire(now));
    expect(eonShot && eonShot->kind == sds::HapticEffectKind::EonSnap,
        "confirmed Eon WeaponFired maps to one EonSnap");
    expect(eonShot && std::fabs(eonShot->gain - 0.3F) < 0.0001F,
        "Eon hapticRating 3 maps to 0.3 gain at HapticStrength 1.0");

    sds::Config eonGroupConfig{};
    eonGroupConfig.weaponHapticsBallisticHandgunsStrength = 2.0F;
    sds::HapticsEngine tunedEon(1.0F);
    expect(tunedEon.setWeaponHapticsConfig(eonGroupConfig),
        "weapon-group settings accept a ballistic-handgun strength change");
    (void)tunedEon.handle(equip("Eon"));
    const auto tunedEonShot = tunedEon.handle(fire(now));
    expect(tunedEonShot && std::fabs(tunedEonShot->gain - 0.6F) < 0.0001F,
        "ballistic-handgun group strength multiplies Eon on both transport backends");

    eonGroupConfig.weaponHapticsBallisticHandguns = false;
    expect(tunedEon.setWeaponHapticsConfig(eonGroupConfig),
        "weapon-group settings accept disabling ballistic handguns");
    expect(!tunedEon.handle(fire(now)),
        "disabled ballistic-handgun group suppresses Eon weapon vibration");

    eonGroupConfig.weaponHapticsBallisticHandguns = true;
    eonGroupConfig.weaponHaptics = false;
    expect(tunedEon.setWeaponHapticsConfig(eonGroupConfig),
        "weapon-group settings accept master weapon-haptics disable");
    expect(!tunedEon.handle(fire(now)),
        "WeaponHaptics=false suppresses weapon vibration without muting non-weapon haptics");
    expect(eonShot && eonShot->when == now,
        "haptic command preserves originating WeaponFired timestamp");

    sds::HapticsEngine bridger(1.0F);
    expect(!bridger.handle(equip("Bridger")), "Bridger equip updates state without emitting haptics");
    const auto bridgerShot = bridger.handle(fire(now));
    expect(bridgerShot && bridgerShot->kind == sds::HapticEffectKind::BridgerConcussion,
        "confirmed Bridger WeaponFired maps to one BridgerConcussion");
    expect(bridgerShot && std::fabs(bridgerShot->gain - 1.0F) < 0.0001F,
        "Bridger hapticRating 10 maps to 1.0 gain");

    sds::HapticsEngine negotiator(1.0F);
    expect(!negotiator.handle(equip("Negotiator")),
        "Negotiator equip updates state without emitting haptics");
    const auto negotiatorShot = negotiator.handle(fire(now));
    expect(negotiatorShot && negotiatorShot->kind == sds::HapticEffectKind::LauncherConcussion,
        "confirmed Negotiator WeaponFired maps to generic LauncherConcussion");
    expect(negotiatorShot && std::fabs(negotiatorShot->gain - 1.0F) < 0.0001F,
        "Negotiator hapticRating 10 maps to 1.0 gain");

    sds::HapticsEngine breechblock(1.0F);
    (void)breechblock.handle(equip("Breechblock"));
    const auto breechblockShot = breechblock.handle(fire(now));
    expect(breechblockShot && breechblockShot->kind == sds::HapticEffectKind::LauncherConcussion,
        "confirmed Breechblock WeaponFired maps to generic LauncherConcussion");

    sds::HapticsEngine penumbra(1.0F);
    (void)penumbra.handle(equip("Va'ruun Penumbra"));
    const auto penumbraShot = penumbra.handle(fire(now));
    expect(penumbraShot && penumbraShot->kind == sds::HapticEffectKind::ParticleLauncherConcussion,
        "confirmed Va'ruun Penumbra WeaponFired maps to ParticleLauncherConcussion");
    expect(penumbraShot && std::fabs(penumbraShot->gain - 0.9F) < 0.0001F,
        "Va'ruun Penumbra hapticRating 9 maps to 0.9 gain");

    const auto penumbraBelow = penumbra.handleRightTriggerInput(23);
    expect(penumbraBelow.kind == sds::HapticContinuousKind::None,
        "Va'ruun Penumbra R2 below 24 does not start stress haptics");
    const auto penumbraStressStart = penumbra.handleRightTriggerInput(24);
    expect(penumbraStressStart.kind == sds::HapticContinuousKind::PenumbraStress,
        "Va'ruun Penumbra R2=24 starts continuous stress haptics");
    expect(std::fabs(penumbraStressStart.gain - 0.9F) < 0.0001F &&
           std::fabs(penumbraStressStart.level - 1.0F) < 0.0001F,
        "Va'ruun Penumbra stress starts at fixed authored intensity");
    const auto penumbraStressDeep = penumbra.handleRightTriggerInput(220);
    expect(penumbraStressDeep == penumbraStressStart,
        "Va'ruun Penumbra stress intensity does not scale with trigger depth");
    const auto penumbraStressHysteresis = penumbra.handleRightTriggerInput(13);
    expect(penumbraStressHysteresis == penumbraStressStart,
        "Va'ruun Penumbra stress remains active through the 13-23 hysteresis band");
    const auto penumbraStressRelease = penumbra.handleRightTriggerInput(12);
    expect(penumbraStressRelease.kind == sds::HapticContinuousKind::None,
        "Va'ruun Penumbra R2<=12 immediately clears stress haptics");
    expect(penumbra.handleRightTriggerInput(20).kind == sds::HapticContinuousKind::None,
        "Va'ruun Penumbra stress cannot restart inside the hysteresis band");
    expect(negotiator.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None &&
           breechblock.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "Negotiator and Breechblock remain free of held-trigger stress haptics");

    const auto launcherWave = negotiatorShot ? sds::synthesizeHapticEffect(*negotiatorShot) : sds::HapticWaveform{};
    const auto particleLauncherWave = penumbraShot ? sds::synthesizeHapticEffect(*penumbraShot) : sds::HapticWaveform{};
    expect(launcherWave.size() == 5760,
        "LauncherConcussion waveform is exactly 120 ms at 48 kHz");
    expect(particleLauncherWave.size() == 4800,
        "ParticleLauncherConcussion waveform is exactly 100 ms at 48 kHz");
    expect(!launcherWave.empty() && launcherWave.back() == sds::HapticFrame{},
        "LauncherConcussion terminates at exact silence");
    expect(!particleLauncherWave.empty() && particleLauncherWave.back() == sds::HapticFrame{},
        "ParticleLauncherConcussion terminates at exact silence");
    bool launcherChannelsSafe = true;
    for (const auto& frame : launcherWave) {
        launcherChannelsSafe = launcherChannelsSafe && frame[0] == 0.0F && frame[1] == 0.0F &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    for (const auto& frame : particleLauncherWave) {
        launcherChannelsSafe = launcherChannelsSafe && frame[0] == 0.0F && frame[1] == 0.0F &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(launcherChannelsSafe,
        "launcher haptics keep reserved channels silent and actuator output bounded");
    sds::GameEvent launcherAim{};
    launcherAim.type = sds::GameEventType::AimStarted;
    expect(!negotiator.handle(launcherAim),
        "launcher haptics remain confirmed-WeaponFired only");

    sds::HapticsEngine magshot(1.0F);
    expect(!magshot.handle(equip("Magshot")),
        "Magshot equip updates state without emitting haptics");
    const auto magshotShot = magshot.handle(fire(now));
    expect(magshotShot && magshotShot->kind == sds::HapticEffectKind::MagneticPulse,
        "confirmed Magshot WeaponFired maps to MagneticPulse");
    expect(magshotShot && std::fabs(magshotShot->gain - 0.6F) < 0.0001F,
        "Magshot hapticRating 6 maps to 0.6 gain");

    sds::HapticsEngine magpulse(1.0F);
    (void)magpulse.handle(equip("Magpulse"));
    const auto magpulseShot = magpulse.handle(fire(now));
    expect(magpulseShot && magpulseShot->kind == sds::HapticEffectKind::MagneticPulse,
        "confirmed Magpulse WeaponFired maps to MagneticPulse");
    expect(magpulseShot && std::fabs(magpulseShot->gain - 0.7F) < 0.0001F,
        "Magpulse hapticRating 7 makes its shared magnetic pulse stronger than Magshot");

    sds::HapticsEngine magshear(1.0F);
    (void)magshear.handle(equip("Magshear"));
    const auto magshearShot = magshear.handle(fire(now));
    expect(magshearShot && magshearShot->kind == sds::HapticEffectKind::MagneticRapid,
        "confirmed Magshear WeaponFired maps to short MagneticRapid");
    expect(magshearShot && std::fabs(magshearShot->gain - 0.5F) < 0.0001F,
        "Magshear hapticRating 5 maps to 0.5 gain");

    sds::HapticsEngine magstorm(1.0F);
    (void)magstorm.handle(equip("Magstorm"));
    const auto magstormShot = magstorm.handle(fire(now));
    expect(magstormShot && magstormShot->kind == sds::HapticEffectKind::MagneticRapid,
        "confirmed Magstorm WeaponFired maps to short MagneticRapid");
    expect(magstormShot && std::fabs(magstormShot->gain - 0.8F) < 0.0001F,
        "Magstorm hapticRating 8 makes its rapid magnetic impulse denser than Magshear");

    sds::HapticsEngine magsniper(1.0F);
    (void)magsniper.handle(equip("Magsniper"));
    const auto magsniperShot = magsniper.handle(fire(now));
    expect(magsniperShot && magsniperShot->kind == sds::HapticEffectKind::MagneticPrecision,
        "confirmed Magsniper WeaponFired maps to heavy MagneticPrecision");
    expect(magsniperShot && std::fabs(magsniperShot->gain - 0.9F) < 0.0001F,
        "Magsniper hapticRating 9 maps to 0.9 gain");
    const auto magsniperChargeBelow = magsniper.handleRightTriggerInput(23);
    expect(magsniperChargeBelow.kind == sds::HapticContinuousKind::None,
        "Magsniper R2 below 24 does not start charge haptics");
    const auto magsniperChargeStart = magsniper.handleRightTriggerInput(24);
    expect(magsniperChargeStart.kind == sds::HapticContinuousKind::MagsniperCharge,
        "Magsniper R2=24 starts continuous magnetic charge haptics");
    expect(std::fabs(magsniperChargeStart.gain - 0.9F) < 0.0001F &&
           std::fabs(magsniperChargeStart.level - 1.0F) < 0.0001F,
        "Magsniper charge starts at fixed authored intensity");
    const auto magsniperChargeDeep = magsniper.handleRightTriggerInput(220);
    expect(magsniperChargeDeep == magsniperChargeStart,
        "Magsniper charge intensity remains constant with trigger depth");
    const auto magsniperChargedShot = magsniper.handle(fire(now));
    expect(magsniperChargedShot &&
           magsniperChargedShot->kind == sds::HapticEffectKind::MagneticPrecision,
        "confirmed Magsniper fire still emits the approved precision discharge while charging");
    expect(magsniper.handleRightTriggerInput(220) == magsniperChargeStart,
        "confirmed Magsniper discharge does not cancel the held charge layer before R2 release");
    const auto magsniperChargeHysteresis = magsniper.handleRightTriggerInput(13);
    expect(magsniperChargeHysteresis == magsniperChargeStart,
        "Magsniper charge remains active through the 13-23 hysteresis band");
    expect(magsniper.handleRightTriggerInput(12).kind == sds::HapticContinuousKind::None,
        "Magsniper R2<=12 immediately clears charge haptics");
    expect(magsniper.handleRightTriggerInput(20).kind == sds::HapticContinuousKind::None,
        "Magsniper charge cannot restart inside the hysteresis band");

    (void)magsniper.handleRightTriggerInput(80);
    sds::GameEvent magsniperPause{};
    magsniperPause.type = sds::GameEventType::GamePaused;
    (void)magsniper.handle(magsniperPause);
    expect(magsniper.handleRightTriggerInput(20).kind == sds::HapticContinuousKind::None,
        "pausing clears Magsniper charge authorization");
    (void)magsniper.handleRightTriggerInput(80);
    (void)magsniper.handle(equip("Magshot"));
    expect(magsniper.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "weapon swap clears Magsniper charge and does not leak into other magnetic weapons");

    const auto magneticPulseWave = magshotShot ?
        sds::synthesizeHapticEffect(*magshotShot) : sds::HapticWaveform{};
    const auto magneticRapidWave = magshearShot ?
        sds::synthesizeHapticEffect(*magshearShot) : sds::HapticWaveform{};
    const auto magneticPrecisionWave = magsniperShot ?
        sds::synthesizeHapticEffect(*magsniperShot) : sds::HapticWaveform{};
    expect(magneticPulseWave.size() == 2016,
        "MagneticPulse waveform is exactly 42 ms at 48 kHz");
    expect(magneticRapidWave.size() == 864,
        "MagneticRapid waveform is exactly 18 ms at 48 kHz");
    expect(magneticPrecisionWave.size() == 4080,
        "MagneticPrecision waveform is exactly 85 ms at 48 kHz");
    expect(!magneticPulseWave.empty() && magneticPulseWave.back() == sds::HapticFrame{} &&
           !magneticRapidWave.empty() && magneticRapidWave.back() == sds::HapticFrame{} &&
           !magneticPrecisionWave.empty() && magneticPrecisionWave.back() == sds::HapticFrame{},
        "all magnetic waveforms terminate at exact silence");
    bool magneticChannelsSafe = true;
    bool magneticHasActuatorOutput = false;
    for (const auto* wave : { &magneticPulseWave, &magneticRapidWave, &magneticPrecisionWave }) {
        for (const auto& frame : *wave) {
            magneticChannelsSafe = magneticChannelsSafe &&
                frame[0] == 0.0F && frame[1] == 0.0F &&
                std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
                std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
            magneticHasActuatorOutput = magneticHasActuatorOutput ||
                std::fabs(frame[2]) > 0.0001F || std::fabs(frame[3]) > 0.0001F;
        }
    }
    expect(magneticChannelsSafe,
        "magnetic haptics keep reserved channels silent and actuator output bounded");
    expect(magneticHasActuatorOutput,
        "magnetic haptics drive the DualSense actuator channels");
    sds::GameEvent magneticAim{};
    magneticAim.type = sds::GameEventType::AimStarted;
    expect(!magshot.handle(magneticAim),
        "magnetic haptics remain confirmed-WeaponFired only");

    sds::HapticsEngine microgun(1.0F);
    expect(!microgun.handle(equip("Microgun")),
        "Microgun equip updates state without emitting haptics");
    const auto microgunShot = microgun.handle(fire(now));
    expect(microgunShot && microgunShot->kind == sds::HapticEffectKind::MicrogunKick,
        "confirmed Microgun WeaponFired maps to one MicrogunKick");
    expect(microgunShot && std::fabs(microgunShot->gain - 0.7F) < 0.0001F,
        "Microgun hapticRating 7 maps to 0.7 gain at HapticStrength 1.0");

    const auto microgunWave = sds::synthesizeHapticEffect(*microgunShot);
    expect(microgunWave.size() == 672,
        "Microgun waveform is exactly 14 ms at 48 kHz");
    expect(!microgunWave.empty() && microgunWave.back() == sds::HapticFrame{},
        "Microgun waveform terminates at exact silence");

    bool microgunReservedSilent = true;
    bool microgunFiniteBounded = true;
    bool microgunActuatorOutput = false;
    for (const auto& frame : microgunWave) {
        microgunReservedSilent = microgunReservedSilent &&
            frame[0] == 0.0F && frame[1] == 0.0F;
        microgunFiniteBounded = microgunFiniteBounded &&
            std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
        microgunActuatorOutput = microgunActuatorOutput ||
            std::fabs(frame[2]) > 0.0001F || std::fabs(frame[3]) > 0.0001F;
    }
    expect(microgunReservedSilent, "Microgun keeps channels 1 and 2 silent");
    expect(microgunFiniteBounded, "Microgun samples remain finite and normalized");
    expect(microgunActuatorOutput, "Microgun drives haptic actuator channels");

    sds::GameEvent aimOnly{};
    aimOnly.type = sds::GameEventType::AimStarted;
    aimOnly.value = 1.0F;
    expect(!microgun.handle(aimOnly),
        "non-fire game events cannot create MicrogunKick");

    sds::HapticsEngine shotgun(1.0F);
    expect(!shotgun.handle(equip("Coachman")),
        "Coachman equip updates state without emitting haptics");
    const auto shotgunShot = shotgun.handle(fire(now));
    expect(shotgunShot && shotgunShot->kind == sds::HapticEffectKind::ShotgunBlast,
        "confirmed Coachman WeaponFired maps to ShotgunBlast");
    expect(shotgunShot && std::fabs(shotgunShot->gain - 0.8F) < 0.0001F,
        "Coachman hapticRating 8 maps to 0.8 gain");

    sds::HapticsEngine laser(1.0F);
    expect(!laser.handle(equip("Equinox")),
        "Equinox equip updates state without emitting haptics");
    const auto laserShot = laser.handle(fire(now));
    expect(laserShot && laserShot->kind == sds::HapticEffectKind::LaserPulse,
        "confirmed Equinox WeaponFired maps to LaserPulse");
    expect(laserShot && std::fabs(laserShot->gain - 0.4F) < 0.0001F,
        "Equinox hapticRating 4 maps to 0.4 gain");

    sds::HapticsEngine particle(1.0F);
    expect(!particle.handle(equip("Va'ruun Inflictor")),
        "Va'ruun Inflictor equip updates state without emitting haptics");
    const auto particleShot = particle.handle(fire(now));
    expect(particleShot && particleShot->kind == sds::HapticEffectKind::ParticlePulse,
        "confirmed Va'ruun Inflictor WeaponFired maps to ParticlePulse");
    expect(particleShot && std::fabs(particleShot->gain - 0.7F) < 0.0001F,
        "Va'ruun Inflictor hapticRating 7 maps to 0.7 gain");

    sds::HapticsEngine particleShotgun(1.0F);
    (void)particleShotgun.handle(equip("Big Bang"));
    const auto bigBangShot = particleShotgun.handle(fire(now));
    expect(bigBangShot && bigBangShot->kind == sds::HapticEffectKind::ParticlePulse,
        "particle shotgun Big Bang uses ParticlePulse instead of ballistic ShotgunBlast");

    const auto shotgunWave = sds::synthesizeHapticEffect(*shotgunShot);
    const auto laserWave = sds::synthesizeHapticEffect(*laserShot);
    const auto particleWave = sds::synthesizeHapticEffect(*particleShot);
    expect(shotgunWave.size() == 4320,
        "ShotgunBlast waveform is exactly 90 ms at 48 kHz");
    expect(laserWave.size() == 1920,
        "LaserPulse waveform is exactly 40 ms at 48 kHz after tactile retune");
    expect(particleWave.size() == 3120,
        "ParticlePulse waveform is exactly 65 ms at 48 kHz");
    expect(zeroCrossings(laserWave) >= zeroCrossings(shotgunWave),
        "LaserPulse remains at least as tightly oscillating as ShotgunBlast after tactile retune");
    expect(actuatorRms(laserWave) > 0.10F,
        "Equinox LaserPulse clears the overall hardware-tactility RMS floor at rating 4");
    expect(lowBandRms(laserWave) > 0.085F,
        "Equinox LaserPulse carries a meaningful sub-150 Hz tactile body at rating 4");
    expect(actuatorRms(shotgunWave) > actuatorRms(laserWave),
        "ShotgunBlast carries more body energy than LaserPulse");
    expect(actuatorRms(particleWave) > actuatorRms(laserWave),
        "ParticlePulse is denser than LaserPulse");

    sds::HapticsEngine novablast(1.0F);
    expect(!novablast.handle(equip("Novablast Disruptor")),
        "Novablast equip emits no discrete haptic command");

    const auto rawHeld = novablast.handleRightTriggerInput(255);
    expect(rawHeld.kind == sds::HapticContinuousKind::None,
        "Novablast raw R2 alone cannot authorize charge haptics");

    expect(!novablast.handle(fireMarker("NovablastChargeStart", now)),
        "Novablast native charge-start marker emits no discrete haptic command");

    const auto below = novablast.handleRightTriggerInput(23);
    expect(below.kind == sds::HapticContinuousKind::None &&
           below.gain == 0.0F && below.level == 0.0F,
        "Novablast authorized charge below R2=24 stays silent");

    const auto threshold = novablast.handleRightTriggerInput(24);
    expect(threshold.kind == sds::HapticContinuousKind::NovablastCharge,
        "Novablast native authorization plus R2=24 begins continuous charge");
    expect(std::fabs(threshold.gain - 0.5F) < 0.0001F &&
           std::fabs(threshold.level - 0.0F) < 0.0001F,
        "Novablast threshold uses hapticRating 5 and zero normalized charge");

    const auto full = novablast.handleRightTriggerInput(255);
    expect(full.kind == sds::HapticContinuousKind::NovablastCharge &&
           std::fabs(full.level - 1.0F) < 0.0001F,
        "authorized Novablast R2=255 maps to full charge");

    const auto released = novablast.handleRightTriggerInput(12);
    expect(released.kind == sds::HapticContinuousKind::None,
        "Novablast low R2 clears current continuous output without revoking native authorization");
    const auto reheld = novablast.handleRightTriggerInput(255);
    expect(reheld.kind == sds::HapticContinuousKind::NovablastCharge,
        "Novablast charge authorization persists until native stop marker");

    expect(!novablast.handle(fireMarker("NovablastChargeStop", now)),
        "Novablast native charge-stop marker emits no discrete haptic command");
    expect(novablast.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "Novablast native charge-stop marker revokes held-R2 charge haptics");

    expect(!novablast.handle(fireMarker("NovablastChargeStart", now)),
        "Novablast native charge can re-arm before a real discharge");
    const auto novablastDischarge = novablast.handle(fire(now));
    expect(novablastDischarge.has_value(),
        "confirmed Novablast WeaponFired emits a discrete discharge haptic");
    expect(novablastDischarge && novablastDischarge->kind == sds::HapticEffectKind::NovablastDischarge,
        "confirmed Novablast fire uses its dedicated discharge effect");
    expect(novablastDischarge && std::fabs(novablastDischarge->gain - 0.5F) < 0.0001F,
        "Novablast discharge uses hapticRating 5 at full haptic strength");
    expect(novablast.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "confirmed Novablast discharge defensively revokes charge authorization");

    const auto novablastWave = sds::synthesizeHapticEffect(*novablastDischarge);
    expect(novablastWave.size() == 2880,
        "Novablast discharge waveform is exactly 60 ms at 48 kHz");
    expect(actuatorRms(novablastWave) > 0.10F,
        "Novablast discharge clears the tactile RMS floor at rating 5");
    expect(lowBandRms(novablastWave) > 0.075F,
        "Novablast discharge carries a compact low-frequency EM thump");
    bool novablastChannelsSafe = true;
    for (const auto& frame : novablastWave) {
        novablastChannelsSafe = novablastChannelsSafe &&
            std::fabs(frame[0]) < 0.000001F &&
            std::fabs(frame[1]) < 0.000001F &&
            std::fabs(frame[2]) <= 1.000001F &&
            std::fabs(frame[3]) <= 1.000001F;
    }
    expect(novablastChannelsSafe,
        "Novablast discharge keeps reserved channels silent and actuator output bounded");

    sds::HapticsEngine novablastIsolation(1.0F);
    (void)novablastIsolation.handle(equip("Magsniper"));
    const auto magneticShotIsolation = novablastIsolation.handle(fire(now));
    expect(magneticShotIsolation && magneticShotIsolation->kind != sds::HapticEffectKind::NovablastDischarge,
        "magnetic weapons do not inherit the Novablast discharge effect");
    (void)novablastIsolation.handle(equip("Va'ruun Inflictor"));
    const auto particleShotIsolation = novablastIsolation.handle(fire(now));
    expect(particleShotIsolation && particleShotIsolation->kind != sds::HapticEffectKind::NovablastDischarge,
        "particle weapons do not inherit the Novablast discharge effect");

    (void)novablast.handle(equip("Eon"));
    expect(novablast.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "equipping another weapon suppresses Novablast charge");

    sds::GameEvent shutdown{};
    shutdown.type = sds::GameEventType::Shutdown;
    (void)novablast.handle(shutdown);
    expect(novablast.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "shutdown leaves continuous haptics cleared");

    sds::HapticsEngine cutter(1.0F);
    expect(!cutter.handle(equip("Cutter")),
        "Cutter equip emits no discrete haptic command");
    expect(cutter.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "Cutter R2 travel alone cannot authorize continuous haptics");
    expect(!cutter.handle(fire(now)),
        "Cutter repeated WeaponFire marker emits no discrete haptic command");
    expect(cutter.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "Cutter repeated WeaponFire marker cannot authorize continuous haptics");

    const auto cutterStart = fireMarker("weaponFireStart", now);
    expect(!cutter.handle(cutterStart),
        "Cutter weaponFireStart authorizes without creating a discrete command");
    const auto cutterThreshold = cutter.handleRightTriggerInput(24);
    expect(cutterThreshold.kind == sds::HapticContinuousKind::CutterBeam,
        "Cutter exact weaponFireStart authorizes continuous CutterBeam");
    expect(std::fabs(cutterThreshold.gain - 0.4F) < 0.0001F &&
           std::fabs(cutterThreshold.level) < 0.0001F,
        "Cutter R2=24 uses hapticRating 4 and zero normalized level");
    const auto cutterFull = cutter.handleRightTriggerInput(255);
    expect(cutterFull.kind == sds::HapticContinuousKind::CutterBeam &&
           std::fabs(cutterFull.level - 1.0F) < 0.0001F,
        "Cutter authorized R2=255 maps to full continuous level");
    const auto cutterRelease = cutter.handleRightTriggerInput(12);
    expect(cutterRelease.kind == sds::HapticContinuousKind::None,
        "Cutter R2 release threshold clears continuous haptics");
    expect(cutter.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "Cutter cannot restart from R2 alone after release clear");

    (void)cutter.handle(cutterStart);
    (void)cutter.handle(equip("Eon"));
    expect(cutter.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "Cutter weapon swap clears beam authorization");

    (void)cutter.handle(equip("Cutter"));
    (void)cutter.handle(cutterStart);
    sds::GameEvent paused{};
    paused.type = sds::GameEventType::GamePaused;
    (void)cutter.handle(paused);
    expect(cutter.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "GamePaused clears Cutter beam authorization");

    (void)cutter.handle(equip("Cutter"));
    (void)cutter.handle(cutterStart);
    sds::GameEvent cutterShutdown{};
    cutterShutdown.type = sds::GameEventType::Shutdown;
    (void)cutter.handle(cutterShutdown);
    expect(cutter.handleRightTriggerInput(255).kind == sds::HapticContinuousKind::None,
        "Shutdown clears Cutter beam authorization");

    constexpr auto arcWelderArcKind = sds::HapticContinuousKind::ArcWelderArc;
    sds::HapticsEngine arcWelder(1.0F);
    expect(!arcWelder.handle(equip("Arc Welder")),
        "Arc Welder equip emits no discrete haptic command");
    expect(arcWelder.handleRightTriggerInput(200).kind == sds::HapticContinuousKind::None,
        "Arc Welder R2 travel alone cannot fabricate sustained haptics");
    expect(!arcWelder.handle(fireMarker("weaponFireStart", now)),
        "Arc Welder weaponFireStart authorizes without creating a discrete command");
    const auto arcWelderHeld = arcWelder.handleRightTriggerInput(200);
    expect(arcWelderHeld.kind == arcWelderArcKind &&
           std::fabs(arcWelderHeld.gain - 0.6F) < 0.0001F &&
           std::fabs(arcWelderHeld.level - 1.0F) < 0.0001F,
        "confirmed Arc Welder start drives dedicated full-strength sustained arc while R2 is held");
    expect(arcWelder.handleRightTriggerInput(12).kind == sds::HapticContinuousKind::None,
        "Arc Welder R2 release immediately stops sustained arc haptics");

    sds::HapticsEngine combatKnife(1.0F);
    auto combatKnifeEquip = equip("Combat Knife");
    combatKnifeEquip.formId = 0x00035A48;
    expect(!combatKnife.handle(combatKnifeEquip),
        "Combat Knife equip updates melee haptic state without emitting a command");
    const auto knifeSwing = combatKnife.handle(meleeSwing(now));
    expect(knifeSwing && knifeSwing->kind == sds::HapticEffectKind::MeleeLightSwing,
        "confirmed Combat Knife weaponSwing maps to light melee swing haptics");
    expect(knifeSwing && std::fabs(knifeSwing->gain - 0.3F) < 0.0001F,
        "Combat Knife swing uses haptic rating 3 as 0.3 gain");
    const auto knifeImpact = combatKnife.handle(meleeImpact(0x00035A48, now + std::chrono::milliseconds(1)));
    expect(knifeImpact && knifeImpact->kind == sds::HapticEffectKind::MeleeLightImpact,
        "confirmed Combat Knife TESHit maps to a separate sharp impact haptic");
    expect(!combatKnife.handle(meleeImpact(0x0004F760, now)),
        "Combat Knife ignores TESHit whose source FormID does not match the equipped weapon");
    expect(!combatKnife.handle(fire(now)),
        "Combat Knife does not convert firearm WeaponFire semantics into melee haptics");

    sds::HapticsEngine rescueAxe(1.0F);
    auto rescueEquip = equip("Rescue Axe");
    rescueEquip.formId = 0x0004F760;
    (void)rescueAxe.handle(rescueEquip);
    const auto rescueSwing = rescueAxe.handle(meleeSwing(now));
    const auto rescueImpact = rescueAxe.handle(meleeImpact(0x0004F760, now));
    expect(rescueSwing && rescueSwing->kind == sds::HapticEffectKind::MeleeHeavySwing,
        "confirmed Rescue Axe weaponSwing maps to heavy melee swing haptics");
    expect(rescueImpact && rescueImpact->kind == sds::HapticEffectKind::MeleeHeavyImpact,
        "confirmed Rescue Axe TESHit maps to heavy melee impact haptics");

    sds::HapticsEngine maulingAxe(1.0F);
    auto maulingEquip = equip("Mauling Axe");
    maulingEquip.formId = 0x0300D2D4;
    (void)maulingAxe.handle(maulingEquip);
    const auto maulingSwing = maulingAxe.handle(meleeSwing(now));
    const auto maulingImpact = maulingAxe.handle(meleeImpact(0x0300D2D4, now));
    expect(maulingSwing && maulingSwing->kind == sds::HapticEffectKind::MeleeVeryHeavySwing,
        "confirmed Mauling Axe weaponSwing maps to very-heavy melee swing haptics");
    expect(maulingImpact && maulingImpact->kind == sds::HapticEffectKind::MeleeVeryHeavyImpact,
        "confirmed Mauling Axe TESHit maps to very-heavy melee impact haptics");

    sds::HapticsEngine unmappedMelee(1.0F);
    auto tantoEquip = equip("Tanto");
    tantoEquip.formId = 0x12345678;
    (void)unmappedMelee.handle(tantoEquip);
    expect(!unmappedMelee.handle(meleeSwing(now)) &&
           !unmappedMelee.handle(meleeImpact(0x12345678, now)),
        "v0.2.66 tuning pass leaves other melee weapons haptically unchanged");

    const auto knifeSwingWave = sds::synthesizeHapticEffect(*knifeSwing);
    const auto rescueSwingWave = sds::synthesizeHapticEffect(*rescueSwing);
    const auto maulingSwingWave = sds::synthesizeHapticEffect(*maulingSwing);
    const auto knifeImpactWave = sds::synthesizeHapticEffect(*knifeImpact);
    const auto rescueImpactWave = sds::synthesizeHapticEffect(*rescueImpact);
    const auto maulingImpactWave = sds::synthesizeHapticEffect(*maulingImpact);
    expect(knifeSwingWave.size() == 2640,
        "light melee swing waveform is exactly 55 ms at 48 kHz");
    expect(rescueSwingWave.size() == 4080,
        "heavy melee swing waveform is exactly 85 ms at 48 kHz");
    expect(maulingSwingWave.size() == 5520,
        "very-heavy melee swing waveform is exactly 115 ms at 48 kHz");
    expect(knifeImpactWave.size() == 1440,
        "tuned light melee impact is a short 30 ms tactile knock at 48 kHz");
    expect(rescueImpactWave.size() == 2640,
        "tuned heavy melee impact is a compact 55 ms tactile thud at 48 kHz");
    expect(maulingImpactWave.size() == 5760,
        "Mauling Axe diagnostic impact uses the full 120 ms known-good launcher duration at 48 kHz");
    const auto knownGoodLauncherAtMaulingGain = sds::synthesizeHapticEffect(sds::HapticCommand{
        sds::HapticEffectKind::LauncherConcussion, maulingImpact->gain, maulingImpact->when });
    expect(maulingImpactWave == knownGoodLauncherAtMaulingGain,
        "Mauling Axe impact diagnostic is sample-identical to LauncherConcussion at the same gain");
    expect(actuatorRms(rescueSwingWave) > actuatorRms(knifeSwingWave) &&
           actuatorRms(maulingSwingWave) > actuatorRms(rescueSwingWave),
        "melee swing body rises from knife to Rescue Axe to Mauling Axe");
    expect(actuatorRms(knifeImpactWave) > actuatorRms(knifeSwingWave) &&
           actuatorRms(rescueImpactWave) > actuatorRms(rescueSwingWave) &&
           actuatorRms(maulingImpactWave) > actuatorRms(maulingSwingWave),
        "each melee impact is stronger than its matching swing texture");
    expect(actuatorRms(knifeImpactWave) > actuatorRms(knifeSwingWave) * 1.60F &&
           actuatorRms(rescueImpactWave) > actuatorRms(rescueSwingWave) * 1.60F,
        "v0.2.68 knife and Rescue Axe impacts remain materially stronger than their matching swing textures");
    expect(actuatorRmsWindow(knifeImpactWave, 0.0F, 10.0F) >
               actuatorRmsWindow(knifeSwingWave, 0.0F, 10.0F) * 2.50F &&
           actuatorRmsWindow(rescueImpactWave, 0.0F, 10.0F) >
               actuatorRmsWindow(rescueSwingWave, 0.0F, 10.0F) * 2.50F &&
           actuatorRmsWindow(maulingImpactWave, 0.0F, 10.0F) >
               actuatorRmsWindow(maulingSwingWave, 0.0F, 10.0F) * 2.50F,
        "tuned melee impacts start with an unmistakable front-loaded strike instead of another swing-like texture");

    sds::HapticsEngine ballisticHandgun(1.0F);
    (void)ballisticHandgun.handle(equip("Sidestar"));
    const auto handgunShot = ballisticHandgun.handle(fire(now));
    expect(handgunShot && handgunShot->kind == sds::HapticEffectKind::BallisticHandgunKick,
        "confirmed Sidestar WeaponFired maps to BallisticHandgunKick");
    expect(handgunShot && std::fabs(handgunShot->gain - 0.3F) < 0.0001F,
        "Sidestar hapticRating 3 maps to 0.3 gain");

    sds::HapticsEngine ballisticRapid(1.0F);
    (void)ballisticRapid.handle(equip("Grendel"));
    const auto rapidShot = ballisticRapid.handle(fire(now));
    expect(rapidShot && rapidShot->kind == sds::HapticEffectKind::BallisticRapidKick,
        "confirmed Grendel WeaponFired maps to BallisticRapidKick");

    sds::HapticsEngine ballisticRifle(1.0F);
    (void)ballisticRifle.handle(equip("Maelstrom"));
    const auto maelstromShot = ballisticRifle.handle(fire(now));
    expect(maelstromShot && maelstromShot->kind == sds::HapticEffectKind::BallisticRifleKick,
        "confirmed Maelstrom WeaponFired maps to BallisticRifleKick");
    expect(maelstromShot && std::fabs(maelstromShot->gain - 0.4F) < 0.0001F,
        "Maelstrom hapticRating 4 maps to 0.4 gain");

    sds::HapticsEngine precisionBallistic(1.0F);
    (void)precisionBallistic.handle(equip("Hard Target"));
    const auto precisionShot = precisionBallistic.handle(fire(now));
    expect(precisionShot && precisionShot->kind == sds::HapticEffectKind::PrecisionBallisticKick,
        "confirmed Hard Target WeaponFired maps to PrecisionBallisticKick");
    expect(precisionShot && std::fabs(precisionShot->gain - 0.9F) < 0.0001F,
        "Hard Target hapticRating 9 maps to 0.9 gain");

    sds::HapticsEngine heavyBallistic(1.0F);
    (void)heavyBallistic.handle(equip("Auto-Rivet"));
    const auto autoRivetShot = heavyBallistic.handle(fire(now));
    expect(autoRivetShot && autoRivetShot->kind == sds::HapticEffectKind::PrecisionBallisticKick,
        "non-Microgun heavy ballistic maps to the heavy precision signature");

    const auto handgunWave = sds::synthesizeHapticEffect(*handgunShot);
    const auto rapidWave = sds::synthesizeHapticEffect(*rapidShot);
    const auto rifleWave = sds::synthesizeHapticEffect(*maelstromShot);
    const auto precisionWave = sds::synthesizeHapticEffect(*precisionShot);
    expect(handgunWave.size() == 3360,
        "BallisticHandgunKick USB waveform is exactly 70 ms at 48 kHz");
    expect(actuatorRmsWindow(handgunWave, 25.0F, 45.0F) > 0.07F,
        "ballistic handgun USB waveform retains meaningful energy through the middle buzz window");
    expect(actuatorRmsWindow(handgunWave, 45.0F, 60.0F) > 0.04F,
        "ballistic handgun USB waveform remains tactile into the extended 70 ms buzz tail");
    expect(rapidWave.size() == 768,
        "BallisticRapidKick waveform is exactly 16 ms at 48 kHz");
    expect(rifleWave.size() == 1728,
        "BallisticRifleKick waveform is exactly 36 ms at 48 kHz");
    expect(precisionWave.size() == 2880,
        "PrecisionBallisticKick waveform is exactly 60 ms at 48 kHz");
    expect(actuatorRms(rifleWave) > actuatorRms(rapidWave),
        "ballistic rifle carries more body than rapid ballistic");
    expect(actuatorRms(precisionWave) > actuatorRms(rifleWave),
        "precision ballistic carries more body than ballistic rifle");

    sds::GameEvent ballisticAim{};
    ballisticAim.type = sds::GameEventType::AimStarted;
    expect(!ballisticRifle.handle(ballisticAim),
        "ballistic haptics remain confirmed-WeaponFired only");

    sds::GameEvent nonFire{};
    nonFire.type = sds::GameEventType::AimStarted;
    nonFire.value = 1.0F;
    expect(!eon.handle(nonFire), "non-WeaponFired input state cannot create an Eon haptic shot");

    sds::HapticsEngine clampedHigh(9.0F);
    (void)clampedHigh.handle(equip("Bridger"));
    const auto clamped = clampedHigh.handle(fire(now));
    expect(clamped && std::fabs(clamped->gain - 5.0F) < 0.0001F,
        "HapticStrength overdrive clamps to 5.0 effective gain");

    sds::HapticsEngine overdriveEon(2.0F);
    (void)overdriveEon.handle(equip("Eon"));
    const auto overdriveEonShot = overdriveEon.handle(fire(now));
    expect(overdriveEonShot && std::fabs(overdriveEonShot->gain - 0.75F) < 0.0001F,
        "HapticStrength 2.0 raises Eon 0.3 base gain to 0.75");

    sds::HapticsEngine extremeEon(3.0F);
    (void)extremeEon.handle(equip("Eon"));
    const auto extremeEonShot = extremeEon.handle(fire(now));
    expect(extremeEonShot &&
            std::fabs(extremeEonShot->gain - 1.5F) < 0.0001F,
        "HapticStrength 3.0 raises Eon 0.3 base gain to 1.5");


    const auto packedNone = sds::packHapticContinuousState({});
    expect(packedNone == 0U,
        "None continuous state packs to exact zero");
    expect(sds::unpackHapticContinuousState(packedNone) == sds::HapticContinuousState{},
        "zero packed state round-trips to exact None");

    const sds::HapticContinuousState sourceState{
        sds::HapticContinuousKind::NovablastCharge, 0.5F, 0.75F };
    const auto packedActive = sds::packHapticContinuousState(sourceState);
    const auto unpackedActive = sds::unpackHapticContinuousState(packedActive);
    expect(unpackedActive.kind == sds::HapticContinuousKind::NovablastCharge,
        "packed continuous state preserves kind");
    expect(std::fabs(unpackedActive.gain - 0.5F) <= (1.0F / 255.0F),
        "packed continuous state quantizes gain deterministically");
    expect(std::fabs(unpackedActive.level - 0.75F) <= (1.0F / 255.0F),
        "packed continuous state quantizes level deterministically");

    const auto packedClamped = sds::unpackHapticContinuousState(
        sds::packHapticContinuousState({
            sds::HapticContinuousKind::NovablastCharge, 4.0F, -2.0F }));
    expect(std::fabs(packedClamped.gain - 1.0F) <= (1.0F / 255.0F) &&
           std::fabs(packedClamped.level - 0.0F) <= (1.0F / 255.0F),
        "continuous packing clamps gain and level");

    std::atomic<std::uint32_t> latestContinuous{ 0U };
    latestContinuous.store(sds::packHapticContinuousState({
        sds::HapticContinuousKind::NovablastCharge, 0.5F, 0.2F }));
    latestContinuous.store(sds::packHapticContinuousState({
        sds::HapticContinuousKind::NovablastCharge, 0.5F, 0.9F }));
    const auto latest = sds::unpackHapticContinuousState(latestContinuous.load());
    expect(std::fabs(latest.level - 0.9F) <= (1.0F / 255.0F),
        "latest continuous state replaces older state instead of queueing");

    const auto eonWave = sds::synthesizeHapticEffect(*eonShot);
    const auto bridgerWave = sds::synthesizeHapticEffect(*bridgerShot);

    expect(eonWave.size() == 3360, "Eon USB waveform is exactly 70 ms at 48 kHz");
    expect(bridgerWave.size() == 6720, "Bridger waveform is exactly 140 ms at 48 kHz");
    expect(bridgerWave.size() >= eonWave.size() * 2,
        "Bridger concussion remains at least twice as long as the Eon buzz");
    expect(actuatorRmsWindow(eonWave, 25.0F, 45.0F) > 0.07F,
        "Eon USB waveform retains meaningful actuator energy through the middle buzz window");
    expect(actuatorRmsWindow(eonWave, 45.0F, 60.0F) > 0.04F,
        "Eon USB waveform remains tactile into the extended 70 ms buzz tail");

    bool eonActuatorOutput = false;
    bool bridgerTailOutput = false;
    bool finiteAndBounded = true;
    bool reservedSilent = true;
    bool symmetric = true;
    for (std::size_t i = 0; i < bridgerWave.size(); ++i) {
        const auto& frame = bridgerWave[i];
        reservedSilent = reservedSilent && frame[0] == 0.0F && frame[1] == 0.0F;
        symmetric = symmetric && std::fabs(frame[2] - frame[3]) < 0.000001F;
        for (float sample : frame) {
            finiteAndBounded = finiteAndBounded && std::isfinite(sample) && std::fabs(sample) <= 1.0F;
        }
        if (i >= 2880 && i < 5760 && std::fabs(frame[2]) > 0.001F) {
            bridgerTailOutput = true;
        }
    }
    for (const auto& frame : eonWave) {
        eonActuatorOutput = eonActuatorOutput || std::fabs(frame[2]) > 0.001F;
        reservedSilent = reservedSilent && frame[0] == 0.0F && frame[1] == 0.0F;
        symmetric = symmetric && std::fabs(frame[2] - frame[3]) < 0.000001F;
        for (float sample : frame) {
            finiteAndBounded = finiteAndBounded && std::isfinite(sample) && std::fabs(sample) <= 1.0F;
        }
    }
    expect(eonActuatorOutput, "Eon waveform drives actuator channels");
    expect(bridgerTailOutput, "Bridger retains a decaying body between 60 and 120 ms");
    expect(reservedSilent, "waveforms keep channels 1 and 2 exactly silent");
    expect(symmetric, "v0.2.43 drives left and right haptic actuators symmetrically");
    expect(finiteAndBounded, "waveform samples remain finite and normalized");
    expect(eonWave.back() == sds::HapticFrame{}, "Eon waveform terminates at exact silence");
    expect(bridgerWave.back() == sds::HapticFrame{}, "Bridger waveform terminates at exact silence");

    sds::HapticMixer lowCharge;
    lowCharge.setContinuous({
        sds::HapticContinuousKind::NovablastCharge, 0.5F, 0.10F });
    std::vector<sds::HapticFrame> lowFrames(4800);
    lowCharge.render(lowFrames);

    sds::HapticMixer highCharge;
    highCharge.setContinuous({
        sds::HapticContinuousKind::NovablastCharge, 0.5F, 1.0F });
    std::vector<sds::HapticFrame> highFrames(4800);
    highCharge.render(highFrames);

    expect(actuatorRms(lowFrames) > 0.0F,
        "Novablast low charge drives continuous actuator output");
    expect(actuatorRms(highFrames) > actuatorRms(lowFrames),
        "Novablast high charge has greater RMS amplitude");
    expect(zeroCrossings(highFrames) > zeroCrossings(lowFrames),
        "Novablast high charge rises in pitch");

    bool continuousReservedSilent = true;
    for (const auto& frame : highFrames) {
        continuousReservedSilent = continuousReservedSilent &&
            frame[0] == 0.0F && frame[1] == 0.0F;
    }
    expect(continuousReservedSilent,
        "Novablast continuous layer keeps channels 1 and 2 silent");

    sds::HapticMixer phaseMixer;
    phaseMixer.setContinuous({
        sds::HapticContinuousKind::NovablastCharge, 0.5F, 0.6F });
    std::vector<sds::HapticFrame> firstBlock(257);
    std::vector<sds::HapticFrame> secondBlock(257);
    phaseMixer.render(firstBlock);
    phaseMixer.render(secondBlock);

    sds::HapticMixer restartedMixer;
    restartedMixer.setContinuous({
        sds::HapticContinuousKind::NovablastCharge, 0.5F, 0.6F });
    std::vector<sds::HapticFrame> restartedBlock(257);
    restartedMixer.render(restartedBlock);
    expect(secondBlock != restartedBlock,
        "Novablast oscillator phase persists across render blocks");

    phaseMixer.setContinuous({});
    std::vector<sds::HapticFrame> stoppedBlock(257);
    phaseMixer.render(stoppedBlock);
    expect(std::all_of(stoppedBlock.begin(), stoppedBlock.end(),
            [](const sds::HapticFrame& frame) { return frame == sds::HapticFrame{}; }),
        "setContinuous(None) is exact silence on next block with no finite voices");

    sds::HapticMixer overlapMixer;
    overlapMixer.setContinuous({
        sds::HapticContinuousKind::NovablastCharge, 1.0F, 1.0F });
    overlapMixer.add(sds::synthesizeHapticEffect({
        sds::HapticEffectKind::MicrogunKick, 1.0F, now }));
    std::vector<sds::HapticFrame> overlapFrames(1024);
    overlapMixer.render(overlapFrames);
    bool overlapSafe = true;
    for (const auto& frame : overlapFrames) {
        overlapSafe = overlapSafe &&
            frame[0] == 0.0F && frame[1] == 0.0F &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(overlapSafe,
        "Microgun plus Novablast remains peak-limited with reserved channels silent");

    sds::HapticMixer lowCutter;
    lowCutter.setContinuous({ sds::HapticContinuousKind::CutterBeam, 0.4F, 0.10F });
    std::vector<sds::HapticFrame> lowCutterFrames(4800);
    lowCutter.render(lowCutterFrames);

    sds::HapticMixer highCutter;
    highCutter.setContinuous({ sds::HapticContinuousKind::CutterBeam, 0.4F, 1.0F });
    std::vector<sds::HapticFrame> highCutterFrames(4800);
    highCutter.render(highCutterFrames);

    expect(actuatorRms(lowCutterFrames) > 0.0F,
        "Cutter low level drives continuous actuator output");
    expect(actuatorRms(highCutterFrames) > actuatorRms(lowCutterFrames),
        "Cutter high level has greater RMS amplitude");
    expect(zeroCrossings(highCutterFrames) > zeroCrossings(lowCutterFrames),
        "Cutter high level rises in mechanical texture frequency");
    bool cutterReservedSilent = true;
    for (const auto& frame : highCutterFrames) {
        cutterReservedSilent = cutterReservedSilent &&
            frame[0] == 0.0F && frame[1] == 0.0F;
    }
    expect(cutterReservedSilent,
        "Cutter continuous layer keeps channels 1 and 2 silent");

    double cutterEarlyEnergy = 0.0;
    double cutterLateEnergy = 0.0;
    for (std::size_t i = 0; i < 48; ++i) {
        cutterEarlyEnergy += static_cast<double>(highCutterFrames[i][2]) * highCutterFrames[i][2];
    }
    for (std::size_t i = 288; i < 576; ++i) {
        cutterLateEnergy += static_cast<double>(highCutterFrames[i][2]) * highCutterFrames[i][2];
    }
    expect(cutterLateEnergy / 288.0 > cutterEarlyEnergy / 48.0,
        "Cutter 6 ms attack ramps up rather than starting at full energy");

    sds::HapticMixer cutterPhaseMixer;
    cutterPhaseMixer.setContinuous({ sds::HapticContinuousKind::CutterBeam, 0.4F, 0.35F });
    std::vector<sds::HapticFrame> cutterFirstBlock(257);
    cutterPhaseMixer.render(cutterFirstBlock);
    cutterPhaseMixer.setContinuous({ sds::HapticContinuousKind::CutterBeam, 0.4F, 0.85F });
    std::vector<sds::HapticFrame> cutterSecondBlock(257);
    cutterPhaseMixer.render(cutterSecondBlock);

    sds::HapticMixer cutterRestartedMixer;
    cutterRestartedMixer.setContinuous({ sds::HapticContinuousKind::CutterBeam, 0.4F, 0.85F });
    std::vector<sds::HapticFrame> cutterRestartedBlock(257);
    cutterRestartedMixer.render(cutterRestartedBlock);
    expect(cutterSecondBlock != cutterRestartedBlock,
        "Cutter level update preserves oscillator phase and attack progress");

    cutterPhaseMixer.setContinuous({});
    std::vector<sds::HapticFrame> cutterStoppedBlock(257);
    cutterPhaseMixer.render(cutterStoppedBlock);
    expect(std::all_of(cutterStoppedBlock.begin(), cutterStoppedBlock.end(),
            [](const sds::HapticFrame& frame) { return frame == sds::HapticFrame{}; }),
        "Cutter None state is exact silence on the next block");

    sds::HapticMixer cutterOverlapMixer;
    cutterOverlapMixer.setContinuous({ sds::HapticContinuousKind::CutterBeam, 1.0F, 1.0F });
    cutterOverlapMixer.add(sds::synthesizeHapticEffect({
        sds::HapticEffectKind::EonSnap, 1.0F, now }));
    std::vector<sds::HapticFrame> cutterOverlapFrames(1024);
    cutterOverlapMixer.render(cutterOverlapFrames);
    bool cutterOverlapSafe = true;
    for (const auto& frame : cutterOverlapFrames) {
        cutterOverlapSafe = cutterOverlapSafe &&
            frame[0] == 0.0F && frame[1] == 0.0F &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(cutterOverlapSafe,
        "finite voice plus Cutter remains peak-limited with reserved channels silent");

    sds::HapticMixer arcWelderMixer;
    arcWelderMixer.setContinuous({ arcWelderArcKind, 0.6F, 1.0F });
    std::vector<sds::HapticFrame> arcWelderFrames(4800);
    arcWelderMixer.render(arcWelderFrames);
    expect(actuatorRms(arcWelderFrames) > 0.10F,
        "Arc Welder sustained arc produces clearly tactile actuator output at rating 6");
    expect(lowBandRms(arcWelderFrames) > 0.045F,
        "Arc Welder sustained arc carries a meaningful low-frequency tactile body");
    expect(zeroCrossings(arcWelderFrames) > 35,
        "Arc Welder sustained arc includes a fast electrical texture rather than a slow Cutter throb");

    sds::HapticMixer penumbraStressMixer;
    penumbraStressMixer.setContinuous({
        sds::HapticContinuousKind::PenumbraStress, 0.9F, 1.0F });
    std::vector<sds::HapticFrame> penumbraStressFrames(4800);
    penumbraStressMixer.render(penumbraStressFrames);
    expect(actuatorRms(penumbraStressFrames) > 0.08F,
        "Penumbra stress produces clearly tactile continuous actuator output");
    expect(actuatorRms(penumbraStressFrames) < actuatorRms(particleLauncherWave),
        "Penumbra held-trigger stress remains lighter than the confirmed launch concussion");
    bool penumbraStressChannelsSafe = true;
    for (const auto& frame : penumbraStressFrames) {
        penumbraStressChannelsSafe = penumbraStressChannelsSafe &&
            frame[0] == 0.0F && frame[1] == 0.0F &&
            std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(penumbraStressChannelsSafe,
        "Penumbra stress keeps reserved channels silent and actuator output bounded");

    sds::HapticMixer penumbraOverlapMixer;
    penumbraOverlapMixer.setContinuous({
        sds::HapticContinuousKind::PenumbraStress, 0.9F, 1.0F });
    penumbraOverlapMixer.add(particleLauncherWave);
    std::vector<sds::HapticFrame> penumbraOverlapFrames(4800);
    penumbraOverlapMixer.render(penumbraOverlapFrames);
    bool penumbraOverlapSafe = true;
    for (const auto& frame : penumbraOverlapFrames) {
        penumbraOverlapSafe = penumbraOverlapSafe &&
            frame[0] == 0.0F && frame[1] == 0.0F &&
            std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(penumbraOverlapSafe,
        "Penumbra stress plus confirmed launch concussion remains peak-limited");

    sds::HapticMixer magsniperChargeMixer;
    magsniperChargeMixer.setContinuous({
        sds::HapticContinuousKind::MagsniperCharge, 0.9F, 1.0F });
    std::vector<sds::HapticFrame> magsniperChargeFrames(4800);
    magsniperChargeMixer.render(magsniperChargeFrames);
    expect(actuatorRms(magsniperChargeFrames) > 0.06F,
        "Magsniper charge produces clearly tactile continuous actuator output");
    expect(actuatorRms(magsniperChargeFrames) < actuatorRms(magneticPrecisionWave),
        "Magsniper held charge remains lighter than its confirmed precision discharge");
    expect(zeroCrossings(magsniperChargeFrames) > zeroCrossings(penumbraStressFrames),
        "Magsniper charge is a tighter higher-frequency coil texture than Penumbra stress");
    bool magsniperChargeSafe = true;
    for (const auto& frame : magsniperChargeFrames) {
        magsniperChargeSafe = magsniperChargeSafe &&
            frame[0] == 0.0F && frame[1] == 0.0F &&
            std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(magsniperChargeSafe,
        "Magsniper charge keeps reserved channels silent and actuator output bounded");

    sds::HapticMixer magsniperOverlapMixer;
    magsniperOverlapMixer.setContinuous({
        sds::HapticContinuousKind::MagsniperCharge, 0.9F, 1.0F });
    magsniperOverlapMixer.add(magneticPrecisionWave);
    std::vector<sds::HapticFrame> magsniperOverlapFrames(4800);
    magsniperOverlapMixer.render(magsniperOverlapFrames);
    bool magsniperOverlapSafe = true;
    for (const auto& frame : magsniperOverlapFrames) {
        magsniperOverlapSafe = magsniperOverlapSafe &&
            frame[0] == 0.0F && frame[1] == 0.0F &&
            std::isfinite(frame[2]) && std::isfinite(frame[3]) &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(magsniperOverlapSafe,
        "Magsniper charge plus confirmed precision discharge remains peak-limited");

    sds::HapticMixer mixer;
    mixer.add(sds::synthesizeHapticEffect(sds::HapticCommand{
        sds::HapticEffectKind::BridgerConcussion, 1.0F, now }));
    mixer.add(sds::synthesizeHapticEffect(sds::HapticCommand{
        sds::HapticEffectKind::BridgerConcussion, 1.0F, now }));
    std::array<sds::HapticFrame, 512> mixed{};
    mixer.render(mixed);
    bool mixedBounded = true;
    for (const auto& frame : mixed) {
        mixedBounded = mixedBounded && frame[0] == 0.0F && frame[1] == 0.0F &&
            std::fabs(frame[2]) <= 1.0F && std::fabs(frame[3]) <= 1.0F;
    }
    expect(mixedBounded, "overlapping voices peak-limit without contaminating reserved channels");

    const std::array<sds::HapticFrame, 4> diagnosticBlock{{
        sds::HapticFrame{ 0.0F, 0.0F, 0.0F, 0.0F },
        sds::HapticFrame{ 0.0F, 0.0F, 0.5F, -0.25F },
        sds::HapticFrame{ 0.0F, 0.0F, -1.0F, 0.75F },
        sds::HapticFrame{ 0.0F, 0.0F, 0.0F, 0.0F },
    }};
    const auto diagnosticStats = sds::measureHapticBlock(diagnosticBlock);
    expect(std::fabs(diagnosticStats.peakCh3 - 1.0F) < 0.0001F &&
           std::fabs(diagnosticStats.peakCh4 - 0.75F) < 0.0001F,
        "output-buffer diagnostic measures independent actuator peaks");
    expect(std::fabs(diagnosticStats.rmsCh3 - std::sqrt(1.25F / 4.0F)) < 0.0001F &&
           std::fabs(diagnosticStats.rmsCh4 - std::sqrt(0.625F / 4.0F)) < 0.0001F,
        "output-buffer diagnostic measures independent actuator RMS");
    expect(diagnosticStats.nonZeroFrames == 2 &&
           std::fabs(diagnosticStats.firstNonZeroCh3 - 0.5F) < 0.0001F &&
           std::fabs(diagnosticStats.firstNonZeroCh4 + 0.25F) < 0.0001F,
        "output-buffer diagnostic records nonzero frame count and first actuator sample");

    sds::HapticCommandQueue queue;
    for (int i = 0; i < 129; ++i) {
        expect(queue.push(sds::HapticCommand{
            sds::HapticEffectKind::EonSnap,
            0.3F,
            now + std::chrono::milliseconds(i) }),
            "running haptic queue accepts newest command");
    }
    const auto queued = queue.drainFresh(now + std::chrono::milliseconds(129), std::chrono::milliseconds(250));
    const auto drops = queue.takeDropCounts();
    expect(queued.size() == 128, "128-command queue retains exactly its fixed capacity");
    expect(queued.front().when == now + std::chrono::milliseconds(1),
        "queue overflow drops the oldest command and keeps freshness");
    expect(drops.overflow == 1, "queue records one overflow drop");

    sds::HapticCommandQueue staleQueue;
    expect(staleQueue.push({ sds::HapticEffectKind::EonSnap, 0.3F, now }), "stale test command enqueues");
    expect(staleQueue.drainFresh(now + std::chrono::milliseconds(251), std::chrono::milliseconds(250)).empty(),
        "command older than 250 ms is discarded");
    expect(staleQueue.takeDropCounts().stale == 1, "stale command drop is counted");
    staleQueue.stop();
    expect(!staleQueue.push({ sds::HapticEffectKind::EonSnap, 0.3F, now }),
        "stopped haptic queue rejects new commands");

    std::vector<sds::HapticEndpointCandidate> endpoints{
        { L"z-default-speakers", L"Speakers (Realtek Audio)", 48000, 4, sds::HapticSampleFormat::Float32 },
        { L"b-dualsense-stereo", L"Wireless Controller", 48000, 2, sds::HapticSampleFormat::Float32 },
        { L"c-dualsense-wrong-rate", L"DualSense Wireless Controller", 44100, 4, sds::HapticSampleFormat::Float32 },
        { L"d-dualsense-unsupported", L"Wireless Controller", 48000, 4, sds::HapticSampleFormat::Unsupported },
        { L"a-dualsense-valid", L"Wireless Controller", 48000, 4, sds::HapticSampleFormat::Float32 },
        { L"f-dualsense-valid", L"DualSense Wireless Controller", 48000, 4, sds::HapticSampleFormat::Pcm16 },
    };
    const auto selected = sds::selectDualSenseHapticEndpoint(endpoints);
    expect(selected && *selected == 4,
        "endpoint selection rejects unrelated/stereo/wrong-rate/unsupported candidates and chooses lowest valid id");

    std::vector<sds::HapticEndpointCandidate> noDualSense{
        { L"default", L"Speakers", 48000, 4, sds::HapticSampleFormat::Float32 },
    };
    expect(!sds::selectDualSenseHapticEndpoint(noDualSense),
        "endpoint selection never falls back to default PC audio");

    sds::Config disabledConfig = sds::Config::defaults();
    disabledConfig.advancedHaptics = false;
    auto disabledState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager disabledManager(
        disabledConfig,
        [disabledState] { return std::make_unique<FakeHapticsBackend>(disabledState); });
    disabledManager.start();
    expect(disabledState->starts == 0, "AdvancedHaptics=false prevents backend startup");
    expect(disabledManager.handle(equip("Eon")), "disabled manager safely ignores equip event");
    expect(disabledManager.handle(fire(now)), "disabled manager safely ignores fire event");
    expect(disabledState->enqueues == 0, "AdvancedHaptics=false prevents command submission");

    sds::Config enabledConfig = sds::Config::defaults();
    enabledConfig.advancedHaptics = true;
    enabledConfig.hapticStrength = 1.0F;
    auto enabledState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager enabledManager(
        enabledConfig,
        [enabledState] { return std::make_unique<FakeHapticsBackend>(enabledState); });
    enabledManager.start();
    expect(enabledState->starts == 1 && enabledManager.active(), "enabled manager starts independent backend");
    expect(enabledManager.handle(equip("Eon")), "enabled manager consumes equip event");
    expect(enabledManager.handle(fire(now)), "enabled manager submits confirmed Eon fire");
    expect(enabledState->enqueues == 1 && enabledState->last.kind == sds::HapticEffectKind::EonSnap,
        "manager submits exactly one EonSnap to backend");

    enabledState->accept = false;
    int primaryCalls = 0;
    int secondaryCalls = 0;
    sds::RuntimeEventRouter router(
        [&](sds::GameEvent) { ++primaryCalls; return true; },
        [&](sds::GameEvent event) { ++secondaryCalls; return enabledManager.handle(std::move(event)); });
    expect(router.dispatch(fire(now)), "secondary haptics failure does not change primary controller success");
    expect(primaryCalls == 1 && secondaryCalls == 1, "router dispatches primary before fail-isolated secondary");

    sds::RuntimeEventRouter primaryFailure(
        [&](sds::GameEvent) { ++primaryCalls; return false; },
        [&](sds::GameEvent) { ++secondaryCalls; return true; });
    expect(!primaryFailure.dispatch(fire(now)), "primary controller queue failure remains authoritative");
    expect(secondaryCalls == 1, "secondary is not called when primary dispatch fails");

    enabledManager.stop();
    expect(enabledState->stops == 1 && !enabledManager.active(), "manager stops backend independently");

    auto meleeTraceState = std::make_shared<FakeHapticsState>();
    std::vector<std::string> meleeTraceLogs;
    sds::HapticsManager meleeTraceManager(
        enabledConfig,
        [meleeTraceState] { return std::make_unique<FakeHapticsBackend>(meleeTraceState); },
        [&](std::string_view message) { meleeTraceLogs.emplace_back(message); });
    meleeTraceManager.start();
    auto traceKnifeEquip = equip("Combat Knife");
    traceKnifeEquip.formId = 0x00035A48;
    expect(meleeTraceManager.handle(traceKnifeEquip),
        "melee delivery trace manager accepts Combat Knife equip");
    expect(meleeTraceManager.handle(meleeImpact(0x00035A48, now)),
        "melee delivery trace manager accepts confirmed impact");
    expect(meleeTraceState->enqueues == 1 &&
           meleeTraceState->last.kind == sds::HapticEffectKind::MeleeLightImpact,
        "melee delivery trace still submits the same light impact command");
    expect(std::any_of(meleeTraceLogs.begin(), meleeTraceLogs.end(), [](const std::string& line) {
        return line.find("Melee impact delivery: stage=engine-produced") != std::string::npos &&
            line.find("kind=MeleeLightImpact") != std::string::npos &&
            line.find("eventWhenUs=") != std::string::npos;
    }), "melee delivery trace logs engine-produced impact with stable kind and timestamp");
    meleeTraceManager.stop();

    auto continuousState = std::make_shared<FakeHapticsState>();
    sds::Config continuousConfig = sds::Config::defaults();
    continuousConfig.advancedHaptics = true;
    continuousConfig.hapticStrength = 1.0F;

    sds::HapticsManager continuousManager(
        continuousConfig,
        [continuousState] {
            return std::make_unique<FakeHapticsBackend>(continuousState);
        });
    continuousManager.start();

    expect(continuousManager.handleRightTriggerInput(255),
        "manager accepts saved R2 before Novablast equip");
    expect(continuousState->continuousSets.load() >= 1,
        "manager forwards continuous state updates");

    expect(continuousManager.handle(equip("Novablast Disruptor")),
        "manager consumes Novablast equip");
    {
        std::scoped_lock lock(continuousState->continuousMutex);
        expect(continuousState->continuous.kind == sds::HapticContinuousKind::None,
            "equipping Novablast while R2 is held does not fabricate charge state");
    }

    expect(continuousManager.handle(fireMarker("NovablastChargeStart", now)),
        "manager accepts native Novablast charge-start marker");
    {
        std::scoped_lock lock(continuousState->continuousMutex);
        expect(continuousState->continuous.kind ==
                   sds::HapticContinuousKind::NovablastCharge &&
               continuousState->continuous.level > 0.99F,
            "native Novablast charge-start marker authorizes already-held R2");
    }

    expect(continuousManager.handle(fireMarker("NovablastChargeStop", now)),
        "manager accepts native Novablast charge-stop marker");
    {
        std::scoped_lock lock(continuousState->continuousMutex);
        expect(continuousState->continuous.kind == sds::HapticContinuousKind::None,
            "native Novablast charge-stop marker clears continuous backend state");
    }

    expect(continuousManager.handleRightTriggerInput(12),
        "manager accepts Novablast R2 release after native stop");
    (void)continuousManager.handleRightTriggerInput(255);
    expect(continuousManager.handle(equip("Eon")),
        "manager consumes weapon swap away from Novablast");
    {
        std::scoped_lock lock(continuousState->continuousMutex);
        expect(continuousState->continuous.kind == sds::HapticContinuousKind::None,
            "weapon swap clears Novablast continuous state while R2 remains held");
    }

    (void)continuousManager.handle(equip("Novablast Disruptor"));
    sds::GameEvent managerShutdown{};
    managerShutdown.type = sds::GameEventType::Shutdown;
    expect(continuousManager.handle(managerShutdown),
        "manager consumes shutdown event");
    {
        std::scoped_lock lock(continuousState->continuousMutex);
        expect(continuousState->continuous.kind == sds::HapticContinuousKind::None,
            "shutdown clears continuous state before backend stop");
    }
    continuousManager.stop();

    auto cutterRaceState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager cutterRaceManager(
        continuousConfig,
        [cutterRaceState] {
            return std::make_unique<FakeHapticsBackend>(cutterRaceState);
        });
    cutterRaceManager.start();
    expect(cutterRaceManager.handle(equip("Cutter")),
        "Cutter race manager consumes equip event");
    expect(cutterRaceManager.handleRightTriggerInput(0, now - std::chrono::milliseconds(20)),
        "Cutter race manager records an initially released trigger");
    expect(cutterRaceManager.handle(fireMarker("weaponFireStart", now)),
        "Cutter race manager consumes start marker before R2 observer catches up");
    {
        std::scoped_lock lock(cutterRaceState->continuousMutex);
        expect(cutterRaceState->continuous.kind == sds::HapticContinuousKind::None,
            "stale released R2 does not fabricate Cutter haptics at marker arrival");
    }
    // Model the exact runtime race: a pre-marker low sample can finish its
    // callback after the marker thread, then a fresh held sample follows.
    expect(cutterRaceManager.handleRightTriggerInput(0, now - std::chrono::milliseconds(5)),
        "Cutter race manager ignores a delayed callback carrying a pre-marker low sample");
    expect(cutterRaceManager.handleRightTriggerInput(200, now + std::chrono::milliseconds(10)),
        "Cutter race manager accepts the later live held-R2 sample");
    {
        std::scoped_lock lock(cutterRaceState->continuousMutex);
        expect(cutterRaceState->continuous.kind == sds::HapticContinuousKind::CutterBeam,
            "confirmed Cutter start survives marker-before-R2 ordering and starts on the fresh held-R2 sample");
    }
    cutterRaceManager.stop();

    auto cutterExpiredRaceState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager cutterExpiredRaceManager(
        continuousConfig,
        [cutterExpiredRaceState] {
            return std::make_unique<FakeHapticsBackend>(cutterExpiredRaceState);
        });
    cutterExpiredRaceManager.start();
    const auto expiredStart = now + std::chrono::seconds(1);
    (void)cutterExpiredRaceManager.handle(equip("Cutter"));
    (void)cutterExpiredRaceManager.handleRightTriggerInput(0, expiredStart - std::chrono::milliseconds(20));
    (void)cutterExpiredRaceManager.handle(fireMarker("weaponFireStart", expiredStart));
    expect(cutterExpiredRaceManager.handleRightTriggerInput(200, expiredStart + std::chrono::milliseconds(300)),
        "Cutter race manager safely consumes a held-R2 sample after catch-up expiry");
    {
        std::scoped_lock lock(cutterExpiredRaceState->continuousMutex);
        expect(cutterExpiredRaceState->continuous.kind == sds::HapticContinuousKind::None,
            "expired Cutter start marker cannot authorize a later unrelated R2 pull");
    }
    cutterExpiredRaceManager.stop();

    auto cutterManagerState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager cutterManager(
        continuousConfig,
        [cutterManagerState] {
            return std::make_unique<FakeHapticsBackend>(cutterManagerState);
        });
    cutterManager.start();
    expect(cutterManager.handleRightTriggerInput(200),
        "Cutter manager accepts saved held R2 before equip");
    expect(cutterManager.handle(equip("Cutter")),
        "Cutter manager consumes equip event");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "equipping Cutter while R2 is held does not fabricate beam haptics");
    }
    expect(cutterManager.handle(fireMarker("weaponFireStart", now)),
        "Cutter manager consumes confirmed weaponFireStart");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::CutterBeam &&
               cutterManagerState->continuous.level > 0.70F,
            "confirmed Cutter start immediately reevaluates saved R2 and submits CutterBeam");
    }
    expect(cutterManagerState->enqueues.load() == 0,
        "Cutter weaponFireStart does not enqueue a discrete haptic command");

    const auto heartbeatBase = now + std::chrono::seconds(3);
    (void)cutterManager.handle(fireMarker("weaponFireStart", heartbeatBase));
    (void)cutterManager.handle(fireMarker("WeaponFire", heartbeatBase + std::chrono::milliseconds(100)));
    expect(cutterManager.tick(heartbeatBase + std::chrono::milliseconds(350)),
        "Cutter heartbeat keeps haptics alive before refreshed deadline");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::CutterBeam,
            "real repeated Cutter WeaponFire heartbeat preserves continuous haptics while beam is alive");
    }
    expect(cutterManager.tick(heartbeatBase + std::chrono::milliseconds(401)),
        "Cutter heartbeat watchdog consumes energy-depletion timeout");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "Cutter heartbeat timeout stops body haptics while R2 remains held");
    }
    (void)cutterManager.handleRightTriggerInput(200, heartbeatBase + std::chrono::milliseconds(410));
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "held R2 cannot restart Cutter haptics after heartbeat timeout without a new fire start");
    }

    expect(cutterManager.handleRightTriggerInput(12),
        "Cutter manager accepts release-threshold R2 update");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "Cutter R2 release immediately submits continuous None");
    }
    (void)cutterManager.handleRightTriggerInput(200);
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "Cutter release retires authorization so held R2 cannot restart it");
    }

    (void)cutterManager.handle(fireMarker("weaponFireStart", now));
    expect(cutterManager.handle(equip("Eon")),
        "Cutter manager consumes weapon swap");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "weapon swap immediately clears Cutter continuous backend state");
    }

    (void)cutterManager.handle(equip("Cutter"));
    (void)cutterManager.handleRightTriggerInput(200);
    (void)cutterManager.handle(fireMarker("weaponFireStart", now));
    sds::GameEvent cutterPause{};
    cutterPause.type = sds::GameEventType::GamePaused;
    expect(cutterManager.handle(cutterPause),
        "Cutter manager consumes GamePaused");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "GamePaused immediately clears Cutter continuous backend state");
    }

    auto cutterResume = cutterPause;
    cutterResume.type = sds::GameEventType::GameUnpaused;
    (void)cutterManager.handle(cutterResume);
    const auto resumedStart = now + std::chrono::seconds(10);
    (void)cutterManager.handleRightTriggerInput(200, resumedStart);
    (void)cutterManager.handle(fireMarker("weaponFireStart", resumedStart));
    expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::CutterBeam,
        "fresh Cutter fire resumes after pause without re-equipping");
    (void)cutterManager.tick(resumedStart + std::chrono::seconds(1));
    expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
        "pause preserves the equipped Cutter's watchdog eligibility");

    const auto pendingStart = resumedStart + std::chrono::seconds(2);
    (void)cutterManager.handleRightTriggerInput(0, pendingStart - std::chrono::milliseconds(1));
    (void)cutterManager.handle(fireMarker("weaponFireStart", pendingStart));
    (void)cutterManager.handleRightTriggerInput(0, pendingStart - std::chrono::milliseconds(1));
    (void)cutterManager.handleRightTriggerInput(200, pendingStart + std::chrono::milliseconds(1));
    expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::CutterBeam,
        "fresh held input resolves marker-first Cutter start");
    (void)cutterManager.tick(pendingStart + std::chrono::seconds(1));
    expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
        "a stale pre-marker release cannot disarm the newer watchdog");

    (void)cutterManager.handle(equip("Cutter"));
    (void)cutterManager.handleRightTriggerInput(200);
    (void)cutterManager.handle(fireMarker("weaponFireStart", now));
    sds::GameEvent cutterManagerShutdown{};
    cutterManagerShutdown.type = sds::GameEventType::Shutdown;
    expect(cutterManager.handle(cutterManagerShutdown),
        "Cutter manager consumes shutdown");
    {
        std::scoped_lock lock(cutterManagerState->continuousMutex);
        expect(cutterManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "shutdown immediately clears Cutter continuous backend state");
    }
    cutterManager.stop();

    auto arcWelderManagerState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager arcWelderManager(
        continuousConfig,
        [arcWelderManagerState] {
            return std::make_unique<FakeHapticsBackend>(arcWelderManagerState);
        });
    arcWelderManager.start();
    const auto arcStart = now + std::chrono::seconds(6);
    expect(arcWelderManager.handle(equip("Arc Welder")),
        "Arc Welder manager consumes equip event");
    expect(arcWelderManager.handleRightTriggerInput(200, arcStart - std::chrono::milliseconds(10)),
        "Arc Welder manager records held R2 before confirmed start");
    expect(arcWelderManager.handle(fireMarker("weaponFireStart", arcStart)),
        "Arc Welder manager consumes confirmed weaponFireStart");
    {
        std::scoped_lock lock(arcWelderManagerState->continuousMutex);
        expect(arcWelderManagerState->continuous.kind == sds::HapticContinuousKind::ArcWelderArc,
            "confirmed Arc Welder start submits sustained arc immediately for held R2");
    }
    expect(arcWelderManager.tick(arcStart + std::chrono::milliseconds(700)),
        "Arc Welder ignores Cutter heartbeat timeout while R2 remains held");
    {
        std::scoped_lock lock(arcWelderManagerState->continuousMutex);
        expect(arcWelderManagerState->continuous.kind == sds::HapticContinuousKind::ArcWelderArc,
            "Arc Welder sustained arc remains active beyond Cutter heartbeat window");
    }
    expect(arcWelderManager.handleRightTriggerInput(12, arcStart + std::chrono::milliseconds(710)),
        "Arc Welder manager accepts release threshold");
    {
        std::scoped_lock lock(arcWelderManagerState->continuousMutex);
        expect(arcWelderManagerState->continuous.kind == sds::HapticContinuousKind::None,
            "Arc Welder release immediately clears sustained arc backend state");
    }
    arcWelderManager.stop();

    auto arcWelderRaceState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager arcWelderRaceManager(
        continuousConfig,
        [arcWelderRaceState] {
            return std::make_unique<FakeHapticsBackend>(arcWelderRaceState);
        });
    arcWelderRaceManager.start();
    const auto arcRaceStart = now + std::chrono::seconds(7);
    expect(arcWelderRaceManager.handle(equip("Arc Welder")),
        "Arc Welder race manager consumes equip event");
    expect(arcWelderRaceManager.handle(fireMarker("weaponFireStart", arcRaceStart)),
        "Arc Welder race manager consumes marker-first confirmed start");
    expect(arcWelderRaceManager.handle(fireMarker("WeaponFire", arcRaceStart + std::chrono::milliseconds(1))),
        "Arc Welder race manager consumes immediate sustained heartbeat before R2 catchup");
    expect(arcWelderRaceManager.handleRightTriggerInput(200, arcRaceStart + std::chrono::milliseconds(10)),
        "Arc Welder race manager consumes post-marker held R2 catchup");
    {
        std::scoped_lock lock(arcWelderRaceState->continuousMutex);
        expect(arcWelderRaceState->continuous.kind == sds::HapticContinuousKind::ArcWelderArc,
            "Arc Welder marker-first start survives immediate heartbeat until R2 catchup");
    }
    arcWelderRaceManager.stop();

    auto arcWelderEndState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager arcWelderEndManager(
        continuousConfig,
        [arcWelderEndState] {
            return std::make_unique<FakeHapticsBackend>(arcWelderEndState);
        });
    arcWelderEndManager.start();
    const auto arcEndStart = now + std::chrono::seconds(8);
    expect(arcWelderEndManager.handle(equip("Arc Welder")),
        "Arc Welder fire-end manager consumes equip event");
    expect(arcWelderEndManager.handleRightTriggerInput(200, arcEndStart - std::chrono::milliseconds(5)),
        "Arc Welder fire-end manager records held R2 before start");
    expect(arcWelderEndManager.handle(fireMarker("weaponFireStart", arcEndStart)),
        "Arc Welder fire-end manager consumes confirmed start");
    {
        std::scoped_lock lock(arcWelderEndState->continuousMutex);
        expect(arcWelderEndState->continuous.kind == sds::HapticContinuousKind::ArcWelderArc,
            "Arc Welder fire-end fixture begins with sustained arc active");
    }
    expect(arcWelderEndManager.handle(fireMarker("weaponFireEnd", arcEndStart + std::chrono::milliseconds(500))),
        "Arc Welder manager consumes confirmed weaponFireEnd while R2 remains held");
    {
        std::scoped_lock lock(arcWelderEndState->continuousMutex);
        expect(arcWelderEndState->continuous.kind == sds::HapticContinuousKind::None,
            "Arc Welder weaponFireEnd immediately clears sustained arc while R2 remains held");
    }
    expect(arcWelderEndManager.handleRightTriggerInput(200, arcEndStart + std::chrono::milliseconds(510)),
        "Arc Welder manager accepts late held-R2 sample after fire end");
    {
        std::scoped_lock lock(arcWelderEndState->continuousMutex);
        expect(arcWelderEndState->continuous.kind == sds::HapticContinuousKind::None,
            "late held R2 cannot resurrect Arc Welder after weaponFireEnd");
    }
    expect(arcWelderEndManager.handle(fireMarker("weaponFireStart", arcEndStart + std::chrono::milliseconds(800))),
        "Arc Welder manager consumes a new confirmed start after fire end");
    {
        std::scoped_lock lock(arcWelderEndState->continuousMutex);
        expect(arcWelderEndState->continuous.kind == sds::HapticContinuousKind::ArcWelderArc,
            "new confirmed weaponFireStart reauthorizes Arc Welder after fire end");
    }
    arcWelderEndManager.stop();

    auto arcWelderPendingEndState = std::make_shared<FakeHapticsState>();
    sds::HapticsManager arcWelderPendingEndManager(
        continuousConfig,
        [arcWelderPendingEndState] {
            return std::make_unique<FakeHapticsBackend>(arcWelderPendingEndState);
        });
    arcWelderPendingEndManager.start();
    const auto arcPendingStart = now + std::chrono::seconds(9);
    expect(arcWelderPendingEndManager.handle(equip("Arc Welder")),
        "Arc Welder pending-end manager consumes equip event");
    expect(arcWelderPendingEndManager.handle(fireMarker("weaponFireStart", arcPendingStart)),
        "Arc Welder pending-end manager consumes marker-first start");
    expect(arcWelderPendingEndManager.handle(fireMarker("weaponFireEnd", arcPendingStart + std::chrono::milliseconds(5))),
        "Arc Welder pending-end manager consumes end before R2 catchup");
    expect(arcWelderPendingEndManager.handleRightTriggerInput(200, arcPendingStart + std::chrono::milliseconds(10)),
        "Arc Welder pending-end manager accepts late held-R2 catchup");
    {
        std::scoped_lock lock(arcWelderPendingEndState->continuousMutex);
        expect(arcWelderPendingEndState->continuous.kind == sds::HapticContinuousKind::None,
            "weaponFireEnd cancels pending Arc Welder start so late R2 cannot resurrect it");
    }
    arcWelderPendingEndManager.stop();

    auto disabledContinuousState = std::make_shared<FakeHapticsState>();
    sds::Config disabledContinuousConfig = sds::Config::defaults();
    disabledContinuousConfig.advancedHaptics = false;
    sds::HapticsManager disabledContinuous(
        disabledContinuousConfig,
        [disabledContinuousState] {
            return std::make_unique<FakeHapticsBackend>(disabledContinuousState);
        });
    disabledContinuous.start();
    expect(disabledContinuous.handleRightTriggerInput(255),
        "disabled manager safely ignores R2 observer input");
    expect(disabledContinuousState->continuousSets.load() == 0,
        "AdvancedHaptics=false prevents continuous backend submission");

    return failures == 0 ? 0 : 1;
}
