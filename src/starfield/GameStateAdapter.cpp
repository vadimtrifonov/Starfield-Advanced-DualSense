#include <StarfieldDualSense/GameStateAdapter.h>
#include <StarfieldDualSense/HapticTypes.h>
#include <StarfieldDualSense/InputDiagnostics.h>
#include <StarfieldDualSense/MeleeEventDiagnostic.h>
#include <StarfieldDualSense/MeleeHaptics.h>
#include <StarfieldDualSense/TESHitSourceDiscovery.h>
#include <StarfieldDualSense/WeaponProfiles.h>

#include <RE/A/ActorValueInfo.h>
#include <RE/B/BSLock.h>

#include <REL/ASM.h>
#include <REL/Trampoline.h>
#include <REL/Relocation.h>
#include <REL/Utility.h>

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using SemanticBroadcasterFunction = void (*)(void*, void*);
    using NativeEnqueueFunction = std::uintptr_t (*)(void*, void*, std::uintptr_t, std::uintptr_t);
    using ButtonEventConstructorFunction = void* (*)(void*);

    std::atomic<sds::InputPresentationObserver>
        g_inputPresentationObserver{ nullptr };

    void notifyInputPresentationDevice(
        sds::InputPresentationDevice device) noexcept
    {
        if (const auto observer =
                g_inputPresentationObserver.load(
                    std::memory_order_acquire)) {

            observer(device);
        }
    }

    constexpr auto kHealthPollInterval = std::chrono::milliseconds(100);
    constexpr auto kShipPropulsionPollInterval = std::chrono::milliseconds(200);
    constexpr auto kFireMarkerCaptureWindow = std::chrono::seconds(20);
    constexpr float kHealthMaterialDelta = 0.0025F;

    constexpr std::uintptr_t kSemanticBroadcasterRva = 0x2541B10;
    constexpr std::uintptr_t kNativeEnqueueRva = 0x22DABC0;
    constexpr std::uintptr_t kButtonEventConstructorRva = 0x22DA3A0;
    constexpr std::uintptr_t kButtonEventPrimaryVtableRva = 0x4D59F50;
    constexpr std::uintptr_t kButtonEventIdVtableRva = 0x4D59F28;
    constexpr std::uintptr_t kButtonEventUserVtableRva = 0x4D59F00;
    constexpr std::uintptr_t kButtonEventSourceVtableRva = 0x4D7E408;
    constexpr auto kLandVehicleReconPollInterval = std::chrono::milliseconds(100);
    constexpr auto kLandVehicleReconSummaryInterval = std::chrono::seconds(1);
    constexpr auto kLandVehicleRawEvidenceHold = std::chrono::seconds(3);
    constexpr std::uintptr_t kInputManagerSingletonRva = 0x5FD9B80;
    constexpr std::uintptr_t kInputQueueSingletonRva = 0x61EEA20;
    constexpr std::uintptr_t kInputQueueLockOffset = 0x1288;
    constexpr std::uintptr_t kInputQueueHeadOffset = 0x1290;
    constexpr std::uintptr_t kInputQueueNextTimeCodeOffset = 0x12A0;
    constexpr std::uintptr_t kDebounceManagerOwnerRva = 0x61DE668;
    constexpr std::uintptr_t kDebounceManagerOffset = 0x2F0;
    constexpr std::uint32_t kNativeButtonFreeTimeCode = 0xFFFFFFFFU;
    constexpr LONG kNativeButtonReservedTimeCode = -2;
    constexpr std::array<std::uintptr_t, 4> kRecycledProducerClusterRvas{
        0x22D70A0,
        0x22D73E0,
        0x22D7541,
        0x22D7619,
    };
    constexpr std::size_t kSemanticBroadcasterDisplacedBytes = 7;
    constexpr std::size_t kButtonEventConstructorDisplacedBytes = 10;


    struct ShipPropulsionActorValues
    {
        RE::ActorValueInfo* maxForwardSpeed{ nullptr };
        RE::ActorValueInfo* forwardSpeedMult{ nullptr };
        RE::ActorValueInfo* forwardForcePerPower{ nullptr };
        RE::ActorValueInfo* maxForwardAcceleration{ nullptr };
        RE::ActorValueInfo* boostFuel{ nullptr };
        RE::ActorValueInfo* boostSpeed{ nullptr };
        RE::ActorValueInfo* boostRechargeRate{ nullptr };
    };

    ShipPropulsionActorValues& shipPropulsionActorValues()
    {
        static ShipPropulsionActorValues values{
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("SpaceshipEnginePartMaxForwardSpeed")),
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("SpaceshipForwardSpeedMult")),
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("SpaceshipForwardForcePerPower")),
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("SpaceshipEnginePartMaxForwardAcceleration")),
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("SpaceshipBoostFuel")),
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("SpaceshipBoostSpeed")),
            RE::TESForm::LookupByEditorID<RE::ActorValueInfo>(RE::BSFixedString("SpaceshipBoostRechargeRate")),
        };
        return values;
    }

    float readShipActorValue(RE::TESObjectREFR* ship, RE::ActorValueInfo* info)
    {
        return ship && info ? ship->GetActorValue(*info) : std::numeric_limits<float>::quiet_NaN();
    }

    float readShipPermanentActorValue(RE::TESObjectREFR* ship, RE::ActorValueInfo* info)
    {
        return ship && info ? ship->GetPermanentActorValue(*info) : std::numeric_limits<float>::quiet_NaN();
    }

    std::atomic<SemanticBroadcasterFunction> g_originalSemanticBroadcaster{ nullptr };
    std::atomic<NativeEnqueueFunction> g_originalNativeEnqueue{ nullptr };
    std::atomic<ButtonEventConstructorFunction> g_originalButtonEventConstructor{ nullptr };
    std::atomic<sds::GameStateAdapter*> g_semanticDiagnosticObserver{ nullptr };
    std::atomic<bool> g_boostpackSemanticObservationArmed{ false };
    sds::BoostpackSemanticObserver g_boostpackSemanticObserver{};
    std::atomic<bool> g_semanticHookInstalled{ false };
    std::atomic<bool> g_nativeEnqueueHookInstalled{ false };
    std::atomic<bool> g_buttonEventConstructorHookInstalled{ false };
    std::atomic<bool> g_shipFlightControlHookInstalled{ false };
    alignas(8) volatile std::uintptr_t g_shipFlightControlClusterObserved{ 0 };
    std::atomic<std::uintptr_t> g_starfieldModuleBase{ 0 };
    std::atomic<std::size_t> g_starfieldModuleSize{ 0 };
    std::atomic<bool> g_upstreamProbeLogged{ false };
    std::atomic<bool> g_enqueueNoMatchLogged{ false };
    SRWLOCK g_nativeEnqueueHistoryLock = SRWLOCK_INIT;
    sds::NativeEnqueueOriginHistory g_nativeEnqueueHistory{};
    SRWLOCK g_buttonConstructorHistoryLock = SRWLOCK_INIT;
    sds::NativeButtonConstructorHistory g_buttonConstructorHistory{};

    enum NativeProducerXrefMask : std::uint32_t
    {
        kXrefButtonPrimary = 1U << 0,
        kXrefButtonId = 1U << 1,
        kXrefButtonUser = 1U << 2,
        kXrefInputQueueGlobal = 1U << 3,
    };

    struct NativeProducerXrefRecord
    {
        std::uint32_t mask{ 0 };
        sds::NativeRipReference reference{};
    };

    struct NativeProducerCandidate
    {
        std::uintptr_t functionStart{ 0 };
        std::uintptr_t functionEnd{ 0 };
        std::uint32_t mask{ 0 };
        std::vector<NativeProducerXrefRecord> references{};
    };

    std::vector<NativeProducerCandidate> g_nativeProducerCandidates{};
    std::uintptr_t g_nativeTextStart{ 0 };
    std::size_t g_nativeTextSize{ 0 };

    void copyText(sds::GameEvent& event, std::string_view text)
    {
        const auto count = (std::min)(text.size(), event.text.size() - 1);
        std::memcpy(event.text.data(), text.data(), count);
        event.text[count] = '\0';
    }

    std::string formDiagnostic(const char* prefix, std::uint32_t formId)
    {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "%s 0x%08X", prefix, formId);
        return buffer;
    }

    std::string formatPrologueMismatch(const std::array<std::uint8_t, 7>& actual)
    {
        char buffer[192]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Semantic observer: broadcaster prologue mismatch at Starfield+0x%llX; got %02X %02X %02X %02X %02X %02X %02X; hook not installed",
            static_cast<unsigned long long>(kSemanticBroadcasterRva),
            actual[0], actual[1], actual[2], actual[3], actual[4], actual[5], actual[6]);
        return buffer;
    }

    std::size_t moduleImageSize(std::uintptr_t moduleBase) noexcept
    {
        if (!moduleBase) {
            return 0;
        }

        IMAGE_DOS_HEADER dos{};
        SIZE_T bytesRead = 0;
        if (::ReadProcessMemory(
                ::GetCurrentProcess(),
                reinterpret_cast<const void*>(moduleBase),
                &dos,
                sizeof(dos),
                &bytesRead) == FALSE ||
            bytesRead != sizeof(dos) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 ||
            dos.e_lfanew > 0x01000000) {
            return 0;
        }

        IMAGE_NT_HEADERS64 nt{};
        bytesRead = 0;
        if (::ReadProcessMemory(
                ::GetCurrentProcess(),
                reinterpret_cast<const void*>(moduleBase + static_cast<std::uintptr_t>(dos.e_lfanew)),
                &nt,
                sizeof(nt),
                &bytesRead) == FALSE ||
            bytesRead != sizeof(nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return 0;
        }

        return static_cast<std::size_t>(nt.OptionalHeader.SizeOfImage);
    }

    bool isModuleAddress(std::uintptr_t address, std::uintptr_t moduleBase, std::size_t moduleSize) noexcept
    {
        return moduleBase != 0 && moduleSize != 0 && address >= moduleBase &&
            address - moduleBase < moduleSize;
    }

    template <class T>
    bool safeReadValue(std::uintptr_t address, T& value) noexcept
    {
        if (!address) {
            return false;
        }
        SIZE_T bytesRead = 0;
        return ::ReadProcessMemory(
                   ::GetCurrentProcess(),
                   reinterpret_cast<const void*>(address),
                   &value,
                   sizeof(value),
                   &bytesRead) != FALSE &&
            bytesRead == sizeof(value);
    }

    struct TargetHitSourceProbe
    {
        std::array<std::uint8_t, sizeof(RE::BSTEventSource<RE::TargetHitEvent>)> snapshot{};
        bool snapshotReadable{ false };
        std::uintptr_t vtable{ 0 };
        bool vtableReadable{ false };
        std::uintptr_t vtableSlot0{ 0 };
        bool vtableSlotReadable{ false };
        bool vtableInStarfield{ false };
        bool vtableSlot0InStarfield{ false };
        std::uint32_t sinkCount{ 0 };
        std::uint32_t sinkCapacity{ 0 };
        std::uintptr_t sinkData{ 0 };
    };

    TargetHitSourceProbe probeTargetHitSource(std::uintptr_t address) noexcept
    {
        TargetHitSourceProbe probe{};
        probe.snapshotReadable = safeReadValue(address, probe.snapshot);
        probe.vtableReadable = safeReadValue(address, probe.vtable) && probe.vtable != 0;
        probe.vtableSlotReadable = probe.vtableReadable && safeReadValue(probe.vtable, probe.vtableSlot0);

        const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
        const auto moduleSize = g_starfieldModuleSize.load(std::memory_order_acquire);
        probe.vtableInStarfield = probe.vtableReadable &&
            isModuleAddress(probe.vtable, moduleBase, moduleSize);
        probe.vtableSlot0InStarfield = probe.vtableSlotReadable &&
            isModuleAddress(probe.vtableSlot0, moduleBase, moduleSize);

        if (probe.snapshotReadable) {
            std::memcpy(&probe.sinkCount, probe.snapshot.data() + 0x08, sizeof(probe.sinkCount));
            std::memcpy(&probe.sinkCapacity, probe.snapshot.data() + 0x0C, sizeof(probe.sinkCapacity));
            std::memcpy(&probe.sinkData, probe.snapshot.data() + 0x10, sizeof(probe.sinkData));
        }
        return probe;
    }

    struct TargetHitSinkMembershipProbe
    {
        bool storageReadable{ false };
        bool present{ false };
        std::uint32_t index{ 0 };
    };

    TargetHitSinkMembershipProbe probeTargetHitSinkMembership(
        const TargetHitSourceProbe& sourceProbe,
        std::uintptr_t expectedSinkAddress) noexcept
    {
        TargetHitSinkMembershipProbe membership{};
        constexpr std::uint32_t kMaxProbeEntries = 4096;
        if (sourceProbe.sinkCount > sourceProbe.sinkCapacity ||
            sourceProbe.sinkCount > kMaxProbeEntries) {
            return membership;
        }
        if (sourceProbe.sinkCount == 0) {
            membership.storageReadable = true;
            return membership;
        }
        if (sourceProbe.sinkData == 0 || expectedSinkAddress == 0) {
            return membership;
        }

        membership.storageReadable = true;
        for (std::uint32_t i = 0; i < sourceProbe.sinkCount; ++i) {
            std::uintptr_t candidate = 0;
            const auto entryAddress = sourceProbe.sinkData +
                static_cast<std::uintptr_t>(i) * sizeof(candidate);
            if (!safeReadValue(entryAddress, candidate)) {
                membership.storageReadable = false;
                membership.present = false;
                membership.index = 0;
                return membership;
            }
            if (candidate == expectedSinkAddress) {
                membership.present = true;
                membership.index = i;
                return membership;
            }
        }
        return membership;
    }

    struct TESHitWritableSection
    {
        std::uintptr_t start{ 0 };
        std::size_t size{ 0 };
        std::array<char, 9> name{};
    };

    struct TESHitSourceDiscoveryCandidate
    {
        std::uintptr_t address{ 0 };
        std::array<char, 9> section{};
        std::uint32_t sinkCount{ 0 };
        std::uint32_t sinkCapacity{ 0 };
        std::uintptr_t sinkData{ 0 };
        bool shapeEligible{ false };
        bool sinkStorageReadable{ false };
        bool containsPlayerSink{ false };
        std::uint32_t playerSinkIndex{ 0 };
    };

    struct TESHitSourceDiscoveryScan
    {
        static constexpr std::size_t kMaxLoggedCandidates = 16;
        std::array<TESHitSourceDiscoveryCandidate, kMaxLoggedCandidates> candidates{};
        std::size_t candidateCount{ 0 };
        std::size_t writableSectionsScanned{ 0 };
        std::size_t bytesScanned{ 0 };
        std::size_t vtableMatches{ 0 };
        std::size_t shapeMatches{ 0 };
        std::size_t verifiedMatches{ 0 };
        std::uintptr_t uniqueVerifiedSource{ 0 };
        std::uintptr_t uniqueShapeSource{ 0 };
        bool candidatesTruncated{ false };
    };

    struct TESHitWritableSectionSet
    {
        static constexpr std::size_t kMaxSections = 32;
        std::array<TESHitWritableSection, kMaxSections> sections{};
        std::size_t count{ 0 };
        bool truncated{ false };
    };

    TESHitWritableSectionSet moduleWritableSections(std::uintptr_t moduleBase) noexcept
    {
        TESHitWritableSectionSet result{};
        if (!moduleBase) {
            return result;
        }

        IMAGE_DOS_HEADER dos{};
        if (!safeReadValue(moduleBase, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || dos.e_lfanew > 0x01000000) {
            return result;
        }

        IMAGE_NT_HEADERS64 nt{};
        const auto ntAddress = moduleBase + static_cast<std::uintptr_t>(dos.e_lfanew);
        if (!safeReadValue(ntAddress, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return result;
        }

        const auto sectionAddress = ntAddress + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            nt.FileHeader.SizeOfOptionalHeader;
        for (std::uint16_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER section{};
            if (!safeReadValue(sectionAddress + static_cast<std::uintptr_t>(i) * sizeof(section), section)) {
                break;
            }

            const bool writable = (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
            const bool executable = (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            if (!writable || executable) {
                continue;
            }

            const auto size = static_cast<std::size_t>(
                section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData);
            if (size == 0) {
                continue;
            }
            if (result.count >= result.sections.size()) {
                result.truncated = true;
                continue;
            }

            auto& writableSection = result.sections[result.count++];
            writableSection.start = moduleBase + section.VirtualAddress;
            writableSection.size = size;
            std::memcpy(writableSection.name.data(), section.Name, 8);
            writableSection.name[8] = '\0';
        }
        return result;
    }

    std::uintptr_t tesHitSourceVtableAddress() noexcept
    {
        try {
            static REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE::BSTEventSource_TESHitEvent_[0] };
            return vtable.address();
        } catch (...) {
            return 0;
        }
    }

    TESHitSourceDiscoveryScan scanWritableModuleSectionsForTESHitSource(
        std::uintptr_t moduleBase,
        std::uintptr_t expectedSourceVtable,
        std::uintptr_t playerSinkAddress) noexcept
    {
        TESHitSourceDiscoveryScan result{};
        if (!moduleBase || !expectedSourceVtable || !playerSinkAddress) {
            return result;
        }

        constexpr std::size_t kChunkBytes = 64 * 1024;
        constexpr std::size_t kMaxScanBytes = 64 * 1024 * 1024;
        std::array<std::byte, kChunkBytes> buffer{};
        const auto sections = moduleWritableSections(moduleBase);

        for (std::size_t sectionIndex = 0; sectionIndex < sections.count; ++sectionIndex) {
            const auto& section = sections.sections[sectionIndex];
            if (result.bytesScanned >= kMaxScanBytes) {
                break;
            }
            ++result.writableSectionsScanned;

            std::size_t sectionOffset = 0;
            while (sectionOffset < section.size && result.bytesScanned < kMaxScanBytes) {
                const auto remaining = section.size - sectionOffset;
                const auto budget = kMaxScanBytes - result.bytesScanned;
                const auto requested = (std::min)({ remaining, buffer.size(), budget });
                if (requested < sizeof(std::uintptr_t)) {
                    break;
                }

                SIZE_T bytesRead = 0;
                const auto chunkAddress = section.start + sectionOffset;
                const bool readable = ::ReadProcessMemory(
                    ::GetCurrentProcess(),
                    reinterpret_cast<const void*>(chunkAddress),
                    buffer.data(),
                    requested,
                    &bytesRead) != FALSE;
                if (!readable || bytesRead < sizeof(std::uintptr_t)) {
                    sectionOffset += requested;
                    result.bytesScanned += requested;
                    continue;
                }

                result.bytesScanned += static_cast<std::size_t>(bytesRead);
                const auto scanBytes = static_cast<std::size_t>(bytesRead);
                for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= scanBytes;
                     offset += alignof(std::uintptr_t)) {
                    std::uintptr_t observedVtable = 0;
                    std::memcpy(&observedVtable, buffer.data() + offset, sizeof(observedVtable));
                    if (observedVtable != expectedSourceVtable) {
                        continue;
                    }

                    ++result.vtableMatches;
                    const auto sourceAddress = chunkAddress + offset;
                    const auto sourceProbe = probeTargetHitSource(sourceAddress);
                    const bool shapeEligible = sds::tesHitSourceShapeEligible(
                        sourceProbe.vtable == expectedSourceVtable,
                        sourceProbe.snapshotReadable,
                        sourceProbe.sinkCount,
                        sourceProbe.sinkCapacity,
                        sourceProbe.sinkData);
                    if (shapeEligible) {
                        ++result.shapeMatches;
                        result.uniqueShapeSource = sourceAddress;
                    }

                    const auto membership = probeTargetHitSinkMembership(sourceProbe, playerSinkAddress);
                    const bool verified = sds::tesHitSourceCandidateVerified(
                        shapeEligible, membership.storageReadable, membership.present);
                    if (verified) {
                        ++result.verifiedMatches;
                        result.uniqueVerifiedSource = sourceAddress;
                    }

                    if (result.candidateCount < result.candidates.size()) {
                        auto& candidate = result.candidates[result.candidateCount++];
                        candidate.address = sourceAddress;
                        candidate.section = section.name;
                        candidate.sinkCount = sourceProbe.sinkCount;
                        candidate.sinkCapacity = sourceProbe.sinkCapacity;
                        candidate.sinkData = sourceProbe.sinkData;
                        candidate.shapeEligible = shapeEligible;
                        candidate.sinkStorageReadable = membership.storageReadable;
                        candidate.containsPlayerSink = membership.present;
                        candidate.playerSinkIndex = membership.index;
                    } else {
                        result.candidatesTruncated = true;
                    }
                }

                sectionOffset += requested;
            }
        }

        if (result.verifiedMatches != 1) {
            result.uniqueVerifiedSource = 0;
        }
        if (result.vtableMatches != 1 || result.shapeMatches != 1) {
            result.uniqueShapeSource = 0;
        }
        return result;
    }

    struct StringPoolEntrySnapshot
    {
        std::uintptr_t left{ 0 };
        std::uintptr_t lengthOrRight{ 0 };
        std::uint32_t refCount{ 0 };
        std::uint8_t flags{ 0 };
        std::array<std::byte, 3> padding{};
    };
    static_assert(sizeof(StringPoolEntrySnapshot) == 0x18);

    struct AnimationGraphEventSnapshot
    {
        const RE::TESObjectREFR* holder{ nullptr };
        std::array<char, 241> tag{};
        std::array<char, 241> payload{};
    };

    template <std::size_t N>
    bool safeReadPooledString(std::uintptr_t fixedStringAddress, std::array<char, N>& output) noexcept
    {
        output.fill('\0');
        std::uintptr_t entryAddress = 0;
        if (!safeReadValue(fixedStringAddress, entryAddress)) {
            return false;
        }
        if (!entryAddress) {
            return true;
        }

        constexpr std::uint8_t kExternalStringFlag = 1U << 1;
        for (std::size_t depth = 0; depth < 8; ++depth) {
            StringPoolEntrySnapshot entry{};
            if (!safeReadValue(entryAddress, entry)) {
                return false;
            }
            if ((entry.flags & kExternalStringFlag) != 0) {
                entryAddress = entry.lengthOrRight;
                if (!entryAddress) {
                    return false;
                }
                continue;
            }

            const auto length = static_cast<std::uint32_t>(entry.lengthOrRight & 0xFFFFFFFFU);
            if (length == 0) {
                return true;
            }
            if (length > 0x100000U) {
                return false;
            }

            const auto count = (std::min)(static_cast<std::size_t>(length), output.size() - 1);
            SIZE_T bytesRead = 0;
            return ::ReadProcessMemory(
                       ::GetCurrentProcess(),
                       reinterpret_cast<const void*>(entryAddress + sizeof(StringPoolEntrySnapshot)),
                       output.data(),
                       count,
                       &bytesRead) != FALSE &&
                bytesRead == count;
        }
        return false;
    }

    bool safeSnapshotAnimationGraphEvent(
        const RE::BSAnimationGraphEvent& event,
        AnimationGraphEventSnapshot& snapshot) noexcept
    {
        const auto eventAddress = reinterpret_cast<std::uintptr_t>(std::addressof(event));
        return safeReadValue(
                   eventAddress + offsetof(RE::BSAnimationGraphEvent, holder),
                   snapshot.holder) &&
            safeReadPooledString(
                eventAddress + offsetof(RE::BSAnimationGraphEvent, tag),
                snapshot.tag) &&
            safeReadPooledString(
                eventAddress + offsetof(RE::BSAnimationGraphEvent, payload),
                snapshot.payload);
    }

    std::optional<std::pair<std::uintptr_t, std::size_t>> moduleTextSection(
        std::uintptr_t moduleBase) noexcept
    {
        if (!moduleBase) {
            return std::nullopt;
        }

        IMAGE_DOS_HEADER dos{};
        if (!safeReadValue(moduleBase, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || dos.e_lfanew > 0x01000000) {
            return std::nullopt;
        }

        IMAGE_NT_HEADERS64 nt{};
        const auto ntAddress = moduleBase + static_cast<std::uintptr_t>(dos.e_lfanew);
        if (!safeReadValue(ntAddress, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return std::nullopt;
        }

        const auto sectionAddress = ntAddress + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            nt.FileHeader.SizeOfOptionalHeader;
        for (std::uint16_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER section{};
            if (!safeReadValue(sectionAddress + static_cast<std::uintptr_t>(i) * sizeof(section), section)) {
                break;
            }

            const bool executableCode =
                (section.Characteristics & IMAGE_SCN_CNT_CODE) != 0 &&
                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            if (!executableCode) {
                continue;
            }

            char name[9]{};
            std::memcpy(name, section.Name, 8);
            if (std::string_view(name) != ".text") {
                continue;
            }

            const auto size = static_cast<std::size_t>(
                section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData);
            if (size == 0) {
                return std::nullopt;
            }
            return std::pair{ moduleBase + section.VirtualAddress, size };
        }
        return std::nullopt;
    }

    const char* ripReferenceKindName(sds::NativeRipReferenceKind kind) noexcept
    {
        switch (kind) {
        case sds::NativeRipReferenceKind::Lea:
            return "lea";
        case sds::NativeRipReferenceKind::MovLoad:
            return "mov-load";
        case sds::NativeRipReferenceKind::MovStore:
            return "mov-store";
        }
        return "unknown";
    }

    const char* producerXrefName(std::uint32_t mask) noexcept
    {
        switch (mask) {
        case kXrefButtonPrimary:
            return "ButtonEvent.primary-vtable";
        case kXrefButtonId:
            return "ButtonEvent.id-vtable";
        case kXrefButtonUser:
            return "ButtonEvent.user-vtable";
        case kXrefInputQueueGlobal:
            return "input-queue-global";
        default:
            return "unknown";
        }
    }

    void scanNativeProducerXrefs(std::uintptr_t moduleBase) noexcept
    {
        g_nativeProducerCandidates.clear();
        g_nativeTextStart = 0;
        g_nativeTextSize = 0;

        const auto section = moduleTextSection(moduleBase);
        if (!section) {
            return;
        }
        g_nativeTextStart = section->first;
        g_nativeTextSize = section->second;

        std::vector<std::uint8_t> bytes(g_nativeTextSize);
        SIZE_T bytesRead = 0;
        if (::ReadProcessMemory(
                ::GetCurrentProcess(),
                reinterpret_cast<const void*>(g_nativeTextStart),
                bytes.data(),
                bytes.size(),
                &bytesRead) == FALSE ||
            bytesRead != bytes.size()) {
            g_nativeTextStart = 0;
            g_nativeTextSize = 0;
            return;
        }

        struct Target
        {
            std::uintptr_t address;
            std::uint32_t mask;
        };
        const std::array targets{
            Target{ moduleBase + kButtonEventPrimaryVtableRva, kXrefButtonPrimary },
            Target{ moduleBase + kButtonEventIdVtableRva, kXrefButtonId },
            Target{ moduleBase + kButtonEventUserVtableRva, kXrefButtonUser },
            Target{ moduleBase + kInputQueueSingletonRva, kXrefInputQueueGlobal },
        };

        std::map<std::uintptr_t, NativeProducerCandidate> grouped;
        for (const auto& target : targets) {
            for (const auto& reference : sds::findRipRelativeReferences(
                     g_nativeTextStart, bytes, target.address)) {
                DWORD64 imageBase = 0;
                std::uintptr_t functionStart = reference.instructionAddress;
                std::uintptr_t functionEnd = reference.instructionAddress + reference.instructionSize;
                if (const auto* runtimeFunction = ::RtlLookupFunctionEntry(
                        static_cast<DWORD64>(reference.instructionAddress), &imageBase, nullptr)) {
                    functionStart = static_cast<std::uintptr_t>(imageBase + runtimeFunction->BeginAddress);
                    functionEnd = static_cast<std::uintptr_t>(imageBase + runtimeFunction->EndAddress);
                }

                auto& candidate = grouped[functionStart];
                candidate.functionStart = functionStart;
                candidate.functionEnd = (std::max)(candidate.functionEnd, functionEnd);
                candidate.mask |= target.mask;
                if (candidate.references.size() < 16) {
                    candidate.references.push_back({ target.mask, reference });
                }
            }
        }

        g_nativeProducerCandidates.reserve(grouped.size());
        for (auto& [_, candidate] : grouped) {
            g_nativeProducerCandidates.push_back(std::move(candidate));
        }

        const auto score = [](const NativeProducerCandidate& candidate) noexcept {
            const auto buttonMask = candidate.mask &
                (kXrefButtonPrimary | kXrefButtonId | kXrefButtonUser);
            const auto buttonRefs = std::popcount(buttonMask);
            const auto queueBonus = (candidate.mask & kXrefInputQueueGlobal) != 0 ? 4 : 0;
            return buttonRefs * 10 + queueBonus;
        };
        std::sort(
            g_nativeProducerCandidates.begin(),
            g_nativeProducerCandidates.end(),
            [&](const auto& lhs, const auto& rhs) {
                const auto lhsScore = score(lhs);
                const auto rhsScore = score(rhs);
                if (lhsScore != rhsScore) {
                    return lhsScore > rhsScore;
                }
                return lhs.functionStart < rhs.functionStart;
            });

        constexpr std::size_t maxCandidates = 32;
        if (g_nativeProducerCandidates.size() > maxCandidates) {
            g_nativeProducerCandidates.resize(maxCandidates);
        }
    }

    std::string formatNativeQueueSnapshot(
        void* source,
        void* observedEvent,
        std::uintptr_t moduleBase) noexcept
    {
        std::ostringstream out;
        out << "Native input queue snapshot:" << std::hex << std::setfill('0');

        std::uintptr_t queueManager = 0;
        if (!safeReadValue(moduleBase + kInputQueueSingletonRva, queueManager) || !queueManager) {
            out << " queue=unavailable";
            return out.str();
        }

        std::uintptr_t head = 0;
        std::uint32_t nextTimeCode = 0;
        std::uint32_t sourceCursor = 0;
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source);
        const auto eventAddress = reinterpret_cast<std::uintptr_t>(observedEvent);
        const bool headOk = safeReadValue(queueManager + kInputQueueHeadOffset, head);
        const bool nextOk = safeReadValue(queueManager + kInputQueueNextTimeCodeOffset, nextTimeCode);
        const bool cursorOk = safeReadValue(sourceAddress + 0x08, sourceCursor);

        out << " queue=0x" << std::setw(static_cast<int>(sizeof(std::uintptr_t) * 2)) << queueManager
            << " lock=0x" << std::setw(static_cast<int>(sizeof(std::uintptr_t) * 2))
            << (queueManager + kInputQueueLockOffset)
            << " head=0x" << std::setw(static_cast<int>(sizeof(std::uintptr_t) * 2)) << (headOk ? head : 0)
            << std::dec
            << " nextTimeCode=" << (nextOk ? std::to_string(nextTimeCode) : std::string("unreadable"))
            << " sourceCursor=" << (cursorOk ? std::to_string(sourceCursor) : std::string("unreadable"))
            << std::hex
            << " observedEvent=0x" << std::setw(static_cast<int>(sizeof(std::uintptr_t) * 2)) << eventAddress
            << " chain=";

        std::uintptr_t node = head;
        bool foundObserved = false;
        constexpr std::size_t maxNodes = 16;
        for (std::size_t index = 0; index < maxNodes && node; ++index) {
            std::uintptr_t next = 0;
            std::uintptr_t vtable = 0;
            std::uint32_t timeCode = 0;
            std::uint32_t eventType = 0;
            std::uint32_t status = 0;
            if (!safeReadValue(node + 0x00, vtable) ||
                !safeReadValue(node + 0x10, eventType) ||
                !safeReadValue(node + 0x18, next) ||
                !safeReadValue(node + 0x20, timeCode) ||
                !safeReadValue(node + 0x24, status)) {
                out << "[read-failed@0x" << node << ']';
                break;
            }

            if (index != 0) {
                out << " -> ";
            }
            const bool observed = node == eventAddress;
            foundObserved = foundObserved || observed;
            out << '[' << std::dec << index
                << ":tc=" << timeCode
                << ",type=" << eventType
                << ",status=" << status
                << (observed ? ",OBSERVED" : "")
                << std::hex << ",obj=0x" << node
                << ",vt=0x" << vtable << ']';
            node = next;
        }
        out << std::dec << " observedInChain=" << (foundObserved ? "true" : "false");
        return out.str();
    }

    std::string formatProducerCandidate(
        const NativeProducerCandidate& candidate,
        std::uintptr_t moduleBase,
        std::size_t rank)
    {
        std::ostringstream out;
        out << "Native producer xref candidate: rank=" << rank << std::hex << std::setfill('0');
        if (candidate.functionStart >= moduleBase) {
            out << " function=Starfield+0x" << (candidate.functionStart - moduleBase)
                << "..0x" << (candidate.functionEnd - moduleBase);
        } else {
            out << " function=outside-Starfield";
        }
        out << std::dec
            << " primary=" << ((candidate.mask & kXrefButtonPrimary) ? "yes" : "no")
            << " id=" << ((candidate.mask & kXrefButtonId) ? "yes" : "no")
            << " user=" << ((candidate.mask & kXrefButtonUser) ? "yes" : "no")
            << " queueGlobal=" << ((candidate.mask & kXrefInputQueueGlobal) ? "yes" : "no")
            << " refs=" << candidate.references.size();
        return out.str();
    }

    std::string formatProducerReference(
        const NativeProducerXrefRecord& record,
        std::uintptr_t moduleBase)
    {
        std::ostringstream out;
        out << "Native producer xref: target=" << producerXrefName(record.mask)
            << " kind=" << ripReferenceKindName(record.reference.kind)
            << std::hex << std::setfill('0');
        if (record.reference.instructionAddress >= moduleBase) {
            out << " at=Starfield+0x" << (record.reference.instructionAddress - moduleBase);
        } else {
            out << " at=outside-Starfield";
        }
        out << " bytes=";
        for (std::size_t i = 0; i < record.reference.instructionByteCount; ++i) {
            if (i != 0) {
                out << ' ';
            }
            out << std::setw(2) << static_cast<unsigned int>(record.reference.instructionBytes[i]);
        }
        return out.str();
    }

    std::string formatProducerFunctionChunk(
        const sds::NativeFunctionChunkDiagnostic& chunk,
        std::size_t rank)
    {
        std::ostringstream out;
        out << "Native producer function dump: rank=" << rank << std::hex << std::setfill('0');
        if (chunk.functionStart >= chunk.moduleBase) {
            out << " function=Starfield+0x" << (chunk.functionStart - chunk.moduleBase)
                << "..0x" << (chunk.functionEnd - chunk.moduleBase);
        } else {
            out << " function=outside-Starfield";
        }
        out << std::dec << " chunk=" << (chunk.chunkIndex + 1) << '/' << chunk.chunkCount << std::hex;
        if (chunk.chunkStart >= chunk.moduleBase) {
            out << " start=Starfield+0x" << (chunk.chunkStart - chunk.moduleBase);
        }
        out << " bytes" << std::dec << chunk.byteCount << '=' << std::hex;
        for (std::size_t i = 0; i < chunk.byteCount; ++i) {
            if (i != 0) {
                out << ' ';
            }
            out << std::setw(2) << static_cast<unsigned int>(chunk.bytes[i]);
        }
        return out.str();
    }

    std::optional<sds::NativeCodeProbeDiagnostic> buildNativeCodeProbe(
        std::uintptr_t frameAddress,
        std::uintptr_t moduleBase,
        std::size_t moduleSize) noexcept
    {
        if (!isModuleAddress(frameAddress, moduleBase, moduleSize)) {
            return std::nullopt;
        }

        sds::NativeCodeProbeDiagnostic probe{};
        probe.frameAddress = frameAddress;
        probe.moduleBase = moduleBase;
        probe.moduleSize = moduleSize;

        DWORD64 imageBase = 0;
        const auto controlPc = static_cast<DWORD64>(frameAddress - 1);
        if (const auto* runtimeFunction = ::RtlLookupFunctionEntry(controlPc, &imageBase, nullptr)) {
            probe.functionStart = static_cast<std::uintptr_t>(imageBase + runtimeFunction->BeginAddress);
            probe.functionEnd = static_cast<std::uintptr_t>(imageBase + runtimeFunction->EndAddress);
        }

        constexpr std::size_t scanBytes = 32;
        auto scanStart = frameAddress > moduleBase + scanBytes ? frameAddress - scanBytes : moduleBase;
        if (isModuleAddress(probe.functionStart, moduleBase, moduleSize)) {
            scanStart = (std::max)(scanStart, probe.functionStart);
        }
        if (scanStart < frameAddress) {
            const auto scanSize = static_cast<std::size_t>(frameAddress - scanStart);
            std::vector<std::uint8_t> bytes(scanSize);
            SIZE_T bytesRead = 0;
            if (::ReadProcessMemory(
                    ::GetCurrentProcess(),
                    reinterpret_cast<const void*>(scanStart),
                    bytes.data(),
                    bytes.size(),
                    &bytesRead) != FALSE &&
                bytesRead == bytes.size()) {
                if (const auto call = sds::findNearestCallBefore(scanStart, bytes, frameAddress)) {
                    probe.callAddress = call->instructionAddress;
                    probe.callTarget = call->targetAddress;
                    probe.callTargetDecoded = call->targetDecoded;
                    probe.callKind = call->kind;
                    probe.callFound = true;
                }
            }
        }

        constexpr std::size_t bytesBeforeCall = 96;
        const auto anchor = probe.callFound ? probe.callAddress : frameAddress;
        probe.windowStart = anchor > moduleBase + bytesBeforeCall
            ? anchor - bytesBeforeCall
            : moduleBase;

        const auto moduleEnd = moduleBase + moduleSize;
        const auto available = probe.windowStart < moduleEnd ? moduleEnd - probe.windowStart : 0;
        const auto wanted = (std::min)(probe.codeBytes.size(), static_cast<std::size_t>(available));
        if (wanted == 0) {
            return probe;
        }

        SIZE_T bytesRead = 0;
        if (::ReadProcessMemory(
                ::GetCurrentProcess(),
                reinterpret_cast<const void*>(probe.windowStart),
                probe.codeBytes.data(),
                wanted,
                &bytesRead) != FALSE) {
            probe.codeSize = (std::min)(static_cast<std::size_t>(bytesRead), probe.codeBytes.size());
        }
        return probe;
    }

    std::vector<sds::NativeFunctionChunkDiagnostic> buildNativeFunctionChunks(
        const sds::NativeCodeProbeDiagnostic& probe) noexcept
    {
        std::vector<sds::NativeFunctionChunkDiagnostic> chunks;
        if (!isModuleAddress(probe.functionStart, probe.moduleBase, probe.moduleSize) ||
            probe.functionEnd <= probe.functionStart ||
            probe.functionEnd > probe.moduleBase + probe.moduleSize) {
            return chunks;
        }

        constexpr std::size_t maxFunctionBytes = 4096;
        const auto functionBytes = static_cast<std::size_t>(probe.functionEnd - probe.functionStart);
        if (functionBytes == 0 || functionBytes > maxFunctionBytes) {
            return chunks;
        }

        const auto chunkCount = (functionBytes + sds::kNativeFunctionChunkBytes - 1) /
            sds::kNativeFunctionChunkBytes;
        chunks.reserve(chunkCount);
        for (std::size_t index = 0; index < chunkCount; ++index) {
            sds::NativeFunctionChunkDiagnostic chunk{};
            chunk.moduleBase = probe.moduleBase;
            chunk.moduleSize = probe.moduleSize;
            chunk.functionStart = probe.functionStart;
            chunk.functionEnd = probe.functionEnd;
            chunk.chunkIndex = index;
            chunk.chunkCount = chunkCount;
            chunk.chunkStart = probe.functionStart + index * sds::kNativeFunctionChunkBytes;
            const auto remaining = static_cast<std::size_t>(probe.functionEnd - chunk.chunkStart);
            const auto wanted = (std::min)(remaining, chunk.bytes.size());
            SIZE_T bytesRead = 0;
            if (::ReadProcessMemory(
                    ::GetCurrentProcess(),
                    reinterpret_cast<const void*>(chunk.chunkStart),
                    chunk.bytes.data(),
                    wanted,
                    &bytesRead) == FALSE ||
                bytesRead == 0) {
                break;
            }
            chunk.byteCount = (std::min)(static_cast<std::size_t>(bytesRead), wanted);
            chunks.push_back(chunk);
        }
        return chunks;
    }

    const char* nativeButtonFieldName(std::uint32_t mask) noexcept
    {
        switch (mask) {
        case sds::kNativeButtonFieldTimeCode:
            return "timeCode(+0x20)";
        case sds::kNativeButtonFieldAction:
            return "action(+0x28)";
        case sds::kNativeButtonFieldIdCode:
            return "idCode(+0x30)";
        case sds::kNativeButtonFieldValue:
            return "value(+0x48)";
        case sds::kNativeButtonFieldHeld:
            return "held(+0x4c)";
        default:
            return "unknown";
        }
    }

    const char* nativeCallKindName(sds::NativeCallKind kind) noexcept
    {
        switch (kind) {
        case sds::NativeCallKind::DirectRel32:
            return "direct-rel32";
        case sds::NativeCallKind::IndirectRegister:
            return "indirect-register";
        case sds::NativeCallKind::IndirectMemory:
            return "indirect-memory";
        }
        return "unknown";
    }

    std::string formatRecycledProducerSummary(
        std::size_t rank,
        std::uintptr_t functionStart,
        std::uintptr_t functionEnd,
        std::uintptr_t moduleBase,
        std::uint32_t fieldMask,
        std::size_t writeCount,
        std::size_t callCount)
    {
        std::ostringstream out;
        out << "Native recycled producer probe: rank=" << std::dec << rank
            << std::hex << std::setfill('0')
            << " function=Starfield+0x" << (functionStart - moduleBase)
            << "..0x" << (functionEnd - moduleBase)
            << std::dec
            << " score=" << (std::popcount(fieldMask) * 10 + (std::min)(writeCount, std::size_t{ 9 }))
            << " fieldWrites=" << writeCount
            << " calls=" << callCount
            << " fields=";
        bool wrote = false;
        constexpr std::array masks{
            sds::kNativeButtonFieldTimeCode,
            sds::kNativeButtonFieldAction,
            sds::kNativeButtonFieldIdCode,
            sds::kNativeButtonFieldValue,
            sds::kNativeButtonFieldHeld,
        };
        for (const auto mask : masks) {
            if ((fieldMask & mask) == 0) {
                continue;
            }
            if (wrote) {
                out << ',';
            }
            out << nativeButtonFieldName(mask);
            wrote = true;
        }
        if (!wrote) {
            out << "none";
        }
        return out.str();
    }

    void logRecycledProducerCluster(
        const sds::GameStateAdapter::LogCallback& logCallback,
        std::uintptr_t moduleBase,
        std::size_t moduleSize) noexcept
    {
        if (!logCallback || !moduleBase || !moduleSize) {
            return;
        }

        for (std::size_t rank = 0; rank < kRecycledProducerClusterRvas.size(); ++rank) {
            const auto probeAddress = moduleBase + kRecycledProducerClusterRvas[rank];
            DWORD64 imageBase = 0;
            const auto* runtimeFunction = ::RtlLookupFunctionEntry(
                static_cast<DWORD64>(probeAddress), &imageBase, nullptr);
            if (!runtimeFunction) {
                char buffer[160]{};
                std::snprintf(buffer, sizeof(buffer),
                    "Native recycled producer probe: rank=%zu rva=Starfield+0x%llX unwind=unavailable",
                    rank + 1,
                    static_cast<unsigned long long>(kRecycledProducerClusterRvas[rank]));
                logCallback(buffer);
                continue;
            }

            const auto functionStart = static_cast<std::uintptr_t>(imageBase + runtimeFunction->BeginAddress);
            const auto functionEnd = static_cast<std::uintptr_t>(imageBase + runtimeFunction->EndAddress);
            if (!isModuleAddress(functionStart, moduleBase, moduleSize) ||
                functionEnd <= functionStart || functionEnd > moduleBase + moduleSize ||
                functionEnd - functionStart > 4096) {
                continue;
            }

            const auto byteCount = static_cast<std::size_t>(functionEnd - functionStart);
            std::vector<std::uint8_t> bytes(byteCount);
            SIZE_T bytesRead = 0;
            if (::ReadProcessMemory(
                    ::GetCurrentProcess(),
                    reinterpret_cast<const void*>(functionStart),
                    bytes.data(), bytes.size(), &bytesRead) == FALSE ||
                bytesRead != bytes.size()) {
                continue;
            }

            const auto writes = sds::findButtonEventFieldWrites(functionStart, bytes);
            const auto calls = sds::findNativeCallSites(functionStart, bytes);
            std::uint32_t fieldMask = 0;
            for (const auto& write : writes) {
                fieldMask |= write.fieldMask;
            }
            logCallback(formatRecycledProducerSummary(
                rank + 1, functionStart, functionEnd, moduleBase,
                fieldMask, writes.size(), calls.size()));

            for (const auto& write : writes) {
                std::ostringstream out;
                out << "Native recycled producer field write: rank=" << std::dec << (rank + 1)
                    << " field=" << nativeButtonFieldName(write.fieldMask)
                    << std::hex << std::setfill('0')
                    << " at=Starfield+0x" << (write.instructionAddress - moduleBase)
                    << " bytes=";
                for (std::size_t i = 0; i < write.instructionByteCount; ++i) {
                    if (i) out << ' ';
                    out << std::setw(2) << static_cast<unsigned int>(write.instructionBytes[i]);
                }
                logCallback(out.str());
            }

            for (const auto& call : calls) {
                std::ostringstream out;
                out << "Native recycled producer call: rank=" << std::dec << (rank + 1)
                    << " kind=" << nativeCallKindName(call.kind)
                    << std::hex << std::setfill('0')
                    << " at=Starfield+0x" << (call.instructionAddress - moduleBase)
                    << " target=";
                if (call.targetDecoded && isModuleAddress(call.targetAddress, moduleBase, moduleSize)) {
                    out << "Starfield+0x" << (call.targetAddress - moduleBase);
                } else if (call.targetDecoded) {
                    out << "outside-Starfield";
                } else {
                    out << "undecoded";
                }
                out << " bytes=";
                for (std::size_t i = 0; i < call.instructionByteCount; ++i) {
                    if (i) out << ' ';
                    out << std::setw(2) << static_cast<unsigned int>(call.instructionBytes[i]);
                }
                logCallback(out.str());
            }

            sds::NativeCodeProbeDiagnostic probe{};
            probe.moduleBase = moduleBase;
            probe.moduleSize = moduleSize;
            probe.functionStart = functionStart;
            probe.functionEnd = functionEnd;
            for (const auto& chunk : buildNativeFunctionChunks(probe)) {
                std::ostringstream out;
                out << "Native recycled producer function dump: rank=" << std::dec << (rank + 1)
                    << std::hex << std::setfill('0')
                    << " function=Starfield+0x" << (chunk.functionStart - moduleBase)
                    << "..0x" << (chunk.functionEnd - moduleBase)
                    << std::dec << " chunk=" << (chunk.chunkIndex + 1) << '/' << chunk.chunkCount
                    << std::hex << " start=Starfield+0x" << (chunk.chunkStart - moduleBase)
                    << " bytes" << std::dec << chunk.byteCount << '=' << std::hex;
                for (std::size_t i = 0; i < chunk.byteCount; ++i) {
                    if (i) out << ' ';
                    out << std::setw(2) << static_cast<unsigned int>(chunk.bytes[i]);
                }
                logCallback(out.str());
            }
        }
    }

    std::optional<sds::NativeSourceSnapshotDiagnostic> buildNativeSourceSnapshot(
        void* source,
        std::uintptr_t moduleBase,
        std::size_t moduleSize) noexcept
    {
        if (!source || !moduleBase || !moduleSize) {
            return std::nullopt;
        }

        sds::NativeSourceSnapshotDiagnostic snapshot{};
        snapshot.moduleBase = moduleBase;
        snapshot.moduleSize = moduleSize;
        snapshot.observedSourceAddress = reinterpret_cast<std::uintptr_t>(source);

        SIZE_T bytesRead = 0;
        std::uintptr_t manager = 0;
        if (::ReadProcessMemory(
                ::GetCurrentProcess(),
                reinterpret_cast<const void*>(moduleBase + kInputManagerSingletonRva),
                &manager,
                sizeof(manager),
                &bytesRead) != FALSE &&
            bytesRead == sizeof(manager) && manager != 0) {
            snapshot.managerAddress = manager;
            snapshot.expectedSourceAddress = manager + 0x10;
            snapshot.sourceMatchesManager = snapshot.expectedSourceAddress == snapshot.observedSourceAddress;
        }

        bytesRead = 0;
        (void)::ReadProcessMemory(
            ::GetCurrentProcess(),
            source,
            &snapshot.sourceVtable,
            sizeof(snapshot.sourceVtable),
            &bytesRead);

        bytesRead = 0;
        if (::ReadProcessMemory(
                ::GetCurrentProcess(),
                source,
                snapshot.bytes.data(),
                snapshot.bytes.size(),
                &bytesRead) != FALSE) {
            snapshot.byteCount = (std::min)(static_cast<std::size_t>(bytesRead), snapshot.bytes.size());
        }
        return snapshot;
    }

}

sds::GameStateAdapter::GameStateAdapter(
    EmitCallback emitCallback,
    LogCallback logCallback,
    bool fireMarkerTraceEnabled,
    bool incomingDamageDiagnosticEnabled) :
    _emit(std::move(emitCallback)),
    _log(std::move(logCallback)),
    _fireMarkerTraceEnabled(fireMarkerTraceEnabled),
    _incomingDamageDiagnosticEnabled(incomingDamageDiagnosticEnabled)
{}
void sds::setBoostpackSemanticObserver(BoostpackSemanticObserver observer)
{
    g_boostpackSemanticObserver = std::move(observer);
}

void sds::setBoostpackSemanticObservationArmed(bool armed) noexcept
{
    g_boostpackSemanticObservationArmed.store(armed, std::memory_order_release);
}

bool sds::boostpackSemanticObservationArmed() noexcept
{
    return g_boostpackSemanticObservationArmed.load(std::memory_order_acquire);
}

sds::GameStateAdapter::~GameStateAdapter()
{
    unregisterSinks();
}

void sds::GameStateAdapter::setLandVehicleMotionCallback(LandVehicleMotionCallback callback)
{
    std::scoped_lock guard(_landVehicleReconMutex);
    _landVehicleMotionCallback = std::move(callback);
}

void sds::GameStateAdapter::setLandVehicleSemanticCallback(LandVehicleSemanticCallback callback)
{
    _landVehicleSemanticCallback = std::move(callback);
}

void sds::GameStateAdapter::clearLandVehicleProductionState(
    std::chrono::steady_clock::time_point when,
    bool emitRelease) noexcept
{
    bool wasActive = false;
    LandVehicleMotionCallback motionCallback{};
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        wasActive = _landVehicleProductionAuthorityActive;
        _landVehicleProductionAuthorityActive = false;
        _landVehicleProductionEpoch = 0;
        motionCallback = _landVehicleMotionCallback;
    }

    if (motionCallback) {
        motionCallback(LandVehicleMotionState{});
    }
    if (emitRelease && wasActive) {
        GameEvent event{};
        event.type = GameEventType::LandVehicleAuthorityReleased;
        event.when = when;
        emit(std::move(event));
    }
}

bool sds::GameStateAdapter::registerSinks()
{
    if (_registered) {
        return true;
    }

    auto* equipSource = RE::ActorItemEquipped::Event::GetEventSource();
    auto* ui = RE::UI::GetSingleton();
    if (!equipSource || !ui) {
        log("Game state: one or more Starfield event sources are unavailable");
        return false;
    }

    equipSource->RegisterSink(this);
    ui->RegisterSink<RE::MenuOpenCloseEvent>(this);
    _worldGate.snapshot(
        ui->IsMenuOpen(RE::BSFixedString("MainMenu")),
        ui->IsMenuOpen(RE::BSFixedString("LoadingMenu")));

    if (!worldAvailable()) {
        log("Game state: world access suspended at sink registration");
    }

    _registered = true;
    _nextHealthPoll = std::chrono::steady_clock::now();
    // CommonLibSF declares WeaponFiredEvent for this runtime, but its Address
    // Library relocation is ID 0. Calling GetEventSource() would terminate
    // plugin initialization with an invalid Address Library ID. Keep live R2
    // tracking enabled, but do not register an unsupported fire-event source.
    log("Game state: equip/menu sinks registered; live R2 enabled; confirmed-fire source disabled (CommonLibSF relocation ID 0)");
    if (_fireMarkerTraceEnabled) {
        log("Fire marker trace: ACTIVE on validated Starfield 1.16.244.0 runtime; passive player animation markers only; controller behavior unchanged; bounded 20-second capture window after weapon equip");
    } else {
        log("Fire marker trace: disabled because this Starfield runtime is not the validated 1.16.244.0 ABI; controller behavior unchanged");
    }

    if (!installSemanticBroadcasterDiagnosticHook()) {         log("Semantic observer: failed to install; native semantic verification logging will be incomplete");         return false;     }     log("CTD RESTORE: semantic native-pipeline hook ACTIVE");
    if (!installShipFlightControlDiagnosticHook()) {         log("Ship flight-control probe: passive writer capture unavailable; propulsion diagnostic will continue with boost-state evidence only");     }     log("CTD RESTORE: ship flight-control hook startup RESTORED; FireMarker lifecycle fix retained");
    if (!registerLandVehicleDriverEventReconSink()) {
        log("Land vehicle recon: VehicleDriverEnterExitEvent source unavailable; camera corroboration will continue diagnostic-only");
    }
    _nextLandVehicleReconPoll = std::chrono::steady_clock::now();
    _nextLandVehicleSummaryLog = _nextLandVehicleReconPoll;
    log("CTD RESTORE: runtimeTick active; semantic and ship direct hooks active");
    return true;
}

void sds::GameStateAdapter::unregisterSinks() noexcept
{
    _worldGate.suspend();
    auto* expectedObserver = this;
    (void)g_semanticDiagnosticObserver.compare_exchange_strong(
        expectedObserver,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);

    unregisterLandVehicleDriverEventReconSink();
    unregisterTESHitSink();
    unregisterTargetHitSink();
    _shipPilotContext.reset();
    clearLandVehicleProductionState(std::chrono::steady_clock::now(), false);
    {
        std::scoped_lock landVehicleGuard(_landVehicleReconMutex);
        _landVehicleReconProbe.reset();
        _landVehicleTelemetryProbe.reset();
        _landVehicleLatestTelemetry = {};
        _landVehiclePhysicsProbe.reset();
        _landVehicleLatestPhysics = {};
        _landVehicleLastDriverEvent = {};
        _landVehicleDriverEventSequence = 0;
        _landVehicleLastPolledDriverEventSequence = 0;
        _landVehicleLoading = false;
        _lastLandVehicleCameraCorroboration = false;
        _landVehicleSuppressionBoundaryActive = false;
        _landVehicleLastDriverEventAt = {};
        _landVehicleCorrelationArmed.store(false, std::memory_order_release);
    }

    if (!_registered) {
        return;
    }

    _fireMarkerCapture.disarm();
    clearFireMarkerSinks();

    try {
        if (auto* source = RE::ActorItemEquipped::Event::GetEventSource()) {
            source->UnregisterSink(this);
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->UnregisterSink<RE::MenuOpenCloseEvent>(this);
        }
    } catch (...) {
        // Game teardown can invalidate singleton state; unloading must fail soft.
    }
    _registered = false;
}

bool sds::GameStateAdapter::registerLandVehicleDriverEventReconSink()
{
    if (_landVehicleDriverEventSinkRegistered) {
        log("Land vehicle recon: VehicleDriverEnterExitEvent sink already registered behavior=passive-read-only");
        return true;
    }

    // The only public SFSE table entry we have for VehicleDriverEnterExitEvent
    // is a legacy executable RVA. RVAs are runtime-version specific; calling it
    // on an unvalidated Starfield build can jump into unrelated code. Fail closed
    // until this runtime has a version-independent relocation/signature.
    log("Land vehicle recon: VehicleDriverEnterExitEvent source disabled reason=unversioned-legacy-rva legacyDirectGetter=no cameraFallback=yes behavior=passive-read-only controllerOutput=none");
    return false;
}

void sds::GameStateAdapter::unregisterLandVehicleDriverEventReconSink() noexcept
{
    if (!_landVehicleDriverEventSinkRegistered || !_landVehicleDriverEventSource) {
        return;
    }
    try {
        _landVehicleDriverEventSource->UnregisterSink(static_cast<RE::BSTEventSink<LandVehicleDriverEnterExitRawEvent>*>(this));
    } catch (...) {
        // Teardown must remain fail-soft if engine event sources are already going away.
    }
    _landVehicleDriverEventSource = nullptr;
    _landVehicleDriverEventSinkRegistered = false;
}

bool sds::GameStateAdapter::installSemanticBroadcasterDiagnosticHook()
{
    if (g_semanticHookInstalled.load(std::memory_order_acquire)) {
        g_semanticDiagnosticObserver.store(this, std::memory_order_release);
        log("Semantic observer: broadcaster hook already installed; observer re-armed");
        return true;
    }

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    if (!moduleBase) {
        log("Semantic observer: Starfield module base unavailable");
        return false;
    }

    const auto target = moduleBase + kSemanticBroadcasterRva;
    std::array<std::uint8_t, kSemanticBroadcasterDisplacedBytes> prologue{};
    std::memcpy(prologue.data(), reinterpret_cast<const void*>(target), prologue.size());
    if (!matchesSemanticBroadcasterPrologue(prologue)) {
        log(formatPrologueMismatch(prologue));
        return false;
    }

    auto& trampoline = REL::GetTrampoline();
    if (trampoline.empty()) {
        // Reserve enough near-module space for the existing semantic/origin observers
        // plus the passive v0.3.62-r2 ship flight-control capture stub.
        trampoline.create(512, reinterpret_cast<void*>(target));
    }
    if (trampoline.free_size() < 64) {
        log("Semantic observer: CommonLib trampoline has insufficient free space");
        return false;
    }

    constexpr auto originalStubSize = kSemanticBroadcasterDisplacedBytes + sizeof(REL::ASM::JMP14);
    auto* originalStub = static_cast<std::uint8_t*>(trampoline.allocate(originalStubSize));
    std::memcpy(originalStub, prologue.data(), prologue.size());
    const REL::ASM::JMP14 jumpBack(target + kSemanticBroadcasterDisplacedBytes);
    std::memcpy(originalStub + kSemanticBroadcasterDisplacedBytes, &jumpBack, sizeof(jumpBack));
    ::FlushInstructionCache(::GetCurrentProcess(), originalStub, originalStubSize);

    const auto thunkAddress = reinterpret_cast<std::uintptr_t>(&GameStateAdapter::semanticBroadcasterDiagnosticThunk);
    const auto branchIsland = trampoline.allocate_branch5(thunkAddress);
    const auto entryPatch = buildSemanticBroadcasterEntryPatch(target, branchIsland);
    if (!entryPatch) {
        log("Semantic observer: CommonLib branch island is outside rel32 range; hook not installed");
        return false;
    }

    g_originalSemanticBroadcaster.store(
        reinterpret_cast<SemanticBroadcasterFunction>(originalStub),
        std::memory_order_release);
    g_starfieldModuleBase.store(moduleBase, std::memory_order_release);
    g_starfieldModuleSize.store(moduleImageSize(moduleBase), std::memory_order_release);
    g_semanticDiagnosticObserver.store(this, std::memory_order_release);

    if (!REL::WriteSafe(target, entryPatch->data(), entryPatch->size())) {
        g_semanticDiagnosticObserver.store(nullptr, std::memory_order_release);
        g_originalSemanticBroadcaster.store(nullptr, std::memory_order_release);
        log("Semantic observer: failed to patch broadcaster entry; hook not installed");
        return false;
    }

    g_semanticHookInstalled.store(true, std::memory_order_release);

    char buffer[256]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Semantic native-pipeline observer: installed at Starfield+0x%llX target=0x%llX trampoline=0x%llX; reusable native action verification enabled",
        static_cast<unsigned long long>(kSemanticBroadcasterRva),
        static_cast<unsigned long long>(target),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(originalStub)));
    log(buffer);
    return true;
}


bool sds::GameStateAdapter::installShipFlightControlDiagnosticHook()
{
    if (g_shipFlightControlHookInstalled.load(std::memory_order_acquire)) {
        log("Ship flight-control probe: observation hook already installed; behavior=passive");
        return true;
    }

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    if (!moduleBase) {
        log("Ship flight-control probe: Starfield module base unavailable");
        return false;
    }

    const auto textSection = moduleTextSection(moduleBase);
    if (!textSection || textSection->second < 8) {
        log("Ship flight-control probe: Starfield .text section unavailable");
        return false;
    }

    std::vector<std::uint8_t> textBytes(textSection->second);
    SIZE_T bytesRead = 0;
    if (::ReadProcessMemory(
            ::GetCurrentProcess(),
            reinterpret_cast<const void*>(textSection->first),
            textBytes.data(),
            textBytes.size(),
            &bytesRead) == FALSE ||
        bytesRead != textBytes.size()) {
        log("Ship flight-control probe: unable to snapshot Starfield .text; hook not installed");
        return false;
    }

    const auto writerOffset = sds::findShipFlightControlWriterSite(textBytes);
    if (!writerOffset) {
        log("Ship flight-control probe: validated writer signature was absent or ambiguous; hook not installed");
        return false;
    }

    constexpr std::array<std::uint8_t, 8> kExpectedRollWriter{
        0x48, 0x8D, 0x50, 0x58, 0xC5, 0xFA, 0x11, 0x02
    };
    const auto target = textSection->first + *writerOffset;
    std::array<std::uint8_t, kExpectedRollWriter.size()> displaced{};
    if (!safeReadValue(target, displaced) || displaced != kExpectedRollWriter) {
        log("Ship flight-control probe: resolved writer bytes changed before patch; hook not installed");
        return false;
    }

    auto& trampoline = REL::GetTrampoline();
    if (trampoline.empty()) {
        trampoline.create(512, reinterpret_cast<void*>(target));
    }

    // pushfq; push rcx; mov rcx, &capture; mov [rcx],rax; pop rcx; popfq;
    // then execute the exact displaced writer and jump back. This observes RAX
    // only; it does not alter throttle, velocity, or any ship-control lane.
    constexpr std::size_t kCapturePrefixSize = 17;
    constexpr std::size_t kStubSize = kCapturePrefixSize + kExpectedRollWriter.size() + sizeof(REL::ASM::JMP14);
    if (trampoline.free_size() < kStubSize + 16) {
        log("Ship flight-control probe: CommonLib trampoline has insufficient free space");
        return false;
    }

    auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(kStubSize));
    std::size_t cursor = 0;
    stub[cursor++] = 0x9C;  // pushfq
    stub[cursor++] = 0x51;  // push rcx
    stub[cursor++] = 0x48;
    stub[cursor++] = 0xB9;  // mov rcx, imm64
    const auto captureAddress = reinterpret_cast<std::uintptr_t>(&g_shipFlightControlClusterObserved);
    std::memcpy(stub + cursor, &captureAddress, sizeof(captureAddress));
    cursor += sizeof(captureAddress);
    stub[cursor++] = 0x48;
    stub[cursor++] = 0x89;
    stub[cursor++] = 0x01;  // mov [rcx], rax
    stub[cursor++] = 0x59;  // pop rcx
    stub[cursor++] = 0x9D;  // popfq
    std::memcpy(stub + cursor, displaced.data(), displaced.size());
    cursor += displaced.size();
    const REL::ASM::JMP14 jumpBack(target + displaced.size());
    std::memcpy(stub + cursor, &jumpBack, sizeof(jumpBack));
    cursor += sizeof(jumpBack);
    if (cursor != kStubSize) {
        log("Ship flight-control probe: internal capture stub size mismatch");
        return false;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), stub, kStubSize);

    const auto entryPatch = sds::buildShipFlightControlCapturePatch(
        target, reinterpret_cast<std::uintptr_t>(stub));
    if (!entryPatch) {
        log("Ship flight-control probe: capture trampoline is outside rel32 range; hook not installed");
        return false;
    }
    if (!REL::WriteSafe(target, entryPatch->data(), entryPatch->size())) {
        log("Ship flight-control probe: failed to patch validated writer; hook not installed");
        return false;
    }

    g_shipFlightControlHookInstalled.store(true, std::memory_order_release);
    char message[384]{};
    std::snprintf(
        message,
        sizeof(message),
        "Ship flight-control probe: observation hook installed at Starfield+0x%llX trampoline=0x%llX lanes=throttleTarget+0x68,effectiveThrottle+0x6C,velocity+0x70 behavior=passive",
        static_cast<unsigned long long>(target - moduleBase),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(stub)));
    log(message);
    return true;
}


bool sds::GameStateAdapter::installNativeEnqueueOriginHook()
{
    if (g_nativeEnqueueHookInstalled.load(std::memory_order_acquire)) {
        log("Native enqueue origin: callsite hooks already installed");
        return true;
    }

    const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
    const auto moduleSize = g_starfieldModuleSize.load(std::memory_order_acquire);
    if (!moduleBase || !moduleSize) {
        log("Native enqueue origin: Starfield module information unavailable");
        return false;
    }

    const auto target = moduleBase + kNativeEnqueueRva;
    const auto textSection = moduleTextSection(moduleBase);
    if (!textSection || textSection->second < 5) {
        log("Native enqueue origin: Starfield .text section unavailable");
        return false;
    }

    std::vector<std::uint8_t> textBytes(textSection->second);
    SIZE_T bytesRead = 0;
    if (::ReadProcessMemory(
            ::GetCurrentProcess(),
            reinterpret_cast<const void*>(textSection->first),
            textBytes.data(),
            textBytes.size(),
            &bytesRead) == FALSE ||
        bytesRead != textBytes.size()) {
        log("Native enqueue origin: failed to snapshot Starfield .text section");
        return false;
    }

    const auto callSites = findDirectRel32CallsToTarget(textSection->first, textBytes, target);
    if (callSites.empty()) {
        log("Native enqueue origin: no direct Starfield callers of Starfield+0x22DABC0 found; hook not installed");
        return false;
    }
    constexpr std::size_t maxCallSites = 64;
    if (callSites.size() > maxCallSites) {
        char buffer[192]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Native enqueue origin: refusing to patch %zu direct callers (limit=%zu)",
            callSites.size(),
            maxCallSites);
        log(buffer);
        return false;
    }

    auto& trampoline = REL::GetTrampoline();
    if (trampoline.empty()) {
        trampoline.create(128, reinterpret_cast<void*>(target));
    }
    if (trampoline.free_size() < 32) {
        log("Native enqueue origin: CommonLib trampoline has insufficient free space");
        return false;
    }

    const auto thunkAddress = reinterpret_cast<std::uintptr_t>(&GameStateAdapter::nativeEnqueueOriginThunk);
    const auto branchIsland = trampoline.allocate_branch5(thunkAddress);
    std::vector<std::array<std::uint8_t, 5>> patches;
    patches.reserve(callSites.size());
    for (const auto& call : callSites) {
        const auto patch = buildRel32CallPatch(call.instructionAddress, branchIsland);
        if (!patch) {
            log("Native enqueue origin: branch island is outside rel32 range for one or more callers; hook not installed");
            return false;
        }
        patches.push_back(*patch);
    }

    g_originalNativeEnqueue.store(
        reinterpret_cast<NativeEnqueueFunction>(target),
        std::memory_order_release);

    std::size_t installed = 0;
    for (std::size_t i = 0; i < callSites.size(); ++i) {
        if (!REL::WriteSafe(callSites[i].instructionAddress, patches[i].data(), patches[i].size())) {
            char buffer[224]{};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "Native enqueue origin: patch failed after %zu/%zu direct callers; already-patched callers remain observation-only",
                installed,
                callSites.size());
            log(buffer);
            if (installed == 0) {
                g_originalNativeEnqueue.store(nullptr, std::memory_order_release);
                return false;
            }
            break;
        }
        ++installed;
    }

    if (installed == 0) {
        g_originalNativeEnqueue.store(nullptr, std::memory_order_release);
        return false;
    }

    g_nativeEnqueueHookInstalled.store(true, std::memory_order_release);

    {
        char buffer[256]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Native enqueue origin: intercepted %zu/%zu direct callers of Starfield+0x%llX via shared branch island=0x%llX; observation only; original target forwarded unchanged",
            installed,
            callSites.size(),
            static_cast<unsigned long long>(kNativeEnqueueRva),
            static_cast<unsigned long long>(branchIsland));
        log(buffer);
    }
    for (std::size_t i = 0; i < installed; ++i) {
        char buffer[160]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Native enqueue origin callsite: at=Starfield+0x%llX target=Starfield+0x%llX",
            static_cast<unsigned long long>(callSites[i].instructionAddress - moduleBase),
            static_cast<unsigned long long>(kNativeEnqueueRva));
        log(buffer);
    }

    if (const auto probe = buildNativeCodeProbe(target + 1, moduleBase, moduleSize)) {
        for (const auto& chunk : buildNativeFunctionChunks(*probe)) {
            std::ostringstream out;
            out << "Native enqueue target dump: function=Starfield+0x" << std::hex
                << (chunk.functionStart - moduleBase) << "..0x" << (chunk.functionEnd - moduleBase)
                << std::dec << " chunk=" << (chunk.chunkIndex + 1) << '/' << chunk.chunkCount
                << std::hex << " start=Starfield+0x" << (chunk.chunkStart - moduleBase)
                << " bytes" << std::dec << chunk.byteCount << '=' << std::hex << std::setfill('0');
            for (std::size_t i = 0; i < chunk.byteCount; ++i) {
                if (i) out << ' ';
                out << std::setw(2) << static_cast<unsigned int>(chunk.bytes[i]);
            }
            log(out.str());
        }
    }
    return true;
}

bool sds::GameStateAdapter::installButtonEventConstructorOriginHook()
{
    if (g_buttonEventConstructorHookInstalled.load(std::memory_order_acquire)) {
        log("ButtonEvent constructor origin: hook already installed");
        return true;
    }

    const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
    if (!moduleBase) {
        log("ButtonEvent constructor origin: Starfield module base unavailable");
        return false;
    }

    const auto target = moduleBase + kButtonEventConstructorRva;
    std::array<std::uint8_t, kButtonEventConstructorDisplacedBytes> prologue{};
    std::memcpy(prologue.data(), reinterpret_cast<const void*>(target), prologue.size());
    if (!matchesButtonEventConstructorPrologue(prologue)) {
        std::ostringstream out;
        out << "ButtonEvent constructor origin: prologue mismatch at Starfield+0x"
            << std::hex << kButtonEventConstructorRva << "; got";
        for (const auto byte : prologue) {
            out << ' ' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
        }
        out << "; hook not installed";
        log(out.str());
        return false;
    }

    auto& trampoline = REL::GetTrampoline();
    if (trampoline.empty()) {
        trampoline.create(192, reinterpret_cast<void*>(target));
    }
    if (trampoline.free_size() < 64) {
        log("ButtonEvent constructor origin: CommonLib trampoline has insufficient free space");
        return false;
    }

    constexpr auto originalStubSize = kButtonEventConstructorDisplacedBytes + sizeof(REL::ASM::JMP14);
    auto* originalStub = static_cast<std::uint8_t*>(trampoline.allocate(originalStubSize));
    std::memcpy(originalStub, prologue.data(), prologue.size());
    const REL::ASM::JMP14 jumpBack(target + kButtonEventConstructorDisplacedBytes);
    std::memcpy(originalStub + kButtonEventConstructorDisplacedBytes, &jumpBack, sizeof(jumpBack));
    ::FlushInstructionCache(::GetCurrentProcess(), originalStub, originalStubSize);

    const auto thunkAddress = reinterpret_cast<std::uintptr_t>(&GameStateAdapter::buttonEventConstructorOriginThunk);
    const auto branchIsland = trampoline.allocate_branch5(thunkAddress);
    const auto entryPatch = buildButtonEventConstructorEntryPatch(target, branchIsland);
    if (!entryPatch) {
        log("ButtonEvent constructor origin: CommonLib branch island is outside rel32 range; hook not installed");
        return false;
    }

    g_originalButtonEventConstructor.store(
        reinterpret_cast<ButtonEventConstructorFunction>(originalStub),
        std::memory_order_release);

    if (!REL::WriteSafe(target, entryPatch->data(), entryPatch->size())) {
        g_originalButtonEventConstructor.store(nullptr, std::memory_order_release);
        log("ButtonEvent constructor origin: failed to patch constructor entry; hook not installed");
        return false;
    }

    g_buttonEventConstructorHookInstalled.store(true, std::memory_order_release);

    char buffer[256]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "ButtonEvent constructor origin: installed at Starfield+0x%llX target=0x%llX trampoline=0x%llX; rolling provenance capture enabled; no constructor calls or queue writes",
        static_cast<unsigned long long>(kButtonEventConstructorRva),
        static_cast<unsigned long long>(target),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(originalStub)));
    log(buffer);
    return true;
}

__declspec(noinline) std::uintptr_t sds::GameStateAdapter::nativeEnqueueOriginThunk(
    void* manager, void* event, std::uintptr_t arg3, std::uintptr_t arg4)
{
    const auto callerReturnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
    const auto moduleSize = g_starfieldModuleSize.load(std::memory_order_acquire);

    bool isButton = false;
    std::uint32_t preTimeCode = static_cast<std::uint32_t>(-1);
    if (event && moduleBase) {
        std::uintptr_t vtable = 0;
        std::uint32_t eventType = static_cast<std::uint32_t>(-1);
        isButton = safeReadValue(reinterpret_cast<std::uintptr_t>(event), vtable) &&
            vtable == moduleBase + kButtonEventPrimaryVtableRva &&
            safeReadValue(reinterpret_cast<std::uintptr_t>(event) + 0x10, eventType) &&
            eventType == static_cast<std::uint32_t>(RE::InputEvent::EventType::kButton);
        if (isButton) {
            (void)safeReadValue(reinterpret_cast<std::uintptr_t>(event) + 0x20, preTimeCode);
        }
    }

    std::array<std::uintptr_t, kSemanticCallerTraceMaxFrames> stackFrames{};
    std::size_t stackFrameCount = 0;
    if (isButton) {
        std::array<void*, kSemanticCallerTraceMaxFrames> frames{};
        const auto captured = ::CaptureStackBackTrace(
            0,
            static_cast<DWORD>(frames.size()),
            frames.data(),
            nullptr);
        stackFrameCount = static_cast<std::size_t>(captured);
        for (std::size_t i = 0; i < stackFrameCount; ++i) {
            stackFrames[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
        }

        ::AcquireSRWLockExclusive(&g_nativeEnqueueHistoryLock);
        g_nativeEnqueueHistory.record(
            reinterpret_cast<std::uintptr_t>(event),
            preTimeCode,
            callerReturnAddress,
            moduleBase,
            moduleSize,
            stackFrames,
            stackFrameCount);
        ::ReleaseSRWLockExclusive(&g_nativeEnqueueHistoryLock);
    }

    std::uintptr_t result = 0;
    if (const auto original = g_originalNativeEnqueue.load(std::memory_order_acquire)) {
        result = original(manager, event, arg3, arg4);
    }

    if (isButton && event) {
        std::uint32_t postTimeCode = preTimeCode;
        if (safeReadValue(reinterpret_cast<std::uintptr_t>(event) + 0x20, postTimeCode) &&
            postTimeCode != preTimeCode) {
            ::AcquireSRWLockExclusive(&g_nativeEnqueueHistoryLock);
            g_nativeEnqueueHistory.record(
                reinterpret_cast<std::uintptr_t>(event),
                postTimeCode,
                callerReturnAddress,
                moduleBase,
                moduleSize,
                stackFrames,
                stackFrameCount);
            ::ReleaseSRWLockExclusive(&g_nativeEnqueueHistoryLock);
        }
    }
    return result;
}

__declspec(noinline) void* sds::GameStateAdapter::buttonEventConstructorOriginThunk(void* eventObject)
{
    const auto callerReturnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
    const auto moduleSize = g_starfieldModuleSize.load(std::memory_order_acquire);

    std::array<void*, kSemanticCallerTraceMaxFrames> capturedFrames{};
    const auto captured = ::CaptureStackBackTrace(
        0,
        static_cast<DWORD>(capturedFrames.size()),
        capturedFrames.data(),
        nullptr);
    std::array<std::uintptr_t, kSemanticCallerTraceMaxFrames> stackFrames{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(captured); ++i) {
        stackFrames[i] = reinterpret_cast<std::uintptr_t>(capturedFrames[i]);
    }

    ::AcquireSRWLockExclusive(&g_buttonConstructorHistoryLock);
    g_buttonConstructorHistory.record(
        reinterpret_cast<std::uintptr_t>(eventObject),
        callerReturnAddress,
        moduleBase,
        moduleSize,
        stackFrames,
        static_cast<std::size_t>(captured));
    ::ReleaseSRWLockExclusive(&g_buttonConstructorHistoryLock);

    if (const auto original = g_originalButtonEventConstructor.load(std::memory_order_acquire)) {
        return original(eventObject);
    }
    return eventObject;
}

void sds::setInputPresentationObserver(
    InputPresentationObserver observer) noexcept
{
    g_inputPresentationObserver.store(
        observer,
        std::memory_order_release);
}

__declspec(noinline) void sds::GameStateAdapter::semanticBroadcasterDiagnosticThunk(void* source, void* event)
{
    const auto callerReturnAddress =
        reinterpret_cast<std::uintptr_t>(_ReturnAddress());

    if (auto* observer =
            g_semanticDiagnosticObserver.load(std::memory_order_acquire)) {
        observer->observeSemanticButton(
            source,
            event,
            callerReturnAddress);
    }

    if (const auto original =
            g_originalSemanticBroadcaster.load(std::memory_order_acquire)) {
        original(source, event);
    }
}
namespace
{
    // Bluetooth physical-event bridge state must be visible to the
    // semantic observer, which appears earlier in this translation unit
    // than the rest of the Bluetooth bridge implementation.
    std::atomic_bool g_bluetoothPhysicalBridgeActive{ false };
}
void sds::GameStateAdapter::observeSemanticButton(
    void* source,
    void* event,
    std::uintptr_t callerReturnAddress) const noexcept
{
    if (!event) {
        return;
    }

    try {
        // Only validated native input events may change keyboard/mouse
        // presentation. The semantic broadcaster also sees objects that are
        // not native InputEvents; treating +0x08 alone as DeviceType can make
        // unrelated data look like keyboard/mouse activity and immediately
        // steal glyph/cursor presentation from an active Bluetooth DualSense.
        const auto presentationEventAddress =
            reinterpret_cast<std::uintptr_t>(event);

        const auto presentationModuleBase =
            g_starfieldModuleBase.load(
                std::memory_order_acquire);

        const auto presentationModuleSize =
            g_starfieldModuleSize.load(
                std::memory_order_acquire);

        std::uintptr_t presentationVtable = 0;

        std::uint32_t presentationDevice =
            static_cast<std::uint32_t>(-1);

        std::uint32_t presentationEventType =
            static_cast<std::uint32_t>(-1);

        const bool presentationHeaderReadable =
            safeReadValue(
                presentationEventAddress,
                presentationVtable) &&
            safeReadValue(
                presentationEventAddress + 0x08u,
                presentationDevice) &&
            safeReadValue(
                presentationEventAddress + 0x10u,
                presentationEventType);

        const bool presentationVtableNative =
            presentationModuleBase != 0 &&
            presentationModuleSize != 0 &&
            presentationVtable >= presentationModuleBase &&
            presentationVtable <
                presentationModuleBase +
                    presentationModuleSize;

        const bool presentationKeyboard =
            presentationDevice ==
                static_cast<std::uint32_t>(
                    RE::InputEvent::DeviceType::kKeyboard);

        const bool presentationMouse =
            presentationDevice ==
                static_cast<std::uint32_t>(
                    RE::InputEvent::DeviceType::kMouse);

        // Keyboard and mouse buttons use Starfield's known native
        // ButtonEvent layout/vtable.
        const bool presentationButton =
            presentationModuleBase != 0 &&
            presentationEventType ==
                static_cast<std::uint32_t>(
                    RE::InputEvent::EventType::kButton) &&
            presentationVtable ==
                presentationModuleBase +
                    kButtonEventPrimaryVtableRva;

        // Mouse movement is a separate native InputEvent type. It does not
        // use ButtonEvent's vtable, so require a real Starfield-module
        // vtable plus the exact MouseMove event type.
        const bool presentationMouseMove =
            presentationMouse &&
            presentationVtableNative &&
            presentationEventType ==
                static_cast<std::uint32_t>(
                    RE::InputEvent::EventType::kMouseMove);

        const bool presentationKeyboardMouseInput =
            presentationHeaderReadable &&
            presentationVtableNative &&
            (
                (
                    presentationButton &&
                    (presentationKeyboard ||
                     presentationMouse)
                ) ||
                presentationMouseMove
            );

        if (presentationKeyboardMouseInput) {
            notifyInputPresentationDevice(
                InputPresentationDevice::KeyboardMouse);
        }

        // Bluetooth bridge correction:
        //
        // Without a native Starfield gamepad delegate, Starfield currently
        // resolves physical stick id 12 as "Cursor". The actual native
        // Native DualSense mapping for id 12 is "Look".
        //
        // Restrict this correction to SAD's Bluetooth physical bridge so
        // normal USB/native controller processing is never modified.
        if (g_bluetoothPhysicalBridgeActive.load(
                std::memory_order_acquire)) {

            const auto btCorrectionAddress =
                reinterpret_cast<std::uintptr_t>(event);

            std::uint32_t btCorrectionDevice =
                static_cast<std::uint32_t>(-1);

            std::uint32_t btCorrectionType =
                static_cast<std::uint32_t>(-1);

            if (safeReadValue(
                    btCorrectionAddress + 0x08,
                    btCorrectionDevice) &&
                safeReadValue(
                    btCorrectionAddress + 0x10,
                    btCorrectionType) &&
                btCorrectionDevice ==
                    static_cast<std::uint32_t>(
                        RE::InputEvent::DeviceType::kGamepad) &&
                btCorrectionType ==
                    static_cast<std::uint32_t>(
                        RE::InputEvent::EventType::kThumbstick)) {

                std::int32_t btCorrectionId = -1;

                std::memcpy(
                    &btCorrectionId,
                    reinterpret_cast<const std::uint8_t*>(event) + 0x30,
                    sizeof(btCorrectionId));

                if (btCorrectionId == 11 &&
                    _monocleOpen &&
                    _landVehicleCorrelationArmed.load(
                        std::memory_order_acquire)) {

                    auto* btScannerMoveUserEvent =
                        reinterpret_cast<RE::BSFixedString*>(
                            reinterpret_cast<std::uint8_t*>(event) +
                            0x28);

                    const char* btScannerMoveName =
                        btScannerMoveUserEvent->c_str();

                    if (btScannerMoveName &&
                        std::string_view(btScannerMoveName) == "Move") {

                        auto* btScannerMoveInputEvent =
                            reinterpret_cast<RE::InputEvent*>(event);

                        btScannerMoveInputEvent->status =
                            RE::InputEvent::Status::kContinue;



                        static std::atomic_bool
                            scannerMoveStatusLogged{ false };

                        if (!scannerMoveStatusLogged.exchange(
                                true,
                                std::memory_order_acq_rel)) {

                            log(
                                "Bluetooth REV-8 scanner Move status parity: ACTIVE status=Continue");
                        }
                    }
                }
                else if (btCorrectionId == 12) {
                    auto* btCorrectionUserEvent =
                        reinterpret_cast<RE::BSFixedString*>(
                            reinterpret_cast<std::uint8_t*>(event) +
                            0x28);

                    const char* currentUserEvent =
                        btCorrectionUserEvent->c_str();

                    if (currentUserEvent &&
                        std::string_view(currentUserEvent) == "Cursor") {

                        *btCorrectionUserEvent =
                            RE::BSFixedString("Look");

                        auto* btCorrectionInputEvent =
                            reinterpret_cast<RE::InputEvent*>(event);

                        btCorrectionInputEvent->status =
                            RE::InputEvent::Status::kContinue;

                        static std::atomic_bool correctionLogged{
                            false
                        };

                        if (!correctionLogged.exchange(
                                true,
                                std::memory_order_acq_rel)) {
                            log(
                                "Bluetooth right-stick correction: "
                                "Cursor -> Look idCode=12 status=Continue");
                        }
                    }
                }
            }
        }

        const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
        if (!moduleBase) {
            return;
        }

        const auto eventAddress = reinterpret_cast<std::uintptr_t>(event);

        const auto primaryVtable = *reinterpret_cast<const std::uintptr_t*>(event);
        const auto expectedPrimaryVtable = moduleBase + kButtonEventPrimaryVtableRva;
        if (primaryVtable != expectedPrimaryVtable) {
            return;
        }

        const auto* button = reinterpret_cast<const RE::ButtonEvent*>(event);
        if (button->eventType != RE::InputEvent::EventType::kButton) {
            return;
        }

        const char* userEvent = button->strUserEvent.c_str();
        if (!userEvent) {
            return;
        }

        const bool bluetoothScannerSemanticDiagnostic =
            _monocleOpen &&
            button->deviceType ==
                RE::InputEvent::DeviceType::kGamepad;

        // Bluetooth X/Y action-stage diagnostic. USB events use the same
        // observer so the log can compare action mapping by transport.
        // This observes Starfield's resolved physical ButtonEvent AFTER the
        // native helper; it does not dispatch, synthesize, or change inputs.
        if (button->deviceType == RE::InputEvent::DeviceType::kGamepad &&
            (button->idCode == 0x8000 || button->idCode == 0x1000)) {
            const bool edge = button->value <= 0.0F || button->heldDownSecs <= 0.045F;
            static std::atomic<std::uint32_t> xyMappedLogCount{ 0u };
            if (edge && xyMappedLogCount.fetch_add(1u, std::memory_order_relaxed) < 160u) {
                char mapped[440]{};
                std::snprintf(
                    mapped, sizeof(mapped),
                    "BT XY RESOLVED: button=%s action='%s' edge=%s value=%.3f held=%.3f disabled=%s status=%u deviceId=%u transportBridge=%s timeCode=%u",
                    button->idCode == 0x8000 ? "X/Square" : "Y/Triangle",
                    userEvent,
                    button->value > 0.0F ? "DOWN" : "UP",
                    static_cast<double>(button->value),
                    static_cast<double>(button->heldDownSecs),
                    button->disabled ? "yes" : "no",
                    static_cast<unsigned>(button->status),
                    static_cast<unsigned>(button->deviceID),
                    g_bluetoothPhysicalBridgeActive.load(std::memory_order_acquire) ? "Bluetooth" : "USB-or-inactive",
                    static_cast<unsigned>(button->timeCode));
                log(mapped);
            }
        }

        const bool mappedNativeInput = isMappedNativeInputUserEvent(userEvent);
        const bool vehicleReconInputActive = _landVehicleCorrelationArmed.load(std::memory_order_acquire);
        const bool boostpackInputActive =
            g_boostpackSemanticObservationArmed.load(std::memory_order_acquire);
        if (!mappedNativeInput && !vehicleReconInputActive && !boostpackInputActive &&
            !bluetoothScannerSemanticDiagnostic) {
            return;
        }

        const bool vehicleReconInputBoundary = button->value <= 0.0F || button->heldDownSecs <= 0.05F;
        if (vehicleReconInputActive && vehicleReconInputBoundary) {
            char vehicleInput[416]{};
            std::snprintf(
                vehicleInput,
                sizeof(vehicleInput),
                "Land vehicle recon: input semantic='%s' edge=%s value=%.4f held=%.4f device=%u boundary=initial-or-release role=annotation-only authority=no controllerOutput=none",
                userEvent,
                button->value > 0.0F ? "active" : "release",
                static_cast<double>(button->value),
                static_cast<double>(button->heldDownSecs),
                static_cast<unsigned>(button->deviceType));
            log(vehicleInput);
        }

        const bool landVehicleProductionSemantic =
            std::strcmp(userEvent, "VehicleFireWeapon") == 0 ||
            std::strcmp(userEvent, "VehicleAim") == 0 ||
            std::strcmp(userEvent, "VehicleBoost") == 0 ||
            std::strcmp(userEvent, "VehicleVertBoost") == 0;

        if (vehicleReconInputActive &&
            _landVehicleSemanticCallback &&
            landVehicleProductionSemantic) {

            _landVehicleSemanticCallback(
                userEvent,
                button->value > 0.0F,
                std::chrono::steady_clock::now());
        }

        if (boostpackInputActive && vehicleReconInputBoundary &&
            g_boostpackSemanticObserver) {
            g_boostpackSemanticObserver(
                userEvent,
                button->value > 0.0F,
                std::chrono::steady_clock::now());
        }

        if (!mappedNativeInput) {
            return;
        }

        SemanticButtonDiagnostic diagnostic{};
        diagnostic.edge = button->value > 0.0F ? InputButtonEdge::Press : InputButtonEdge::Release;
        diagnostic.sourceAddress = reinterpret_cast<std::uintptr_t>(source);
        diagnostic.sourceVtable = source ? *reinterpret_cast<const std::uintptr_t*>(source) : 0;
        diagnostic.sourceVtableExpected = diagnostic.sourceVtable == moduleBase + kButtonEventSourceVtableRva;
        diagnostic.eventAddress = eventAddress;
        diagnostic.primaryVtable = primaryVtable;
        diagnostic.primaryVtableExpected = true;
        diagnostic.deviceType = static_cast<std::uint32_t>(button->deviceType);
        diagnostic.deviceId = button->deviceID;
        diagnostic.eventType = static_cast<std::uint32_t>(button->eventType);
        diagnostic.status = static_cast<std::uint32_t>(button->status);
        diagnostic.timeCode = button->timeCode;
        diagnostic.idCode = button->idCode;
        diagnostic.userEvent = userEvent;
        diagnostic.disabled = button->disabled;
        diagnostic.value = button->value;
        diagnostic.heldDownSecs = button->heldDownSecs;
        diagnostic.unk50 = reinterpret_cast<std::uintptr_t>(button->unk50);
        diagnostic.debounceManager = reinterpret_cast<std::uintptr_t>(button->debounceManager);

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(event);
        std::memcpy(diagnostic.rawBytes.data(), bytes, diagnostic.rawBytes.size());
        std::memcpy(&diagnostic.idVtable, bytes + 0x38, sizeof(diagnostic.idVtable));
        std::memcpy(&diagnostic.userVtable, bytes + 0x40, sizeof(diagnostic.userVtable));

        log(formatSemanticButtonDiagnostic(diagnostic));

        SemanticCallerTraceDiagnostic callerTrace{};
        callerTrace.edge = diagnostic.edge;
        callerTrace.returnAddress = callerReturnAddress;
        callerTrace.moduleBase = moduleBase;
        callerTrace.moduleSize = g_starfieldModuleSize.load(std::memory_order_acquire);

        std::array<void*, kSemanticCallerTraceMaxFrames> frames{};
        const auto captured = ::CaptureStackBackTrace(
            0,
            static_cast<DWORD>(frames.size()),
            frames.data(),
            nullptr);
        callerTrace.stackFrameCount = static_cast<std::size_t>(captured);
        for (std::size_t i = 0; i < callerTrace.stackFrameCount; ++i) {
            callerTrace.stackFrames[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
        }
        log(formatSemanticCallerTrace(callerTrace));

        char semanticBuffer[224]{};
        std::snprintf(
            semanticBuffer,
            sizeof(semanticBuffer),
            "Native semantic: broadcaster observed queued action='%s' edge=%s",
            userEvent,
            diagnostic.edge == InputButtonEdge::Press ? "press" : "release");
        log(semanticBuffer);
    } catch (...) {
        // A diagnostic observer must never change or block Starfield's native input path.
    }
}


bool sds::GameStateAdapter::queueNativeInputAction(InputAction action) noexcept
{
    if (action == InputAction::OpenPhotoMode) {
        if (_pendingPhotoMode) {
            log("Native input: Photo Mode staging already pending; duplicate Create ignored");
            return false;
        }
        if (_nativeInputSequencer.active()) {
            log("Native input: semantic pulse already active; Create ignored");
            return false;
        }

        if (_monocleOpen) {
            if (!_nativeInputSequencer.enqueueUserEvent("PhotoMode")) {
                log("Native input: failed to queue verified PhotoMode semantic");
                return false;
            }
            log("Native input: queued action='PhotoMode' 5-frame gamepad pulse (MonocleMenu already open)");
            return true;
        }

        const auto* definition = nativeInputDefinitionForAction(action);
        if (!definition || !_nativeInputSequencer.enqueue(action)) {
            log("Native input: no verified Monocle definition for Photo Mode");
            return false;
        }

        _pendingPhotoMode = true;
        _photoModeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        log("Native input: Photo Mode staged through action='Monocle'; waiting for MonocleMenu");
        return true;
    }

    const auto* definition = nativeInputDefinitionForAction(action);
    if (!definition) {
        log("Native input: no verified semantic definition for requested action");
        return false;
    }
    if (!_nativeInputSequencer.enqueue(action)) {
        log("Native input: semantic pulse already active; duplicate action ignored");
        return false;
    }

    char buffer[224]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Native input: queued action='%.*s' %zu-frame gamepad pulse",
        static_cast<int>(definition->userEvent.size()),
        definition->userEvent.data(),
        definition->pulse.size());
    log(buffer);
    return true;
}

namespace
{
    struct BluetoothPhysicalButtonRuntime
    {
        bool down{ false };
        float heldSeconds{ 0.0F };
    };

    struct BluetoothPhysicalBridgeRuntime
    {
        std::array<BluetoothPhysicalButtonRuntime, 16> buttons{};
        bool touchpadClickHeld{ false };
        bool touchpadLeftClick{ false };

        float leftX{ 0.0F };
        float leftY{ 0.0F };
        float rightX{ 0.0F };
        float rightY{ 0.0F };

        std::uint8_t leftDirection{ 0 };
        std::uint8_t rightDirection{ 0 };
    };

    BluetoothPhysicalBridgeRuntime g_bluetoothPhysicalBridgeRuntime{};

    struct BluetoothStickValue
    {
        float x{ 0.0F };
        float y{ 0.0F };
    };

    [[nodiscard]] float bluetoothRawAxis(
        std::uint8_t raw,
        bool invert) noexcept
    {
        float value =
            (static_cast<float>(raw) / 255.0F) * 2.0F - 1.0F;

        if (invert) {
            value = -value;
        }

        if (value > 1.0F) {
            value = 1.0F;
        } else if (value < -1.0F) {
            value = -1.0F;
        }

        return value;
    }

    [[nodiscard]] BluetoothStickValue bluetoothStickValue(
        std::uint8_t rawX,
        std::uint8_t rawY) noexcept
    {
        float x = bluetoothRawAxis(rawX, false);
        float y = bluetoothRawAxis(rawY, true);



        const float magnitude = std::sqrt(x * x + y * y);

        // Conservative radial deadzone. The native DualSense path performs its
        // own radial shaping before constructing ThumbstickEvent; this keeps
        // Bluetooth centered while retaining the same -1..+1 convention.
        constexpr float kDeadzone = 0.10F;

        if (magnitude <= kDeadzone) {
            return {};
        }

        const float clampedMagnitude =
            magnitude > 1.0F ? 1.0F : magnitude;

        const float scaledMagnitude =
            (clampedMagnitude - kDeadzone) /
            (1.0F - kDeadzone);

        const float scale =
            magnitude > 0.0F ?
                scaledMagnitude / magnitude :
                0.0F;

        return {
            .x = x * scale,
            .y = y * scale,
        };
    }

    [[nodiscard]] std::uint8_t bluetoothStickDirection(
        float x,
        float y) noexcept
    {
        constexpr float kDirectionEpsilon = 0.0001F;

        if (std::fabs(x) <= kDirectionEpsilon &&
            std::fabs(y) <= kDirectionEpsilon) {
            return 0;
        }

        if (std::fabs(x) >= std::fabs(y)) {
            return x >= 0.0F ? 2 : 4;  // right / left
        }

        return y >= 0.0F ? 1 : 3;      // up / down
    }

    [[nodiscard]] bool bluetoothDpadUp(std::uint8_t hat) noexcept
    {
        return hat == 0 || hat == 1 || hat == 7;
    }

    [[nodiscard]] bool bluetoothDpadRight(std::uint8_t hat) noexcept
    {
        return hat == 1 || hat == 2 || hat == 3;
    }

    [[nodiscard]] bool bluetoothDpadDown(std::uint8_t hat) noexcept
    {
        return hat == 3 || hat == 4 || hat == 5;
    }

    [[nodiscard]] bool bluetoothDpadLeft(std::uint8_t hat) noexcept
    {
        return hat == 5 || hat == 6 || hat == 7;
    }
}

void sds::GameStateAdapter::dispatchBluetoothPhysicalInput(
    const TouchState& state,
    float deltaSeconds,
    void* gamepadDevice) noexcept
{
    try {
        // Starfield native USB DualSense InputEvents consistently use
        // deviceId 0x0D01 (3329). Preserve that physical controller
        // identity for SAD-owned Bluetooth events as well.
        constexpr std::uint32_t kBluetoothNativeDualSenseDeviceId = 0u;

        g_bluetoothPhysicalBridgeActive.store(
            true,
            std::memory_order_release);
        if (deltaSeconds < 0.0F) {
            deltaSeconds = 0.0F;
        } else if (deltaSeconds > 0.100F) {
            deltaSeconds = 0.100F;
        }

        const auto moduleBase =
            g_starfieldModuleBase.load(std::memory_order_acquire);

        if (!moduleBase) {
            return;
        }

        std::uintptr_t manager = 0;
        if (!safeReadValue(
                moduleBase + kInputQueueSingletonRva,
                manager) ||
            manager < 0x10000u) {
            return;
        }

        static std::atomic_bool bluetoothDeviceIdLogged{ false };

        if (!bluetoothDeviceIdLogged.exchange(
                true,
                std::memory_order_acq_rel)) {
            log(
                "Bluetooth gameplay input identity: deviceType=2 deviceId=3329/0x0D01 source=native-DualSense-USB-parity");
        }

        auto emitButton =
            [this, deltaSeconds, moduleBase, manager, gamepadDevice](
                std::size_t stateIndex,
                std::int32_t idCode,
                float currentValue) noexcept -> bool
        {
            auto& runtime =
                g_bluetoothPhysicalBridgeRuntime.buttons[stateIndex];

            if (currentValue < 0.0F) {
                currentValue = 0.0F;
            } else if (currentValue > 1.0F) {
                currentValue = 1.0F;
            }

            constexpr float kActiveThreshold = 0.0001F;

            const bool currentDown =
                currentValue > kActiveThreshold;
            const bool previousDown =
                runtime.down;

            if (!currentDown && !previousDown) {
                runtime.heldSeconds = 0.0F;
                return true;
            }

            const bool useNativeShadowButtonTransition =
                gamepadDevice != nullptr &&
                (idCode == 0x0002 ||
                 idCode == 0x2000 ||
                 idCode == 0x8000 || // X / Square: draw/holster
                 idCode == 0x1000);  // Y / Triangle: jump

            if (useNativeShadowButtonTransition) {

                constexpr std::uintptr_t
                    kNativeGamepadButtonTransitionRva =
                        0x22FC2A0u;

                using NativeGamepadButtonTransition =
                    void (*)(
                        void*,
                        std::int32_t,
                        float,
                        float,
                        float);

                const auto nativeTransition =
                    reinterpret_cast<
                        NativeGamepadButtonTransition>(
                            moduleBase +
                            kNativeGamepadButtonTransitionRva);

                const float previousValue =
                    previousDown ?
                        1.0F :
                        0.0F;

                nativeTransition(
                    gamepadDevice,
                    idCode,
                    deltaSeconds,
                    previousValue,
                    currentValue);

                runtime.down =
                    currentDown;

                // X/Y previously reached the native queue but could be
                // ignored in context. Trial: match the L3/Circle native
                // transition path while retaining current HID ownership.
                if ((idCode == 0x8000 || idCode == 0x1000) &&
                    currentDown != previousDown) {
                    notifyInputPresentationDevice(
                        InputPresentationDevice::Gamepad);
                    log(idCode == 0x8000
                        ? (currentDown
                            ? "BT XY NATIVE: X/Square DOWN via Starfield transition helper"
                            : "BT XY NATIVE: X/Square UP via Starfield transition helper")
                        : (currentDown
                            ? "BT XY NATIVE: Y/Triangle DOWN via Starfield transition helper"
                            : "BT XY NATIVE: Y/Triangle UP via Starfield transition helper"));
                }

                // Starfield's native transition helper now owns the
                // authoritative held-time bookkeeping for this button.
                runtime.heldSeconds = 0.0F;

                static std::atomic_bool
                    nativeButtonTransitionLogged{
                        false };

                if (!nativeButtonTransitionLogged.exchange(
                        true,
                        std::memory_order_acq_rel)) {

                    log(
                        "Bluetooth native button transition helper: "
                        "ACTIVE rva=Starfield+0x22FC2A0 "
                        "buttons=L3,Circle,X/Square,Y/Triangle "
                        "gamepad=SAD-shadow "
                        "hardwarePolling=no");
                }

                return true;
            }

            const float previousHeld =
                runtime.heldSeconds;

            float currentHeld = previousHeld;

            if (currentDown) {
                if (previousDown) {
                    currentHeld += deltaSeconds;
                } else {
                    currentHeld = 0.0F;
                }
            }

            NativeInputEmission emission{};
            emission.edge =
                currentDown ?
                    NativeInputEdge::Press :
                    NativeInputEdge::Release;
            emission.deviceType = 2;
            emission.deviceId =
                kBluetoothNativeDualSenseDeviceId;
            emission.eventType = 0;
            emission.status = 0;
            emission.idCode = idCode;

            // Empty semantic name is intentional: this is a physical gamepad
            // event. Starfield performs the mapping/context resolution after
            // native enqueue, exactly as it does for its own gamepad producer.
            emission.userEvent = "";
            emission.disabled = false;
            emission.value = currentValue;
            emission.heldDownSecs =
                currentDown ? currentHeld : previousHeld;
            emission.previousHeldDownSecs = previousHeld;
            emission.stepIndex = 0;

            notifyInputPresentationDevice(
                InputPresentationDevice::Gamepad);

            // Diagnostic only: correlate each observed X/Y transition
            // with the actual native queue submission. Logs only edges,
            // never the steady held frames or other controller buttons.
            const bool xyTransition =
                (idCode == 0x8000 || idCode == 0x1000) &&
                currentDown != previousDown;
            const bool published = dispatchNativeInputFrame(emission);
            if (xyTransition) {
                char diagnostic[256]{};
                std::snprintf(
                    diagnostic,
                    sizeof(diagnostic),
                    "BT XY BRIDGE: button=%s edge=%s publish=%s held=%.3f shadow=%s stage=runtime-tick",
                    idCode == 0x8000 ? "X/Square" : "Y/Triangle",
                    currentDown ? "DOWN" : "UP",
                    published ? "OK" : "FAILED",
                    static_cast<double>(emission.heldDownSecs),
                    gamepadDevice ? "yes" : "no");
                log(diagnostic);
            }
            if (!published) {
                return false;
            }

            runtime.down = currentDown;
            runtime.heldSeconds =
                currentDown ? currentHeld : 0.0F;

            return true;
        };

        // Starfield native DualSense/ScePad digital ButtonEvent ids.
        //
        // Reconstructed directly from BSPCDualSenseGamepadDevice::slot2:
        // its 13-entry table is tested against current/previous ScePad
        // state and the same value is passed through Starfield+0x22FC220
        // / +0x22FC2A0 into Starfield+0x22DA4A0, which stores R9D
        // directly at ButtonEvent+0x30 (idCode).
        //
        // Create is intentionally absent: Starfield's native DualSense
        // digital loop has 13 entries and SAD already handles Create
        // independently through CreatePressed -> Photo Mode.
        (void)emitButton(
            0,
            0x0010,
            bluetoothDpadUp(state.dpad) ? 1.0F : 0.0F);

        (void)emitButton(
            1,
            0x0040,
            bluetoothDpadDown(state.dpad) ? 1.0F : 0.0F);

        (void)emitButton(
            2,
            0x0080,
            bluetoothDpadLeft(state.dpad) ? 1.0F : 0.0F);

        (void)emitButton(
            3,
            0x0020,
            bluetoothDpadRight(state.dpad) ? 1.0F : 0.0F);

        (void)emitButton(
            4,
            0x0008,
            state.options ? 1.0F : 0.0F);

        (void)emitButton(
            5,
            0x0002,
            state.l3 ? 1.0F : 0.0F);

        (void)emitButton(
            6,
            0x0004,
            state.r3 ? 1.0F : 0.0F);

        (void)emitButton(
            7,
            0x0400,
            state.l1 ? 1.0F : 0.0F);

        (void)emitButton(
            8,
            0x0800,
            state.r1 ? 1.0F : 0.0F);

        (void)emitButton(
            9,
            0x4000,
            state.cross ? 1.0F : 0.0F);

        (void)emitButton(
            10,
            0x2000,
            state.circle ? 1.0F : 0.0F);

        (void)emitButton(
            11,
            0x8000,
            state.square ? 1.0F : 0.0F);

        (void)emitButton(
            12,
            0x1000,
            state.triangle ? 1.0F : 0.0F);

        // Reproduce the physical left touchpad click over Bluetooth.
        // Starfield determines its action from the current UI context.
        // Button slot 13 is unused by the existing physical bridge.
        //
        // Latch the touched half when the click begins. Moving the
        // finger while holding the button must not change its action.
        if (state.click &&
            !g_bluetoothPhysicalBridgeRuntime.touchpadClickHeld) {

            const TouchPoint* finger =
                state.first.down ? &state.first :
                (state.second.down ? &state.second : nullptr);

            g_bluetoothPhysicalBridgeRuntime.touchpadClickHeld = true;
            g_bluetoothPhysicalBridgeRuntime.touchpadLeftClick =
                !finger || finger->x < 960;

        } else if (!state.click) {
            g_bluetoothPhysicalBridgeRuntime.touchpadClickHeld = false;
            g_bluetoothPhysicalBridgeRuntime.touchpadLeftClick = false;
        }

        const bool nativeLeftClick =
            g_bluetoothPhysicalBridgeRuntime.touchpadClickHeld &&
            g_bluetoothPhysicalBridgeRuntime.touchpadLeftClick;

        (void)emitButton(
            13,
            0x00200000,
            nativeLeftClick ? 1.0F : 0.0F);

        const float leftTrigger =
            state.l2 <= 2 ?
                0.0F :
                static_cast<float>(state.l2) / 255.0F;

        const float rightTrigger =
            state.r2 <= 2 ?
                0.0F :
                static_cast<float>(state.r2) / 255.0F;

        // Native physical trigger ids proven by the Starfield gamepad path.
        (void)emitButton(14, 9, leftTrigger);

        // Scanner and land-vehicle contexts are sensitive to when R2 enters
        // Starfield's gamepad pipeline. When the SAD Bluetooth shadow exists,
        // slot2 dispatch owns R2 for those contexts. Keep the runtime mirror
        // synchronized here so context exit cannot fabricate a stale edge.
        const bool nativePollR2Context =
            gamepadDevice != nullptr &&
            (_monocleOpen ||
             _landVehicleCorrelationArmed.load(
                 std::memory_order_acquire));

        if (nativePollR2Context) {
            constexpr float kTriggerActiveThreshold = 0.0001F;
            auto& runtime =
                g_bluetoothPhysicalBridgeRuntime.buttons[15];

            runtime.down =
                rightTrigger > kTriggerActiveThreshold;
            runtime.heldSeconds = 0.0F;
        } else {
            (void)emitButton(15, 10, rightTrigger);
        }

        using NativeThumbstickProducer = void (*)(
            void* manager,
            std::uint32_t deviceId,
            std::uint8_t idCode,
            float x,
            float y,
            std::uint8_t previousDirection,
            std::uint8_t currentDirection);

        constexpr std::uintptr_t kNativeThumbstickProducerRva =
            0x22DA530u;

        const auto produceThumbstick =
            reinterpret_cast<NativeThumbstickProducer>(
                moduleBase + kNativeThumbstickProducerRva);

        const auto left =
            bluetoothStickValue(
                state.leftX,
                state.leftY);

        const auto right =
            bluetoothStickValue(
                state.rightX,
                state.rightY);

        auto emitStick =
            [&](std::uint8_t idCode,
                float x,
                float y,
                float& previousX,
                float& previousY,
                std::uint8_t& previousDirection) noexcept
        {
            constexpr float kActiveThreshold = 0.0001F;

            const bool currentActive =
                std::fabs(x) > kActiveThreshold ||
                std::fabs(y) > kActiveThreshold;

            const bool previousActive =
                std::fabs(previousX) > kActiveThreshold ||
                std::fabs(previousY) > kActiveThreshold;

            if (!currentActive && !previousActive) {
                previousDirection = 0;
                previousX = 0.0F;
                previousY = 0.0F;
                return;
            }

            const auto currentDirection =
                bluetoothStickDirection(x, y);
            // REV-8 scanner native-poll timing test.
            //
            // MonocleMenu changes Starfield's input context. While
            // it is open in the REV-8, stick 11 and 12 are produced
            // from the native slot-2 gamepad poll instead of the
            // runtime-tick replay path.
            const bool vehicleNativePollRightStick =
                gamepadDevice != nullptr &&
                _landVehicleCorrelationArmed.load(
                    std::memory_order_acquire);

            const bool rev8ScannerNativePollSticks =
                vehicleNativePollRightStick &&
                _monocleOpen;

            // Galaxy-map cursor movement should be produced at Starfield's
            // native gamepad polling cadence, just like wired USB. The
            // runtime permanent task can execute substantially more often
            // than the rendered-frame/native gamepad poll and over-apply
            // held map-stick input (observed at both 30 and 60 FPS).
            // Keep the existing runtime path when no SAD shadow is present.
            bool galaxyMapNativePollLeftStick = false;
            if (idCode == 11 &&
                gamepadDevice != nullptr &&
                !rev8ScannerNativePollSticks) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    galaxyMapNativePollLeftStick =
                        ui->IsMenuOpen(
                            RE::BSFixedString("GalaxyStarMapMenu"));
                }
            }

            const bool suppressRuntimeStick =
                (idCode == 12 &&
                 vehicleNativePollRightStick) ||
                (idCode == 11 &&
                 (rev8ScannerNativePollSticks ||
                  galaxyMapNativePollLeftStick));

            if (suppressRuntimeStick) {
                // Keep runtime state synchronized so leaving vehicle/scanner
                // context cannot create a stale activation edge.
                previousX = x;
                previousY = y;
                previousDirection = currentDirection;
                return;
            }
            notifyInputPresentationDevice(
                InputPresentationDevice::Gamepad);

            // BLUETOOTH CONTEXT-SENSITIVE STICK CADENCE TEST V5
            // On-foot movement/look remains on unrestricted runtime replay.
            // The galaxy-map left stick uses native slot2 timing when the
            // shadow is present; this cadence path is the fallback without
            // a shadow. Other protected contexts retain their prior behavior.
            // Keep the REV-8 scanner overlay on the proven vehicle-safe
            // stick cadence. MonocleMenu can alter Starfield input/camera
            // context while REV-8 authority itself remains valid.
            bool cadenceLimitedContext = false;
            bool rev8ScannerContext = false;
            bool galaxyMapContext = false;
            bool cursorMenuContext = false;

            if (auto* ui = RE::UI::GetSingleton()) {
                rev8ScannerContext =
                    _landVehicleCorrelationArmed.load(
                        std::memory_order_acquire) &&
                    ui->IsMenuOpen(
                        RE::BSFixedString("MonocleMenu"));

                galaxyMapContext =
                    ui->IsMenuOpen(
                        RE::BSFixedString("GalaxyStarMapMenu"));

                cursorMenuContext =
                    ui->IsMenuOpen(
                        RE::BSFixedString("CursorMenu"));

                cadenceLimitedContext =
                    rev8ScannerContext ||
                    galaxyMapContext ||
                    cursorMenuContext;
            }

            // Ordinary land-vehicle driving must keep the unrestricted
            // runtime stick cadence. The blanket kVehicle 60 Hz cap was an
            // REV-8-era experiment and can pulse analog acceleration on newer
            // vehicle implementations. Scanner and map/cursor contexts remain
            // explicitly cadence-limited below.
            bool vehicleCameraContext = false;

            if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                vehicleCameraContext =
                    camera->QCameraEquals(
                        RE::CameraState::kVehicle);
            }

            if (vehicleCameraContext &&
                !rev8ScannerContext) {

                static std::atomic_bool
                    splitVehicleCadenceLogged{ false };

                if (!splitVehicleCadenceLogged.exchange(
                        true,
                        std::memory_order_acq_rel)) {

                    log(
                        "Bluetooth land-vehicle sticks: "
                        "LEFT=unrestricted RIGHT=60Hz "
                        "reason=acceleration-vs-camera-parity");
                }
            }

            if (rev8ScannerContext) {
                static std::atomic_bool scannerCadenceLogged{ false };

                if (!scannerCadenceLogged.exchange(
                        true,
                        std::memory_order_acq_rel)) {

                    log(
                        "Bluetooth REV-8 scanner stick cadence: ACTIVE mode=60Hz-pinned");
                }
            }

            static float leftStickCadenceSeconds = 0.0F;
            static float rightStickCadenceSeconds = 0.0F;

            float& stickCadenceSeconds =
                idCode == 12 ?
                    rightStickCadenceSeconds :
                    leftStickCadenceSeconds;

            const bool leftVehicleAcceleration =
                vehicleCameraContext &&
                    idCode == 11 &&
                    !cadenceLimitedContext;

            // Galaxy-map Bluetooth parity:
            // Native USB hardware measurement delivers both sticks at roughly
            // 158 Hz in GalaxyStarMapMenu. A 160 Hz Bluetooth target closely
            // matches native USB while protected 60 Hz contexts remain intact.
            const bool leftGalaxyMapParityCadence =
                galaxyMapContext &&
                    idCode == 11 &&
                    !rev8ScannerContext;

            const bool rightGalaxyMapParityCadence =
                galaxyMapContext &&
                    idCode == 12 &&
                    !rev8ScannerContext;

            const bool rightVehicleCamera =
                vehicleCameraContext &&
                    idCode == 12;

            const bool stickCadenceLimitedContext =
                cadenceLimitedContext ||
                rightVehicleCamera;

            if (!stickCadenceLimitedContext ||
                leftVehicleAcceleration) {

                stickCadenceSeconds = 0.0F;
            }
            else {
                if (deltaSeconds > 0.0F) {
                    stickCadenceSeconds +=
                        deltaSeconds;
                }

                if (stickCadenceSeconds > 0.100F) {
                    stickCadenceSeconds = 0.100F;
                }

                const bool activationEdge =
                    currentActive &&
                    !previousActive;

                const bool releaseEdge =
                    !currentActive &&
                    previousActive;

                const float kStickPublishIntervalSeconds =
                    leftGalaxyMapParityCadence ?
                        (1.0F / 160.0F) :
                    rightGalaxyMapParityCadence ?
                        (1.0F / 160.0F) :
                        (1.0F / 60.0F);

                if (activationEdge || releaseEdge) {
                    stickCadenceSeconds = 0.0F;
                }
                else {
                    if (stickCadenceSeconds <
                        kStickPublishIntervalSeconds) {
                        return;
                    }

                    // Preserve the fractional cadence remainder so the
                    // sustained event rate actually averages 60 Hz even when
                    // Starfield's runtime tick does not divide evenly into
                    // 60 Hz. Discarding the remainder undersamples the stick
                    // at common frame rates such as 90 Hz and 144 Hz.
                    stickCadenceSeconds = std::fmod(
                        stickCadenceSeconds,
                        kStickPublishIntervalSeconds);
                }
            }

            produceThumbstick(
                reinterpret_cast<void*>(manager),
                kBluetoothNativeDualSenseDeviceId,
                idCode,
                x,
                y,
                previousDirection,
                currentDirection);
            previousX = x;
            previousY = y;
            previousDirection = currentDirection;
        };

        emitStick(
            11,
            left.x,
            left.y,
            g_bluetoothPhysicalBridgeRuntime.leftX,
            g_bluetoothPhysicalBridgeRuntime.leftY,
            g_bluetoothPhysicalBridgeRuntime.leftDirection);

        emitStick(
            12,
            right.x,
            right.y,
            g_bluetoothPhysicalBridgeRuntime.rightX,
            g_bluetoothPhysicalBridgeRuntime.rightY,
            g_bluetoothPhysicalBridgeRuntime.rightDirection);
    } catch (...) {
        log(
            "Bluetooth gameplay input bridge: exception while dispatching physical state");
    }
}

void sds::GameStateAdapter::dispatchBluetoothScannerSticksAtNativePoll(
    const TouchState& state,
    float deltaSeconds,
    void* gamepadDevice) noexcept
{
    try {
        static float leftPreviousX = 0.0F;
        static float leftPreviousY = 0.0F;
        static std::uint8_t leftPreviousDirection = 0;

        static float rightPreviousX = 0.0F;
        static float rightPreviousY = 0.0F;
        static std::uint8_t rightPreviousDirection = 0;

        static void* r2TransitionGamepad = nullptr;
        static float previousNativePollR2 = 0.0F;
        static bool r2NativePollActive = false;
        static bool r2NativePollTimingLogged = false;

        static bool vehicleNativePollActive = false;
        static bool scannerNativePollActive = false;
        static bool galaxyMapNativePollActive = false;

        const bool vehicleActive =
            _landVehicleCorrelationArmed.load(
                std::memory_order_acquire);

        const bool scannerActive =
            vehicleActive &&
            _monocleOpen;

        bool galaxyMapActive = false;
        if (!scannerActive) {
            if (auto* ui = RE::UI::GetSingleton()) {
                galaxyMapActive = ui->IsMenuOpen(
                    RE::BSFixedString("GalaxyStarMapMenu"));
            }
        }
        const bool leftNativePollActive =
            scannerActive || galaxyMapActive;

        // R2 needs native slot2 timing in BOTH scanner contexts:
        //  - on-foot MonocleMenu (Cutter)
        //  - land vehicles (vehicle weapons)
        //
        // REV-8 scanner stick timing remains vehicle+Monocle only.
        const bool r2ContextActive =
            _monocleOpen ||
            vehicleActive;

        if (r2TransitionGamepad != gamepadDevice) {
            r2TransitionGamepad = gamepadDevice;
            previousNativePollR2 = 0.0F;
            r2NativePollActive = false;
        }

        if (!vehicleActive) {
            if (vehicleNativePollActive) {
                leftPreviousX = 0.0F;
                leftPreviousY = 0.0F;
                leftPreviousDirection = 0;
                rightPreviousX = 0.0F;
                rightPreviousY = 0.0F;
                rightPreviousDirection = 0;
            }

            vehicleNativePollActive = false;
        } else {
            vehicleNativePollActive = true;
        }

        if (!leftNativePollActive &&
            (scannerNativePollActive ||
             galaxyMapNativePollActive)) {

            leftPreviousX = 0.0F;
            leftPreviousY = 0.0F;
            leftPreviousDirection = 0;
        }

        scannerNativePollActive = scannerActive;
        galaxyMapNativePollActive = galaxyMapActive;

        const auto moduleBase =
            g_starfieldModuleBase.load(
                std::memory_order_acquire);

        if (!moduleBase) {
            return;
        }

        // Starfield's native slot2 path uses the same transition helper for
        // analog triggers. Feed R2 here only in Scanner/land-vehicle contexts
        // and release native ownership cleanly when leaving those contexts.
        if (gamepadDevice != nullptr) {
            using NativeGamepadButtonTransition =
                void (*)(
                    void*,
                    std::int32_t,
                    float,
                    float,
                    float);

            constexpr std::uintptr_t
                kNativeGamepadButtonTransitionRva =
                    0x22FC2A0u;

            constexpr std::int32_t
                kNativeR2ButtonId =
                    10;

            float nativeDeltaSeconds =
                deltaSeconds;

            if (nativeDeltaSeconds < 0.0F) {
                nativeDeltaSeconds = 0.0F;
            } else if (nativeDeltaSeconds > 0.100F) {
                nativeDeltaSeconds = 0.100F;
            }

            const auto transition =
                reinterpret_cast<
                    NativeGamepadButtonTransition>(
                        moduleBase +
                        kNativeGamepadButtonTransitionRva);

            const float currentR2 =
                state.r2 <= 2 ?
                    0.0F :
                    static_cast<float>(state.r2) /
                        255.0F;

            if (r2ContextActive) {
                if (currentR2 > 0.0001F ||
                    previousNativePollR2 > 0.0001F) {

                    notifyInputPresentationDevice(
                        InputPresentationDevice::Gamepad);

                    transition(
                        gamepadDevice,
                        kNativeR2ButtonId,
                        nativeDeltaSeconds,
                        previousNativePollR2,
                        currentR2);
                }

                previousNativePollR2 =
                    currentR2;
                r2NativePollActive = true;

                if (!r2NativePollTimingLogged) {
                    log(
                        "Bluetooth R2 context timing: NATIVE-SLOT2-TIMING "
                        "id=10 contexts=MonocleMenu,land-vehicle "
                        "runtimeReplay=suppressed");
                    r2NativePollTimingLogged = true;
                }
            } else if (r2NativePollActive) {
                if (previousNativePollR2 > 0.0001F) {
                    transition(
                        gamepadDevice,
                        kNativeR2ButtonId,
                        nativeDeltaSeconds,
                        previousNativePollR2,
                        0.0F);
                }

                previousNativePollR2 = 0.0F;
                r2NativePollActive = false;
            }
        }

        // Native-poll stick reproduction applies to land-vehicle camera and
        // REV-8 scanner contexts, plus the galaxy-map left cursor stick.
        // On-foot Scanner without a map still handles R2 above only.
        if (!vehicleActive && !galaxyMapActive) {
            return;
        }

        std::uintptr_t manager = 0;

        if (!safeReadValue(
                moduleBase + kInputQueueSingletonRva,
                manager) ||
            manager < 0x10000u) {

            return;
        }

        using NativeThumbstickProducer = void (*)(
            void* manager,
            std::uint32_t deviceId,
            std::uint8_t idCode,
            float x,
            float y,
            std::uint8_t previousDirection,
            std::uint8_t currentDirection);

        constexpr std::uintptr_t kNativeThumbstickProducerRva =
            0x22DA530u;

        const auto produceThumbstick =
            reinterpret_cast<NativeThumbstickProducer>(
                moduleBase +
                kNativeThumbstickProducerRva);

        const auto left =
            bluetoothStickValue(
                state.leftX,
                state.leftY);

        const auto right =
            bluetoothStickValue(
                state.rightX,
                state.rightY);

        auto emitNativePollStick =
            [&](std::uint8_t idCode,
                float x,
                float y,
                float& previousX,
                float& previousY,
                std::uint8_t& previousDirection) noexcept
        {
            constexpr float kActiveThreshold =
                0.0001F;

            const bool currentActive =
                std::fabs(x) > kActiveThreshold ||
                std::fabs(y) > kActiveThreshold;

            const bool previousActive =
                std::fabs(previousX) > kActiveThreshold ||
                std::fabs(previousY) > kActiveThreshold;

            if (!currentActive && !previousActive) {
                previousX = 0.0F;
                previousY = 0.0F;
                previousDirection = 0;
                return;
            }

            std::uint8_t currentDirection =
                bluetoothStickDirection(
                    x,
                    y);

            // Real native USB captures change direction around
            // the 0.50 dominant-axis boundary. Apply the same
            // behavior to BOTH scanner sticks.
            constexpr float kNativeDirectionThreshold =
                0.50F;

            const float dominantMagnitude =
                std::max(
                    std::fabs(x),
                    std::fabs(y));

            if (dominantMagnitude <
                kNativeDirectionThreshold) {

                currentDirection = 0;
            }

            g_bluetoothPhysicalBridgeActive.store(
                true,
                std::memory_order_release);

            notifyInputPresentationDevice(
                InputPresentationDevice::Gamepad);

            produceThumbstick(
                reinterpret_cast<void*>(manager),
                0u,
                idCode,
                x,
                y,
                previousDirection,
                currentDirection);

            previousX = x;
            previousY = y;
            previousDirection = currentDirection;
        };

        if (leftNativePollActive) {
            emitNativePollStick(
                11,
                left.x,
                left.y,
                leftPreviousX,
                leftPreviousY,
                leftPreviousDirection);
        }

        if (galaxyMapNativePollActive) {
            static std::atomic_bool galaxyMapTimingLogged{ false };
            if (!galaxyMapTimingLogged.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                log(
                    "Bluetooth galaxy-map left stick: NATIVE-SLOT2-TIMING "
                    "id=11 runtimeStickReplay=suppressed "
                    "fallback=runtime-when-shadow-unavailable");
            }
        }

        // Vehicle right-stick Look follows Starfield's own slot2 poll timing.
        // This avoids both runtime-tick over-application (hyper-sensitive)
        // and coarse fixed-cadence replay (visible camera stutter).
        if (vehicleActive) {
            emitNativePollStick(
                12,
                right.x,
                right.y,
                rightPreviousX,
                rightPreviousY,
                rightPreviousDirection);

            static std::atomic_bool vehicleRightTimingLogged{ false };
            if (!vehicleRightTimingLogged.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                log(
                    "Bluetooth land-vehicle right stick: NATIVE-SLOT2-TIMING "
                    "id=12 runtimeStickReplay=suppressed");
            }
        }

        if (scannerNativePollActive) {
            static std::atomic_bool scannerTimingLogged{ false };

            if (!scannerTimingLogged.exchange(
                    true,
                    std::memory_order_acq_rel)) {

                log(
                    "Bluetooth REV-8 scanner sticks: NATIVE-SLOT2-TIMING "
                    "ids=11/12 runtimeStickReplay=suppressed");
            }
        }
    }
    catch (...) {
        log(
            "Bluetooth REV-8 scanner sticks: native slot2 dispatch exception");
    }
}
void sds::GameStateAdapter::resetBluetoothPhysicalInput() noexcept
{
    TouchState neutral{};

    // One neutral frame releases any physical buttons/triggers and recenters
    // both native ThumbstickEvent streams before local state is forgotten.
    dispatchBluetoothPhysicalInput(neutral, 0.0F);

    g_bluetoothPhysicalBridgeRuntime =
        BluetoothPhysicalBridgeRuntime{};

    g_bluetoothPhysicalBridgeActive.store(
        false,
        std::memory_order_release);
}
void sds::GameStateAdapter::pollNativeInputInjection() noexcept
{
    const auto now = std::chrono::steady_clock::now();

    if (_pendingPhotoMode) {
        if (now >= _photoModeDeadline) {
            _pendingPhotoMode = false;
            log("Native input: Photo Mode cancelled because MonocleMenu did not open");
        } else if (_monocleOpen && !_nativeInputSequencer.active()) {
            if (_nativeInputSequencer.enqueueUserEvent("PhotoMode")) {
                _pendingPhotoMode = false;
                log("Native input: MonocleMenu confirmed; queued action='PhotoMode' 5-frame gamepad pulse");
            } else {
                _pendingPhotoMode = false;
                log("Native input: MonocleMenu opened but verified PhotoMode semantic could not be queued");
            }
        }
    }

    const auto emission = _nativeInputSequencer.poll(now);
    if (!emission) {
        return;
    }
    if (!dispatchNativeInputFrame(*emission)) {
        _nativeInputSequencer.reset();
        _pendingPhotoMode = false;
        log("Native input: frame dispatch failed; pulse aborted");
    }
}

bool sds::GameStateAdapter::dispatchNativeInputFrame(
    const NativeInputEmission& emission) noexcept
{
    std::uintptr_t reservedEventAddress = 0;
    bool recycledFromQueue = false;
    try {
        const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
        if (!moduleBase) {
            log("Native input: Starfield module base unavailable");
            return false;
        }

        std::uintptr_t manager = 0;
        if (!safeReadValue(moduleBase + kInputQueueSingletonRva, manager) || !manager) {
            log("Native input: native input manager unavailable");
            return false;
        }

        constexpr std::size_t kNativeButtonPoolSize = 30;
        constexpr std::size_t kNativeButtonSlotSize = 0x60;
        constexpr std::uintptr_t kNativeButtonPoolOffset = 0x10;
        constexpr std::uintptr_t kInputQueueTailOffset = 0x1298;
        const auto poolBase = manager + kNativeButtonPoolOffset;
        const auto expectedPrimary = moduleBase + kButtonEventPrimaryVtableRva;
        const auto expectedId = moduleBase + kButtonEventIdVtableRva;
        const auto expectedUser = moduleBase + kButtonEventUserVtableRva;

        // Match Starfield's physical ButtonEvent producer: use one of its 30
        // engine-owned slots when a free sentinel is available. Reserve with
        // -2 so a concurrent native producer cannot claim the same slot.
        for (std::size_t i = 0; i < kNativeButtonPoolSize; ++i) {
            const auto address = poolBase + i * kNativeButtonSlotSize;
            NativeButtonSlotState state{};
            if (!safeReadValue(address + 0x00, state.primaryVtable) ||
                !safeReadValue(address + 0x20, state.timeCode) ||
                !safeReadValue(address + 0x38, state.idVtable) ||
                !safeReadValue(address + 0x40, state.userVtable)) {
                continue;
            }
            if (!isReusableNativeButtonSlot(state, expectedPrimary, expectedId, expectedUser)) {
                continue;
            }

            auto* timeCode = reinterpret_cast<volatile LONG*>(address + 0x20);
            if (::InterlockedCompareExchange(
                    timeCode,
                    kNativeButtonReservedTimeCode,
                    static_cast<LONG>(kNativeButtonFreeTimeCode)) ==
                static_cast<LONG>(kNativeButtonFreeTimeCode)) {
                reservedEventAddress = address;
                break;
            }
        }

        // Starfield normally reaches this branch because its 30 ButtonEvents
        // remain linked in the native queue until a producer needs another one.
        // Reproduce only the proven producer recycle operation, under the exact
        // native BSSpinLock at manager+0x1288. External ButtonEvents are never
        // selected: the candidate must be aligned inside Starfield's own pool.
        if (!reservedEventAddress) {
            auto* queueLock = reinterpret_cast<RE::BSSpinLock*>(manager + kInputQueueLockOffset);
            {
                RE::BSAutoLock<RE::BSSpinLock> queueGuard(queueLock);

                auto* headPtr = reinterpret_cast<std::uintptr_t*>(manager + kInputQueueHeadOffset);
                auto* tailPtr = reinterpret_cast<std::uintptr_t*>(manager + kInputQueueTailOffset);
                const auto head = *headPtr;
                const auto tail = *tailPtr;

                std::uintptr_t previous = 0;
                std::uintptr_t current = head;
                std::size_t traversed = 0;
                constexpr std::size_t kMaxQueueTraversal = 256;

                while (current && traversed++ < kMaxQueueTraversal) {
                    std::uintptr_t next = 0;
                    std::uint8_t eventType = 0xFF;
                    std::uint32_t timeCode = kNativeButtonFreeTimeCode;
                    if (!safeReadValue(current + 0x18, next) ||
                        !safeReadValue(current + 0x10, eventType) ||
                        !safeReadValue(current + 0x20, timeCode)) {
                        log("Native input pool: queue traversal read failed; recycle aborted");
                        break;
                    }

                    const bool nativePoolButton =
                        eventType == 0 &&
                        timeCode != kNativeButtonFreeTimeCode &&
                        timeCode != static_cast<std::uint32_t>(kNativeButtonReservedTimeCode) &&
                        isNativeButtonPoolAddress(
                            current,
                            poolBase,
                            kNativeButtonPoolSize,
                            kNativeButtonSlotSize);

                    if (!nativePoolButton) {
                        previous = current;
                        current = next;
                        continue;
                    }

                    const auto plan = planNativeButtonQueueRecycle({
                        .head = head,
                        .tail = tail,
                        .previous = previous,
                        .selected = current,
                        .selectedNext = next,
                    });
                    if (!plan) {
                        log("Native input pool: queue unlink plan rejected inconsistent links");
                        break;
                    }

                    if (plan->writePreviousNext) {
                        *reinterpret_cast<std::uintptr_t*>(previous + 0x18) = plan->previousNext;
                    }
                    *headPtr = plan->newHead;
                    *tailPtr = plan->newTail;
                    *reinterpret_cast<std::uintptr_t*>(current + 0x18) = 0;
                    ::InterlockedExchange(
                        reinterpret_cast<volatile LONG*>(current + 0x20),
                        kNativeButtonReservedTimeCode);

                    reservedEventAddress = current;
                    recycledFromQueue = true;
                    break;
                }

                if (traversed >= kMaxQueueTraversal && !reservedEventAddress) {
                    log("Native input pool: queue traversal limit reached; recycle aborted");
                }
            }
        }

        if (!reservedEventAddress) {
            log("Native input pool: no free or recyclable engine-owned ButtonEvent available");
            return false;
        }

        auto releaseReservation = [&reservedEventAddress]() noexcept {
            if (!reservedEventAddress) {
                return;
            }
            auto* reservedTimeCode = reinterpret_cast<volatile LONG*>(reservedEventAddress + 0x20);
            (void)::InterlockedCompareExchange(
                reservedTimeCode,
                static_cast<LONG>(kNativeButtonFreeTimeCode),
                kNativeButtonReservedTimeCode);
            reservedEventAddress = 0;
        };

        NativeButtonSlotState selectedState{};
        if (!safeReadValue(reservedEventAddress + 0x00, selectedState.primaryVtable) ||
            !safeReadValue(reservedEventAddress + 0x38, selectedState.idVtable) ||
            !safeReadValue(reservedEventAddress + 0x40, selectedState.userVtable) ||
            selectedState.primaryVtable != expectedPrimary ||
            selectedState.idVtable != expectedId ||
            selectedState.userVtable != expectedUser) {
            releaseReservation();
            log("Native input pool: selected engine ButtonEvent failed vtable validation");
            return false;
        }

        std::uintptr_t debounceOwner = 0;
        if (!safeReadValue(moduleBase + kDebounceManagerOwnerRva, debounceOwner) || !debounceOwner) {
            releaseReservation();
            log("Native input: debounce-manager owner unavailable");
            return false;
        }
        const auto debounceManager = debounceOwner + kDebounceManagerOffset;

        auto* button = reinterpret_cast<RE::ButtonEvent*>(reservedEventAddress);
        button->deviceType = static_cast<RE::InputEvent::DeviceType>(emission.deviceType);
        button->deviceID = emission.deviceId;
        button->eventType = static_cast<RE::InputEvent::EventType>(emission.eventType);
        button->next = nullptr;
        button->status = static_cast<RE::InputEvent::Status>(emission.status);
        button->strUserEvent = emission.userEvent.data();
        button->idCode = emission.idCode;
        button->disabled = emission.disabled;
        button->value = emission.value;
        button->heldDownSecs = emission.heldDownSecs;

        const auto packedDebounceBase = packNativeButtonDebounceState(
            emission.idCode,
            emission.previousHeldDownSecs);

        std::uint64_t packedDebounce =
            static_cast<std::uint64_t>(
                packedDebounceBase);

        const bool bluetoothPhysicalGamepadEvent =
            emission.deviceType == 2 &&
            emission.userEvent.empty();

        if (bluetoothPhysicalGamepadEvent) {
            packedDebounce |=
                (static_cast<std::uint64_t>(
                     emission.deviceType) &
                 0xFFFFull)
                << 48u;

            static std::atomic_bool
                bluetoothDebounceParityLogged{
                    false };

            if (!bluetoothDebounceParityLogged.exchange(
                    true,
                    std::memory_order_acq_rel)) {

                log(
                    "Bluetooth button debounce parity: "
                    "deviceType=2 packedBits48_63=2 "
                    "source=native-USB-parity");
            }
        }

        std::memcpy(
            &button->unk50,
            &packedDebounce,
            sizeof(packedDebounce));
        button->debounceManager = reinterpret_cast<void*>(debounceManager);

        using NativeQueuePublishFunction = void (*)(void*, void*);
        const auto publish = reinterpret_cast<NativeQueuePublishFunction>(moduleBase + kNativeEnqueueRva);
        publish(reinterpret_cast<void*>(manager), reinterpret_cast<void*>(reservedEventAddress));

        std::uint32_t assignedTimeCode = kNativeButtonFreeTimeCode;
        (void)safeReadValue(reservedEventAddress + 0x20, assignedTimeCode);
        if (assignedTimeCode == static_cast<std::uint32_t>(kNativeButtonReservedTimeCode)) {
            releaseReservation();
            log("Native input: native enqueue returned without assigning timeCode");
            return false;
        }

        const auto storageIndex = static_cast<std::size_t>(
            (reservedEventAddress - poolBase) / kNativeButtonSlotSize);
        const auto eventAddress = reservedEventAddress;
        reservedEventAddress = 0;

        // Physical Bluetooth bridge events intentionally enter Starfield with
        // an empty semantic string. Starfield resolves the physical idCode
        // downstream. Do not flood the log for held physical controls.
        if (emission.userEvent.empty()) {
            return true;
        }

        char buffer[448]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Native input: enqueued action='%.*s' step=%u edge=%s storage=native-pool index=%zu recycled=%s event=0x%llX timeCode=%u device=%u idCode=%d value=%.3f held=%.3f previousHeld=%.3f",
            static_cast<int>(emission.userEvent.size()),
            emission.userEvent.data(),
            static_cast<unsigned int>(emission.stepIndex),
            emission.edge == NativeInputEdge::Press ? "press" : "release",
            storageIndex,
            recycledFromQueue ? "true" : "false",
            static_cast<unsigned long long>(eventAddress),
            assignedTimeCode,
            emission.deviceType,
            emission.idCode,
            emission.value,
            emission.heldDownSecs,
            emission.previousHeldDownSecs);
        log(buffer);
        return true;
    } catch (...) {
        if (reservedEventAddress) {
            auto* timeCode = reinterpret_cast<volatile LONG*>(reservedEventAddress + 0x20);
            (void)::InterlockedCompareExchange(
                timeCode,
                static_cast<LONG>(kNativeButtonFreeTimeCode),
                kNativeButtonReservedTimeCode);
        }
        log("Native input: exception while preparing native ButtonEvent");
        return false;
    }
}

void sds::GameStateAdapter::refreshFireMarkerSinks() noexcept
{
    if (!worldAvailable() || !_registered || !_fireMarkerTraceEnabled) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!_fireMarkerCapture.active(now)) {
        return;
    }

    try {
        std::unique_lock sourceGuard(_fireMarkerGraphMutex);
        if (!worldAvailable()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if (!player->GetAnimationGraphManagerImpl(manager) || !manager) {
            return;
        }

        std::array<RE::BSTSmartPointer<RE::BSAnimationGraph>, kMaxFireMarkerSources> currentGraphs{};
        std::size_t currentGraphCount = 0;
        {
            RE::BSAutoLock<RE::BSSpinLock> graphGuard(manager->updateLock);
            const auto graphCount = manager->graphs.size();
            for (std::uint32_t i = 0;
                 i < graphCount && currentGraphCount < currentGraphs.size();
                 ++i) {
                auto graph = manager->graphs[i];
                if (!graph) {
                    continue;
                }
                const auto duplicate = std::find_if(
                    currentGraphs.begin(),
                    currentGraphs.begin() + currentGraphCount,
                    [&](const auto& candidate) { return candidate.get() == graph.get(); });
                if (duplicate == currentGraphs.begin() + currentGraphCount) {
                    currentGraphs[currentGraphCount++] = std::move(graph);
                }
            }
        }

        std::size_t newlyRegistered = 0;
        std::size_t removed = 0;
        std::size_t total = 0;
        {
            for (std::size_t i = 0; i < _fireMarkerSourceCount; ++i) {
                auto* oldGraph = _fireMarkerGraphs[i].get();
                const auto stillCurrent = std::find_if(
                    currentGraphs.begin(),
                    currentGraphs.begin() + currentGraphCount,
                    [&](const auto& candidate) { return candidate.get() == oldGraph; });
                if (stillCurrent == currentGraphs.begin() + currentGraphCount) {
                    auto* source = static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(oldGraph);
                    source->UnregisterSink(this);
                    _fireMarkerGraphs[i].reset();
                    ++removed;
                }
            }

            std::size_t retained = 0;
            for (std::size_t i = 0; i < _fireMarkerSourceCount; ++i) {
                if (_fireMarkerGraphs[i]) {
                    if (retained != i) {
                        _fireMarkerGraphs[retained] = std::move(_fireMarkerGraphs[i]);
                    }
                    ++retained;
                }
            }
            for (std::size_t i = retained; i < _fireMarkerGraphs.size(); ++i) {
                _fireMarkerGraphs[i].reset();
            }
            _fireMarkerSourceCount = retained;

            for (std::size_t i = 0;
                 i < currentGraphCount && _fireMarkerSourceCount < _fireMarkerGraphs.size();
                 ++i) {
                auto* graph = currentGraphs[i].get();
                const auto tracked = std::find_if(
                    _fireMarkerGraphs.begin(),
                    _fireMarkerGraphs.begin() + _fireMarkerSourceCount,
                    [&](const auto& candidate) { return candidate.get() == graph; });
                if (tracked != _fireMarkerGraphs.begin() + _fireMarkerSourceCount) {
                    continue;
                }

                auto* source = static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(graph);
                source->RegisterSink(this);
                _fireMarkerGraphs[_fireMarkerSourceCount++] = currentGraphs[i];
                ++newlyRegistered;
            }
            total = _fireMarkerSourceCount;
        }
        sourceGuard.unlock();

        if (newlyRegistered != 0 || removed != 0) {
            char message[192]{};
            std::snprintf(
                message,
                sizeof(message),
                "Fire marker trace: reconciled player animation graphs added=%zu removed=%zu total=%zu",
                newlyRegistered,
                removed,
                total);
            log(message);
        }
    } catch (...) {
        // This is a passive diagnostic. Graph availability must not affect play.
    }
}

void sds::GameStateAdapter::clearFireMarkerSinks() noexcept
{
    try {
        std::scoped_lock sourceGuard(_fireMarkerGraphMutex);
        for (std::size_t i = 0; i < _fireMarkerSourceCount; ++i) {
            auto* graph = _fireMarkerGraphs[i].get();
            if (graph) {
                auto* source = static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(graph);
                source->UnregisterSink(this);
            }
            _fireMarkerGraphs[i].reset();
        }
        for (std::size_t i = _fireMarkerSourceCount; i < _fireMarkerGraphs.size(); ++i) {
            _fireMarkerGraphs[i].reset();
        }
        _fireMarkerSourceCount = 0;
    } catch (...) {
        // Strong graph references remain valid until the best-effort teardown
        // finishes; teardown must not affect Starfield shutdown.
    }
}

bool sds::GameStateAdapter::ensureTargetHitSinkRegistered(RE::PlayerCharacter* player) noexcept
{
    if (!player) {
        log("Melee TargetHit diagnostic: registration skipped because PlayerCharacter is unavailable");
        return false;
    }

    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    const auto sourceAddress = targetHitDocumentedSourceAddress(playerAddress);
    auto* targetHitSource = reinterpret_cast<RE::BSTEventSource<RE::TargetHitEvent>*>(sourceAddress);
    auto* expectedSink = static_cast<RE::BSTEventSink<RE::TargetHitEvent>*>(this);
    const auto expectedSinkAddress = reinterpret_cast<std::uintptr_t>(expectedSink);

    {
        std::scoped_lock lock(_targetHitMutex);
        if (_targetHitSinkRegistered && _targetHitSource == targetHitSource) {
            return true;
        }
    }

    unregisterTargetHitSink();

    const auto beforeProbe = probeTargetHitSource(sourceAddress);
    if (!targetHitSourceRegistrationEligible(
            beforeProbe.snapshotReadable,
            beforeProbe.vtableReadable,
            beforeProbe.vtableInStarfield,
            beforeProbe.vtableSlotReadable,
            beforeProbe.vtableSlot0InStarfield,
            beforeProbe.sinkCount,
            beforeProbe.sinkCapacity,
            beforeProbe.sinkData)) {
        char rejected[640]{};
        std::snprintf(
            rejected,
            sizeof(rejected),
            "Melee TargetHit diagnostic: corrected source rejected before registration source=%p offset=0x5D0 snapshot=%s vtable=%p vtableInStarfield=%s slot0=%p slot0InStarfield=%s sinks=%u/%u sinkData=%p",
            static_cast<void*>(targetHitSource),
            beforeProbe.snapshotReadable ? "readable" : "unreadable",
            reinterpret_cast<void*>(beforeProbe.vtable),
            beforeProbe.vtableInStarfield ? "yes" : "no",
            reinterpret_cast<void*>(beforeProbe.vtableSlot0),
            beforeProbe.vtableSlot0InStarfield ? "yes" : "no",
            beforeProbe.sinkCount,
            beforeProbe.sinkCapacity,
            reinterpret_cast<void*>(beforeProbe.sinkData));
        log(rejected);
        return false;
    }

    try {
        targetHitSource->RegisterSink(expectedSink);
    } catch (...) {
        log("Melee TargetHit diagnostic: corrected source RegisterSink threw; registration abandoned");
        return false;
    }

    const auto afterProbe = probeTargetHitSource(sourceAddress);
    const auto membership = probeTargetHitSinkMembership(afterProbe, expectedSinkAddress);
    const bool verified = targetHitSourceRegistrationEligible(
            afterProbe.snapshotReadable,
            afterProbe.vtableReadable,
            afterProbe.vtableInStarfield,
            afterProbe.vtableSlotReadable,
            afterProbe.vtableSlot0InStarfield,
            afterProbe.sinkCount,
            afterProbe.sinkCapacity,
            afterProbe.sinkData) &&
        targetHitRegistrationVerified(
            beforeProbe.sinkCount,
            afterProbe.sinkCount,
            membership.storageReadable,
            membership.present);

    if (!verified) {
        char failed[768]{};
        std::snprintf(
            failed,
            sizeof(failed),
            "Melee TargetHit diagnostic: sink verification FAIL source=%p expectedSink=%p sinksBefore=%u/%u sinksAfter=%u/%u sinkStorageReadable=%s sinkPresent=%s sinkIndex=%u; unregistering best-effort and disarming",
            static_cast<void*>(targetHitSource),
            static_cast<void*>(expectedSink),
            beforeProbe.sinkCount,
            beforeProbe.sinkCapacity,
            afterProbe.sinkCount,
            afterProbe.sinkCapacity,
            membership.storageReadable ? "yes" : "no",
            membership.present ? "yes" : "no",
            membership.index);
        log(failed);
        try {
            targetHitSource->UnregisterSink(expectedSink);
        } catch (...) {
            // The source was validated before registration, but verification
            // failed. Do not allow cleanup failure to affect gameplay.
        }
        return false;
    }

    {
        std::scoped_lock lock(_targetHitMutex);
        _targetHitSource = targetHitSource;
        _targetHitSinkRegistered = true;
        _targetHitSinkCountBeforeRegistration = beforeProbe.sinkCount;
        _targetHitSinkIndex = membership.index;
    }

    char registered[768]{};
    std::snprintf(
        registered,
        sizeof(registered),
        "Melee TargetHit diagnostic: sink registered source=%p offset=0x5D0 expectedSink=%p sinksBefore=%u/%u sinksAfter=%u/%u sinkData=%p sink verification PASS sinkPresent=yes sinkIndex=%u; diagnostic only",
        static_cast<void*>(targetHitSource),
        static_cast<void*>(expectedSink),
        beforeProbe.sinkCount,
        beforeProbe.sinkCapacity,
        afterProbe.sinkCount,
        afterProbe.sinkCapacity,
        reinterpret_cast<void*>(afterProbe.sinkData),
        membership.index);
    log(registered);
    return true;
}

void sds::GameStateAdapter::unregisterTargetHitSink() noexcept
{
    RE::BSTEventSource<RE::TargetHitEvent>* source = nullptr;
    std::uint32_t preRegistrationCount = 0;
    std::uint32_t registeredIndex = 0;
    {
        std::scoped_lock lock(_targetHitMutex);
        _targetHitDiagnosticActive = false;
        if (!_targetHitSinkRegistered || !_targetHitSource) {
            _targetHitSinkRegistered = false;
            _targetHitSource = nullptr;
            _targetHitSinkCountBeforeRegistration = 0;
            _targetHitSinkIndex = 0;
            return;
        }
        source = _targetHitSource;
        preRegistrationCount = _targetHitSinkCountBeforeRegistration;
        registeredIndex = _targetHitSinkIndex;
        _targetHitSinkRegistered = false;
        _targetHitSource = nullptr;
        _targetHitSinkCountBeforeRegistration = 0;
        _targetHitSinkIndex = 0;
    }

    const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source);
    const auto beforeProbe = probeTargetHitSource(sourceAddress);
    if (!targetHitSourceRegistrationEligible(
            beforeProbe.snapshotReadable,
            beforeProbe.vtableReadable,
            beforeProbe.vtableInStarfield,
            beforeProbe.vtableSlotReadable,
            beforeProbe.vtableSlot0InStarfield,
            beforeProbe.sinkCount,
            beforeProbe.sinkCapacity,
            beforeProbe.sinkData)) {
        return;
    }

    auto* expectedSink = static_cast<RE::BSTEventSink<RE::TargetHitEvent>*>(this);
    const auto expectedSinkAddress = reinterpret_cast<std::uintptr_t>(expectedSink);
    const auto beforeMembership = probeTargetHitSinkMembership(beforeProbe, expectedSinkAddress);

    try {
        source->UnregisterSink(expectedSink);
    } catch (...) {
        // Starfield teardown can invalidate event-source state. The sink is
        // already retired locally, so shutdown must remain fail-soft.
        return;
    }

    const auto afterProbe = probeTargetHitSource(sourceAddress);
    const auto afterMembership = probeTargetHitSinkMembership(afterProbe, expectedSinkAddress);
    const bool verified = targetHitUnregistrationVerified(
        preRegistrationCount,
        afterProbe.sinkCount,
        afterMembership.storageReadable,
        afterMembership.present);

    char message[768]{};
    std::snprintf(
        message,
        sizeof(message),
        "Melee TargetHit diagnostic: sink unregistered source=%p expectedSink=%p registeredIndex=%u sinksBeforeUnregister=%u/%u sinkPresentBefore=%s sinksAfter=%u/%u sinkPresentAfter=%s verification=%s",
        static_cast<void*>(source),
        static_cast<void*>(expectedSink),
        registeredIndex,
        beforeProbe.sinkCount,
        beforeProbe.sinkCapacity,
        beforeMembership.present ? "yes" : "no",
        afterProbe.sinkCount,
        afterProbe.sinkCapacity,
        afterMembership.present ? "yes" : "no",
        verified ? "PASS" : "FAIL");
    log(message);
}

void sds::GameStateAdapter::armTargetHitDiagnostic(
    std::string_view weapon,
    std::chrono::steady_clock::time_point now) noexcept
{
    std::scoped_lock lock(_targetHitMutex);
    _targetHitWeapon.fill('\0');
    const auto count = (std::min)(weapon.size(), _targetHitWeapon.size() - 1);
    std::memcpy(_targetHitWeapon.data(), weapon.data(), count);
    _targetHitSequence = 0;
    _targetHitArmedAt = now;
    _targetHitDiagnosticActive = _targetHitSinkRegistered;
}

void sds::GameStateAdapter::disarmTargetHitDiagnostic() noexcept
{
    std::scoped_lock lock(_targetHitMutex);
    _targetHitDiagnosticActive = false;
}

bool sds::GameStateAdapter::ensureTESHitSinkRegistered(
    RE::PlayerCharacter* player,
    std::string_view weapon) noexcept
{
    if (!player) {
        log("TESHit diagnostic: PlayerCharacter unavailable; registration skipped");
        return false;
    }

    {
        std::scoped_lock lock(_tesHitMutex);
        if (_tesHitSinkRegistered && _tesHitSource) {
            return true;
        }
    }

    const auto moduleBase = g_starfieldModuleBase.load(std::memory_order_acquire);
    const auto moduleSize = g_starfieldModuleSize.load(std::memory_order_acquire);
    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    const auto playerSinkAddress = tesHitDocumentedPlayerSinkAddress(playerAddress);
    const auto expectedSourceVtable = tesHitSourceVtableAddress();

    std::uintptr_t playerSinkVtable = 0;
    std::uintptr_t playerSinkSlot0 = 0;
    const bool playerSinkVtableReadable = safeReadValue(playerSinkAddress, playerSinkVtable) &&
        playerSinkVtable != 0;
    const bool playerSinkSlotReadable = playerSinkVtableReadable &&
        safeReadValue(playerSinkVtable, playerSinkSlot0);
    const bool playerSinkVtableInStarfield = playerSinkVtableReadable &&
        isModuleAddress(playerSinkVtable, moduleBase, moduleSize);
    const bool playerSinkSlot0InStarfield = playerSinkSlotReadable &&
        isModuleAddress(playerSinkSlot0, moduleBase, moduleSize);
    const bool expectedSourceVtableInStarfield = expectedSourceVtable != 0 &&
        isModuleAddress(expectedSourceVtable, moduleBase, moduleSize);

    if (!moduleBase || !moduleSize || !expectedSourceVtableInStarfield) {
        char rejected[768]{};
        std::snprintf(
            rejected,
            sizeof(rejected),
            "TESHit source discovery: prerequisites rejected context='%.*s' player=%p playerSink=%p documentedOffset=0x620 playerSinkVtable=%p vtableReadable=%s vtableInStarfield=%s slot0=%p slot0Readable=%s slot0InStarfield=%s expectedSourceVtable=%p expectedVtableInStarfield=%s; no registration; controller behavior unchanged",
            static_cast<int>(weapon.size()),
            weapon.data(),
            static_cast<void*>(player),
            reinterpret_cast<void*>(playerSinkAddress),
            reinterpret_cast<void*>(playerSinkVtable),
            playerSinkVtableReadable ? "yes" : "no",
            playerSinkVtableInStarfield ? "yes" : "no",
            reinterpret_cast<void*>(playerSinkSlot0),
            playerSinkSlotReadable ? "yes" : "no",
            playerSinkSlot0InStarfield ? "yes" : "no",
            reinterpret_cast<void*>(expectedSourceVtable),
            expectedSourceVtableInStarfield ? "yes" : "no");
        log(rejected);
        return false;
    }

    const auto scan = scanWritableModuleSectionsForTESHitSource(
        moduleBase, expectedSourceVtable, playerSinkAddress);

    for (std::size_t i = 0; i < scan.candidateCount; ++i) {
        const auto& candidate = scan.candidates[i];
        char candidateMessage[768]{};
        std::snprintf(
            candidateMessage,
            sizeof(candidateMessage),
            "TESHit source discovery: candidate=%zu source=%p section='%s' sinks=%u/%u sinkData=%p shapeEligible=%s sinkStorageReadable=%s containsPlayerSink=%s playerSinkIndex=%u",
            i + 1,
            reinterpret_cast<void*>(candidate.address),
            candidate.section.data(),
            candidate.sinkCount,
            candidate.sinkCapacity,
            reinterpret_cast<void*>(candidate.sinkData),
            candidate.shapeEligible ? "yes" : "no",
            candidate.sinkStorageReadable ? "yes" : "no",
            candidate.containsPlayerSink ? "yes" : "no",
            candidate.playerSinkIndex);
        log(candidateMessage);
        if (candidate.containsPlayerSink) {
            char legacyEvidence[512]{};
            std::snprintf(
                legacyEvidence,
                sizeof(legacyEvidence),
                "TESHit source discovery: legacy player-sink evidence source=%p containsPlayerSink=yes playerSinkIndex=%u; not required for v0.2.65 registration",
                reinterpret_cast<void*>(candidate.address),
                candidate.playerSinkIndex);
            log(legacyEvidence);
        }
    }

    const bool uniqueEligible = tesHitUniqueSourceRegistrationEligible(
        scan.vtableMatches, scan.shapeMatches, scan.uniqueShapeSource);
    const char* result = uniqueEligible ? "UNIQUE_SOURCE" :
        (scan.shapeMatches == 0 ? "NO_MATCH" : "AMBIGUOUS");
    char summary[1024]{};
    std::snprintf(
        summary,
        sizeof(summary),
        "TESHit source discovery: summary context='%.*s' player=%p playerSink=%p documentedOffset=0x620 playerSinkVtable=%p playerSinkSlot0=%p expectedSourceVtable=%p writableSections=%zu bytesScanned=%zu vtableMatches=%zu shapeMatches=%zu legacyPlayerSinkMatches=%zu uniqueShapeSource=%p candidatesLogged=%zu truncated=%s result=%s; legacyTESHitGetter=not-called; controller behavior unchanged",
        static_cast<int>(weapon.size()),
        weapon.data(),
        static_cast<void*>(player),
        reinterpret_cast<void*>(playerSinkAddress),
        reinterpret_cast<void*>(playerSinkVtable),
        reinterpret_cast<void*>(playerSinkSlot0),
        reinterpret_cast<void*>(expectedSourceVtable),
        scan.writableSectionsScanned,
        scan.bytesScanned,
        scan.vtableMatches,
        scan.shapeMatches,
        scan.verifiedMatches,
        reinterpret_cast<void*>(scan.uniqueShapeSource),
        scan.candidateCount,
        scan.candidatesTruncated ? "yes" : "no",
        result);
    log(summary);

    if (!uniqueEligible) {
        return false;
    }

    const auto sourceAddress = scan.uniqueShapeSource;
    auto* source = reinterpret_cast<RE::BSTEventSource<RE::TESHitEvent>*>(sourceAddress);
    auto* expectedSink = static_cast<RE::BSTEventSink<RE::TESHitEvent>*>(this);
    const auto expectedSinkAddress = reinterpret_cast<std::uintptr_t>(expectedSink);
    const auto beforeProbe = probeTargetHitSource(sourceAddress);
    const bool beforeEligible = tesHitSourceShapeEligible(
        beforeProbe.vtable == expectedSourceVtable,
        beforeProbe.snapshotReadable,
        beforeProbe.sinkCount,
        beforeProbe.sinkCapacity,
        beforeProbe.sinkData) &&
        beforeProbe.vtableSlotReadable && beforeProbe.vtableSlot0InStarfield;
    if (!beforeEligible) {
        char rejected[768]{};
        std::snprintf(
            rejected,
            sizeof(rejected),
            "TESHit diagnostic: unique source rejected before registration source=%p expectedVtable=%p observedVtable=%p slot0=%p slot0InStarfield=%s sinks=%u/%u sinkData=%p",
            static_cast<void*>(source),
            reinterpret_cast<void*>(expectedSourceVtable),
            reinterpret_cast<void*>(beforeProbe.vtable),
            reinterpret_cast<void*>(beforeProbe.vtableSlot0),
            beforeProbe.vtableSlot0InStarfield ? "yes" : "no",
            beforeProbe.sinkCount,
            beforeProbe.sinkCapacity,
            reinterpret_cast<void*>(beforeProbe.sinkData));
        log(rejected);
        return false;
    }

    try {
        source->RegisterSink(expectedSink);
    } catch (...) {
        log("TESHit diagnostic: RegisterSink threw; registration abandoned");
        return false;
    }

    const auto afterProbe = probeTargetHitSource(sourceAddress);
    const auto membership = probeTargetHitSinkMembership(afterProbe, expectedSinkAddress);
    const bool afterEligible = tesHitSourceShapeEligible(
        afterProbe.vtable == expectedSourceVtable,
        afterProbe.snapshotReadable,
        afterProbe.sinkCount,
        afterProbe.sinkCapacity,
        afterProbe.sinkData) &&
        afterProbe.vtableSlotReadable && afterProbe.vtableSlot0InStarfield;
    const bool verified = afterEligible && tesHitRegistrationVerified(
        beforeProbe.sinkCount,
        afterProbe.sinkCount,
        membership.storageReadable,
        membership.present);

    if (!verified) {
        char failed[768]{};
        std::snprintf(
            failed,
            sizeof(failed),
            "TESHit diagnostic: sink verification FAIL source=%p expectedSink=%p sinksBefore=%u/%u sinksAfter=%u/%u sinkStorageReadable=%s sinkPresent=%s sinkIndex=%u; unregistering best-effort and disarming",
            static_cast<void*>(source),
            static_cast<void*>(expectedSink),
            beforeProbe.sinkCount,
            beforeProbe.sinkCapacity,
            afterProbe.sinkCount,
            afterProbe.sinkCapacity,
            membership.storageReadable ? "yes" : "no",
            membership.present ? "yes" : "no",
            membership.index);
        log(failed);
        try {
            source->UnregisterSink(expectedSink);
        } catch (...) {
            // Registration verification failed; cleanup remains best-effort.
        }
        return false;
    }

    {
        std::scoped_lock lock(_tesHitMutex);
        _tesHitSource = source;
        _tesHitSinkRegistered = true;
        _tesHitSinkCountBeforeRegistration = beforeProbe.sinkCount;
        _tesHitSinkIndex = membership.index;
    }

    char registered[768]{};
    std::snprintf(
        registered,
        sizeof(registered),
        "TESHit diagnostic: sink registered source=%p expectedSink=%p sinksBefore=%u/%u sinksAfter=%u/%u sinkData=%p verification=PASS sinkPresent=yes sinkIndex=%u; diagnostic only",
        static_cast<void*>(source),
        static_cast<void*>(expectedSink),
        beforeProbe.sinkCount,
        beforeProbe.sinkCapacity,
        afterProbe.sinkCount,
        afterProbe.sinkCapacity,
        reinterpret_cast<void*>(afterProbe.sinkData),
        membership.index);
    log(registered);
    return true;
}

void sds::GameStateAdapter::unregisterTESHitSink() noexcept
{
    RE::BSTEventSource<RE::TESHitEvent>* source = nullptr;
    std::uint32_t registeredIndex = 0;
    {
        std::scoped_lock lock(_tesHitMutex);
        _tesHitDiagnosticActive = false;
        if (!_tesHitSinkRegistered || !_tesHitSource) {
            _tesHitSinkRegistered = false;
            _tesHitSource = nullptr;
            _tesHitSinkCountBeforeRegistration = 0;
            _tesHitSinkIndex = 0;
            return;
        }
        source = _tesHitSource;
        registeredIndex = _tesHitSinkIndex;
        _tesHitSinkRegistered = false;
        _tesHitSource = nullptr;
        _tesHitSinkCountBeforeRegistration = 0;
        _tesHitSinkIndex = 0;
    }

    const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source);
    const auto expectedSourceVtable = tesHitSourceVtableAddress();
    const auto beforeProbe = probeTargetHitSource(sourceAddress);
    if (!tesHitSourceShapeEligible(
            beforeProbe.vtable == expectedSourceVtable,
            beforeProbe.snapshotReadable,
            beforeProbe.sinkCount,
            beforeProbe.sinkCapacity,
            beforeProbe.sinkData)) {
        return;
    }

    auto* expectedSink = static_cast<RE::BSTEventSink<RE::TESHitEvent>*>(this);
    const auto expectedSinkAddress = reinterpret_cast<std::uintptr_t>(expectedSink);
    const auto beforeMembership = probeTargetHitSinkMembership(beforeProbe, expectedSinkAddress);

    try {
        source->UnregisterSink(expectedSink);
    } catch (...) {
        return;
    }

    const auto afterProbe = probeTargetHitSource(sourceAddress);
    const auto afterMembership = probeTargetHitSinkMembership(afterProbe, expectedSinkAddress);
    const bool verified = beforeMembership.storageReadable && beforeMembership.present &&
        tesHitUnregistrationVerified(
            beforeProbe.sinkCount,
            afterProbe.sinkCount,
            afterMembership.storageReadable,
            afterMembership.present);

    char message[768]{};
    std::snprintf(
        message,
        sizeof(message),
        "TESHit diagnostic: sink unregistered source=%p expectedSink=%p registeredIndex=%u sinksBeforeUnregister=%u/%u sinkPresentBefore=%s sinksAfter=%u/%u sinkPresentAfter=%s verification=%s",
        static_cast<void*>(source),
        static_cast<void*>(expectedSink),
        registeredIndex,
        beforeProbe.sinkCount,
        beforeProbe.sinkCapacity,
        beforeMembership.present ? "yes" : "no",
        afterProbe.sinkCount,
        afterProbe.sinkCapacity,
        afterMembership.present ? "yes" : "no",
        verified ? "PASS" : "FAIL");
    log(message);
}

void sds::GameStateAdapter::armTESHitDiagnostic(
    std::string_view weapon,
    std::uint32_t weaponFormId,
    std::chrono::steady_clock::time_point now) noexcept
{
    std::scoped_lock lock(_tesHitMutex);
    _tesHitWeapon.fill('\0');
    const auto count = (std::min)(weapon.size(), _tesHitWeapon.size() - 1);
    std::memcpy(_tesHitWeapon.data(), weapon.data(), count);
    _tesHitSequence = 0;
    _tesHitWeaponFormId = weaponFormId;
    _tesHitArmedAt = now;
    _tesHitDiagnosticActive = _tesHitSinkRegistered;
}

void sds::GameStateAdapter::disarmTESHitDiagnostic() noexcept
{
    std::scoped_lock lock(_tesHitMutex);
    _tesHitDiagnosticActive = false;
    _tesHitWeaponFormId = 0;
}

void sds::GameStateAdapter::runTESHitSourceDiscovery(
    RE::PlayerCharacter* player,
    std::string_view weapon,
    std::uint32_t weaponFormId) noexcept
{
    if (ensureTESHitSinkRegistered(player, weapon)) {
        armTESHitDiagnostic(weapon, weaponFormId, std::chrono::steady_clock::now());
    } else {
        disarmTESHitDiagnostic();
    }
}

std::optional<float> sds::GameStateAdapter::readPlayerHealthRatio(
    RE::PlayerCharacter* player) const noexcept
{
    if (!worldAvailable()) {
        return std::nullopt;
    }
    auto* actorValues = RE::ActorValue::GetSingleton();
    if (!player || !actorValues || !actorValues->health) {
        return std::nullopt;
    }

    const float current = player->GetActorValue(*actorValues->health);
    const float maximum = player->GetPermanentActorValue(*actorValues->health);
    if (!std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0.0F) {
        return std::nullopt;
    }

    return std::clamp(current / maximum, 0.0F, 1.0F);
}

void sds::GameStateAdapter::drainIncomingDamageCorrelations(
    std::chrono::steady_clock::time_point now) noexcept
{
    while (true) {
        std::optional<IncomingDamageCorrelationSummary> summary;
        std::uint64_t dropped = 0;
        bool reportDrops = false;
        {
            std::scoped_lock lock(_incomingDamageMutex);
            dropped = _incomingDamageDiagnostic.droppedCount();
            if (dropped != _lastIncomingDamageDropCountLogged) {
                _lastIncomingDamageDropCountLogged = dropped;
                reportDrops = true;
            }
            summary = _incomingDamageDiagnostic.takeExpired(now);
        }

        if (reportDrops) {
            char droppedMessage[256]{};
            std::snprintf(
                droppedMessage,
                sizeof(droppedMessage),
                "Incoming damage diagnostic: capacityDrops=%llu maxActive=8 newestRejected=yes controllerBehavior=unchanged",
                static_cast<unsigned long long>(dropped));
            log(droppedMessage);
        }

        if (!summary) {
            break;
        }

        char directHealth[32]{};
        if (summary->directHealthAvailable) {
            std::snprintf(directHealth, sizeof(directHealth), "%.4f", summary->directHealthRatio);
        } else {
            std::snprintf(directHealth, sizeof(directHealth), "unavailable");
        }

        char message[512]{};
        if (summary->healthLossAvailable) {
            std::snprintf(
                message,
                sizeof(message),
                "Incoming damage correlation: seq=%llu windowMs=300 healthAvailable=yes prePoll=%.4f atEvent=%s minimum=%.4f loss=%.4f overlap=%s overlapCount=%u attribution=%s",
                static_cast<unsigned long long>(summary->sequence),
                summary->prePollHealthRatio,
                directHealth,
                summary->minimumHealthRatio,
                summary->healthLossRatio,
                summary->overlap ? "yes" : "no",
                summary->overlapCount,
                summary->overlap ? "ambiguous" : "isolated");
        } else {
            std::snprintf(
                message,
                sizeof(message),
                "Incoming damage correlation: seq=%llu windowMs=300 healthAvailable=no prePoll=unavailable atEvent=%s minimum=unavailable loss=unavailable overlap=%s overlapCount=%u attribution=%s",
                static_cast<unsigned long long>(summary->sequence),
                directHealth,
                summary->overlap ? "yes" : "no",
                summary->overlapCount,
                summary->overlap ? "ambiguous" : "isolated");
        }
        log(message);
    }
}

std::optional<sds::ShipLandingReconStateObservation> sds::GameStateAdapter::pollShipLandingReconStatePrecision()
{
    if (!worldAvailable()) {
        return std::nullopt;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return std::nullopt;
    }

    auto* ship = player->GetSpaceship();
    if (!ship) {
        return std::nullopt;
    }

    auto* pilot = ship->GetSpaceshipPilot();
    if (pilot != player) {
        return std::nullopt;
    }

    ShipPropulsionState state{};
    state.landed = ship->IsSpaceshipLanded();
    state.docked = ship->IsSpaceshipDocked();
    const auto observedWhen = std::chrono::steady_clock::now();

    char message[288]{};
    std::snprintf(
        message,
        sizeof(message),
        "Ship landing precision state: ship=0x%08X pilotConfirmed=yes landed=%s docked=%s source=fresh-current-ship cadence=runtime-tick diagnostic-only",
        static_cast<unsigned int>(ship->GetFormID()),
        state.landed ? "yes" : "no",
        state.docked ? "yes" : "no");
    log(message);
    return ShipLandingReconStateObservation{
        .state = state,
        .when = observedWhen,
    };
}

std::optional<sds::ShipPropulsionState> sds::GameStateAdapter::pollShipLandingReconState()
{
    if (!worldAvailable()) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < _nextShipPropulsionPoll) {
        return std::nullopt;
    }
    _nextShipPropulsionPoll = now + kShipPropulsionPollInterval;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return std::nullopt;
    }

    auto* ship = player->GetSpaceship();
    if (!ship) {
        return std::nullopt;
    }

    auto* pilot = ship->GetSpaceshipPilot();
    if (pilot != player) {
        return std::nullopt;
    }

    ShipPropulsionState state{};
    state.landed = ship->IsSpaceshipLanded();
    state.docked = ship->IsSpaceshipDocked();

    char message[256]{};
    std::snprintf(
        message,
        sizeof(message),
        "Ship landing recon state: ship=0x%08X pilotConfirmed=yes landed=%s docked=%s source=fresh-current-ship diagnostic-only",
        static_cast<unsigned int>(ship->GetFormID()),
        state.landed ? "yes" : "no",
        state.docked ? "yes" : "no");
    log(message);
    return state;
}

std::optional<sds::LandVehicleReconResult> sds::GameStateAdapter::pollLandVehicleReconState()
{
    if (!worldAvailable()) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < _nextLandVehicleReconPoll) {
        return std::nullopt;
    }
    _nextLandVehicleReconPoll = now + kLandVehicleReconPollInterval;

    bool cameraVehicle = false;
    try {
        if (auto* camera = RE::PlayerCamera::GetSingleton()) {
            cameraVehicle = camera->QCameraEquals(RE::CameraState::kVehicle);
        }
    } catch (...) {
        cameraVehicle = false;
    }

    LandVehicleReconObservation observation{};
    observation.whenUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count());
    observation.cameraVehicle = cameraVehicle;

    LandVehicleDriverEventSnapshot rawSnapshot{};
    std::uint64_t rawSequence = 0;
    bool rawFresh = false;
    bool loading = false;
    bool cameraChanged = false;
    bool recentRaw = false;
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        loading = _landVehicleLoading;
        observation.loading = loading;
        if (_landVehicleDriverEventSequence != _landVehicleLastPolledDriverEventSequence) {
            rawSnapshot = _landVehicleLastDriverEvent;
            rawSequence = _landVehicleDriverEventSequence;
            _landVehicleLastPolledDriverEventSequence = _landVehicleDriverEventSequence;
            rawFresh = true;
            observation.rawDriverEventObserved = true;
            observation.rawFingerprint = rawSnapshot.fingerprint;
        }
        recentRaw = _landVehicleLastDriverEventAt.time_since_epoch().count() != 0 &&
            now - _landVehicleLastDriverEventAt <= kLandVehicleRawEvidenceHold;
        cameraChanged = cameraVehicle != _lastLandVehicleCameraCorroboration;
        _lastLandVehicleCameraCorroboration = cameraVehicle;
    }

    const bool shipPilot = _shipPilotContext.piloting();
    if (shipPilot) {
        cameraVehicle = false;
        observation.cameraVehicle = false;
        recentRaw = false;
    }

    LandVehicleTelemetryObservation telemetryObservation{};
    telemetryObservation.whenUs = observation.whenUs;
    if (!loading && !shipPilot && cameraVehicle) {
        try {
            if (auto* player = RE::PlayerCharacter::GetSingleton();
                player && player->currentProcess && player->currentProcess->middleHigh) {
                auto& occupiedFurniture = player->currentProcess->middleHigh->occupiedFurniture;
                telemetryObservation.occupiedHandle = occupiedFurniture.get_handle();
                if (telemetryObservation.occupiedHandle != 0) {
                    auto vehicleReference = occupiedFurniture.get();
                    if (vehicleReference) {
                        telemetryObservation.identityReadable = true;
                        telemetryObservation.referenceAddress = reinterpret_cast<std::uintptr_t>(vehicleReference.get());
                        telemetryObservation.referenceFormId = vehicleReference->GetFormID();
                        if (auto baseObject = vehicleReference->GetBaseObject()) {
                            telemetryObservation.baseFormId = baseObject->GetFormID();
                            telemetryObservation.baseFormType = static_cast<std::uint32_t>(baseObject->GetFormType());
                        }
                        const auto position = vehicleReference->GetPosition();
                        telemetryObservation.positionReadable =
                            std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
                        telemetryObservation.positionX = position.x;
                        telemetryObservation.positionY = position.y;
                        telemetryObservation.positionZ = position.z;
                    }
                }
            }
        } catch (...) {
            telemetryObservation = {};
            telemetryObservation.whenUs = observation.whenUs;
        }
    }

    LandVehicleTelemetryResult telemetry{};
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        telemetry = _landVehicleTelemetryProbe.observe(telemetryObservation);
        _landVehicleLatestTelemetry = telemetry;
    }
    const bool baseIsFurniture = telemetry.baseFormType == static_cast<std::uint32_t>(RE::FormType::kFURN);
    const bool validatedVehicleIdentity = !loading && !shipPilot && cameraVehicle && telemetry.identityReadable &&
        telemetry.referenceAddress != 0 && telemetry.referenceFormId != 0 && baseIsFurniture;

    // v0.3.83 promotion: the hardware-validated occupiedFurniture reference is Tier-A
    // land-vehicle identity only inside kVehicle. kVehicle itself remains corroboration
    // and the identity fails closed immediately if the resolved furniture reference vanishes.
    observation.identityReadable = true;
    observation.identityFresh = true;
    if (validatedVehicleIdentity) {
        observation.vehicleAddress = telemetry.referenceAddress;
        observation.vehicleFormId = telemetry.referenceFormId;
        observation.velocityReadable = telemetry.velocityReadable;
        observation.velocityX = telemetry.velocityX;
        observation.velocityY = telemetry.velocityY;
        observation.velocityZ = telemetry.velocityZ;
    } else {
        observation.vehicleAddress = 0;
        observation.vehicleFormId = 0;
        observation.velocityReadable = false;
    }

    if (telemetry.identityChanged) {
        char telemetryIdentity[512]{};
        std::snprintf(
            telemetryIdentity,
            sizeof(telemetryIdentity),
            "Land vehicle telemetry: IDENTITY source=AIProcess.occupiedFurniture handle=0x%08X ref=0x%08X refPtr=0x%llX base=0x%08X baseType=0x%02X baseIsFurniture=%s identityPromotion=hardware-validated authority=%s controllerOutput=none",
            static_cast<unsigned>(telemetry.occupiedHandle),
            static_cast<unsigned>(telemetry.referenceFormId),
            static_cast<unsigned long long>(telemetry.referenceAddress),
            static_cast<unsigned>(telemetry.baseFormId),
            static_cast<unsigned>(telemetry.baseFormType),
            baseIsFurniture ? "yes" : "no",
            validatedVehicleIdentity ? "tier-a-candidate" : "no");
        log(telemetryIdentity);
    }

    _landVehicleCorrelationArmed.store(!loading && !shipPilot && (cameraVehicle || recentRaw), std::memory_order_release);

    bool suppressionBoundaryEntered = false;
    bool suppressionBoundaryExited = false;
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        if (shipPilot) {
            // Ship context owns suppression from its own authoritative lifecycle.
            // Do not emit a fake land-vehicle exit on takeover.
            _landVehicleSuppressionBoundaryActive = false;
        } else if (!loading) {
            const bool desiredSuppression = cameraVehicle;
            if (desiredSuppression != _landVehicleSuppressionBoundaryActive) {
                _landVehicleSuppressionBoundaryActive = desiredSuppression;
                suppressionBoundaryEntered = desiredSuppression;
                suppressionBoundaryExited = !desiredSuppression;
            }
        }
    }

    if (suppressionBoundaryEntered || suppressionBoundaryExited) {
        GameEvent context{};
        context.type = suppressionBoundaryEntered ?
            GameEventType::LandVehicleContextEntered : GameEventType::LandVehicleContextExited;
        context.when = now;
        copyText(context, "kVehicle");
        emit(
            context,
            suppressionBoundaryEntered ?
                "Land vehicle context: ENTER source=kVehicle role=suppression-only authority=no controllerOutput=clear-handheld" :
                "Land vehicle context: EXIT source=kVehicle role=suppression-only authority=no refresh=fresh-on-foot");
    }

    LandVehicleReconResult result{};
    LandVehiclePhysicsResult physics{};
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        result = _landVehicleReconProbe.observe(observation);
        physics = _landVehiclePhysicsProbe.observe({
            .authorityActive = result.authorityActive,
            .authorityEpoch = result.epoch,
            .velocityReadable = telemetry.velocityReadable && result.authorityActive,
            .speed = telemetry.speed,
            .verticalSpeed = telemetry.velocityZ,
        });
        _landVehicleLatestPhysics = physics;
    }

    const bool productionActive = result.authorityActive;
    bool productionReleased = false;
    bool productionAcquired = false;
    LandVehicleMotionCallback motionCallback{};
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        const bool epochChanged = productionActive && _landVehicleProductionAuthorityActive &&
            result.epoch != _landVehicleProductionEpoch;

        if (epochChanged || (!productionActive && _landVehicleProductionAuthorityActive)) {
            productionReleased = true;
            _landVehicleProductionAuthorityActive = false;
            _landVehicleProductionEpoch = 0;
        }
        if (productionActive && (!_landVehicleProductionAuthorityActive || epochChanged)) {
            _landVehicleProductionAuthorityActive = true;
            _landVehicleProductionEpoch = result.epoch;
            productionAcquired = true;
        }
        motionCallback = _landVehicleMotionCallback;
    }

    if (productionReleased) {
        GameEvent event{};
        event.type = GameEventType::LandVehicleAuthorityReleased;
        event.when = now;
        emit(std::move(event));
    }
    if (productionAcquired) {
        GameEvent event{};
        event.type = GameEventType::LandVehicleAuthorityAcquired;
        event.when = now;
        emit(std::move(event));
    }

    LandVehicleMotionState motion{};
    motion.authorityActive = result.authorityActive;
    motion.authorityEpoch = result.epoch;
    motion.speed = telemetry.velocityReadable && result.authorityActive ? telemetry.speed : 0.0F;
    motion.acceleration = telemetry.accelerationReadable && result.authorityActive ? telemetry.acceleration : 0.0F;
    motion.airborne = physics.airborne;
    motion.descending = physics.descending;
    if (motionCallback) {
        motionCallback(motion);
    }

    if (result.transition == LandVehicleReconTransition::AuthorityAnchored ||
        result.transition == LandVehicleReconTransition::AuthorityReacquired ||
        result.transition == LandVehicleReconTransition::IdentityChanged) {
        char authorityMessage[384]{};
        std::snprintf(
            authorityMessage,
            sizeof(authorityMessage),
            "Land vehicle authority: ACTIVE source=AIProcess.occupiedFurniture role=tier-a identity=0x%08X epoch=%llu cameraRole=corroboration-only controllerOutput=none",
            static_cast<unsigned>(telemetry.referenceFormId),
            static_cast<unsigned long long>(result.epoch));
        log(authorityMessage);
    } else if (result.transition == LandVehicleReconTransition::AuthorityExited ||
               result.transition == LandVehicleReconTransition::Invalidated) {
        log("Land vehicle authority: INACTIVE source=occupiedFurniture-fail-closed controllerOutput=none");
    }

    if (physics.transition == LandVehiclePhysicsTransition::Airborne) {
        log("Land vehicle physics: AIRBORNE source=vertical-velocity authority=tier-a controllerOutput=none");
    } else if (physics.transition == LandVehiclePhysicsTransition::Descending) {
        log("Land vehicle physics: DESCENDING source=vertical-velocity authority=tier-a controllerOutput=none");
    } else if (physics.transition == LandVehiclePhysicsTransition::Touchdown) {
        GameEvent touchdown{};
        touchdown.type = GameEventType::LandVehicleTouchdown;
        touchdown.when = now;
        touchdown.value = physics.impactVerticalSpeed;
        emit(std::move(touchdown));

        char touchdownMessage[384]{};
        std::snprintf(
            touchdownMessage,
            sizeof(touchdownMessage),
            "Land vehicle physics: TOUCHDOWN source=physics-derived impactVerticalSpeed=%.3f authority=tier-a WwiseAuthority=none controllerOutput=none",
            static_cast<double>(physics.impactVerticalSpeed));
        log(touchdownMessage);
    }

    const bool summaryDue = now >= _nextLandVehicleSummaryLog;
    const bool summaryInteresting = cameraVehicle || recentRaw;
    if (cameraChanged || rawFresh || (summaryDue && summaryInteresting)) {
        if (summaryDue && summaryInteresting) {
            _nextLandVehicleSummaryLog = now + kLandVehicleReconSummaryInterval;
        }
        char message[1400]{};
        std::snprintf(
            message,
            sizeof(message),
            "Land vehicle recon: state cameraVehicle=%s cameraAuthority=no cameraRole=corroboration-only loading=%s shipPilot=%s rawEventFresh=%s rawSeq=%llu fingerprint=0x%016llX pointerCandidates=%zu occupiedCandidate=%s occupiedHandle=0x%08X telemetryRef=0x%08X telemetryBase=0x%08X telemetryBaseType=0x%02X positionReadable=%s velocityReadable=%s speed=%.3f verticalSpeed=%.3f accelerationReadable=%s acceleration=%.3f sampleMs=%.1f identityPromotion=hardware-validated provenVehicleIdentity=%s authorityActive=%s epoch=%llu WwiseCorrelationArmed=%s controllerOutput=none",
            cameraVehicle ? "yes" : "no",
            loading ? "yes" : "no",
            shipPilot ? "yes" : "no",
            rawFresh ? "yes" : "no",
            static_cast<unsigned long long>(rawSequence),
            static_cast<unsigned long long>(rawSnapshot.fingerprint),
            rawSnapshot.pointerCandidateCount,
            telemetry.identityReadable ? "yes" : "no",
            static_cast<unsigned>(telemetry.occupiedHandle),
            static_cast<unsigned>(telemetry.referenceFormId),
            static_cast<unsigned>(telemetry.baseFormId),
            static_cast<unsigned>(telemetry.baseFormType),
            telemetry.positionReadable ? "yes" : "no",
            telemetry.velocityReadable ? "yes" : "no",
            static_cast<double>(telemetry.speed),
            static_cast<double>(telemetry.velocityZ),
            telemetry.accelerationReadable ? "yes" : "no",
            static_cast<double>(telemetry.acceleration),
            static_cast<double>(telemetry.sampleIntervalSeconds * 1000.0F),
            validatedVehicleIdentity ? "yes" : "no",
            result.authorityActive ? "yes" : "no",
            static_cast<unsigned long long>(result.epoch),
            _landVehicleCorrelationArmed.load(std::memory_order_acquire) ? "yes" : "no");
        log(message);
    }
    return result;
}

sds::LandVehicleTelemetryResult sds::GameStateAdapter::latestLandVehicleTelemetrySnapshot() noexcept
{
    std::scoped_lock guard(_landVehicleReconMutex);
    return _landVehicleLatestTelemetry;
}

sds::LandVehiclePhysicsResult sds::GameStateAdapter::latestLandVehiclePhysicsSnapshot() noexcept
{
    std::scoped_lock guard(_landVehicleReconMutex);
    return _landVehicleLatestPhysics;
}

bool sds::GameStateAdapter::armLandVehicleVerticalBoost() noexcept
{
    bool armed = false;
    std::uint64_t epoch = 0;
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        if (_landVehicleLatestPhysics.authorityActive) {
            epoch = _landVehicleLatestPhysics.authorityEpoch;
            armed = _landVehiclePhysicsProbe.armVerticalBoost(_landVehicleLatestPhysics.authorityEpoch);
        }
    }

    if (armed) {
        GameEvent event{};
        event.type = GameEventType::LandVehicleBoostStarted;
        event.when = std::chrono::steady_clock::now();
        emit(std::move(event));

        char message[256]{};
        std::snprintf(
            message,
            sizeof(message),
            "Land vehicle physics: JUMP-ARMED source=accepted-vertical-boost-Wwise authority=tier-a epoch=%llu controllerOutput=none",
            static_cast<unsigned long long>(epoch));
        log(message);
    }
    return armed;
}

void sds::GameStateAdapter::pollShipLightbarSeatAuthority()
{
    if (!worldAvailable()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < _nextShipLightbarSeatPoll) {
        return;
    }
    _nextShipLightbarSeatPoll =
        now + std::chrono::milliseconds(50);

    // Never release physical seat authority from a transient loading-world
    // pointer gap. The first post-load poll resolves the real current state.
    auto* ui = RE::UI::GetSingleton();
    if (ui &&
        ui->IsMenuOpen(RE::BSFixedString("LoadingMenu"))) {
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }

    auto* ship = player->GetSpaceship();

    bool seatActive = false;
    std::uint32_t shipFormId = 0;

    if (ship) {
        shipFormId = ship->GetFormID();
        seatActive =
            ship->GetSpaceshipPilot() == player;
    }

    // Establish an on-foot startup baseline silently. A real first seat
    // acquisition still emits the normalized ENTER event.
    if (!_shipLightbarSeatKnown && !seatActive) {
        _shipLightbarSeatKnown = true;
        _shipLightbarSeatActive = false;
        return;
    }

    if (_shipLightbarSeatKnown &&
        seatActive == _shipLightbarSeatActive) {
        return;
    }

    GameEvent seatEvent{};
    seatEvent.type =
        seatActive ?
            GameEventType::ShipLightbarSeatEntered :
            GameEventType::ShipLightbarSeatExited;
    seatEvent.formId = shipFormId;
    seatEvent.when = now;

    char message[256]{};
    std::snprintf(
        message,
        sizeof(message),
        "Game state: ship lightbar seat %s ship=0x%08X authority=current-ship-pilot-pointer",
        seatActive ? "ENTER" : "EXIT",
        static_cast<unsigned int>(shipFormId));

    if (!emit(seatEvent, message)) {
        return;
    }

    _shipLightbarSeatKnown = true;
    _shipLightbarSeatActive = seatActive;

    // Physical pilot-pointer authority arrives before SpaceshipHudMenu when
    // auto-entering from outside the ship. Force one current hull-health
    // sample here so ship lightbar ownership never has to fall back to black
    // while waiting for the normal propulsion/menu path.
    if (seatActive && ship) {
        auto* actorValues =
            RE::ActorValue::GetSingleton();

        if (actorValues && actorValues->health) {
            const float healthCurrent =
                readShipActorValue(
                    ship,
                    actorValues->health);

            const float healthMaximum =
                readShipPermanentActorValue(
                    ship,
                    actorValues->health);

            if (std::isfinite(healthCurrent) &&
                std::isfinite(healthMaximum) &&
                healthMaximum > 0.0F) {

                const float healthRatio =
                    std::clamp(
                        healthCurrent / healthMaximum,
                        0.0F,
                        1.0F);

                GameEvent healthEvent{};
                healthEvent.type =
                    GameEventType::ShipHealthChanged;
                healthEvent.value =
                    healthRatio;
                healthEvent.formId =
                    shipFormId;
                healthEvent.when =
                    now;

                char healthMessage[256]{};
                std::snprintf(
                    healthMessage,
                    sizeof(healthMessage),
                    "Game state: ship hull health %.1f%% current=%.3f max=%.3f ship=0x%08X source=physical-seat-acquire",
                    healthRatio * 100.0F,
                    healthCurrent,
                    healthMaximum,
                    static_cast<unsigned int>(
                        shipFormId));

                if (emit(
                        std::move(healthEvent),
                        healthMessage)) {

                    _lastShipHealthRatio =
                        healthRatio;
                    _lastShipHealthFormId =
                        shipFormId;
                }
            }
        }
    }

    // A real physical seat exit must rebuild on-foot lightbar state even when
    // health did not move enough to trigger the normal material-delta poll.
    if (!seatActive) {
        (void)refreshPlayerHealth();
    }
}
std::optional<sds::ShipPropulsionState> sds::GameStateAdapter::pollShipPropulsionState()
{
    if (!worldAvailable()) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < _nextShipPropulsionPoll) {
        return std::nullopt;
    }
    _nextShipPropulsionPoll = now + kShipPropulsionPollInterval;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        _shipPropulsionProbe.reset();
        log("Ship propulsion state: unavailable reason=no-player haptic=clear");
        return ShipPropulsionState{};
    }

    auto* ship = player->GetSpaceship();
    if (!ship) {
        _shipPropulsionProbe.reset();
        log("Ship propulsion state: unavailable reason=no-current-ship haptic=clear");
        return ShipPropulsionState{};
    }

    auto* pilot = ship->GetSpaceshipPilot();
    if (pilot != player) {
        _shipPropulsionProbe.reset();
        char message[192]{};
        std::snprintf(
            message,
            sizeof(message),
            "Ship propulsion state: unavailable reason=player-not-pilot ship=0x%08X pilot=%p player=%p haptic=clear",
            static_cast<unsigned int>(ship->GetFormID()),
            static_cast<void*>(pilot),
            static_cast<void*>(player));
        log(message);
        return ShipPropulsionState{};
    }

    //
    // Ship hull-health candidate. First hardware pass validates
    // generic Health against the actual Starfield hull meter.
    //
    auto* actorValues =
        RE::ActorValue::GetSingleton();

    if (actorValues && actorValues->health) {
        const float healthCurrent =
            readShipActorValue(
                ship,
                actorValues->health);

        const float healthMaximum =
            readShipPermanentActorValue(
                ship,
                actorValues->health);

        if (std::isfinite(healthCurrent) &&
            std::isfinite(healthMaximum) &&
            healthMaximum > 0.0F) {

            const float healthRatio =
                std::clamp(
                    healthCurrent / healthMaximum,
                    0.0F,
                    1.0F);

            const auto shipFormId =
                ship->GetFormID();

            const bool freshShip =
                shipFormId != _lastShipHealthFormId;

            const bool materialChange =
                _lastShipHealthRatio < 0.0F ||
                std::fabs(
                    healthRatio - _lastShipHealthRatio) >=
                    kHealthMaterialDelta;

            if (freshShip || materialChange) {
                GameEvent healthEvent{};
                healthEvent.type =
                    GameEventType::ShipHealthChanged;
                healthEvent.value =
                    healthRatio;
                healthEvent.formId =
                    shipFormId;
                healthEvent.when =
                    now;

                char healthMessage[256]{};
                std::snprintf(
                    healthMessage,
                    sizeof(healthMessage),
                    "Game state: ship hull health %.1f%% current=%.3f max=%.3f ship=0x%08X source=Health-actor-value",
                    healthRatio * 100.0F,
                    healthCurrent,
                    healthMaximum,
                    static_cast<unsigned int>(shipFormId));

                if (emit(healthEvent, healthMessage)) {
                    _lastShipHealthRatio =
                        healthRatio;
                    _lastShipHealthFormId =
                        shipFormId;
                }
            }
        }
    }

    std::uintptr_t cluster = 0;
    (void)safeReadValue(
        reinterpret_cast<std::uintptr_t>(&g_shipFlightControlClusterObserved),
        cluster);

    ShipPropulsionState state{};
    state.landed = ship->IsSpaceshipLanded();
    state.docked = ship->IsSpaceshipDocked();
    state.throttleTargetReadable = cluster != 0 &&
        safeReadValue(cluster + 0x68, state.throttleTarget) &&
        std::isfinite(state.throttleTarget);
    state.effectiveThrottleReadable = cluster != 0 &&
        safeReadValue(cluster + 0x6C, state.effectiveThrottle) &&
        std::isfinite(state.effectiveThrottle);
    state.velocityReadable = cluster != 0 &&
        safeReadValue(cluster + 0x70, state.velocity) &&
        std::isfinite(state.velocity);

    auto& av = shipPropulsionActorValues();
    state.maxForwardSpeed = readShipActorValue(ship, av.maxForwardSpeed);
    const float forwardSpeedMult = readShipActorValue(ship, av.forwardSpeedMult);
    const float forwardForcePerPower = readShipActorValue(ship, av.forwardForcePerPower);
    const float maxForwardAcceleration = readShipActorValue(ship, av.maxForwardAcceleration);
    state.boostFuelCurrent = readShipActorValue(ship, av.boostFuel);
    state.boostFuelPermanent = readShipPermanentActorValue(ship, av.boostFuel);
    state.boostSpeed = readShipActorValue(ship, av.boostSpeed);
    const float boostRechargeRate = readShipActorValue(ship, av.boostRechargeRate);

    char message[896]{};
    std::snprintf(
        message,
        sizeof(message),
        "Ship propulsion state: ship=0x%08X pilotConfirmed=yes landed=%s docked=%s cluster=0x%llX throttleTarget=%.4f throttleTargetReadable=%s effectiveThrottle=%.4f effectiveThrottleReadable=%s velocity=%.4f velocityReadable=%s boostFuelCurrent=%.4f boostFuelPermanent=%.4f boostSpeed=%.4f boostRechargeRate=%.4f maxForwardSpeed=%.4f forwardSpeedMult=%.4f forwardForcePerPower=%.4f maxForwardAccel=%.4f lanes=+0x68/+0x6C/+0x70 behavior=passive-read-only",
        static_cast<unsigned int>(ship->GetFormID()),
        state.landed ? "yes" : "no",
        state.docked ? "yes" : "no",
        static_cast<unsigned long long>(cluster),
        state.throttleTarget,
        state.throttleTargetReadable ? "yes" : "no",
        state.effectiveThrottle,
        state.effectiveThrottleReadable ? "yes" : "no",
        state.velocity,
        state.velocityReadable ? "yes" : "no",
        state.boostFuelCurrent,
        state.boostFuelPermanent,
        state.boostSpeed,
        boostRechargeRate,
        state.maxForwardSpeed,
        forwardSpeedMult,
        forwardForcePerPower,
        maxForwardAcceleration);
    log(message);
    return state;
}

bool sds::GameStateAdapter::refreshPlayerHealth()
{
    if (!worldAvailable()) {
        return false;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto ratioValue = readPlayerHealthRatio(player);
    if (!ratioValue) {
        return false;
    }

    const float ratio = *ratioValue;
    _lastHealthRatio = ratio;

    GameEvent event{};
    event.type = GameEventType::PlayerHealthChanged;
    event.value = ratio;
    event.when = std::chrono::steady_clock::now();

    char message[160]{};
    std::snprintf(
        message,
        sizeof(message),
        "Game state: player health %.1f%% source=fresh-context-refresh",
        ratio * 100.0F);
    return emit(event, message);
}

void sds::GameStateAdapter::pollHealth()
{
    if (!worldAvailable()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < _nextHealthPoll) {
        return;
    }
    _nextHealthPoll = now + kHealthPollInterval;
    if (_fireMarkerTraceEnabled) {
        if (_fireMarkerCapture.expired(now)) {
            _fireMarkerCapture.disarm();
            clearFireMarkerSinks();
            log("Fire marker trace: capture window closed; player animation graph sinks released");
        } else {
            refreshFireMarkerSinks();
        }
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }

    if (_incomingDamageDiagnosticEnabled && !_tesHitSessionDiscoveryAttempted) {
        _tesHitSessionDiscoveryAttempted = true;
        const bool registered = ensureTESHitSinkRegistered(player, "incoming-damage-startup");
        log(registered ?
            "Incoming damage diagnostic: TESHit source registration=PASS windowMs=300 duplicateWindowMs=5 nullTargetFallbackMs=125 maxActive=8 hapticOutput=health-confirmed-cumulative-centered" :
            "Incoming damage diagnostic: TESHit source registration=FAIL failClosed=yes hapticOutput=disabled");
    }

    const auto ratioValue = readPlayerHealthRatio(player);
    if (!ratioValue) {
        if (_incomingDamageDiagnosticEnabled) {
            drainIncomingDamageCorrelations(now);
        }
        return;
    }
    const float ratio = *ratioValue;

    if (_incomingDamageDiagnosticEnabled) {
        std::optional<IncomingDamageConfirmation> confirmation;
        std::optional<IncomingDamageFallbackConfirmation> fallbackConfirmation;
        bool previousPeriodicHealthAvailable = false;
        float previousPeriodicHealth = 0.0F;
        bool primaryIncidentActive = false;
        {
            std::scoped_lock lock(_incomingDamageMutex);
            previousPeriodicHealthAvailable = _lastPeriodicHealthAvailable;
            previousPeriodicHealth = _lastPeriodicHealthRatio;
            primaryIncidentActive = _incomingDamageDiagnostic.activeCount() != 0;
            _incomingDamageDiagnostic.observeHealthSample(ratio, now);
            if (previousPeriodicHealthAvailable) {
                confirmation = _incomingDamageDiagnostic.confirmHealthDrop(
                    previousPeriodicHealth, ratio, now);
                if (primaryIncidentActive) {
                    _incomingDamageNullTargetFallback.clear();
                } else {
                    fallbackConfirmation = _incomingDamageNullTargetFallback.confirmHealthDrop(
                        previousPeriodicHealth, ratio, now, primaryIncidentActive);
                }
            }
            _lastPeriodicHealthAvailable = true;
            _lastPeriodicHealthRatio = ratio;
            _lastPeriodicHealthSampleAt = now;
        }

        if (confirmation) {
            GameEvent confirmed{};
            confirmed.type = GameEventType::IncomingDamage;
            confirmed.value = confirmation->healthLossRatio;
            confirmed.when = now;

            char attackerDirection[64]{};
            if (confirmation->attackerDirection.valid) {
                const auto label = incomingDirectionLabelName(confirmation->attackerDirection.label);
                std::snprintf(
                    attackerDirection,
                    sizeof(attackerDirection),
                    "%.*s/%.1f",
                    static_cast<int>(label.size()),
                    label.data(),
                    confirmation->attackerDirection.angleDegrees);
            } else {
                std::snprintf(attackerDirection, sizeof(attackerDirection), "Unknown");
            }

            char confirmationMessage[512]{};
            std::snprintf(
                confirmationMessage,
                sizeof(confirmationMessage),
                "Incoming damage confirmed: incidentSeq=%llu healthLoss=%.4f severityPct=%.1f incidents=%u mergedEvents=%u attribution=%s attackerDir=%s confirmationMode=cumulative-prepoll haptic=requested-centered",
                static_cast<unsigned long long>(confirmation->sequence),
                static_cast<double>(confirmation->healthLossRatio),
                static_cast<double>(confirmation->healthLossRatio * 100.0F),
                confirmation->incidentCount,
                confirmation->mergedEventCount,
                confirmation->attributionAmbiguous ? "ambiguous" : "isolated",
                attackerDirection);
            (void)emit(confirmed, confirmationMessage);
        } else if (fallbackConfirmation) {
            GameEvent confirmed{};
            confirmed.type = GameEventType::IncomingDamage;
            confirmed.value = fallbackConfirmation->healthLossRatio;
            confirmed.when = now;

            char confirmationMessage[512]{};
            std::snprintf(
                confirmationMessage,
                sizeof(confirmationMessage),
                "Incoming damage confirmed: fallbackSeq=%llu healthLoss=%.4f severityPct=%.1f incidents=0 mergedEvents=0 attribution=fallback attackerDir=Unknown confirmationMode=null-target-recent-teshit fallbackWindowMs=125 haptic=requested-centered",
                static_cast<unsigned long long>(fallbackConfirmation->sequence),
                static_cast<double>(fallbackConfirmation->healthLossRatio),
                static_cast<double>(fallbackConfirmation->healthLossRatio * 100.0F));
            (void)emit(confirmed, confirmationMessage);
        }

        drainIncomingDamageCorrelations(now);
    }

    if (_lastHealthRatio >= 0.0F && std::fabs(ratio - _lastHealthRatio) < kHealthMaterialDelta) {
        return;
    }
    _lastHealthRatio = ratio;

    GameEvent event{};
    event.type = GameEventType::PlayerHealthChanged;
    event.value = ratio;
    event.when = now;

    char message[128]{};
    std::snprintf(message, sizeof(message), "Game state: player health %.1f%%", ratio * 100.0F);
    emit(event, message);
}

RE::BSEventNotifyControl sds::GameStateAdapter::ProcessEvent(
    const RE::ActorItemEquipped::Event& event,
    RE::BSTEventSource<RE::ActorItemEquipped::Event>*)
{
    if (!worldAvailable()) {
        return RE::BSEventNotifyControl::kContinue;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || event.actor.get() != player || !event.item ||
        event.item->GetFormType() != RE::FormType::kWEAP) {
        return RE::BSEventNotifyControl::kContinue;
    }

    GameEvent normalized{};
    normalized.type = GameEventType::WeaponEquipped;
    normalized.formId = event.item->GetFormID();
    normalized.when = std::chrono::steady_clock::now();

    const auto* weapon = static_cast<const RE::TESObjectWEAP*>(event.item);
    const char* editorId = event.item->GetFormEditorID();
    const char* fullName = weapon ? weapon->GetFullName() : nullptr;

    std::string identity;
    if (editorId && *editorId) {
        identity += editorId;
    }
    if (fullName && *fullName) {
        if (!identity.empty()) {
            identity += '|';
        }
        identity += fullName;
    }
    const bool customProfile =
        isCustomWeaponProfileMatch(identity);

    const auto* profile =
        findWeaponProfile(identity);

    copyText(
        normalized,
        profile && !customProfile
            ? profile->name
            : std::string_view(identity));

    disarmTargetHitDiagnostic();
    disarmTESHitDiagnostic();
    if (profile && profile->triggerFamily == WeaponTriggerFamily::Melee) {
        runTESHitSourceDiscovery(player, profile->name, normalized.formId);
    }

    if (_fireMarkerTraceEnabled) {
        const auto markerWeapon = profile ? profile->name : std::string_view(identity);
        _fireMarkerCapture.arm(
            markerWeapon.empty() ? std::string_view("<unknown>") : markerWeapon,
            normalized.when,
            kFireMarkerCaptureWindow);
        refreshFireMarkerSinks();
    }

    std::ostringstream diagnostic;
    diagnostic << "Game state: player weapon equipped form=0x"
               << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << normalized.formId
               << std::dec << " identity='" << (identity.empty() ? "<unknown>" : identity) << "'";
    if (profile) {
        diagnostic << " profile='" << profile->name << "'"
                   << " family='" << profile->familyLabel << "'"
                   << " intensity='" << weaponIntensityName(profile->intensity) << "'"
                   << " cadence='" << profile->cadenceLabel << "'"
                   << " R2=" << static_cast<unsigned int>(profile->r2Rating);
    } else {
        diagnostic << " profile=unknown fallbackR2=4";
    }
    emit(normalized, diagnostic.str());
    if (_fireMarkerTraceEnabled) {
        log("Fire marker trace: capture window armed for equipped weapon; fire immediately to record exact shot/beam markers");
    }
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl sds::GameStateAdapter::ProcessEvent(
    const RE::BSAnimationGraphEvent& event,
    RE::BSTEventSource<RE::BSAnimationGraphEvent>* source)
{
    try {
        if (!worldAvailable() || !_fireMarkerTraceEnabled) {
            return RE::BSEventNotifyControl::kContinue;
        }

        const auto* player = RE::PlayerCharacter::GetSingleton();
        const auto now = std::chrono::steady_clock::now();
        AnimationGraphEventSnapshot snapshot{};
        if (!safeSnapshotAnimationGraphEvent(event, snapshot)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        const auto record = _fireMarkerCapture.tryCapture(
            player && snapshot.holder == player,
            now,
            snapshot.tag.data(),
            snapshot.payload.data(),
            reinterpret_cast<std::uintptr_t>(source));
        if (!record) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (record->kind == FireMarkerRecordKind::LimitReached) {
            log("Fire marker trace: strict 1024-line capture limit reached; remaining markers suppressed");
            return RE::BSEventNotifyControl::kContinue;
        }

        char message[768]{};
        std::snprintf(
            message,
            sizeof(message),
            "Fire marker trace: seq=%llu tUs=%lld weapon='%.*s' tag='%.*s' payload='%.*s' source=%p",
            static_cast<unsigned long long>(record->sequence),
            static_cast<long long>(record->elapsedUs),
            120,
            record->weapon.data(),
            240,
            record->tag.data(),
            240,
            record->payload.data(),
            reinterpret_cast<void*>(record->source));
        log(message);
    } catch (...) {
        // Marker tracing is strictly observational and must never affect input.
    }
    return RE::BSEventNotifyControl::kContinue;
}


RE::BSEventNotifyControl sds::GameStateAdapter::ProcessEvent(
    const RE::TargetHitEvent&,
    RE::BSTEventSource<RE::TargetHitEvent>* source)
{
    std::uint64_t sequence = 0;
    std::int64_t elapsedUs = 0;
    std::array<char, 121> weapon{};
    {
        std::scoped_lock lock(_targetHitMutex);
        if (!_targetHitSinkRegistered || !_targetHitDiagnosticActive ||
            source != _targetHitSource) {
            return RE::BSEventNotifyControl::kContinue;
        }
        sequence = ++_targetHitSequence;
        elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - _targetHitArmedAt).count();
        weapon = _targetHitWeapon;
    }

    char message[512]{};
    std::snprintf(
        message,
        sizeof(message),
        "Melee TargetHit diagnostic: seq=%llu tUs=%lld weapon='%.*s' source=%p confirmed=TargetHitEvent",
        static_cast<unsigned long long>(sequence),
        static_cast<long long>(elapsedUs),
        120,
        weapon.data(),
        static_cast<void*>(source));
    log(message);
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl sds::GameStateAdapter::ProcessEvent(
    const RE::TESHitEvent& event,
    RE::BSTEventSource<RE::TESHitEvent>* source)
{
    if (!worldAvailable()) {
        return RE::BSEventNotifyControl::kContinue;
    }
    const auto now = std::chrono::steady_clock::now();
    bool meleeContextActive = false;
    std::uint64_t sequence = 0;
    std::int64_t elapsedUs = 0;
    std::array<char, 121> weapon{};
    std::uint32_t equippedMeleeFormId = 0;
    {
        std::scoped_lock lock(_tesHitMutex);
        if (!_tesHitSinkRegistered || source != _tesHitSource) {
            return RE::BSEventNotifyControl::kContinue;
        }
        meleeContextActive = _tesHitDiagnosticActive;
        if (meleeContextActive) {
            sequence = ++_tesHitSequence;
            elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                now - _tesHitArmedAt).count();
            weapon = _tesHitWeapon;
            equippedMeleeFormId = _tesHitWeaponFormId;
        }
    }

    if (!meleeContextActive && !_incomingDamageDiagnosticEnabled) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* target = event.target.get();
    auto* cause = event.cause.get();
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto targetForm = target ? target->GetFormID() : 0;
    const auto causeForm = cause ? cause->GetFormID() : 0;
    const bool targetIsPlayer = player && target == player;
    const bool causeIsPlayer = player && cause == player;

    std::uint32_t aggressorHandle = 0;
    std::uint32_t hitTargetHandle = 0;
    std::uint32_t sourceRefHandle = 0;
    const RE::TESForm* hitWeaponObject = nullptr;
    const RE::TESAmmo* ammo = nullptr;
    std::uint64_t attackData = 0;
    float impactX = 0.0F;
    float impactY = 0.0F;
    float impactZ = 0.0F;
    if (event.usesHitData) {
        std::memcpy(&aggressorHandle, &event.hitData.aggressor,
            (std::min)(sizeof(aggressorHandle), sizeof(event.hitData.aggressor)));
        std::memcpy(&hitTargetHandle, &event.hitData.target,
            (std::min)(sizeof(hitTargetHandle), sizeof(event.hitData.target)));
        std::memcpy(&sourceRefHandle, &event.hitData.sourceRef,
            (std::min)(sizeof(sourceRefHandle), sizeof(event.hitData.sourceRef)));
        hitWeaponObject = event.hitData.weapon.object;
        ammo = event.hitData.ammo;
        attackData = event.hitData.attackData;
        impactX = event.hitData.impactData.location.x;
        impactY = event.hitData.impactData.location.y;
        impactZ = event.hitData.impactData.location.z;
    }

    const char* material = event.material.c_str();
    if (_incomingDamageDiagnosticEnabled && player && target == nullptr && !causeIsPlayer) {
        std::uint64_t fallbackSequence = 0;
        bool fallbackArmed = false;
        {
            std::scoped_lock lock(_incomingDamageMutex);
            fallbackSequence = ++_incomingDamageSequence;
            fallbackArmed = _incomingDamageNullTargetFallback.armTesHit(
                fallbackSequence, now, target == nullptr, causeIsPlayer, _hudMenuOpen);
        }
        if (fallbackArmed) {
            char fallbackMessage[512]{};
            std::snprintf(
                fallbackMessage,
                sizeof(fallbackMessage),
                "Incoming damage null-target fallback: armed seq=%llu target=null causeIsPlayer=no sourceForm=0x%08X projectileForm=0x%08X usesHitData=%s fallbackWindowMs=125 hud=active",
                static_cast<unsigned long long>(fallbackSequence),
                static_cast<unsigned int>(event.sourceFormID),
                static_cast<unsigned int>(event.projectileFormID),
                event.usesHitData ? "yes" : "no");
            log(fallbackMessage);
        }
    }

    if (_incomingDamageDiagnosticEnabled && targetIsPlayer && player) {
        IncomingDamageEvidence evidence{};
        evidence.when = now;
        evidence.targetFormId = targetForm;
        evidence.causePresent = cause != nullptr;
        evidence.causeFormId = causeForm;
        evidence.sourceFormId = event.sourceFormID;
        evidence.projectileFormId = event.projectileFormID;
        evidence.usesHitData = event.usesHitData;
        evidence.aggressorHandle = aggressorHandle;
        evidence.hitTargetHandle = hitTargetHandle;
        evidence.sourceRefHandle = sourceRefHandle;
        evidence.hitWeaponFormId = hitWeaponObject ? hitWeaponObject->GetFormID() : 0;
        evidence.ammoFormId = ammo ? ammo->GetFormID() : 0;
        evidence.attackData = attackData;

        if (material) {
            const auto materialLength = std::strlen(material);
            const auto materialCount = (std::min)(materialLength, evidence.material.size() - 1);
            std::memcpy(evidence.material.data(), material, materialCount);
            evidence.material[materialCount] = '\0';
        }

        if (const auto direct = readPlayerHealthRatio(player)) {
            evidence.directHealthAvailable = true;
            evidence.directHealthRatio = *direct;
        }

        const auto playerPos = player->GetPosition();
        const float playerHeading = player->GetAngleZ();
        const IncomingWorldPoint playerPoint{ playerPos.x, playerPos.y, playerPos.z };
        if (event.usesHitData) {
            evidence.impact = { impactX, impactY, impactZ };
            evidence.impactPresent = true;
            evidence.impactDirection = calculateIncomingDirection(
                playerPoint, playerHeading, evidence.impact);
        }
        if (cause && cause != player) {
            const auto causePos = cause->GetPosition();
            evidence.attackerDirection = calculateIncomingDirection(
                playerPoint,
                playerHeading,
                { causePos.x, causePos.y, causePos.z });
        }

        IncomingDamageBeginResult beginResult{};
        {
            std::scoped_lock lock(_incomingDamageMutex);
            _incomingDamageNullTargetFallback.clear();
            evidence.sequence = ++_incomingDamageSequence;
            if (_lastPeriodicHealthAvailable && _lastPeriodicHealthSampleAt <= now) {
                evidence.prePollHealthAvailable = true;
                evidence.prePollHealthRatio = _lastPeriodicHealthRatio;
                evidence.prePollAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - _lastPeriodicHealthSampleAt).count();
            }
            beginResult = _incomingDamageDiagnostic.beginOrMergeHit(evidence);
        }
        const bool recordAccepted = beginResult.disposition != IncomingDamageBeginDisposition::Dropped;
        const char* recordDisposition = beginResult.disposition == IncomingDamageBeginDisposition::Started ? "started" :
            beginResult.disposition == IncomingDamageBeginDisposition::Merged ? "merged" : "dropped";

        char directHealth[32]{};
        std::snprintf(
            directHealth,
            sizeof(directHealth),
            evidence.directHealthAvailable ? "%.4f" : "unavailable",
            evidence.directHealthRatio);
        char prePollHealth[32]{};
        std::snprintf(
            prePollHealth,
            sizeof(prePollHealth),
            evidence.prePollHealthAvailable ? "%.4f" : "unavailable",
            evidence.prePollHealthRatio);
        char impactText[96]{};
        if (evidence.impactPresent) {
            std::snprintf(
                impactText,
                sizeof(impactText),
                "(%.3f,%.3f,%.3f)",
                evidence.impact.x,
                evidence.impact.y,
                evidence.impact.z);
        } else {
            std::snprintf(impactText, sizeof(impactText), "unavailable");
        }
        char impactDirection[64]{};
        if (evidence.impactDirection.valid) {
            const auto label = incomingDirectionLabelName(evidence.impactDirection.label);
            std::snprintf(
                impactDirection,
                sizeof(impactDirection),
                "%.*s/%.1f",
                static_cast<int>(label.size()),
                label.data(),
                evidence.impactDirection.angleDegrees);
        } else {
            std::snprintf(impactDirection, sizeof(impactDirection), "Unknown");
        }
        char attackerDirection[64]{};
        if (evidence.attackerDirection.valid) {
            const auto label = incomingDirectionLabelName(evidence.attackerDirection.label);
            std::snprintf(
                attackerDirection,
                sizeof(attackerDirection),
                "%.*s/%.1f",
                static_cast<int>(label.size()),
                label.data(),
                evidence.attackerDirection.angleDegrees);
        } else {
            std::snprintf(attackerDirection, sizeof(attackerDirection), "Unknown");
        }
        char disagreement[32]{};
        if (evidence.impactDirection.valid && evidence.attackerDirection.valid) {
            std::snprintf(
                disagreement,
                sizeof(disagreement),
                "%.1f",
                incomingDirectionDisagreementDegrees(
                    evidence.impactDirection,
                    evidence.attackerDirection));
        } else {
            std::snprintf(disagreement, sizeof(disagreement), "unavailable");
        }

        char incomingMessage[1400]{};
        std::snprintf(
            incomingMessage,
            sizeof(incomingMessage),
            "Incoming damage diagnostic: seq=%llu targetIsPlayer=yes causePresent=%s causeForm=0x%08X sourceForm=0x%08X projectileForm=0x%08X usesHitData=%s weaponForm=0x%08X ammoForm=0x%08X material='%.*s' healthAtEvent=%s prePollHealth=%s prePollAgeMs=%lld impact=%s impactDir=%s attackerDir=%s disagreementDeg=%s recordAccepted=%s disposition=%s incidentSeq=%llu mergedEvents=%u",
            static_cast<unsigned long long>(evidence.sequence),
            evidence.causePresent ? "yes" : "no",
            static_cast<unsigned int>(evidence.causeFormId),
            static_cast<unsigned int>(evidence.sourceFormId),
            static_cast<unsigned int>(evidence.projectileFormId),
            evidence.usesHitData ? "yes" : "no",
            static_cast<unsigned int>(evidence.hitWeaponFormId),
            static_cast<unsigned int>(evidence.ammoFormId),
            80,
            evidence.material.data(),
            directHealth,
            prePollHealth,
            static_cast<long long>(evidence.prePollAgeMs),
            impactText,
            impactDirection,
            attackerDirection,
            disagreement,
            recordAccepted ? "yes" : "no",
            recordDisposition,
            static_cast<unsigned long long>(beginResult.incidentSequence),
            beginResult.mergedEventCount);
        log(incomingMessage);
    }

    if (meleeContextActive) {
        char message[1200]{};
        std::snprintf(
            message,
            sizeof(message),
            "Melee TESHit diagnostic: seq=%llu tUs=%lld weapon='%.*s' source=%p target=%p targetForm=0x%08X targetIsPlayer=%s cause=%p causeForm=0x%08X causeIsPlayer=%s sourceFormID=0x%08X projectileFormID=0x%08X usesHitData=%s material='%.*s' hitDataAggressorHandle=0x%08X hitDataTargetHandle=0x%08X hitDataSourceRefHandle=0x%08X hitDataWeaponObject=%p hitDataAmmo=%p attackData=0x%016llX impact=(%.3f,%.3f,%.3f)",
            static_cast<unsigned long long>(sequence),
            static_cast<long long>(elapsedUs),
            120,
            weapon.data(),
            static_cast<void*>(source),
            static_cast<void*>(target),
            static_cast<unsigned int>(targetForm),
            targetIsPlayer ? "yes" : "no",
            static_cast<void*>(cause),
            static_cast<unsigned int>(causeForm),
            causeIsPlayer ? "yes" : "no",
            static_cast<unsigned int>(event.sourceFormID),
            static_cast<unsigned int>(event.projectileFormID),
            event.usesHitData ? "yes" : "no",
            80,
            material ? material : "",
            aggressorHandle,
            hitTargetHandle,
            sourceRefHandle,
            static_cast<const void*>(hitWeaponObject),
            static_cast<const void*>(ammo),
            static_cast<unsigned long long>(attackData),
            impactX,
            impactY,
            impactZ);
        log(message);

        if (confirmedPlayerMeleeImpact(
                target != nullptr,
                targetIsPlayer,
                causeIsPlayer,
                event.sourceFormID,
                event.projectileFormID,
                equippedMeleeFormId)) {
            GameEvent normalized{};
            normalized.type = GameEventType::MeleeImpact;
            normalized.formId = event.sourceFormID;
            normalized.when = std::chrono::steady_clock::now();
            copyText(normalized, weapon.data());
            const bool emitted = emit(normalized);
            char delivery[512]{};
            std::snprintf(
                delivery,
                sizeof(delivery),
                emitted ?
                    "Melee impact delivery: stage=normalized-emitted tesHitSeq=%llu eventWhenUs=%lld weapon='%.*s' form=0x%08X" :
                    "Melee impact delivery: stage=normalized-rejected tesHitSeq=%llu eventWhenUs=%lld weapon='%.*s' form=0x%08X",
                static_cast<unsigned long long>(sequence),
                static_cast<long long>(hapticEventTimestampMicros(normalized.when)),
                120,
                weapon.data(),
                static_cast<unsigned int>(normalized.formId));
            log(delivery);
        }

    }

    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl sds::GameStateAdapter::ProcessEvent(
    const LandVehicleDriverEnterExitRawEvent& event,
    RE::BSTEventSource<LandVehicleDriverEnterExitRawEvent>*)
{
    std::array<std::byte, LandVehicleDriverEventSnapshot::kPayloadBytes> payload{};
    SIZE_T bytesRead = 0;
    const bool readable = ::ReadProcessMemory(
        ::GetCurrentProcess(),
        static_cast<const void*>(&event),
        payload.data(),
        payload.size(),
        &bytesRead) != FALSE && bytesRead == payload.size();
    if (!readable) {
        log("Land vehicle recon: driver-event payload unreadable; ignored behavior=passive-read-only");
        return RE::BSEventNotifyControl::kContinue;
    }

    const auto snapshot = decodeLandVehicleDriverEvent(payload);
    const auto now = std::chrono::steady_clock::now();
    std::uint64_t sequence = 0;
    {
        std::scoped_lock guard(_landVehicleReconMutex);
        _landVehicleLastDriverEvent = snapshot;
        sequence = ++_landVehicleDriverEventSequence;
        _landVehicleLastDriverEventAt = now;
    }
    if (!worldAvailable()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    bool cameraVehicle = false;
    try {
        if (auto* camera = RE::PlayerCamera::GetSingleton()) {
            cameraVehicle = camera->QCameraEquals(RE::CameraState::kVehicle);
        }
    } catch (...) {
        cameraVehicle = false;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    const auto moduleSize = moduleImageSize(moduleBase);
    std::uint32_t objectLikeMask = 0;
    int playerLane = -1;
    for (std::size_t i = 0; i < snapshot.qwords.size(); ++i) {
        if (!snapshot.pointerLike[i]) {
            continue;
        }
        const auto candidate = static_cast<std::uintptr_t>(snapshot.qwords[i]);
        if (candidate == playerAddress) {
            playerLane = static_cast<int>(i);
        }
        std::uintptr_t candidateVtable = 0;
        if (safeReadValue(candidate, candidateVtable) &&
            isModuleAddress(candidateVtable, moduleBase, moduleSize)) {
            objectLikeMask |= (1U << i);
        }
    }

    char message[1400]{};
    std::snprintf(
        message,
        sizeof(message),
        "Land vehicle recon: driver-event raw seq=%llu fingerprint=0x%016llX cameraVehicle=%s cameraRole=corroboration-only player=%p playerLane=%d pointerMask=%02X objectLikeMask=%02X q0=%016llX q1=%016llX q2=%016llX q3=%016llX q4=%016llX q5=%016llX q6=%016llX q7=%016llX semanticPromotion=none behavior=passive-read-only controllerOutput=none",
        static_cast<unsigned long long>(sequence),
        static_cast<unsigned long long>(snapshot.fingerprint),
        cameraVehicle ? "yes" : "no",
        static_cast<void*>(player),
        playerLane,
        [&snapshot]() {
            std::uint32_t mask = 0;
            for (std::size_t i = 0; i < snapshot.pointerLike.size(); ++i) {
                if (snapshot.pointerLike[i]) mask |= (1U << i);
            }
            return mask;
        }(),
        objectLikeMask,
        static_cast<unsigned long long>(snapshot.qwords[0]),
        static_cast<unsigned long long>(snapshot.qwords[1]),
        static_cast<unsigned long long>(snapshot.qwords[2]),
        static_cast<unsigned long long>(snapshot.qwords[3]),
        static_cast<unsigned long long>(snapshot.qwords[4]),
        static_cast<unsigned long long>(snapshot.qwords[5]),
        static_cast<unsigned long long>(snapshot.qwords[6]),
        static_cast<unsigned long long>(snapshot.qwords[7]));
    log(message);
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl sds::GameStateAdapter::ProcessEvent(
    const RE::MenuOpenCloseEvent& event,
    RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
    GameEvent normalized{};
    normalized.type = event.opening ? GameEventType::MenuOpened : GameEventType::MenuClosed;
    normalized.when = std::chrono::steady_clock::now();
    const char* name = event.menuName.c_str();

    ShipPilotTransition shipTransition = ShipPilotTransition::None;
    if (name) {
        copyText(normalized, name);
        _worldGate.observeMenu(name, event.opening);
        const bool mainMenu = std::strcmp(name, "MainMenu") == 0;
        const bool loadingMenu = std::strcmp(name, "LoadingMenu") == 0;
        if (event.opening && (mainMenu || loadingMenu)) {
            _shipPropulsionProbe.reset();
            _fireMarkerCapture.disarm();
            clearFireMarkerSinks();
            unregisterTargetHitSink();
            unregisterTESHitSink();
            _tesHitSessionDiscoveryAttempted = false;
            _lastHealthRatio = -1.0F;
            {
                std::scoped_lock healthGuard(_incomingDamageMutex);
                _incomingDamageDiagnostic = {};
                _incomingDamageNullTargetFallback.clear();
                _lastIncomingDamageDropCountLogged = 0;
                _lastPeriodicHealthAvailable = false;
                _lastPeriodicHealthRatio = 0.0F;
                _lastPeriodicHealthSampleAt = {};
            }
            log("Game state: world access suspended; player graph sinks and world evidence cleared");
        }
        if (mainMenu || loadingMenu) {
            _nextHealthPoll = normalized.when;
            _nextLandVehicleReconPoll = normalized.when;
            _nextShipPropulsionPoll = {};
        }
        if (mainMenu && event.opening) {
            shipTransition = _shipPilotContext.piloting() ? ShipPilotTransition::Invalidated : ShipPilotTransition::None;
            _shipPilotContext.reset();
        } else {
            shipTransition = _shipPilotContext.observeMenu(name, event.opening);
        }
        if (std::strcmp(name, "MonocleMenu") == 0) {
            _monocleOpen = event.opening;
        }
        if (loadingMenu || (mainMenu && event.opening)) {
            {
                std::scoped_lock landVehicleGuard(_landVehicleReconMutex);
                if (loadingMenu) {
                    _landVehicleLoading = event.opening;
                }
                if (event.opening) {
                    (void)_landVehicleReconProbe.invalidateForLoading(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(normalized.when.time_since_epoch()).count()));
                    _landVehicleTelemetryProbe.reset();
                    _landVehicleLatestTelemetry = {};
                    _landVehiclePhysicsProbe.reset();
                    _landVehicleLatestPhysics = {};
                    _landVehicleLastDriverEventAt = {};
                    _landVehicleLastPolledDriverEventSequence = _landVehicleDriverEventSequence;
                    _landVehicleCorrelationArmed.store(false, std::memory_order_release);
                }
            }
            if (event.opening) {
                clearLandVehicleProductionState(normalized.when, true);
                log("Land vehicle recon: INVALIDATE source=world-teardown freshEvidenceRequired=yes controllerOutput=none");
            } else {
                log("Land vehicle recon: LoadingMenu closed freshEvidenceRequired=yes controllerOutput=none");
            }
        }
        if (std::strcmp(name, "HUDMenu") == 0) {
            std::scoped_lock lock(_incomingDamageMutex);
            _hudMenuOpen = event.opening;
            if (!event.opening) {
                _incomingDamageNullTargetFallback.clear();
            }
        }
    }
    emit(normalized, std::string("Game state: menu ") + (event.opening ? "opened: " : "closed: ") + (name ? name : "<unknown>"));

    if (shipTransition != ShipPilotTransition::None) {
        _shipPropulsionProbe.reset();
        _nextShipPropulsionPoll = {};
        _nextShipLightbarSeatPoll = {};

        _lastShipHealthRatio = -1.0F;
        _lastShipHealthFormId = 0;
    }
    if (shipTransition == ShipPilotTransition::Entered || shipTransition == ShipPilotTransition::Resumed) {
        {
            std::scoped_lock landVehicleGuard(_landVehicleReconMutex);
            _landVehicleReconProbe.reset();
            _landVehicleTelemetryProbe.reset();
            _landVehicleLatestTelemetry = {};
            _landVehiclePhysicsProbe.reset();
            _landVehicleLatestPhysics = {};
            _landVehicleLastDriverEventAt = {};
            _landVehicleLastPolledDriverEventSequence = _landVehicleDriverEventSequence;
            _landVehicleCorrelationArmed.store(false, std::memory_order_release);
        }
        clearLandVehicleProductionState(normalized.when, true);
        log("Land vehicle recon: CLEAR reason=ship-pilot-authority controllerOutput=none");
    }

    GameEvent shipEvent{};
    shipEvent.when = normalized.when;
    switch (shipTransition) {
    case ShipPilotTransition::Entered:
        shipEvent.type = GameEventType::ShipPilotEntered;
        copyText(shipEvent, "SpaceshipHudMenu");
        emit(shipEvent);
        break;
    case ShipPilotTransition::Exited:
        shipEvent.type = GameEventType::ShipPilotExited;
        copyText(shipEvent, "SpaceshipHudMenu");
        emit(shipEvent);
        break;
    case ShipPilotTransition::Invalidated:
        shipEvent.type = GameEventType::ShipPilotInvalidated;
        copyText(shipEvent, name ? name : "LoadingMenu");
        emit(shipEvent);
        break;
    case ShipPilotTransition::Resumed:
        shipEvent.type = GameEventType::ShipPilotResumed;
        copyText(shipEvent, "LoadingMenu");
        emit(shipEvent);
        break;
    case ShipPilotTransition::ExitedAfterLoad:
        shipEvent.type = GameEventType::ShipPilotExited;
        copyText(shipEvent, "LoadingMenu");
        emit(shipEvent);
        break;
    case ShipPilotTransition::None:
        break;
    }

    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl sds::GameStateAdapter::ProcessEvent(
    const RE::BGSAppPausedEvent& event,
    RE::BSTEventSource<RE::BGSAppPausedEvent>*)
{
    GameEvent normalized{};
    normalized.type = event.paused ? GameEventType::GamePaused : GameEventType::GameUnpaused;
    normalized.when = std::chrono::steady_clock::now();
    emit(normalized, event.paused ? "Game state: paused" : "Game state: unpaused");
    return RE::BSEventNotifyControl::kContinue;
}

bool sds::GameStateAdapter::emit(GameEvent event, std::string_view diagnostic)
{
    if (!_emit || !_emit(std::move(event))) {
        log("Game state: event queue full; event dropped");
        return false;
    }
    if (!diagnostic.empty()) {
        log(diagnostic);
    }
    return true;
}

void sds::GameStateAdapter::log(std::string_view message) const noexcept
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
