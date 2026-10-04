#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace sds
{
    // UI/controller services remain usable while player and world objects are being replaced.
    class GameWorldGate
    {
    public:
        void snapshot(bool mainMenuOpen, bool loadingMenuOpen) noexcept
        {
            _blocked.store(static_cast<std::uint8_t>(
                (mainMenuOpen ? kMainMenu : 0) | (loadingMenuOpen ? kLoadingMenu : 0)),
                std::memory_order_release);
        }

        void observeMenu(std::string_view menu, bool opening) noexcept
        {
            const std::uint8_t bit = menu == "MainMenu" ? kMainMenu : menu == "LoadingMenu" ? kLoadingMenu : 0;
            if (opening) {
                _blocked.fetch_or(bit, std::memory_order_acq_rel);
            } else {
                _blocked.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
            }
        }

        [[nodiscard]] bool available() const noexcept
        {
            return _blocked.load(std::memory_order_acquire) == 0;
        }

        [[nodiscard]] bool mainMenuOpen() const noexcept
        {
            return (_blocked.load(std::memory_order_acquire) & kMainMenu) != 0;
        }

        void suspend() noexcept { _blocked.store(kMainMenu | kLoadingMenu, std::memory_order_release); }

    private:
        static constexpr std::uint8_t kMainMenu = 1, kLoadingMenu = 2;
        std::atomic<std::uint8_t> _blocked{kMainMenu | kLoadingMenu};
    };
}
