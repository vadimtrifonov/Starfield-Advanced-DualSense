#pragma once

#include <StarfieldDualSense/Types.h>

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace sds
{
    template <std::size_t Capacity>
    class EventQueue
    {
    public:
        static_assert(Capacity > 0);

        // Consumers track menu state from open/close edges, so dropping one leaves them
        // stale until the menu cycles again. Capacity limits only other events.
        bool push(GameEvent event)
        {
            const bool menuEdge =
                event.type == GameEventType::MenuOpened || event.type == GameEventType::MenuClosed;
            std::scoped_lock lock(_mutex);
            if (_stopped || (_queue.size() >= Capacity && !menuEdge)) {
                return false;
            }
            _queue.push_back(std::move(event));
            return true;
        }

        [[nodiscard]] std::optional<GameEvent> tryPop()
        {
            std::scoped_lock lock(_mutex);
            if (_queue.empty()) {
                return std::nullopt;
            }
            auto event = std::move(_queue.front());
            _queue.pop_front();
            return event;
        }

        void stop()
        {
            std::scoped_lock lock(_mutex);
            _stopped = true;
        }

        [[nodiscard]] bool stopped() const
        {
            std::scoped_lock lock(_mutex);
            return _stopped;
        }

    private:
        mutable std::mutex _mutex;
        std::deque<GameEvent> _queue;
        bool _stopped{ false };
    };
}
