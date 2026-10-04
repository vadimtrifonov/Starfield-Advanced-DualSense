#include <StarfieldDualSense/RemoteVoAudioPipeline.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

namespace
{
    void check(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAIL " << message << '\n';
            std::abort(); // Do not hang in a future or pipeline destructor after a timeout.
        }
    }

    struct Gate
    {
        std::promise<void> entered;
        std::future<void> started{ entered.get_future() };
        std::promise<void> release;
        std::shared_future<void> released{ release.get_future().share() };

        void block()
        {
            entered.set_value();
            check(released.wait_for(5s) == std::future_status::ready, "preparation released");
        }
        void wait()
        {
            check(started.wait_for(5s) == std::future_status::ready, "worker entered preparation");
        }
    };

    sds::RemoteVoAudioRequest request(std::uint64_t sequence)
    {
        return { .filePath = L"voice-" + std::to_wstring(sequence), .sequence = sequence };
    }

    sds::RemoteVoControllerPlaybackPreparation prepared(const sds::RemoteVoAudioRequest& item)
    {
        return { .prepared = true, .sourceFrames = static_cast<std::uint32_t>(item.sequence) };
    }

    auto waitResult(sds::RemoteVoAudioPipeline& pipeline, std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto result = pipeline.tryTake()) {
                return result;
            }
            std::this_thread::sleep_for(1ms);
        }
        return std::optional<sds::RemoteVoAudioResult>{};
    }

    void latestRequestWins()
    {
        Gate gate;
        std::atomic_uint calls{0};
        const auto caller = std::this_thread::get_id();
        sds::RemoteVoAudioPipeline pipeline([&](const auto& item) {
            check(std::this_thread::get_id() != caller, "preparation runs off the caller thread");
            ++calls;
            if (item.sequence == 1) gate.block();
            return prepared(item);
        });
        pipeline.enqueue(request(1));
        gate.wait();
        auto handoff = std::async(std::launch::async, [&] {
            pipeline.enqueue(request(2));
            auto latest = request(3);
            pipeline.enqueue(latest);
            latest.filePath = L"modified by caller";
            return !pipeline.tryTake();
        });
        check(handoff.wait_for(5s) == std::future_status::ready, "handoff does not wait for decoding");
        check(handoff.get(), "in-flight work is not prematurely published");
        gate.release.set_value();
        const auto result = waitResult(pipeline);
        check(result && result->request.filePath == L"voice-3" &&
            result->playback.sourceFrames == 3 && calls == 2,
            "latest owned request replaces pending work and invalidates the old result");
    }

    void clearInvalidatesWork()
    {
        Gate gate;
        std::atomic_uint calls{0};
        sds::RemoteVoAudioPipeline pipeline([&](const auto& item) {
            ++calls;
            if (item.sequence == 1) gate.block();
            return prepared(item);
        });
        pipeline.enqueue(request(1));
        gate.wait();
        pipeline.enqueue(request(2));
        pipeline.clear();
        gate.release.set_value();
        check(!waitResult(pipeline, 100ms) && calls == 1,
            "clear discards both pending work and in-flight completion");
        pipeline.enqueue(request(3));
        const auto result = waitResult(pipeline);
        check(result && result->request.sequence == 3, "clear permits later playback");
    }

    void preparationFailureDoesNotStopWorker()
    {
        sds::RemoteVoAudioPipeline pipeline([](const auto& item) {
            if (item.sequence == 1) throw std::runtime_error("decode failed");
            return prepared(item);
        });
        pipeline.enqueue(request(1));
        const auto failure = waitResult(pipeline);
        check(failure && !failure->playback.prepared && failure->playback.error == "decode failed",
            "preparation errors are results, not worker termination");
        pipeline.enqueue(request(2));
        const auto success = waitResult(pipeline);
        check(success && success->playback.prepared, "worker continues after failure");
    }

    void shutdownJoinsWorker()
    {
        Gate gate;
        sds::RemoteVoAudioPipeline pipeline([&](const auto& item) {
            gate.block();
            return prepared(item);
        });
        pipeline.enqueue(request(1));
        gate.wait();
        std::promise<void> stopping;
        auto stopped = std::async(std::launch::async, [&] { stopping.set_value(); pipeline.stop(); });
        check(stopping.get_future().wait_for(5s) == std::future_status::ready, "shutdown started");
        check(stopped.wait_for(20ms) == std::future_status::timeout, "shutdown waits for preparation");
        gate.release.set_value();
        check(stopped.wait_for(5s) == std::future_status::ready, "shutdown joins completed worker");
        stopped.get();
        pipeline.enqueue(request(2));
        check(!pipeline.tryTake(), "shutdown accepts neither new work nor the old completion");
    }
}

int main()
{
    latestRequestWins();
    clearInvalidatesWork();
    preparationFailureDoesNotStopWorker();
    shutdownJoinsWorker();
}
