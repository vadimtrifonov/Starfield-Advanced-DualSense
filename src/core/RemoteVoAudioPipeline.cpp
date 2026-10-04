#include <StarfieldDualSense/RemoteVoAudioPipeline.h>

#include <exception>
#include <utility>

sds::RemoteVoAudioPipeline::RemoteVoAudioPipeline(PrepareCallback prepare) :
    _prepare(std::move(prepare)),
    _worker([this] { run(); })
{}

sds::RemoteVoAudioPipeline::~RemoteVoAudioPipeline()
{
    stop();
}

void sds::RemoteVoAudioPipeline::stop()
{
    {
        std::scoped_lock lock(_mutex);
        _stopping = true;
        _pending.reset();
        _ready.reset();
    }
    _wake.notify_one();
    if (_worker.joinable()) {
        _worker.join();
    }
}

void sds::RemoteVoAudioPipeline::enqueue(RemoteVoAudioRequest request)
{
    {
        std::scoped_lock lock(_mutex);
        if (_stopping) {
            return;
        }
        ++_generation;
        _pending = std::move(request);
        _ready.reset();
    }
    _wake.notify_one();
}

std::optional<sds::RemoteVoAudioResult> sds::RemoteVoAudioPipeline::tryTake()
{
    std::scoped_lock lock(_mutex);
    return std::exchange(_ready, std::nullopt);
}

void sds::RemoteVoAudioPipeline::clear()
{
    std::scoped_lock lock(_mutex);
    ++_generation;
    _pending.reset();
    _ready.reset();
}

void sds::RemoteVoAudioPipeline::run()
{
    for (;;) {
        RemoteVoAudioResult result{};
        std::uint64_t generation = 0;
        {
            std::unique_lock lock(_mutex);
            _wake.wait(lock, [this] { return _stopping || _pending.has_value(); });
            if (_stopping) {
                return;
            }
            result.request = std::move(*_pending);
            _pending.reset();
            generation = _generation;
        }

        try {
            result.playback = _prepare(result.request);
        } catch (const std::exception& error) {
            result.playback.error = error.what();
        } catch (...) {
            result.playback.error = "voice preparation failed";
        }
        result.preparedAt = std::chrono::steady_clock::now();

        std::scoped_lock lock(_mutex);
        if (!_stopping && generation == _generation) {
            _ready = std::move(result);
        }
    }
}
