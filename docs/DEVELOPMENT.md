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
host tools, JDK 17 or 21, SDK 35/build-tools 35.0.0 and NDK 26.1.10909125. Set
`ANDROID_SDK_ROOT`, `ANDROID_NDK_ROOT`, `QT_HOST_PATH` and `QT_ROOT_DIR` to those installations.

```sh
export QT_ROOT_DIR=/path/to/Qt/6.8.3/android_arm64_v8a
cmake --preset android-arm64 -DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"
cmake --build --preset android-arm64 --target apk yolopdf-cli
```

The shared QML app uses system-picker file access and a cache-before-SAF
Save As. Android bridges cover PDF open/share intents, an image clipboard
provider and PrintManager; GUI/device validation remains pending. Android
presets produce debug-signed APKs for testing. The Qt toolchain wraps
the NDK and correctly roots Qt package discovery. MuPDF uses the actual Android
API level, with the Bionic entropy declaration included at build time.
The native Android CLI has info/text-runs/replace-text and avoids
creating a Qt application without a JVM. Latin-accent and CJK replacements have
run on an API 29 x86_64 emulator and passed qpdf, Poppler and PDFium checks.
For the complete GUI/SAF emulator test, build the x86_64 preset with a matching Qt SDK and
`-DYOLOPDF_ANDROID_TEST_HOOKS=ON`, then run:

```sh
bash tests/android/run-emulator.sh build/android-x86_64
```

The harness needs a running adb emulator, host Python test requirements, qpdf
and Poppler. It exercises a test-only DocumentsProvider protected by
MANAGE_DOCUMENTS and pulls PDFs for verification. Test hooks and the provider
default OFF and must never ship.
The normal manifest declares no Internet permission. An APK build and a working
on-device editing/save spike are separate gates.

## Checks and cloud activation

CTest runs fourteen Linux suites, covering engine contracts, core sessions,
recovery, stamp/font libraries, real process-kill tests, independent qpdf/Poppler/
PDFium markings and print conversion, CLI/native replay, engine boundaries,
Qt Quick drop rules, real QML window interactions and two app smoke checks.
Missing independent tools are failures. UI tests draw a marker, add a strikeout,
undo/redo, move an image, preview imported fonts, open a password-protected batch,
and save editable/flattened copies. See [PROGRESS.md](PROGRESS.md) for limitations.
Windows omits the Unix native replay and Linux process-kill targets. Private PDF documents stay outside
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

## Packaging and corresponding source

Linux install layout: `cmake --install build/linux --prefix /tmp/yolopdf-package`.
The install includes desktop integration, documentation and dependency notices.
See `packaging/flatpak/README.md` and `packaging/windows/README.md` for package
builds. `tools/package_source.py` bundles a clean checkout with every recursive
MuPDF dependency; source archives carry provenance for building without Git.
`tools/fetch_qt_source.py` verifies the exact Qt runtime modules' upstream source
archives against vendor SHA-256 checksums. CI collects source alongside binaries.
Production signing and store publication are separate, unconfigured release steps.
