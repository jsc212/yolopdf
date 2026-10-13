# Development

## Toolchains

CI pins Qt 6.8.3 (LTS), C++20 and Python 3.12. Local Qt 6.8+ is supported;
this cloud instance uses Debian's Qt 6.8.2. MuPDF is pinned to 1.28.5
(`8ad45e92f0935d3d87f1db3f873086472a5e1b24`); initialize submodules recursively.
The upstream Makefile builds static MuPDF on Linux/Android, while Windows
uses upstream MSBuild with MSVC v143, x64 Release. Build outputs remain under
each preset's build directory. Four MuPDF jobs are the default; use
`-DYOLOPDF_MUPDF_JOBS=2` on smaller machines. Incremental builds reuse outputs.
Only `src/engine/mupdf/` includes MuPDF headers; other layers use our API types.
Qt is dynamically linked and Qt PDF is not used.

## Linux / Fedora

Install Qt Base/Declarative development files, Qt Quick Dialogs, CMake 3.24+,
Ninja, Make, a C++ compiler, Python with venv/pip, qpdf and Poppler. For Fedora
with Qt 6.8+ (package names may differ by release):

```sh
sudo dnf install gcc-c++ cmake ninja-build make python3 python3-pip qt6-qtbase-devel qt6-qtdeclarative-devel qpdf poppler-utils
git submodule update --init --recursive
python3 -m venv /tmp/yolopdf-test-venv
/tmp/yolopdf-test-venv/bin/python -m pip install -r tests/requirements.txt
cmake --preset linux -DPython3_EXECUTABLE=/tmp/yolopdf-test-venv/bin/python
cmake --build --preset linux
ctest --preset linux
build/linux/yolopdf-cli info document.pdf
build/linux/yolopdf
```

For an externally installed Qt, add `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64`.
For engine/CLI work only, set `-DYOLOPDF_BUILD_APP=OFF`. Test-only requirements
pin fontTools for generated subsets and collections, PDFium for independent
rendering/extraction, and Pillow for image comparisons. Generated files stay
in the build tree. See [STAGE1.md](STAGE1.md) for commands and supported scope.

## Windows

Install Visual Studio 2022 with Desktop development with C++, CMake, Ninja,
Python 3.12 and Qt 6.8.3 `msvc2022_64`. Install qpdf and Poppler and add their
binary directories to PATH (CI uses Chocolatey). In an x64 Native Tools prompt,
add Qt's `bin` to PATH for runtime DLLs:

```bat
git submodule update --init --recursive
python -m pip install -r tests/requirements.txt
cmake --preset windows-msvc -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build --preset windows-msvc
ctest --preset windows-msvc
build\windows-msvc\yolopdf-cli.exe info document.pdf
```

The MuPDF wrapper supports x64 Release only. Windows execution is an open CI
gate; a Linux build does not validate it.

## Android

Use Qt 6.8.3 `android_arm64_v8a` (or `android_x86_64`) with matching Qt Linux
host tools, JDK 17, SDK 35/build-tools 35.0.0 and NDK 26.1.10909125. Set
`ANDROID_SDK_ROOT`, `ANDROID_NDK_ROOT` and `QT_HOST_PATH` to those installations.

```sh
cmake --preset android-arm64 -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/android_arm64_v8a
cmake --build --preset android-arm64 --target apk yolopdf-cli
```

The shared QML app uses system-picker file access and a cache-before-SAF
Save As. Android bridges cover PDF open/share intents, an image clipboard
provider and PrintManager; device validation remains pending. The native Android CLI has only info/text-runs/replace-text and avoids
creating a Qt application without a JVM. Device execution remains unverified
here. For the emulator test, build the x86_64 preset with a matching Qt SDK and
`-DYOLOPDF_ANDROID_TEST_HOOKS=ON`, then run:

```sh
bash tests/android/run-emulator.sh build/android-x86_64
```

The harness needs a running adb emulator, host Python test requirements, qpdf
and Poppler. It exercises a nonexported test DocumentsProvider and pulls PDFs
for verification. Test hooks and the provider default OFF and must never ship.
The normal manifest declares no Internet permission. An APK build and a working
on-device editing/save spike are separate gates.

## Checks and cloud activation

CTest runs nine Linux suites: basic/Stage 1 engine contracts, session/recovery/
parallel-rendering/print contracts, independent exports, native-driver replay,
CLI, engine boundary, QML startup and sample replacement. Independent verifiers are required; missing tools fail the suite.
The tests include undo/redo, rollback, saving/reopening, source preservation,
exact outside-edit pixel comparisons, font coverage and shared-object isolation.
The suite also checks mixed-style line edits, empty-space wrapping, encrypted
recovery, composite page transfers, highlight appearances in raster print copies
and physical page geometry after print conversion. See [PROGRESS.md](PROGRESS.md)
for untested platform/UI requirements.
Windows omits the Unix native replay target. Private PDF documents stay outside
the checkout. No real-document corpus was available in this task.

The cloud toolchain is installed outside the repository, with a test venv and
writable font caches. Activate it in each new shell:

```sh
source /workspace/.cache/yolopdf/environment/env.sh
cd /workspace/yolopdf
cmake --preset linux -DPython3_EXECUTABLE=/workspace/.cache/yolopdf/test-venv/bin/python
cmake --build --preset linux
ctest --preset linux
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software build/linux/yolopdf --fixture-spike --smoke-test
```

Use the existing isolated checkout; do not create a worktree unless requested.
No persistent server is needed. Cloud instructions are saved as a configuration
draft; reviewing/saving/publishing it is separate from current-instance checks.
