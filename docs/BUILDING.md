# Building from Source

## Requirements

- 64-bit Windows.
- Visual Studio or Visual Studio Build Tools with the **Desktop development with C++** workload, including MSVC and a Windows SDK. CI uses Visual Studio 2022.
- CMake 3.24 or newer, available on `PATH`. The CMake tools included with Visual Studio can be used.

The project uses C++20 and requires MSVC. Linux, macOS, and non-MSVC builds are not supported.

## Build

Open a Visual Studio Developer Command Prompt in the repository root and run:

```batch
make.cmd configure
make.cmd build
```

The Release executable is written to `build\Release\steamfix.exe`. The `build` task also configures the build tree, so the separate `configure` step is optional.

For a Debug build:

```batch
make.cmd debug
```

The Debug executable is written to `build\Debug\steamfix.exe`.

To remove the build directory:

```batch
make.cmd clean
```

This preserves runtime logs and recovery files. Do not delete `steamfix.dat` from a game directory to clean a build; it may be needed to restore Steam overlay permissions.

## Direct CMake Commands

To use the same generator and architecture as CI:

```batch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
cmake --build build --config Release --parallel
```

## Validation

Build both Debug and Release when changing code. The compiler treats warnings as errors (`/W4 /WX`), and both configurations must compile successfully.

[GitHub Actions](../.github/workflows/build.yml) builds both configurations on Windows x64 for pushes and pull requests. The executables are available as workflow artifacts named `steamfix-windows-x64-Debug` and `steamfix-windows-x64-Release`.

There is no automated runtime test suite in the repository. See [AGENTS.md](../AGENTS.md) for behavior to check manually when changing launch or recovery code. Use the [README](../README.md) for installation and normal use.
