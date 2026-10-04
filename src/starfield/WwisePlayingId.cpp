#include <StarfieldDualSense/WwisePlayingId.h>

#include <RE/Starfield.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr REL::ID kAkPlayingManagerID{ 874878 };
    constexpr REL::ID kAkGetGameObjectFromPlayingID{ 151255 };
    constexpr std::array<std::uint8_t, 24> kQueryPrologue{
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
        0x8B, 0xF1, 0x8B, 0xFA, 0x48, 0x83, 0xC1, 0x18
    };
    constexpr std::uint64_t kAkInvalidGameObject = UINT64_MAX;
    using GetGameObjectFromPlayingIDFn = std::uint64_t (*)(void*, std::uint32_t);

    constexpr REL::ID kAkStopVoiceID{ 150360 };
    constexpr std::array<std::uint8_t, 16> kStopPrologue{
        0x85, 0xD2, 0x74, 0x6C, 0x48, 0x89, 0x5C, 0x24,
        0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89
    };
    constexpr std::uint32_t kAkActionStop = 0;
    constexpr std::uint32_t kAkCurveLinear = 4;

    using ExecuteActionOnPlayingIDFn = void (*)(
        std::uint32_t,
        std::uint32_t,
        std::int32_t,
        std::uint32_t);
}

bool sds::isWwisePlayingIdActive(std::uint32_t playingId) noexcept
{
    if (playingId == 0) {
        return false;
    }

    // CAkPlayingMgr's lookup holds its own lock and returns AK_INVALID_GAME_OBJECT
    // after the playing ID is retired. Confirmed against Starfield 1.16.244.0.
    static const GetGameObjectFromPlayingIDFn query = []() noexcept -> GetGameObjectFromPlayingIDFn {
        const auto address = kAkGetGameObjectFromPlayingID.address();
        const auto* code = reinterpret_cast<const std::uint8_t*>(address);
        if (!code || std::memcmp(code, kQueryPrologue.data(), kQueryPrologue.size()) != 0) {
            return nullptr;
        }
        return reinterpret_cast<GetGameObjectFromPlayingIDFn>(address);
    }();
    if (!query) {
        return false;
    }

    static const auto managerAddress = kAkPlayingManagerID.address();
    if (!managerAddress) {
        return false;
    }
    auto* const manager = *reinterpret_cast<void* const*>(managerAddress);
    return manager && query(manager, playingId) != kAkInvalidGameObject;
}

bool sds::stopWwisePlayingId(std::uint32_t playingId) noexcept
{
    if (playingId == 0) {
        return false;
    }

    static const ExecuteActionOnPlayingIDFn stop = []() noexcept -> ExecuteActionOnPlayingIDFn {
        const auto address = kAkStopVoiceID.address();
        const auto* code = reinterpret_cast<const std::uint8_t*>(address);
        if (!code || std::memcmp(code, kStopPrologue.data(), kStopPrologue.size()) != 0) {
            return nullptr;
        }
        return reinterpret_cast<ExecuteActionOnPlayingIDFn>(address);
    }();

    if (!stop) {
        return false;
    }

    stop(kAkActionStop, playingId, 0, kAkCurveLinear);
    return true;
}
