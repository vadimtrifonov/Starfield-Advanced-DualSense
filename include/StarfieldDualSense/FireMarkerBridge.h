#pragma once

#include <StarfieldDualSense/FireMarkerEventTag.h>
#include <StarfieldDualSense/FireMarkerTrace.h>
#include <StarfieldDualSense/FireMarkerSourceScope.h>
#include <StarfieldDualSense/MeleeEventDiagnostic.h>
#include <StarfieldDualSense/Types.h>
#include <StarfieldDualSense/WeaponProfiles.h>

#include <RE/Starfield.h>
#include <RE/B/BSAnimationGraph.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sds
{
    class FireMarkerBridge final :
        public RE::BSTEventSink<RE::ActorItemEquipped::Event>,
        public RE::BSTEventSink<RE::BSAnimationGraphEvent>
    {
    public:
        using EmitCallback = std::function<bool(GameEvent)>;
        using WorldAvailableCallback = std::function<bool()>;
        using LogCallback = std::function<void(std::string_view)>;
        using AnimationMarkerObserver = std::function<bool(
            std::string_view,
            std::string_view,
            std::chrono::steady_clock::time_point)>;

        FireMarkerBridge(
            EmitCallback emit,
            WorldAvailableCallback worldAvailable,
            LogCallback log = {},
            bool debugLogging = false,
            AnimationMarkerObserver animationMarkerObserver = {}) :
            _emit(std::move(emit)),
            _worldAvailable(std::move(worldAvailable)),
            _log(std::move(log)),
            _debugLogging(debugLogging),
            _animationMarkerObserver(std::move(animationMarkerObserver))
        {}

        ~FireMarkerBridge() override
        {
            stop();
        }

        FireMarkerBridge(const FireMarkerBridge&) = delete;
        FireMarkerBridge& operator=(const FireMarkerBridge&) = delete;

        bool start()
        {
            std::scoped_lock lock(_mutex);
            if (_started) {
                return true;
            }

            auto* equipSource = RE::ActorItemEquipped::Event::GetEventSource();
            if (!equipSource) {
                log("Fire marker bridge: player equip source unavailable");
                return false;
            }

            equipSource->RegisterSink(this);
            _started = true;
            log("Fire marker bridge: ACTIVE; confirmed player animation markers feed live weapon effects; sourceIdentityGate=exact; h4 arbitration unchanged");
            return true;
        }

        void stop() noexcept
        {
            try {
                std::scoped_lock lock(_mutex);
                unregisterPlayerGraphsLocked();
                if (_started) {
                    if (auto* equipSource = RE::ActorItemEquipped::Event::GetEventSource()) {
                        equipSource->UnregisterSink(this);
                    }
                }
                _started = false;
                _weaponArmed.store(false, std::memory_order_release);
                _novablastChargeMarkerArmed.store(false, std::memory_order_release);
                _arcWelderStopDiagnosticArmed.store(false, std::memory_order_release);
                _meleeEventDiagnostic.disarm();
            } catch (...) {
                _graphs.clear();
                clearRegisteredPlayerGraphSourcesLocked();
                _started = false;
                _weaponArmed.store(false, std::memory_order_release);
                _novablastChargeMarkerArmed.store(false, std::memory_order_release);
                _arcWelderStopDiagnosticArmed.store(false, std::memory_order_release);
                _meleeEventDiagnostic.disarm();
            }
        }

        void suspendPlayerGraphsForWorldTeardown() noexcept
        {
            try {
                std::scoped_lock lock(_mutex);
                unregisterPlayerGraphsLocked();
                log("Fire marker bridge: player graph sinks detached for world teardown");
            } catch (...) {
                log("Fire marker bridge: player graph detach failed during world teardown");
            }
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::ActorItemEquipped::Event& event,
            RE::BSTEventSource<RE::ActorItemEquipped::Event>*) override
        {
            try {
                std::scoped_lock lock(_mutex);
                if (!_started || !_worldAvailable || !_worldAvailable()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player || event.actor.get() != player || !event.item ||
                    event.item->GetFormType() != RE::FormType::kWEAP) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const auto identity = weaponIdentity(event.item);
                const auto* profile = findWeaponProfile(identity);
                const auto triggerFamily = profile ? profile->triggerFamily : WeaponTriggerFamily::BallisticRifle;
                const bool arcWelderStopDiagnostic =
                    _debugLogging && profile && profile->name == "Arc Welder";
                const bool novablastChargeMarker =
                    profile && profile->name == "Novablast Disruptor";

                const auto graphCount = reconcilePlayerGraphsLocked(player);
                _triggerFamily.store(triggerFamily, std::memory_order_release);
                _weaponArmed.store(graphCount != 0, std::memory_order_release);
                _novablastChargeMarkerArmed.store(
                    novablastChargeMarker && graphCount != 0,
                    std::memory_order_release);
                _arcWelderStopDiagnosticMarkers.store(0, std::memory_order_relaxed);
                _arcWelderStopDiagnosticArmed.store(
                    arcWelderStopDiagnostic && graphCount != 0,
                    std::memory_order_release);

                _meleeEventDiagnostic.disarm();
                if (profile && shouldArmMeleeEventDiagnostic(triggerFamily, graphCount)) {
                    const auto diagnosticNow = std::chrono::steady_clock::now();
                    _meleeEventDiagnostic.arm(
                        profile->name,
                        diagnosticNow,
                        kMeleeEventDiagnosticWindow);

                    char diagnosticBuffer[512]{};
                    std::snprintf(
                        diagnosticBuffer,
                        sizeof(diagnosticBuffer),
                        "Melee event diagnostic: armed weapon='%.*s' window=20s graphs=%zu; controller behavior unchanged",
                        static_cast<int>(profile->name.size()),
                        profile->name.data(),
                        graphCount);
                    log(diagnosticBuffer);
                }

                char buffer[512]{};
                const std::string_view cadenceLabel = profile ? profile->cadenceLabel : std::string_view("Single/fallback");
                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "Fire marker bridge: weapon armed identity='%s' cadence='%.*s' graphs=%zu sourceIdentityGate=exact",
                    identity.c_str(),
                    static_cast<int>(cadenceLabel.size()),
                    cadenceLabel.data(),
                    graphCount);
                log(buffer);
            } catch (...) {
                log("Fire marker bridge: exception while arming player weapon graphs");
            }

            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::BSAnimationGraphEvent& graphEvent,
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* source) override
        {
            if (!_worldAvailable || !_worldAvailable() ||
                !_weaponArmed.load(std::memory_order_acquire) || !sourceMatchesRegisteredPlayerGraph(source)) {
                return RE::BSEventNotifyControl::kContinue;
            }

            RawAnimationEvent event{};
            if (!safeSnapshotEvent(graphEvent, event) || event.words[1] == 0) {
                return RE::BSEventNotifyControl::kContinue;
            }

            std::array<std::byte, 64> tagObject{};
            if (!safeSnapshotTagObject(event.words[1], tagObject)) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const auto decoded = decodeInlineFireMarkerTag(tagObject);
            if (!decoded.valid) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (_debugLogging && _animationMarkerObserver && decoded.kind == FireMarkerKind::Other) {
                std::string payload;
                if (event.words[2] != 0) {
                    std::array<std::byte, 64> payloadObject{};
                    if (safeSnapshotTagObject(event.words[2], payloadObject)) {
                        const auto decodedPayload = decodeInlineFireMarkerTag(payloadObject);
                        if (decodedPayload.valid) {
                            payload = decodedPayload.text;
                        }
                    }
                }

                bool candidate = false;
                try {
                    candidate = _animationMarkerObserver(
                        decoded.text,
                        payload,
                        std::chrono::steady_clock::now());
                } catch (...) {
                    // Discovery diagnostics cannot affect gameplay routing.
                }

                if (candidate) {
                    const auto count = _drawHolsterDiagnosticMarkers.fetch_add(1, std::memory_order_relaxed) + 1;
                    char markerBuffer[768]{};
                    std::snprintf(
                        markerBuffer,
                        sizeof(markerBuffer),
                        "Weapon draw/holster marker diagnostic: seq=%llu tag='%s' payload='%s' candidate=yes",
                        static_cast<unsigned long long>(count),
                        decoded.text.c_str(),
                        payload.c_str());
                    log(markerBuffer);
                }
            }

            if (_novablastChargeMarkerArmed.load(std::memory_order_acquire) &&
                (decoded.text == "SoundPlay" || decoded.text == "SoundStop") &&
                event.words[2] != 0) {
                std::array<std::byte, 64> payloadObject{};
                if (safeSnapshotTagObject(event.words[2], payloadObject)) {
                    const auto decodedPayload = decodeInlineFireMarkerTag(payloadObject);
                    if (decodedPayload.valid) {
                        const auto chargeAction = routeNovablastChargeMarker(
                            decoded.text, decodedPayload.text);
                        if (chargeAction != NovablastChargeMarkerAction::None) {
                            GameEvent chargeEvent{};
                            chargeEvent.type = GameEventType::WeaponFired;
                            chargeEvent.when = std::chrono::steady_clock::now();
                            copyMarkerText(
                                chargeEvent,
                                chargeAction == NovablastChargeMarkerAction::Start ?
                                    "NovablastChargeStart" :
                                    "NovablastChargeStop");
                            if (!_emit || !_emit(std::move(chargeEvent))) {
                                log("Fire marker bridge: controller event queue full; Novablast charge marker dropped");
                            }
                        }
                    }
                }
            }

            if (isSpeakerReloadMarker(decoded.text)) {
                GameEvent reloadEvent{};
                reloadEvent.type = GameEventType::ReloadCompleted;
                reloadEvent.when = std::chrono::steady_clock::now();
                copyMarkerText(reloadEvent, "ReloadComplete");
                if (!_emit || !_emit(std::move(reloadEvent))) {
                    log("Fire marker bridge: controller event queue full; reload semantic dropped");
                }
            }

            const auto diagnosticNow = std::chrono::steady_clock::now();
            if (_meleeEventDiagnostic.active(diagnosticNow)) {
                std::string payload = "<unavailable>";
                if (event.words[2] != 0) {
                    std::array<std::byte, 64> payloadObject{};
                    if (safeSnapshotTagObject(event.words[2], payloadObject)) {
                        const auto decodedPayload = decodeInlineFireMarkerTag(payloadObject);
                        if (decodedPayload.valid) {
                            payload = decodedPayload.text;
                        }
                    }
                }

                const auto record = _meleeEventDiagnostic.tryCapture(
                    true,
                    diagnosticNow,
                    decoded.text,
                    payload,
                    reinterpret_cast<std::uintptr_t>(source));
                if (record) {
                    if (record->kind == FireMarkerRecordKind::LimitReached) {
                        log("Melee event diagnostic: strict 1024-line capture limit reached; remaining events suppressed");
                    } else {
                        char diagnosticBuffer[1024]{};
                        std::snprintf(
                            diagnosticBuffer,
                            sizeof(diagnosticBuffer),
                            "Melee event diagnostic: seq=%llu tUs=%lld weapon='%.*s' tag='%.*s' payload='%.*s' raw0=%016llX raw1=%016llX raw2=%016llX source=%p",
                            static_cast<unsigned long long>(record->sequence),
                            static_cast<long long>(record->elapsedUs),
                            120,
                            record->weapon.data(),
                            240,
                            record->tag.data(),
                            240,
                            record->payload.data(),
                            static_cast<unsigned long long>(event.words[0]),
                            static_cast<unsigned long long>(event.words[1]),
                            static_cast<unsigned long long>(event.words[2]),
                            reinterpret_cast<void*>(record->source));
                        log(diagnosticBuffer);
                    }
                }
            }

            // Diagnostic-only observation for the Arc Welder stop-state probe.
            // Production routing intentionally ignores FireMarkerKind::Other; log
            // those tags before the normal route/drop decision so reload, empty,
            // dry-fire, and stop-state markers can be identified from hardware.
            if (_arcWelderStopDiagnosticArmed.load(std::memory_order_acquire) &&
                decoded.kind == FireMarkerKind::Other) {
                const auto count =
                    _arcWelderStopDiagnosticMarkers.fetch_add(1, std::memory_order_relaxed) + 1;
                constexpr std::uint64_t kDiagnosticTagLimit = 512;
                if (count <= kDiagnosticTagLimit) {
                    char buffer[384]{};
                    std::snprintf(
                        buffer,
                        sizeof(buffer),
                        "Arc Welder stop-state diagnostic: tag='%s' seq=%llu source=%p",
                        decoded.text.c_str(),
                        static_cast<unsigned long long>(count),
                        reinterpret_cast<void*>(source));
                    log(buffer);
                } else if (count == kDiagnosticTagLimit + 1) {
                    log("Arc Welder stop-state diagnostic: 512 non-fire tags captured; further diagnostic tags suppressed");
                }
            }

            const auto triggerFamily = _triggerFamily.load(std::memory_order_acquire);
            const auto action = routeFireMarker(decoded.kind, triggerFamily);
            if (action == FireMarkerAction::None) {
                return RE::BSEventNotifyControl::kContinue;
            }

            GameEvent normalized{};
            normalized.type = action == FireMarkerAction::MeleeSwingPulse ?
                GameEventType::MeleeSwing :
                GameEventType::WeaponFired;
            normalized.when = std::chrono::steady_clock::now();
            copyMarkerText(normalized, decoded.text);

            const bool queued = _emit && _emit(std::move(normalized));
            if (!queued) {
                log("Fire marker bridge: controller event queue full; confirmed fire marker dropped");
                return RE::BSEventNotifyControl::kContinue;
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        struct RawAnimationEvent
        {
            std::array<std::uintptr_t, 3> words{};
        };
        static_assert(sizeof(RawAnimationEvent) == 0x18);

        static constexpr std::size_t kMaxPlayerGraphSources = 8;

        static std::string weaponIdentity(const RE::TESForm* item)
        {
            if (!item) {
                return "<unknown>";
            }

            std::string identity;
            if (const char* editorId = item->GetFormEditorID(); editorId && *editorId) {
                identity = editorId;
            }
            if (const auto* weapon = static_cast<const RE::TESObjectWEAP*>(item)) {
                if (const char* fullName = weapon->GetFullName(); fullName && *fullName) {
                    if (!identity.empty()) {
                        identity += '|';
                    }
                    identity += fullName;
                }
            }
            return identity.empty() ? "<unknown>" : identity;
        }

        static bool safeSnapshotEvent(const RE::BSAnimationGraphEvent& event, RawAnimationEvent& out) noexcept
        {
            SIZE_T bytesRead = 0;
            return ::ReadProcessMemory(
                       ::GetCurrentProcess(),
                       std::addressof(event),
                       std::addressof(out),
                       sizeof(out),
                       std::addressof(bytesRead)) != FALSE &&
                bytesRead == sizeof(out);
        }

        static bool safeSnapshotTagObject(
            std::uintptr_t address,
            std::array<std::byte, 64>& out) noexcept
        {
            SIZE_T bytesRead = 0;
            return ::ReadProcessMemory(
                       ::GetCurrentProcess(),
                       reinterpret_cast<const void*>(address),
                       out.data(),
                       out.size(),
                       std::addressof(bytesRead)) != FALSE &&
                bytesRead == out.size();
        }

        static void copyMarkerText(GameEvent& event, std::string_view text) noexcept
        {
            const auto count = (std::min)(text.size(), event.text.size() - 1);
            std::memcpy(event.text.data(), text.data(), count);
            event.text[count] = '\0';
        }

        std::size_t reconcilePlayerGraphsLocked(RE::PlayerCharacter* player)
        {
            unregisterPlayerGraphsLocked();
            if (!player) {
                return 0;
            }

            RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
            if (!player->GetAnimationGraphManagerImpl(manager) || !manager) {
                log("Fire marker bridge: player animation graph manager unavailable");
                return 0;
            }

            const auto graphCount = static_cast<std::size_t>(manager->graphs.size());
            _graphs.reserve((std::min)(graphCount, kMaxPlayerGraphSources));
            for (std::uint32_t index = 0;
                 index < manager->graphs.size() && _graphs.size() < kMaxPlayerGraphSources;
                 ++index) {
                auto graph = manager->graphs[index];
                if (!graph) {
                    continue;
                }
                auto* graphSource = static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(graph.get());
                graphSource->RegisterSink(this);
                const auto registryIndex = _graphs.size();
                _playerGraphSources[registryIndex].store(
                    reinterpret_cast<std::uintptr_t>(graphSource),
                    std::memory_order_relaxed);
                _graphs.push_back(std::move(graph));
            }
            _registeredGraphSourceCount.store(_graphs.size(), std::memory_order_release);
            return _graphs.size();
        }

        void unregisterPlayerGraphsLocked() noexcept
        {
            _weaponArmed.store(false, std::memory_order_release);
            _novablastChargeMarkerArmed.store(false, std::memory_order_release);
            _arcWelderStopDiagnosticArmed.store(false, std::memory_order_release);
            _meleeEventDiagnostic.disarm();
            clearRegisteredPlayerGraphSourcesLocked();
            for (auto& graph : _graphs) {
                try {
                    if (graph) {
                        auto* source = static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(graph.get());
                        source->UnregisterSink(this);
                    }
                } catch (...) {
                    // Shutdown/rearm must fail soft.
                }
            }
            _graphs.clear();
        }

        void clearRegisteredPlayerGraphSourcesLocked() noexcept
        {
            _registeredGraphSourceCount.store(0, std::memory_order_release);
            for (auto& source : _playerGraphSources) {
                source.store(0, std::memory_order_relaxed);
            }
        }

        bool sourceMatchesRegisteredPlayerGraph(
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* source) const noexcept
        {
            std::array<std::uintptr_t, kMaxPlayerGraphSources> registered{};
            const auto count = (std::min)(
                _registeredGraphSourceCount.load(std::memory_order_acquire),
                kMaxPlayerGraphSources);
            for (std::size_t index = 0; index < count; ++index) {
                registered[index] = _playerGraphSources[index].load(std::memory_order_acquire);
            }
            return isRegisteredPlayerGraphSource(
                reinterpret_cast<std::uintptr_t>(source),
                registered,
                count);
        }

        void log(std::string_view message) const noexcept
        {
            if (!_log) {
                return;
            }
            try {
                _log(message);
            } catch (...) {
                // Diagnostics are non-critical.
            }
        }

        EmitCallback _emit{};
        WorldAvailableCallback _worldAvailable{};
        LogCallback _log{};
        bool _debugLogging{ false };
        AnimationMarkerObserver _animationMarkerObserver{};
        std::atomic<std::uint64_t> _drawHolsterDiagnosticMarkers{ 0 };
        mutable std::mutex _mutex{};
        bool _started{ false };
        std::atomic<bool> _weaponArmed{ false };
        std::atomic<bool> _novablastChargeMarkerArmed{ false };
        std::atomic<bool> _arcWelderStopDiagnosticArmed{ false };
        std::atomic<std::uint64_t> _arcWelderStopDiagnosticMarkers{ 0 };
        FireMarkerCaptureState _meleeEventDiagnostic{};
        std::atomic<WeaponTriggerFamily> _triggerFamily{ WeaponTriggerFamily::BallisticRifle };
        std::vector<RE::BSTSmartPointer<RE::BSAnimationGraph>> _graphs{};
        std::array<std::atomic<std::uintptr_t>, kMaxPlayerGraphSources> _playerGraphSources{};
        std::atomic<std::size_t> _registeredGraphSourceCount{ 0 };
    };

}
