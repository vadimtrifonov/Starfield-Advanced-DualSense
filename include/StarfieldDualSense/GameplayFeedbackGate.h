#pragma once

#include <StarfieldDualSense/Types.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace sds
{
    inline constexpr std::array<std::string_view, 8> kGameplayBlockingMenus{
        "DataMenu", "PauseMenu", "LoadingMenu", "MainMenu", "GalaxyStarMapMenu", "FaderMenu",
        "CharGenMenu", "ChargenMenu"
    };

    [[nodiscard]] constexpr std::uint8_t gameplayBlockingMenuBit(std::string_view menu) noexcept
    {
        for (std::size_t i = 0; i < kGameplayBlockingMenus.size(); ++i) {
            if (menu == kGameplayBlockingMenus[i]) {
                return static_cast<std::uint8_t>(1u << i);
            }
        }
        return 0;
    }

    // Menu state belongs to gameplay, not to the current weapon or vehicle.
    class GameplayFeedbackGate
    {
    public:
        [[nodiscard]] bool handle(const GameEvent& event) noexcept
        {
            if (event.type == GameEventType::GamePaused || event.type == GameEventType::GameUnpaused) {
                _paused = event.type == GameEventType::GamePaused;
                return true;
            }
            if (event.type != GameEventType::MenuOpened && event.type != GameEventType::MenuClosed) {
                return false;
            }
            std::size_t length = 0;
            while (length < event.text.size() && event.text[length] != '\0') {
                ++length;
            }
            const auto bit = gameplayBlockingMenuBit({event.text.data(), length});
            if (event.type == GameEventType::MenuOpened) {
                _menus |= bit;
            } else {
                _menus &= static_cast<std::uint8_t>(~bit);
            }
            return bit != 0;
        }

        [[nodiscard]] bool blocked() const noexcept { return _paused || _menus != 0; }

        // A fade may be part of takeoff or touchdown, not a request to mute it.
        [[nodiscard]] bool shipTransitionBlocked() const noexcept
        {
            return _paused || (_menus & ~gameplayBlockingMenuBit("FaderMenu")) != 0;
        }

    private:
        std::uint8_t _menus{};
        bool _paused{};
    };
}
