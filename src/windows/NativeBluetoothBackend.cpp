#include <StarfieldDualSense/NativeBluetoothBackend.h>

#include "OverlappedIo.h"

#include <StarfieldDualSense/BluetoothDualSenseReports.h>
#include <StarfieldDualSense/DeviceClassifier.h>
#include <StarfieldDualSense/Touchpad.h>

#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <BluetoothAPIs.h>
#include <winioctl.h>
#include <bthioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    int bluetoothHexNibble(wchar_t value) noexcept
    {
        if (value >= L'0' && value <= L'9') {
            return static_cast<int>(value - L'0');
        }

        if (value >= L'a' && value <= L'f') {
            return 10 + static_cast<int>(value - L'a');
        }

        if (value >= L'A' && value <= L'F') {
            return 10 + static_cast<int>(value - L'A');
        }

        return -1;
    }

    bool parseBluetoothAddress(
        std::wstring_view serial,
        BTH_ADDR& address) noexcept
    {
        std::uint64_t value = 0;
        std::size_t digits = 0;

        for (const wchar_t ch : serial) {
            if (ch == L':' ||
                ch == L'-' ||
                ch == L' ') {

                continue;
            }

            const int nibble = bluetoothHexNibble(ch);

            if (nibble < 0 || digits >= 12U) {
                return false;
            }

            value =
                (value << 4U) |
                static_cast<std::uint64_t>(nibble);

            ++digits;
        }

        if (digits != 12U) {
            return false;
        }

        address = static_cast<BTH_ADDR>(value);
        return true;
    }

    bool readBluetoothAddressFromHid(
        HANDLE handle,
        BTH_ADDR& address) noexcept
    {
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        std::array<wchar_t, 64> serial{};

        if (!HidD_GetSerialNumberString(
                handle,
                serial.data(),
                static_cast<ULONG>(
                    serial.size() * sizeof(wchar_t)))) {

            return false;
        }

        return parseBluetoothAddress(
            std::wstring_view(serial.data()),
            address);
    }

    bool disconnectBluetoothRemoteDevice(
        BTH_ADDR address,
        DWORD& lastError) noexcept
    {
        lastError = ERROR_SUCCESS;

        BLUETOOTH_FIND_RADIO_PARAMS params{};
        params.dwSize = sizeof(params);

        HANDLE radio = nullptr;

        HBLUETOOTH_RADIO_FIND find =
            BluetoothFindFirstRadio(
                &params,
                &radio);

        if (!find) {
            lastError = GetLastError();
            return false;
        }

        bool disconnected = false;

        for (;;) {
            DWORD bytesReturned = 0;

            SetLastError(ERROR_SUCCESS);

            const BOOL ok =
                DeviceIoControl(
                    radio,
                    IOCTL_BTH_DISCONNECT_DEVICE,
                    &address,
                    static_cast<DWORD>(sizeof(address)),
                    nullptr,
                    0,
                    &bytesReturned,
                    nullptr);

            if (ok) {
                disconnected = true;
            } else {
                lastError = GetLastError();
            }

            CloseHandle(radio);
            radio = nullptr;

            if (disconnected) {
                break;
            }

            if (!BluetoothFindNextRadio(
                    find,
                    &radio)) {

                if (lastError == ERROR_SUCCESS) {
                    lastError = GetLastError();
                }

                break;
            }
        }

        BluetoothFindRadioClose(find);
        return disconnected;
    }

    std::string modelName(sds::ControllerType type)
    {
        switch (type) {
        case sds::ControllerType::DualSense:
            return "DualSense";
        case sds::ControllerType::DualSenseEdge:
            return "DualSense Edge";
        default:
            return "Unknown controller";
        }
    }
}

struct sds::NativeBluetoothBackend::Impl
{
    explicit Impl(LogCallback callback, bool passivePresenceOnly) :
        log(std::move(callback)),
        presenceOnly(passivePresenceOnly)
    {}

    LogCallback log{};
    HANDLE handle{ INVALID_HANDLE_VALUE };
    HANDLE readEvent{ nullptr };
    OVERLAPPED readOverlapped{};
    bool readPending{ false };
    std::array<std::uint8_t, 78> inputReport{};
    DeviceIdentity device{};
    OutputState output{};
    std::uint8_t rumbleLeft{ 0 };
    std::uint8_t rumbleRight{ 0 };
    bool lightbarInitialized{ false };
    bool lightbarStartupReady{ false };
    bool lightbarReleasePending{ false };
    bool transportHandoffLedReleasePending{ false };
    // Bluetooth RGB writes may be accepted before the physical lightbar takes
    // ownership. These retries are STEADY RGB only: no repeat setup/release.
    std::chrono::steady_clock::time_point firstRgbTakeoverAt{};
    std::uint8_t postTakeoverReassertCount{ 0 };
    std::chrono::steady_clock::time_point connectionEstablishedAt{};
    std::uint16_t outputReportLength{ 0 };
    std::uint16_t featureReportLength{ 0 };
    std::uint8_t txSequence{ 0 };
    bool presenceOnly{ false };

    void writeLog(std::string_view message) const
    {
        if (log) {
            try {
                log(message);
            } catch (...) {
            }
        }
    }

    std::uint8_t nextSequence() noexcept
    {
        const auto current = txSequence;
        txSequence = static_cast<std::uint8_t>((txSequence + 1U) & 0x0FU);
        return current;
    }

    void markDisconnected() noexcept
    {
        if (handle != INVALID_HANDLE_VALUE) {
            if (readPending) {
                cancelIoAndWait(handle, readOverlapped);
            }
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }

        if (readEvent) {
            CloseHandle(readEvent);
            readEvent = nullptr;
        }

        std::memset(&readOverlapped, 0, sizeof(readOverlapped));
        readPending = false;
        device = {};
        output = {};
        rumbleLeft = 0;
        rumbleRight = 0;
        lightbarInitialized = false;
        lightbarStartupReady = false;
        lightbarReleasePending = false;
        transportHandoffLedReleasePending = false;
        firstRgbTakeoverAt = {};
        postTakeoverReassertCount = 0;
        connectionEstablishedAt = {};
        outputReportLength = 0;
        featureReportLength = 0;
        txSequence = 0;
    }

    bool enableEnhancedMode()
    {
        if (presenceOnly || handle == INVALID_HANDLE_VALUE) {
            return presenceOnly;
        }

        const std::size_t length =
            std::max<std::size_t>(featureReportLength, 41U);
        std::vector<std::uint8_t> feature(length, 0);
        feature[0] = 0x05;

        if (!HidD_GetFeature(
                handle,
                feature.data(),
                static_cast<ULONG>(feature.size()))) {
            writeLog(
                "Native Bluetooth: feature 0x05 failed; enhanced mode unavailable");
            return false;
        }

        writeLog(
            "Native Bluetooth: enhanced input mode ACTIVE feature=0x05");
        return true;
    }

    bool writeReport(
        const BluetoothDualSenseOutputReport& report,
        std::string_view kind,
        bool disconnectOnFailure = true)
    {
        if (presenceOnly || handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        OVERLAPPED writeOverlapped{};
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            writeLog("Native Bluetooth: failed to create write event");
            return false;
        }

        writeOverlapped.hEvent = event;

        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(
            handle,
            report.data(),
            static_cast<DWORD>(report.size()),
            &bytesWritten,
            &writeOverlapped);

        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(event, 100);
            if (wait == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(
                    handle,
                    &writeOverlapped,
                    &bytesWritten,
                    FALSE);
            } else {
                cancelIoAndWait(handle, writeOverlapped);
                ok = FALSE;
            }
        }

        const DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(event);


        // Some Windows Bluetooth HID stacks report the descriptor-wide
        // output length here even when the accepted 0x31 packet itself
        // is 78 bytes. Successful completion is the authority.
        if (!ok) {
            writeLog(
                std::string("Native Bluetooth: HID write failed error=") +
                std::to_string(lastError));

            if (disconnectOnFailure) {
                markDisconnected();
            }
            return false;
        }

        return true;
    }

    bool ensureLightbarInitialized()
    {
        if (lightbarInitialized) {
            return true;
        }

        const auto report =
            buildBluetoothLightbarInitializationReport(nextSequence());

        if (!writeReport(report, "init")) {
            return false;
        }

        writeLog(
            std::string("Native Bluetooth: lightbar setup accepted ") +
            "flags2=" +
            std::to_string(static_cast<unsigned>(report[41])) +
            " setup=" +
            std::to_string(static_cast<unsigned>(report[44])));

        // Do not send RELEASE_LEDS as an empty standalone packet.
        // Carry it with the first real RGB packet instead.
        lightbarReleasePending = true;
        lightbarInitialized = true;
        return true;
    }

    bool writeCurrentOutput(bool includeLightbar = true, bool forceCompatibleRumble = false)
    {
        if (includeLightbar &&
            lightbarStartupReady &&
            lightbarInitialized &&
            transportHandoffLedReleasePending) {

            const auto release =
                buildBluetoothLightbarReleaseReport(
                    nextSequence());

            if (!writeReport(
                    release,
                    "release-leds")) {
                return false;
            }

            transportHandoffLedReleasePending =
                false;

            // Do not duplicate RELEASE_LEDS on the following RGB packet.
            lightbarReleasePending = false;

            writeLog(
                "Bluetooth LED takeover: transport handoff standalone RELEASE_LEDS");
        }

        auto report =
            buildBluetoothOutputReport(
                output,
                nextSequence(),
                includeLightbar);
        if (forceCompatibleRumble ||
            rumbleLeft != 0 ||
            rumbleRight != 0) {
            applyBluetoothCompatibleVibration(
                report,
                rumbleLeft,
                rumbleRight);
        }
        bool releasingLedsWithThisPacket = false;

        if (includeLightbar &&
            lightbarReleasePending &&
            (output.lightbar.r != 0 ||
             output.lightbar.g != 0 ||
             output.lightbar.b != 0)) {

            applyBluetoothLightbarRelease(report);
            releasingLedsWithThisPacket = true;

            writeLog(
                std::string("Bluetooth LED takeover: first RGB packet ") +
                "flags0=" +
                std::to_string(static_cast<unsigned>(report[3])) +
                " flags1=" +
                std::to_string(static_cast<unsigned>(report[4])) +
                " flags2=" +
                std::to_string(static_cast<unsigned>(report[41])) +
                " playerLeds=" +
                std::to_string(static_cast<unsigned>(report[46])) +
                " rgb=" +
                std::to_string(static_cast<unsigned>(report[47])) + "," +
                std::to_string(static_cast<unsigned>(report[48])) + "," +
                std::to_string(static_cast<unsigned>(report[49])));

        }
        const bool wrote =
            writeReport(
                report,
                includeLightbar ? "steady" : "trigger-only");

        if (releasingLedsWithThisPacket) {
            writeLog(
                wrote ?
                    "Bluetooth LED takeover: first RGB write accepted" :
                    "Bluetooth LED takeover: first RGB write FAILED");
            if (wrote) {
                lightbarReleasePending = false;
                firstRgbTakeoverAt = std::chrono::steady_clock::now();
                postTakeoverReassertCount = 0;
                writeLog(
                    "Bluetooth LED takeover: bounded post-takeover "
                    "steady-RGB reassert armed delaysMs=600,1600,3100 "
                    "setup=none release=none");
            }
        }

        return wrote;
    }

    void maybeReassertPostTakeoverLightbar()
    {
        if (!lightbarStartupReady ||
            !lightbarInitialized ||
            lightbarReleasePending ||
            firstRgbTakeoverAt ==
                std::chrono::steady_clock::time_point{} ||
            postTakeoverReassertCount >= 3U) {
            return;
        }

        constexpr std::array<std::chrono::milliseconds, 3>
            kReassertDelays{
                std::chrono::milliseconds(600),
                std::chrono::milliseconds(1600),
                std::chrono::milliseconds(3100)
            };

        const auto elapsed =
            std::chrono::steady_clock::now() - firstRgbTakeoverAt;
        const auto index =
            static_cast<std::size_t>(postTakeoverReassertCount);
        if (elapsed < kReassertDelays[index]) {
            return;
        }

        // Mark the attempt before writing, because failed writes can reset
        // the backend during HID disconnect. Do not issue init or release.
        ++postTakeoverReassertCount;
        const auto color = output.lightbar;
        const bool wrote = writeCurrentOutput(true);
        writeLog(
            std::string("Bluetooth LED takeover: post-takeover ") +
            "steady-RGB reassert=" + std::to_string(index + 1U) +
            "/3 result=" + (wrote ? "OK" : "FAILED") +
            " rgb=" +
            std::to_string(static_cast<unsigned>(color.r)) + "," +
            std::to_string(static_cast<unsigned>(color.g)) + "," +
            std::to_string(static_cast<unsigned>(color.b)) +
            " setup=none release=none");
    }

    void observeLightbarStartupState()
    {
        // DualSense sensor timestamps advance at about 3 million ticks/sec.
        // The previous 10.2M-tick takeover failed in the captured third
        // Bluetooth handoff (timestamp=10,204,570), while the two working
        // takeovers occurred at 13.66M and 25.79M ticks. This is a cautious
        // one-time startup timing experiment, not a confirmed firmware limit.
        constexpr std::uint32_t kLedStartupCompleteTimestamp =
            13500000U;

        constexpr auto kHostLedSettleDelay =
            std::chrono::milliseconds(2000);

        if (lightbarStartupReady) {
            maybeReassertPostTakeoverLightbar();
            return;
        }
        if (inputReport[0] != 0x31U) {
            return;
        }

        const std::uint32_t timestamp =
            static_cast<std::uint32_t>(inputReport[29]) |
            (static_cast<std::uint32_t>(inputReport[30]) << 8U) |
            (static_cast<std::uint32_t>(inputReport[31]) << 16U) |
            (static_cast<std::uint32_t>(inputReport[32]) << 24U);

        if (timestamp < kLedStartupCompleteTimestamp) {
            return;
        }

        const auto now =
            std::chrono::steady_clock::now();

        if (connectionEstablishedAt !=
                std::chrono::steady_clock::time_point{} &&
            now - connectionEstablishedAt <
                kHostLedSettleDelay) {
            return;
        }

        lightbarStartupReady = true;

        const auto hostElapsedMs =
            connectionEstablishedAt ==
                std::chrono::steady_clock::time_point{} ?
                0LL :
                std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    now - connectionEstablishedAt).count();

        writeLog(
            std::string(
                "Native Bluetooth: LED startup complete timestamp=") +
            std::to_string(timestamp) +
            " threshold=" +
            std::to_string(kLedStartupCompleteTimestamp) +
            " hostElapsedMs=" +
            std::to_string(hostElapsedMs));

        if (!lightbarInitialized &&
            ensureLightbarInitialized()) {

            (void)writeCurrentOutput(true);
        }
    }
};

sds::NativeBluetoothBackend::NativeBluetoothBackend(
    LogCallback log,
    bool presenceOnly) :
    _impl(std::make_unique<Impl>(
        std::move(log),
        presenceOnly))
{}

sds::NativeBluetoothBackend::~NativeBluetoothBackend()
{
    disconnect();
}

sds::NativeBluetoothBackend::NativeBluetoothBackend(
    NativeBluetoothBackend&&) noexcept = default;

sds::NativeBluetoothBackend&
sds::NativeBluetoothBackend::operator=(
    NativeBluetoothBackend&&) noexcept = default;

bool sds::NativeBluetoothBackend::connect()
{
    disconnect();

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        &hidGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        _impl->writeLog(
            "Native Bluetooth: SetupDiGetClassDevs failed");
        return false;
    }

    bool connectedDevice = false;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);

        if (!SetupDiEnumDeviceInterfaces(
                deviceInfoSet,
                nullptr,
                &hidGuid,
                index,
                &interfaceData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet,
            &interfaceData,
            nullptr,
            0,
            &requiredSize,
            nullptr);

        if (requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<std::byte> detailStorage(requiredSize);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
                detailStorage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(
                deviceInfoSet,
                &interfaceData,
                detail,
                requiredSize,
                nullptr,
                nullptr)) {
            continue;
        }

        const DWORD desiredAccess =
            _impl->presenceOnly ?
                GENERIC_READ :
                (GENERIC_READ | GENERIC_WRITE);

        HANDLE candidate = CreateFileW(
            detail->DevicePath,
            desiredAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);

        if (candidate == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);

        if (!HidD_GetAttributes(candidate, &attributes)) {
            CloseHandle(candidate);
            continue;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};

        if (!HidD_GetPreparsedData(candidate, &preparsed)) {
            CloseHandle(candidate);
            continue;
        }

        const bool gotCaps =
            HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData(preparsed);

        if (!gotCaps) {
            CloseHandle(candidate);
            continue;
        }

        auto identity = classifyDevice(
            attributes.VendorID,
            attributes.ProductID,
            caps.InputReportByteLength);

        if (!identity.supported() ||
            identity.connection != ConnectionType::Bluetooth) {
            CloseHandle(candidate);
            continue;
        }

        HANDLE readEvent = nullptr;
        if (!_impl->presenceOnly) {
            readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!readEvent) {
                CloseHandle(candidate);
                continue;
            }
        }

        identity.path = detail->DevicePath;
        _impl->handle = candidate;
        _impl->readEvent = readEvent;
        _impl->readOverlapped = {};
        _impl->readOverlapped.hEvent = readEvent;
        _impl->device = std::move(identity);
        _impl->output = {};
        _impl->rumbleLeft = 0;
        _impl->rumbleRight = 0;
        _impl->lightbarInitialized = false;
        _impl->lightbarStartupReady = false;
        _impl->lightbarReleasePending = false;
        _impl->outputReportLength = caps.OutputReportByteLength;
        _impl->featureReportLength = caps.FeatureReportByteLength;
        _impl->readPending = false;
        _impl->txSequence = 0;

        _impl->writeLog(
            std::string("Native Bluetooth: HID caps input=") +
            std::to_string(caps.InputReportByteLength) +
            " output=" + std::to_string(caps.OutputReportByteLength) +
            " feature=" + std::to_string(caps.FeatureReportByteLength));

        if (!_impl->enableEnhancedMode()) {
            _impl->markDisconnected();
            continue;
        }

        _impl->connectionEstablishedAt =
            std::chrono::steady_clock::now();

        connectedDevice = true;
        _impl->writeLog(
            std::string("Native Bluetooth: connected ") +
            modelName(_impl->device.type));

        break;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return connectedDevice;
}

bool sds::NativeBluetoothBackend::powerOffBluetooth() noexcept
{
    if (!_impl ||
        _impl->presenceOnly ||
        !connected()) {

        return false;
    }

    try {
        BTH_ADDR address = 0;

        if (!readBluetoothAddressFromHid(
                _impl->handle,
                address)) {

            _impl->writeLog(
                "Native Bluetooth: OS link disconnect FAILED "
                "reason=controller-address-unavailable");

            return false;
        }

        DWORD error = ERROR_SUCCESS;

        if (!disconnectBluetoothRemoteDevice(
                address,
                error)) {

            _impl->writeLog(
                std::string(
                    "Native Bluetooth: OS link disconnect FAILED "
                    "ioctl=IOCTL_BTH_DISCONNECT_DEVICE error=") +
                std::to_string(error));

            return false;
        }

        _impl->writeLog(
            "Native Bluetooth: OS link disconnect SENT "
            "ioctl=IOCTL_BTH_DISCONNECT_DEVICE");

        // Windows has now torn down the physical Bluetooth link.
        // Retire SAD's HID state immediately as an intentional
        // disconnect rather than waiting for the next failed read.
        _impl->markDisconnected();
        return true;
    } catch (...) {
        _impl->writeLog(
            "Native Bluetooth: OS link disconnect exception ignored");

        return false;
    }
}

void sds::NativeBluetoothBackend::disconnect() noexcept
{
    if (!_impl) {
        return;
    }

    if (_impl->handle != INVALID_HANDLE_VALUE &&
        !_impl->presenceOnly) {
        _impl->output = {};
        _impl->rumbleLeft = 0;
        _impl->rumbleRight = 0;
        (void)_impl->writeCurrentOutput(true, true);
    }

    _impl->markDisconnected();
}

bool sds::NativeBluetoothBackend::connected() const noexcept
{
    return _impl && _impl->handle != INVALID_HANDLE_VALUE;
}

bool sds::NativeBluetoothBackend::refreshPresence()
{
    if (!connected()) {
        return false;
    }

    if (!_impl->presenceOnly) {
        return true;
    }

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        _impl->markDisconnected();
        return false;
    }

    bool present = false;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);

        if (!SetupDiEnumDeviceInterfaces(
                deviceInfoSet, nullptr, &hidGuid,
                index, &interfaceData)) {
            break;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet, &interfaceData,
            nullptr, 0, &requiredSize, nullptr);

        if (requiredSize == 0) {
            continue;
        }

        std::vector<std::byte> storage(requiredSize);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
                storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(
                deviceInfoSet, &interfaceData, detail,
                requiredSize, nullptr, nullptr)) {
            continue;
        }

        if (CompareStringOrdinal(
                detail->DevicePath, -1,
                _impl->device.path.c_str(), -1,
                TRUE) == CSTR_EQUAL) {
            present = true;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    if (!present) {
        _impl->markDisconnected();
    }

    return present;
}

sds::DeviceIdentity sds::NativeBluetoothBackend::identity() const
{
    return _impl ? _impl->device : DeviceIdentity{};
}

sds::Capabilities
sds::NativeBluetoothBackend::capabilities() const noexcept
{
    if (!connected()) {
        return {};
    }

    return {
        .adaptiveTriggers = true,
        .lightbar = true,
        .touchpadInput = true,
        .advancedHaptics = false,
        .controllerSpeaker = false,
        .bluetoothTransport = true
    };
}

std::optional<sds::TouchState>
sds::NativeBluetoothBackend::pollTouch()
{
    if (!connected() || _impl->presenceOnly) {
        return std::nullopt;
    }


    DWORD bytesRead = 0;

    if (_impl->readPending) {
        if (!GetOverlappedResult(
                _impl->handle,
                &_impl->readOverlapped,
                &bytesRead,
                FALSE)) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                return std::nullopt;
            }
            _impl->markDisconnected();
            return std::nullopt;
        }

        _impl->readPending = false;
        if (bytesRead == _impl->inputReport.size()) {
            _impl->observeLightbarStartupState();
            return parseBluetoothInputReport(_impl->inputReport);
        }
        return std::nullopt;
    }

    ResetEvent(_impl->readEvent);
    std::memset(
        &_impl->readOverlapped,
        0,
        sizeof(_impl->readOverlapped));
    _impl->readOverlapped.hEvent = _impl->readEvent;
    _impl->inputReport.fill(0);

    if (ReadFile(
            _impl->handle,
            _impl->inputReport.data(),
            static_cast<DWORD>(_impl->inputReport.size()),
            &bytesRead,
            &_impl->readOverlapped)) {
        if (bytesRead == _impl->inputReport.size()) {
            _impl->observeLightbarStartupState();
            return parseBluetoothInputReport(_impl->inputReport);
        }
        return std::nullopt;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        _impl->readPending = true;
        return std::nullopt;
    }

    _impl->markDisconnected();
    return std::nullopt;
}

bool sds::NativeBluetoothBackend::setLightbar(Color color)
{
    if (!connected()) {
        return false;
    }

    _impl->output.lightbar = color;

    if (!_impl->lightbarStartupReady) {
        return true;
    }

    if (!_impl->ensureLightbarInitialized()) {
        return false;
    }

    return _impl->writeCurrentOutput(true);
}

bool sds::NativeBluetoothBackend::setTriggers(
    const TriggerEffect& left,
    const TriggerEffect& right)
{
    if (!connected()) {
        return false;
    }

    _impl->output.leftTrigger = left;
    _impl->output.rightTrigger = right;

    if (!_impl->lightbarStartupReady) {
        return _impl->writeCurrentOutput(false);
    }

    if (!_impl->ensureLightbarInitialized()) {
        return false;
    }

    return _impl->writeCurrentOutput(true);
}

bool sds::NativeBluetoothBackend::setOutputState(
    const OutputState& state)
{
    if (!connected()) {
        return false;
    }

    _impl->output = state;

    if (!_impl->lightbarStartupReady) {
        return _impl->writeCurrentOutput(false);
    }

    if (!_impl->ensureLightbarInitialized()) {
        return false;
    }

    return _impl->writeCurrentOutput(true);
}

bool sds::NativeBluetoothBackend::setCompatibleRumble(
    std::uint8_t left,
    std::uint8_t right)
{
    if (!connected() || _impl->presenceOnly) {
        return false;
    }

    _impl->rumbleLeft = left;
    _impl->rumbleRight = right;

    // Preserve the currently-owned lightbar state when available.
    const bool includeLightbar =
        _impl->lightbarStartupReady &&
        _impl->lightbarInitialized;

    return _impl->writeCurrentOutput(
        includeLightbar,
        true);
}
void sds::NativeBluetoothBackend::prepareTransportHandoff(
    ConnectionType previous) noexcept
{
    if (!_impl ||
        _impl->presenceOnly ||
        !connected() ||
        previous != ConnectionType::Usb) {

        return;
    }

    // A freshly reopened Bluetooth HID connection already runs the normal
    // Bluetooth LED startup sequence. Do not layer a second handoff-only
    // RELEASE_LEDS phase on top of that sequence after USB.
    _impl->transportHandoffLedReleasePending =
        false;

    _impl->writeLog(
        "Native Bluetooth: transport handoff LED uses standard startup takeover "
        "previous=USB specialRelease=off");
}

void sds::NativeBluetoothBackend::resetOutputs() noexcept
{
    if (!connected() || _impl->presenceOnly) {
        return;
    }

    _impl->output = {};
    _impl->rumbleLeft = 0;
    _impl->rumbleRight = 0;
    (void)_impl->writeCurrentOutput(true, true);
}
