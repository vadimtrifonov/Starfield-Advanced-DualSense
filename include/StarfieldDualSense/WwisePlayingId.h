#pragma once

#include <cstdint>

namespace sds
{
    [[nodiscard]] bool isWwisePlayingIdActive(std::uint32_t playingId) noexcept;
    [[nodiscard]] bool stopWwisePlayingId(std::uint32_t playingId) noexcept;
}
