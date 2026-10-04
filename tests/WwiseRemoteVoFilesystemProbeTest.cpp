#include <StarfieldDualSense/WwiseRemoteVoFilesystemProbe.h>

#include <cassert>
#include <filesystem>
#include <string>

int main()
{
    const std::wstring captured = L"Sound\\Voice\\Starfield.esm\\GenericFemaleEvenToned\\00588978.wem";
    const std::filesystem::path executable =
        std::filesystem::path(L"C:\\Games\\Steam\\steamapps\\common\\Starfield\\Starfield.exe");

    const auto candidates = sds::buildRemoteVoFilesystemCandidates(captured, executable);
    assert(candidates.raw.label == "raw");
    assert(candidates.dataRoot.label == "data-root");
    assert(candidates.raw.path == std::filesystem::path(captured));

#ifdef _WIN32
    assert(candidates.dataRoot.path ==
        std::filesystem::path(L"C:\\Games\\Steam\\steamapps\\common\\Starfield\\Data\\Sound\\Voice\\Starfield.esm\\GenericFemaleEvenToned\\00588978.wem"));
#else
    // Portable test: the helper must split Windows-style Wwise separators into
    // filesystem components even when the test host uses '/'.
    const auto text = candidates.dataRoot.path.generic_string();
    assert(text.find("Data/Sound/Voice/Starfield.esm/GenericFemaleEvenToned/00588978.wem") != std::string::npos);
#endif

    return 0;
}
