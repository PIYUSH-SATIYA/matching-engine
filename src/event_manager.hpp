
#pragma once

#include "order.hpp"

#include <functional>
#include <vector>
#include <utility>
#include <stdexcept>
#include <cstddef>

class EventManager {
public:

    // Callback type to receive EngineEvent
    using EventCallback = std::function<void(const EngineEvent&)>;

    EventManager() = default;

    ~EventManager() = default;

   

    // Register listeners (such as Logger and Benchmark)
    void registerListener(EventCallback listener) {

        // Prevent empty callbacks
        if (!listener) {
            throw std::invalid_argument("Cannot register an empty listener");
        }

        listeners_.push_back(std::move(listener));
    }

    // Publish/broadcast EngineEvent to all registered listeners
    void publish(const EngineEvent& event) const {

        for (const auto& listener : listeners_) {
            listener(event);
        }
    }

    // Return the number of registered listeners
    std::size_t listenerCount() const {
        return listeners_.size();
    }

 // Disable copying to maintain single-instance data flow
    EventManager(const EventManager&) = delete;
    EventManager& operator=(const EventManager&) = delete;
private:

    // Stores all registered listener callbacks
    std::vector<EventCallback> listeners_;
};
