#include <StarfieldDualSense/BluetoothSpeakerBackend.h>

#include "OverlappedIo.h"

#include <StarfieldDualSense/SpeakerMixer.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    constexpr std::uint16_t kSonyVid =
        0x054C;

    constexpr std::uint16_t kDualSensePid =
        0x0CE6;

    constexpr std::uint16_t kDualSenseEdgePid =
        0x0DF2;

    constexpr std::size_t kStatePayloadSize =
        47;

    constexpr std::size_t kStateReportSize =
        78;

    constexpr std::size_t kInitPrimeSize =
        142;

    constexpr std::size_t kAudioReportSize =
        334;

    constexpr std::size_t kMixerFrames =
        512;

    constexpr std::size_t kOpusFrames =
        480;

    constexpr std::size_t kChannels =
        2;

    constexpr std::size_t kOpusBytes =
        200;

    constexpr std::uint8_t kOutputCrcSeed =
        0xA2;

    constexpr int kPrerollPackets =
        8;

    constexpr int kPostrollPackets =
        8;

    constexpr auto kStreamIdleGrace =
        std::chrono::milliseconds(500);

    constexpr auto kAudioTick =
        std::chrono::nanoseconds(
            10'666'667);

    std::uint32_t crc32Le(
        std::uint32_t crc,
        const std::uint8_t* data,
        std::size_t length) noexcept
    {
        for (std::size_t i = 0;
             i < length;
             ++i) {

            crc ^= data[i];

            for (int bit = 0;
                 bit < 8;
                 ++bit) {

                const auto mask =
                    static_cast<std::uint32_t>(
                        -static_cast<std::int32_t>(
                            crc & 1U));

                crc =
                    (crc >> 1U) ^
                    (0xEDB88320U & mask);
            }
        }

        return crc;
    }

    template <std::size_t N>
    void signReport(
        std::array<std::uint8_t, N>& report) noexcept
    {
        std::uint32_t crc =
            crc32Le(
                0xFFFFFFFFU,
                &kOutputCrcSeed,
                1);

        crc =
            ~crc32Le(
                crc,
                report.data(),
                report.size() - 4);

        const auto offset =
            report.size() - 4;

        report[offset + 0] =
            static_cast<std::uint8_t>(
                crc & 0xFFU);

        report[offset + 1] =
            static_cast<std::uint8_t>(
                (crc >> 8U) & 0xFFU);

        report[offset + 2] =
            static_cast<std::uint8_t>(
                (crc >> 16U) & 0xFFU);

        report[offset + 3] =
            static_cast<std::uint8_t>(
                (crc >> 24U) & 0xFFU);
    }

    std::array<
        std::uint8_t,
        kStatePayloadSize>
    speakerState(
        bool enabled) noexcept
    {
        std::array<
            std::uint8_t,
            kStatePayloadSize>
            state{};

        // Validates speaker volume and audio control.
        state[0] =
            0xA0;

        // Validates AudioControl2 / speaker pre-gain.
        state[1] =
            0x80;

        // Match SAD's hardware-proven USB route values.
        state[5] =
            enabled ? 0x64 : 0x00;

        // Internal speaker receives the right audio lane.
        state[7] =
            enabled ? 0x30 : 0x00;

        state[37] =
            enabled ? 0x05 : 0x00;

        return state;
    }

    std::array<
        std::uint8_t,
        kStateReportSize>
    buildSpeakerStateReport(
        std::uint8_t sequence,
        bool enabled) noexcept
    {
        std::array<
            std::uint8_t,
            kStateReportSize>
            report{};

        report[0] =
            0x31;

        report[1] =
            static_cast<std::uint8_t>(
                (sequence & 0x0FU) << 4U);

        report[2] =
            0x10;

        const auto state =
            speakerState(enabled);

        std::copy(
            state.begin(),
            state.end(),
            report.begin() + 3);

        signReport(report);

        return report;
    }

    std::array<
        std::uint8_t,
        kInitPrimeSize>
    buildInitPrime() noexcept
    {
        std::array<
            std::uint8_t,
            kInitPrimeSize>
            report{};

        report[0] =
            0x32;

        report[1] =
            0x10;

        // Sized packet 0x10, 63-byte state block.
        report[2] =
            0x90;

        report[3] =
            0x3F;

        const auto state =
            speakerState(true);

        std::copy(
            state.begin(),
            state.end(),
            report.begin() + 4);

        signReport(report);

        return report;
    }

    std::array<
        std::uint8_t,
        kAudioReportSize>
    buildAudioReport(
        std::uint8_t& sequence,
        std::uint8_t& packetCounter,
        const std::array<
            std::uint8_t,
            kOpusBytes>& opusFrame) noexcept
    {
        std::array<
            std::uint8_t,
            kAudioReportSize>
            report{};

        report[0] =
            0x35;

        report[1] =
            static_cast<std::uint8_t>(
                (sequence & 0x0FU) << 4U);

        sequence =
            static_cast<std::uint8_t>(
                (sequence + 1U) &
                0x0FU);

        // Sized packet 0x11.
        //
        // 0xFE intentionally leaves microphone capture disabled.
        report[2] =
            0x91;

        report[3] =
            0x07;

        report[4] =
            0xFE;

        report[5] =
            0x40;

        report[6] =
            0x40;

        report[7] =
            0x40;

        report[8] =
            0x40;

        report[9] =
            0x40;

        report[10] =
            packetCounter++;

        // Sized packet 0x13, 200-byte speaker Opus frame.
        report[11] =
            0x93;

        report[12] =
            static_cast<std::uint8_t>(
                kOpusBytes);

        std::copy(
            opusFrame.begin(),
            opusFrame.end(),
            report.begin() + 13);

        signReport(report);

        return report;
    }

    void convertMixerToPcm16(
        const std::array<
            sds::StereoSpeakerFrame,
            kMixerFrames>& source,
        std::array<
            opus_int16,
            kMixerFrames * kChannels>& target) noexcept
    {
        for (std::size_t frame = 0;
             frame < kMixerFrames;
             ++frame) {

            const auto convert =
                [](float value) noexcept {
                    const float clamped =
                        std::clamp(
                            value,
                            -1.0F,
                            1.0F);

                    const int scaled =
                        static_cast<int>(
                            std::lround(
                                clamped *
                                32767.0F));

                    return static_cast<opus_int16>(
                        std::clamp(
                            scaled,
                            -32768,
                            32767));
                };

            target[
                frame * 2] =
                convert(
                    source[frame].left);

            target[
                frame * 2 + 1] =
                convert(
                    source[frame].right);
        }
    }

    void resample512To480(
        const std::array<
            opus_int16,
            kMixerFrames * kChannels>& source,
        std::array<
            opus_int16,
            kOpusFrames * kChannels>& target) noexcept
    {
        constexpr double step =
            static_cast<double>(
                kMixerFrames - 1) /
            static_cast<double>(
                kOpusFrames - 1);

        for (std::size_t i = 0;
             i < kOpusFrames;
             ++i) {

            if (i ==
                kOpusFrames - 1) {

                target[i * 2] =
                    source[
                        (kMixerFrames - 1) *
                        2];

                target[i * 2 + 1] =
                    source[
                        (kMixerFrames - 1) *
                        2 + 1];

                continue;
            }

            const double position =
                static_cast<double>(i) *
                step;

            const auto index =
                static_cast<std::size_t>(
                    position);

            const auto next =
                index + 1;

            const double fraction =
                position -
                static_cast<double>(index);

            for (std::size_t channel = 0;
                 channel < 2;
                 ++channel) {

                const int a =
                    source[
                        index * 2 +
                        channel];

                const int b =
                    source[
                        next * 2 +
                        channel];

                target[
                    i * 2 +
                    channel] =
                    static_cast<opus_int16>(
                        static_cast<int>(
                            static_cast<double>(a) +
                            static_cast<double>(
                                b - a) *
                            fraction));
            }
        }
    }

    void waitUntil(
        std::chrono::steady_clock::time_point target)
    {
        using namespace std::chrono_literals;

        for (;;) {
            const auto now =
                std::chrono::steady_clock::now();

            if (now >= target) {
                return;
            }

            const auto remaining =
                target - now;

            if (remaining >
                800us) {

                std::this_thread::sleep_for(
                    remaining -
                    500us);

            } else {
                YieldProcessor();
            }
        }
    }
}

struct sds::BluetoothSpeakerBackend::Impl
{
    LogCallback logCallback{};
    std::chrono::milliseconds reconnectInterval{
        250
    };

    SpeakerMixer mixer;

    mutable std::mutex mixerMutex{};
    std::condition_variable wake{};

    std::atomic<bool> running{
        false
    };

    std::atomic<bool> available{
        false
    };

    std::thread worker{};

    HANDLE handle{
        INVALID_HANDLE_VALUE
    };

    std::uint16_t featureReportLength{
        0
    };

    OpusEncoder* encoder{
        nullptr
    };

    std::uint8_t stateSequence{
        0
    };

    std::uint8_t audioSequence{
        0
    };

    std::uint8_t packetCounter{
        0
    };

    bool streaming{
        false
    };

    Impl(
        float speakerVolume,
        LogCallback log,
        std::chrono::milliseconds reconnect) :
        logCallback(
            std::move(log)),
        reconnectInterval(
            reconnect),
        mixer(
            speakerVolume)
    {}

    ~Impl()
    {
        stop();
    }

    void log(
        std::string_view message) const noexcept
    {
        try {
            if (logCallback) {
                logCallback(message);
            }
        } catch (...) {
        }
    }

    void destroyEncoder() noexcept
    {
        if (encoder) {
            opus_encoder_destroy(
                encoder);

            encoder =
                nullptr;
        }
    }

    bool createEncoder()
    {
        destroyEncoder();

        int error =
            OPUS_OK;

        encoder =
            opus_encoder_create(
                48000,
                2,
                OPUS_APPLICATION_AUDIO,
                &error);

        if (!encoder ||
            error != OPUS_OK) {

            log(
                "Bluetooth speaker: "
                "Opus encoder creation failed");

            destroyEncoder();
            return false;
        }

        if (opus_encoder_ctl(
                encoder,
                OPUS_SET_BITRATE(
                    160000)) !=
                OPUS_OK ||
            opus_encoder_ctl(
                encoder,
                OPUS_SET_VBR(
                    0)) !=
                OPUS_OK ||
            opus_encoder_ctl(
                encoder,
                OPUS_SET_COMPLEXITY(
                    0)) !=
                OPUS_OK) {

            log(
                "Bluetooth speaker: "
                "Opus encoder configuration failed");

            destroyEncoder();
            return false;
        }

        return true;
    }

    template <std::size_t N>
    bool writeReport(
        const std::array<
            std::uint8_t,
            N>& report,
        std::string_view kind)
    {
        if (handle ==
            INVALID_HANDLE_VALUE) {

            return false;
        }

        OVERLAPPED overlapped{};

        HANDLE event =
            CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                nullptr);

        if (!event) {
            log(
                "Bluetooth speaker: "
                "write event creation failed");

            return false;
        }

        overlapped.hEvent =
            event;

        DWORD bytesWritten =
            0;

        BOOL ok =
            WriteFile(
                handle,
                report.data(),
                static_cast<DWORD>(
                    report.size()),
                &bytesWritten,
                &overlapped);

        if (!ok &&
            GetLastError() ==
                ERROR_IO_PENDING) {

            const DWORD waitResult =
                WaitForSingleObject(
                    event,
                    250);

            if (waitResult ==
                WAIT_OBJECT_0) {

                ok =
                    GetOverlappedResult(
                        handle,
                        &overlapped,
                        &bytesWritten,
                        FALSE);

            } else {
                cancelIoAndWait(handle, overlapped);

                ok =
                    FALSE;
            }
        }

        const DWORD error =
            ok ?
                ERROR_SUCCESS :
                GetLastError();

        CloseHandle(
            event);

        if (!ok) {
            log(
                std::string(
                    "Bluetooth speaker: HID write failed kind=") +
                std::string(kind) +
                " error=" +
                std::to_string(error));

            return false;
        }

        return true;
    }

    bool enableEnhancedMode()
    {
        if (handle ==
            INVALID_HANDLE_VALUE) {

            return false;
        }

        const std::size_t length =
            std::max<std::size_t>(
                featureReportLength,
                41U);

        std::vector<std::uint8_t>
            feature(
                length,
                0);

        feature[0] =
            0x05;

        if (!HidD_GetFeature(
                handle,
                feature.data(),
                static_cast<ULONG>(
                    feature.size()))) {

            log(
                "Bluetooth speaker: "
                "feature 0x05 failed");

            return false;
        }

        return true;
    }

    void closeController() noexcept
    {
        available.store(
            false,
            std::memory_order_release);

        streaming =
            false;

        destroyEncoder();

        if (handle !=
            INVALID_HANDLE_VALUE) {

            CloseHandle(
                handle);

            handle =
                INVALID_HANDLE_VALUE;
        }

        featureReportLength =
            0;

        stateSequence =
            0;

        audioSequence =
            0;

        packetCounter =
            0;
    }

    bool connectController()
    {
        closeController();

        GUID hidGuid{};
        HidD_GetHidGuid(
            &hidGuid);

        HDEVINFO info =
            SetupDiGetClassDevsW(
                &hidGuid,
                nullptr,
                nullptr,
                DIGCF_PRESENT |
                    DIGCF_DEVICEINTERFACE);

        if (info ==
            INVALID_HANDLE_VALUE) {

            return false;
        }

        bool connected =
            false;

        for (DWORD index = 0;
             ;
             ++index) {

            SP_DEVICE_INTERFACE_DATA
                interfaceData{};

            interfaceData.cbSize =
                sizeof(
                    interfaceData);

            if (!SetupDiEnumDeviceInterfaces(
                    info,
                    nullptr,
                    &hidGuid,
                    index,
                    &interfaceData)) {

                if (GetLastError() ==
                    ERROR_NO_MORE_ITEMS) {

                    break;
                }

                continue;
            }

            DWORD required =
                0;

            SetupDiGetDeviceInterfaceDetailW(
                info,
                &interfaceData,
                nullptr,
                0,
                &required,
                nullptr);

            if (required <
                sizeof(
                    SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {

                continue;
            }

            std::vector<std::byte>
                storage(
                    required);

            auto* detail =
                reinterpret_cast<
                    SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
                        storage.data());

            detail->cbSize =
                sizeof(
                    SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (!SetupDiGetDeviceInterfaceDetailW(
                    info,
                    &interfaceData,
                    detail,
                    required,
                    nullptr,
                    nullptr)) {

                continue;
            }

            HANDLE candidate =
                CreateFileW(
                    detail->DevicePath,
                    GENERIC_READ |
                        GENERIC_WRITE,
                    FILE_SHARE_READ |
                        FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    nullptr);

            if (candidate ==
                INVALID_HANDLE_VALUE) {

                continue;
            }

            HIDD_ATTRIBUTES
                attributes{};

            attributes.Size =
                sizeof(
                    attributes);

            if (!HidD_GetAttributes(
                    candidate,
                    &attributes)) {

                CloseHandle(
                    candidate);

                continue;
            }

            if (attributes.VendorID !=
                    kSonyVid ||
                (attributes.ProductID !=
                    kDualSensePid &&
                 attributes.ProductID !=
                    kDualSenseEdgePid)) {

                CloseHandle(
                    candidate);

                continue;
            }

            PHIDP_PREPARSED_DATA
                preparsed =
                    nullptr;

            HIDP_CAPS caps{};

            if (!HidD_GetPreparsedData(
                    candidate,
                    &preparsed)) {

                CloseHandle(
                    candidate);

                continue;
            }

            const bool gotCaps =
                HidP_GetCaps(
                    preparsed,
                    &caps) ==
                HIDP_STATUS_SUCCESS;

            HidD_FreePreparsedData(
                preparsed);

            if (!gotCaps ||
                caps.InputReportByteLength <
                    78 ||
                caps.OutputReportByteLength <
                    334) {

                CloseHandle(
                    candidate);

                continue;
            }

            handle =
                candidate;

            featureReportLength =
                caps.FeatureReportByteLength;

            if (!enableEnhancedMode()) {
                CloseHandle(
                    candidate);

                handle =
                    INVALID_HANDLE_VALUE;

                continue;
            }

            connected =
                true;

            break;
        }

        SetupDiDestroyDeviceInfoList(
            info);

        if (!connected) {
            return false;
        }

        available.store(
            true,
            std::memory_order_release);

        log(
            "Bluetooth speaker: "
            "DualSense audio HID transport AVAILABLE");

        return true;
    }

    bool encodeBlock(
        const std::array<
            sds::StereoSpeakerFrame,
            kMixerFrames>& frames,
        std::array<
            std::uint8_t,
            kOpusBytes>& encoded)
    {
        if (!encoder) {
            return false;
        }

        std::array<
            opus_int16,
            kMixerFrames *
            kChannels>
            pcm512{};

        std::array<
            opus_int16,
            kOpusFrames *
            kChannels>
            pcm480{};

        convertMixerToPcm16(
            frames,
            pcm512);

        resample512To480(
            pcm512,
            pcm480);

        const int written =
            opus_encode(
                encoder,
                pcm480.data(),
                static_cast<int>(
                    kOpusFrames),
                encoded.data(),
                static_cast<opus_int32>(
                    encoded.size()));

        if (written !=
            static_cast<int>(
                kOpusBytes)) {

            log(
                std::string(
                    "Bluetooth speaker: "
                    "Opus frame size mismatch bytes=") +
                std::to_string(
                    written) +
                " expected=200");

            return false;
        }

        return true;
    }

    bool sendEncoded(
        const std::array<
            std::uint8_t,
            kOpusBytes>& encoded)
    {
        const auto report =
            buildAudioReport(
                audioSequence,
                packetCounter,
                encoded);

        return writeReport(
            report,
            "audio-0x35");
    }

    bool encodeSilence(
        std::array<
            std::uint8_t,
            kOpusBytes>& encoded)
    {
        std::array<
            opus_int16,
            kOpusFrames *
            kChannels>
            silence{};

        if (!encoder) {
            return false;
        }

        const int written =
            opus_encode(
                encoder,
                silence.data(),
                static_cast<int>(
                    kOpusFrames),
                encoded.data(),
                static_cast<opus_int32>(
                    encoded.size()));

        if (written !=
            static_cast<int>(
                kOpusBytes)) {

            log(
                std::string(
                    "Bluetooth speaker: "
                    "Opus silence frame size mismatch bytes=") +
                std::to_string(
                    written));

            return false;
        }

        return true;
    }

    bool beginStream()
    {
        if (handle ==
            INVALID_HANDLE_VALUE) {

            return false;
        }

        if (!createEncoder()) {
            return false;
        }

        audioSequence =
            0;

        packetCounter =
            0;

        const auto routeOn =
            buildSpeakerStateReport(
                stateSequence++,
                true);

        if (!writeReport(
                routeOn,
                "speaker-route-on")) {

            return false;
        }

        const auto prime =
            buildInitPrime();

        if (!writeReport(
                prime,
                "audio-init-0x32")) {

            return false;
        }

        std::array<
            std::uint8_t,
            kOpusBytes>
            silence{};

        if (!encodeSilence(
                silence)) {

            return false;
        }

        auto next =
            std::chrono::steady_clock::now();

        for (int i = 0;
             i < kPrerollPackets;
             ++i) {

            if (!writeReport(
                    buildAudioReport(
                        audioSequence,
                        packetCounter,
                        silence),
                    "audio-preroll")) {

                return false;
            }

            next +=
                kAudioTick;

            waitUntil(
                next);
        }

        streaming =
            true;

        log(
            "Bluetooth speaker: "
            "stream ACTIVE route=internal-speaker "
            "codec=Opus48kStereo bitrate=160000 "
            "payload=200 cadenceUs=10667");

        return true;
    }

    void endStream() noexcept
    {
        if (handle ==
                INVALID_HANDLE_VALUE ||
            !streaming) {

            destroyEncoder();
            streaming =
                false;

            return;
        }

        try {
            std::array<
                std::uint8_t,
                kOpusBytes>
                silence{};

            if (encodeSilence(
                    silence)) {

                auto next =
                    std::chrono::steady_clock::now();

                for (int i = 0;
                     i < kPostrollPackets;
                     ++i) {

                    if (!writeReport(
                            buildAudioReport(
                                audioSequence,
                                packetCounter,
                                silence),
                            "audio-postroll")) {

                        break;
                    }

                    next +=
                        kAudioTick;

                    waitUntil(
                        next);
                }
            }

            const auto routeOff =
                buildSpeakerStateReport(
                    stateSequence++,
                    false);

            (void)writeReport(
                routeOff,
                "speaker-route-off");

        } catch (...) {
        }

        streaming =
            false;

        destroyEncoder();

        log(
            "Bluetooth speaker: "
            "stream INACTIVE");
    }

    bool mixerHasWork()
    {
        std::scoped_lock lock(
            mixerMutex);

        return !mixer.empty();
    }

    void workerMain() noexcept
    {
        try {
            while (running.load(
                std::memory_order_acquire)) {

                if (handle ==
                    INVALID_HANDLE_VALUE) {

                    if (!connectController()) {
                        std::unique_lock lock(
                            mixerMutex);

                        wake.wait_for(
                            lock,
                            reconnectInterval);

                        continue;
                    }
                }

                {
                    std::unique_lock lock(
                        mixerMutex);

                    if (mixer.empty()) {
                        wake.wait_for(
                            lock,
                            std::chrono::milliseconds(
                                100),
                            [this] {
                                return
                                    !running.load(
                                        std::memory_order_acquire) ||
                                    !mixer.empty();
                            });
                    }
                }

                if (!running.load(
                        std::memory_order_acquire)) {

                    break;
                }

                if (!mixerHasWork()) {
                    continue;
                }

                if (!beginStream()) {
                    log(
                        "Bluetooth speaker: "
                        "stream start failed; reconnecting");

                    closeController();
                    continue;
                }

                auto next =
                    std::chrono::steady_clock::now();

                bool writeFailure =
                    false;

                for (;;) {
                    if (!running.load(
                            std::memory_order_acquire)) {

                        break;
                    }

                    std::array<
                        sds::StereoSpeakerFrame,
                        kMixerFrames>
                        block{};

                    bool emptyAfterRender =
                        false;

                    {
                        std::scoped_lock lock(
                            mixerMutex);

                        mixer.render(
                            block);

                        emptyAfterRender =
                            mixer.empty();
                    }

                    std::array<
                        std::uint8_t,
                        kOpusBytes>
                        encoded{};

                    if (!encodeBlock(
                            block,
                            encoded) ||
                        !sendEncoded(
                            encoded)) {

                        writeFailure =
                            true;

                        break;
                    }

                    next +=
                        kAudioTick;

                    waitUntil(
                        next);

                    if (emptyAfterRender) {
                        std::array<
                            std::uint8_t,
                            kOpusBytes>
                            silence{};

                        if (!encodeSilence(
                                silence)) {

                            writeFailure = true;
                            break;
                        }

                        const auto idleDeadline =
                            std::chrono::steady_clock::now() +
                            kStreamIdleGrace;

                        bool resumedDuringIdleGrace =
                            false;

                        while (running.load(
                                   std::memory_order_acquire) &&
                               std::chrono::steady_clock::now() <
                                   idleDeadline) {

                            if (mixerHasWork()) {
                                resumedDuringIdleGrace =
                                    true;
                                break;
                            }

                            if (!writeReport(
                                    buildAudioReport(
                                        audioSequence,
                                        packetCounter,
                                        silence),
                                    "audio-idle-grace")) {

                                writeFailure = true;
                                break;
                            }

                            next +=
                                kAudioTick;

                            waitUntil(
                                next);
                        }

                        if (writeFailure) {
                            break;
                        }

                        if (resumedDuringIdleGrace) {
                            continue;
                        }

                        break;
                    }
                }

                if (writeFailure) {
                    log(
                        "Bluetooth speaker: "
                        "audio write failed; reconnecting");

                    closeController();
                    continue;
                }

                endStream();
            }

            endStream();
            closeController();

        } catch (...) {
            log(
                "Bluetooth speaker: "
                "worker exception; transport stopped");

            endStream();
            closeController();
        }
    }

    void start()
    {
        bool expected =
            false;

        if (!running.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {

            return;
        }

        worker =
            std::thread(
                [this] {
                    workerMain();
                });
    }

    void stop() noexcept
    {
        if (!running.exchange(
                false,
                std::memory_order_acq_rel)) {

            return;
        }

        wake.notify_all();

        if (worker.joinable()) {
            worker.join();
        }

        {
            std::scoped_lock lock(
                mixerMutex);

            mixer.clear();
        }

        available.store(
            false,
            std::memory_order_release);
    }
};

sds::BluetoothSpeakerBackend::
BluetoothSpeakerBackend(
    float speakerVolume,
    LogCallback log,
    std::chrono::milliseconds reconnectInterval) :
    _impl(
        std::make_unique<Impl>(
            speakerVolume,
            std::move(log),
            reconnectInterval))
{}

sds::BluetoothSpeakerBackend::
~BluetoothSpeakerBackend()
{
    stop();
}

void
sds::BluetoothSpeakerBackend::
start()
{
    if (_impl) {
        _impl->start();
    }
}

void
sds::BluetoothSpeakerBackend::
stop() noexcept
{
    if (_impl) {
        _impl->stop();
    }
}

void
sds::BluetoothSpeakerBackend::
clearPlayback() noexcept
{
    if (!_impl) {
        return;
    }

    try {
        std::scoped_lock lock(
            _impl->mixerMutex);

        _impl->mixer.clear();

        _impl->wake.notify_all();

    } catch (...) {
    }
}

void
sds::BluetoothSpeakerBackend::
setSpeakerVolume(
    float volume) noexcept
{
    if (!_impl) {
        return;
    }

    try {
        std::scoped_lock lock(
            _impl->mixerMutex);

        _impl->mixer.setGlobalVolume(
            volume);

    } catch (...) {
    }
}
bool
sds::BluetoothSpeakerBackend::
enqueue(
    const SpeakerCommand& command) noexcept
{
    if (!_impl) {
        return false;
    }

    try {
        std::scoped_lock lock(
            _impl->mixerMutex);

        const bool accepted =
            _impl->mixer.add(
                command);

        if (accepted) {
            _impl->wake.notify_all();
        }

        return accepted;

    } catch (...) {
        return false;
    }
}

bool
sds::BluetoothSpeakerBackend::
enqueuePreparedPcm(
    const PreparedSpeakerPcm& pcm) noexcept
{
    if (!_impl) {
        return false;
    }

    try {
        std::scoped_lock lock(
            _impl->mixerMutex);

        const bool accepted =
            _impl->mixer.addPrepared(
                pcm);

        if (accepted) {
            _impl->wake.notify_all();
        }

        return accepted;

    } catch (...) {
        return false;
    }
}

bool
sds::BluetoothSpeakerBackend::
replacePreparedPcm(
    PreparedSpeakerPcm pcm) noexcept
{
    if (!_impl) {
        return false;
    }

    try {
        std::scoped_lock lock(
            _impl->mixerMutex);

        _impl->mixer.clearPrepared();

        const bool accepted =
            _impl->mixer.addPrepared(
                pcm);

        if (accepted) {
            _impl->wake.notify_all();
        }

        return accepted;

    } catch (...) {
        return false;
    }
}

bool
sds::BluetoothSpeakerBackend::
setPersistentPreparedPcm(
    PersistentPreparedSpeakerPcm voice) noexcept
{
    if (!_impl) {
        return false;
    }

    try {
        std::scoped_lock lock(
            _impl->mixerMutex);

        const bool accepted =
            _impl->mixer.setPersistentPrepared(
                std::move(
                    voice));

        if (accepted) {
            _impl->wake.notify_all();
        }

        return accepted;

    } catch (...) {
        return false;
    }
}

bool
sds::BluetoothSpeakerBackend::
clearPersistentPreparedPcm(
    std::uint64_t owner,
    bool force) noexcept
{
    if (!_impl) {
        return false;
    }

    try {
        std::scoped_lock lock(
            _impl->mixerMutex);

        return
            _impl->mixer.clearPersistentPrepared(
                owner,
                force);

    } catch (...) {
        return false;
    }
}

bool
sds::BluetoothSpeakerBackend::
active() const noexcept
{
    return
        _impl &&
        _impl->running.load(
            std::memory_order_acquire) &&
        _impl->available.load(
            std::memory_order_acquire);
}