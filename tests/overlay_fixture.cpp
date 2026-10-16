// A test-owned DLL used only to exercise real module enumeration.
extern "C" __declspec(dllexport) int steamfix_overlay_fixture() noexcept {
    return 1085660;
}
