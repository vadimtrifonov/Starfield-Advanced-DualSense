#include <StarfieldDualSense/NativeUsbBackend.h>

#include "OverlappedIo.h"

#include <StarfieldDualSense/DeviceClassifier.h>
#include <StarfieldDualSense/DualSenseReports.h>
#include <StarfieldDualSense/HidWriteTrace.h>
#include <StarfieldDualSense/Touchpad.h>

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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
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

struct sds::NativeUsbBackend::Impl
{
    explicit Impl(LogCallback callback, bool requestSpeaker, bool passivePresenceOnly) :
        log(std::move(callback)),
        speakerRoutingRequested(requestSpeaker),
        presenceOnly(passivePresenceOnly)
    {}

    LogCallback log{};
    HANDLE handle{ INVALID_HANDLE_VALUE };
    HANDLE readEvent{ nullptr };
    OVERLAPPED overlapped{};
    bool readPending{ false };
    std::array<std::uint8_t, 64> inputReport{};
    DeviceIdentity device{};
    OutputState output{};
    bool lightbarInitialized{ false };
    std::uint16_t outputReportLength{ 0 };
    std::uint64_t txSequence{ 0 };
    bool hidOwnershipRefreshFirstPending{ false };
    bool hidOwnershipRefreshSecondPending{ false };
    std::chrono::steady_clock::time_point hidOwnershipRefreshFirstDue{};
    std::chrono::steady_clock::time_point hidOwnershipRefreshSecondDue{};
    bool speakerRoutingRequested{ false };
    bool speakerRoutingActive{ false };
    bool presenceOnly{ false };
    bool transportHandoffLedReleasePending{ false };

    void writeLog(std::string_view message) const
    {
        if (log) {
            try {
                log(message);
            } catch (...) {
                // Logging must never escape into controller code.
            }
        }
    }

    void markDisconnected() noexcept
    {
        if (!presenceOnly) {
            HidWriteTrace::stop();
        }

        if (handle != INVALID_HANDLE_VALUE) {
            if (readPending) {
                cancelIoAndWait(handle, overlapped);
            }
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
        if (readEvent) {
            CloseHandle(readEvent);
            readEvent = nullptr;
        }
        std::memset(&overlapped, 0, sizeof(overlapped));
        readPending = false;
        device = {};
        output = {};
        lightbarInitialized = false;
        outputReportLength = 0;
        txSequence = 0;
        hidOwnershipRefreshFirstPending = false;
        hidOwnershipRefreshSecondPending = false;
        hidOwnershipRefreshFirstDue = {};
        hidOwnershipRefreshSecondDue = {};
        speakerRoutingActive = false;
        transportHandoffLedReleasePending = false;
    }

    bool writeReport(
        const std::array<std::uint8_t, kDualSenseUsbOutputReportSize>& report,
        std::string_view kind,
        bool disconnectOnFailure = true)
    {
        if (presenceOnly) {
            return false;
        }
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        OVERLAPPED writeOverlapped{};
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            writeLog("Native USB: failed to create write event");
            return false;
        }
        writeOverlapped.hEvent = event;

        const auto sequence = ++txSequence;

        // Standard DualSense USB uses the 48-byte payload below. DualSense
        // Edge reports a larger USB output length (64 bytes on field-tested
        // hardware), and Windows requires WriteFile to submit that full size.
        // Preserve the proven 48-byte payload exactly and zero-pad only the
        // unused tail when HID caps advertise a larger report.
        const auto sendLength =
            outputReportLength > report.size() ?
                static_cast<std::size_t>(outputReportLength) :
                report.size();

        const bool padded =
            sendLength > report.size();

        std::vector<std::uint8_t> paddedReport;
        const void* sendBuffer =
            report.data();

        if (padded) {
            paddedReport.assign(sendLength, 0U);
            std::memcpy(
                paddedReport.data(),
                report.data(),
                report.size());
            sendBuffer =
                paddedReport.data();
        }

        writeLog(
            std::string("Native USB TX #") + std::to_string(sequence) +
            " kind=" + std::string(kind) + " requested=" +
            std::to_string(sendLength) + " baseReport=" +
            std::to_string(report.size()) + " capsOut=" +
            std::to_string(outputReportLength) + " padded=" +
            (padded ? "yes" : "no") + " " +
            describeUsbOutputReport(report));

        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(
            handle,
            sendBuffer,
            static_cast<DWORD>(sendLength),
            &bytesWritten,
            &writeOverlapped);

        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(event, 100);
            if (wait == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(handle, &writeOverlapped, &bytesWritten, FALSE);
            } else {
                cancelIoAndWait(handle, writeOverlapped);
                ok = FALSE;
            }
        }

        CloseHandle(event);

        if (!ok || bytesWritten != sendLength) {
            writeLog(
                std::string("Native USB TX #") + std::to_string(sequence) +
                " result=FAIL bytesWritten=" + std::to_string(bytesWritten) +
                " lastError=" + std::to_string(GetLastError()));
            if (disconnectOnFailure) {
                writeLog("Native USB: HID write failed; controller will be rediscovered");
                markDisconnected();
            } else {
                writeLog("Native USB: speaker-routing HID write failed; controller remains available for existing HID features");
            }
            return false;
        }
        writeLog(
            std::string("Native USB TX #") + std::to_string(sequence) +
            " result=OK bytesWritten=" + std::to_string(bytesWritten));
        return true;
    }

    bool ensureLightbarInitialized()
    {
        if (lightbarInitialized) {
            return true;
        }
        const auto report = buildUsbLightbarInitializationReport();
        if (!writeReport(report, "init")) {
            return false;
        }
        lightbarInitialized = true;

        if (transportHandoffLedReleasePending) {
            std::array<
                std::uint8_t,
                kDualSenseUsbOutputReportSize> release{};

            release[0] = 0x02U;
            // report[2] is common valid_flag_1.
            // RELEASE_LEDS is bit 3 and is intentionally sent alone.
            release[2] = 0x08U;

            if (!writeReport(
                    release,
                    "release-leds")) {
                return false;
            }

            transportHandoffLedReleasePending =
                false;

            writeLog(
                "Native USB: transport handoff LED ownership RELEASE_LEDS");
        }

        return true;
    }

    bool establishSpeakerRouting()
    {
        if (!speakerRoutingRequested || handle == INVALID_HANDLE_VALUE) {
            speakerRoutingActive = false;
            return false;
        }
        std::array<std::uint8_t, kDualSenseUsbOutputReportSize> report{};
        report[0] = 0x02;
        applyUsbInternalSpeakerRouting(report);
        const bool ok = writeReport(report, "speaker-route", false);
        speakerRoutingActive = ok;
        if (ok) {
            writeLog("Native USB: controller speaker routing ACTIVE path=internal-right volume=0x64 preamp=0x05");
        } else {
            writeLog("Native USB: controller speaker routing unavailable; speaker probe may be silent, other HID features continue");
        }
        return ok;
    }

    bool clearSpeakerRouting()
    {
        speakerRoutingActive = false;
        if (handle == INVALID_HANDLE_VALUE) {
            return true;
        }

        std::array<std::uint8_t, kDualSenseUsbOutputReportSize> report{};
        report[0] = 0x02;
        applyUsbInternalSpeakerRoutingDisabled(report);
        const bool ok = writeReport(report, "speaker-route-off", false);
        if (ok) {
            writeLog("Native USB: controller speaker routing INACTIVE");
        } else {
            writeLog("Native USB: controller speaker route-off write failed; other HID features continue");
        }
        return ok;
    }

    void armHidOwnershipRefresh()
    {
        const auto now = std::chrono::steady_clock::now();
        hidOwnershipRefreshFirstDue =
            now + std::chrono::milliseconds(500);
        hidOwnershipRefreshSecondDue =
            now + std::chrono::milliseconds(1500);
        hidOwnershipRefreshFirstPending = true;
        hidOwnershipRefreshSecondPending = true;
        writeLog(
            "Native USB: transport handoff HID ownership refresh armed "
            "previous=Bluetooth delaysMs=500,1500 rehook=no");
    }

    void serviceHidOwnershipRefresh()
    {
        if (handle == INVALID_HANDLE_VALUE ||
            (!hidOwnershipRefreshFirstPending &&
             !hidOwnershipRefreshSecondPending)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        if (hidOwnershipRefreshFirstPending &&
            now >= hidOwnershipRefreshFirstDue) {
            HidWriteTrace::refresh(handle);
            hidOwnershipRefreshFirstPending = false;
            writeLog(
                "Native USB: HID ownership refresh pass=1/2 "
                "reason=post-Bluetooth-USB-late-handle");
        }

        if (hidOwnershipRefreshSecondPending &&
            now >= hidOwnershipRefreshSecondDue) {
            HidWriteTrace::refresh(handle);
            hidOwnershipRefreshSecondPending = false;
            writeLog(
                "Native USB: HID ownership refresh pass=2/2 "
                "reason=post-Bluetooth-USB-final");
        }
    }

    bool writeCurrentOutput()
    {
        serviceHidOwnershipRefresh();
        auto report = buildUsbOutputReport(output);
        if (speakerRoutingActive) {
            applyUsbInternalSpeakerRouting(report);
        }
        return writeReport(report, "steady");
    }
};

sds::NativeUsbBackend::NativeUsbBackend(
    LogCallback log,
    bool requestControllerSpeaker,
    bool presenceOnly) :
    _impl(std::make_unique<Impl>(
        std::move(log),
        requestControllerSpeaker,
        presenceOnly))
{}

sds::NativeUsbBackend::~NativeUsbBackend()
{
    disconnect();
}

sds::NativeUsbBackend::NativeUsbBackend(NativeUsbBackend&&) noexcept = default;
sds::NativeUsbBackend& sds::NativeUsbBackend::operator=(NativeUsbBackend&&) noexcept = default;

bool sds::NativeUsbBackend::connect()
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
        _impl->writeLog("Native USB: SetupDiGetClassDevs failed");
        return false;
    }

    bool connectedDevice = false;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &hidGuid, index, &interfaceData)) {
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
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailStorage.data());
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
        const bool gotCaps = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData(preparsed);
        if (!gotCaps) {
            CloseHandle(candidate);
            continue;
        }

        auto identity = classifyDevice(
            attributes.VendorID,
            attributes.ProductID,
            caps.InputReportByteLength);
        if (!identity.supported() || identity.connection != ConnectionType::Usb) {
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
        _impl->overlapped = {};
        _impl->overlapped.hEvent = readEvent;
        _impl->device = std::move(identity);
        _impl->output = {};
        _impl->lightbarInitialized = false;
        _impl->outputReportLength = caps.OutputReportByteLength;
        _impl->readPending = false;
        connectedDevice = true;
        _impl->writeLog(
            std::string("Native USB: HID caps input=") +
            std::to_string(caps.InputReportByteLength) +
            " output=" + std::to_string(caps.OutputReportByteLength) +
            " feature=" + std::to_string(caps.FeatureReportByteLength));
        _impl->writeLog(std::string("Native USB: connected ") + modelName(_impl->device.type));
        if (!_impl->presenceOnly) {
            HidWriteTrace::start(_impl->handle, _impl->log);
            if (_impl->speakerRoutingRequested) {
                (void)_impl->establishSpeakerRouting();
            }
        } else {
            _impl->writeLog(
                "Native USB: presence-only monitor ACTIVE access=read-only hidWrites=none touchR2=none speakerRoute=none");
        }
        break;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return connectedDevice;
}

void sds::NativeUsbBackend::disconnect() noexcept
{
    if (!_impl) {
        return;
    }

    if (_impl->handle != INVALID_HANDLE_VALUE && !_impl->presenceOnly) {
        // Full mode best effort: release trigger resistance and LEDs before closing.
        _impl->output = {};
        _impl->writeCurrentOutput();
    }
    _impl->markDisconnected();
}

bool sds::NativeUsbBackend::connected() const noexcept
{
    return _impl && _impl->handle != INVALID_HANDLE_VALUE;
}

bool sds::NativeUsbBackend::refreshPresence()
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
        &hidGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        _impl->writeLog(
            "Native USB: presence enumeration failed; controller will be rediscovered");
        _impl->markDisconnected();
        return false;
    }

    bool interfacePresent = false;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);

        if (!SetupDiEnumDeviceInterfaces(
                deviceInfoSet,
                nullptr,
                &hidGuid,
                index,
                &interfaceData)) {
            break;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet,
            &interfaceData,
            nullptr,
            0,
            &requiredSize,
            nullptr);

        if (requiredSize == 0) {
            continue;
        }

        std::vector<std::byte> detailStorage(requiredSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
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

        if (CompareStringOrdinal(
                detail->DevicePath,
                -1,
                _impl->device.path.c_str(),
                -1,
                TRUE) == CSTR_EQUAL) {
            interfacePresent = true;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    if (interfacePresent) {
        return true;
    }

    _impl->writeLog(
        "Native USB: presence-only interface absent; controller will be rediscovered");
    _impl->markDisconnected();
    return false;
}

sds::DeviceIdentity sds::NativeUsbBackend::identity() const
{
    return _impl ? _impl->device : DeviceIdentity{};
}

sds::Capabilities sds::NativeUsbBackend::capabilities() const noexcept
{
    if (!connected()) {
        return {};
    }
    return {
        .adaptiveTriggers = true,
        .lightbar = true,
        .touchpadInput = true,
        .advancedHaptics = false,
        .controllerSpeaker = _impl->speakerRoutingActive,
        .bluetoothTransport = false
    };
}

std::optional<sds::TouchState> sds::NativeUsbBackend::pollTouch()
{
    if (_impl && _impl->presenceOnly) {
        return std::nullopt;
    }
    if (!connected()) {
        return std::nullopt;
    }

    DWORD bytesRead = 0;

    if (_impl->readPending) {
        if (!GetOverlappedResult(_impl->handle, &_impl->overlapped, &bytesRead, FALSE)) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                return std::nullopt;
            }
            _impl->writeLog("Native USB: HID read failed; controller will be rediscovered");
            _impl->markDisconnected();
            return std::nullopt;
        }
        _impl->readPending = false;
        if (bytesRead == _impl->inputReport.size()) {
            return parseUsbInputReport(_impl->inputReport);
        }
        return std::nullopt;
    }

    ResetEvent(_impl->readEvent);
    std::memset(&_impl->overlapped, 0, sizeof(_impl->overlapped));
    _impl->overlapped.hEvent = _impl->readEvent;
    _impl->inputReport.fill(0);
    _impl->inputReport[0] = 0x01;

    if (ReadFile(
            _impl->handle,
            _impl->inputReport.data(),
            static_cast<DWORD>(_impl->inputReport.size()),
            &bytesRead,
            &_impl->overlapped)) {
        if (bytesRead == _impl->inputReport.size()) {
            return parseUsbInputReport(_impl->inputReport);
        }
        return std::nullopt;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        _impl->readPending = true;
        return std::nullopt;
    }

    _impl->writeLog("Native USB: failed to start HID read; controller will be rediscovered");
    _impl->markDisconnected();
    return std::nullopt;
}

bool sds::NativeUsbBackend::setLightbar(Color color)
{
    if (!connected() || !_impl->ensureLightbarInitialized()) {
        return false;
    }
    _impl->output.lightbar = color;
    return _impl->writeCurrentOutput();
}

bool sds::NativeUsbBackend::setTriggers(const TriggerEffect& left, const TriggerEffect& right)
{
    if (!connected() || !_impl->ensureLightbarInitialized()) {
        return false;
    }
    _impl->output.leftTrigger = left;
    _impl->output.rightTrigger = right;
    return _impl->writeCurrentOutput();
}

bool sds::NativeUsbBackend::setOutputState(const OutputState& state)
{
    if (!connected() || !_impl->ensureLightbarInitialized()) {
        return false;
    }
    _impl->output = state;
    return _impl->writeCurrentOutput();
}

bool sds::NativeUsbBackend::setControllerSpeakerRoutingEnabled(bool enabled)
{
    if (!_impl) {
        return !enabled;
    }
    if (_impl->presenceOnly) {
        return !enabled;
    }

    if (_impl->speakerRoutingRequested == enabled) {
        if (!enabled || _impl->speakerRoutingActive) {
            return true;
        }
    }

    _impl->speakerRoutingRequested = enabled;
    if (!connected()) {
        _impl->speakerRoutingActive = false;
        return !enabled;
    }

    return enabled ?
        _impl->establishSpeakerRouting() :
        _impl->clearSpeakerRouting();
}

void sds::NativeUsbBackend::prepareTransportHandoff(
    ConnectionType previous) noexcept
{
    if (!_impl ||
        _impl->presenceOnly ||
        !connected() ||
        previous != ConnectionType::Bluetooth) {

        return;
    }

    // The controller already carries a valid LED state across the physical
    // Bluetooth -> USB transition. Hardware testing showed that forcing the
    // USB LIGHTBAR_SETUP/LIGHT_OFF + RELEASE_LEDS sequence at this point is
    // exactly when the previously-correct LEDs disappear.
    //
    // Preserve that controller-owned state for the handoff and let the first
    // ordinary steady packet update RGB/player indicators directly.
    _impl->transportHandoffLedReleasePending =
        false;

    _impl->lightbarInitialized =
        true;

    _impl->writeLog(
        "Native USB: transport handoff LED preserve mode "
        "previous=Bluetooth setup=skip release=skip behavior=steady-only");
    _impl->armHidOwnershipRefresh();
}

void sds::NativeUsbBackend::resetOutputs() noexcept
{
    if (!connected() || (_impl && _impl->presenceOnly)) {
        return;
    }
    _impl->output = {};
    _impl->writeCurrentOutput();
}
