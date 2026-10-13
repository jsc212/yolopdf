# yolopdf

An offline PDF editor for Fedora, Windows, and Android, sharing one MuPDF engine
behind a C++ boundary. Tools focus on text, images, stamps, and pages.

**Status:** development preview. The desktop UI has tabs, continuous viewing,
text editing, image tools, stamps, page organization, annotations and printing.
Safe saves, undo/redo, encrypted/signed-file handling and crash recovery are
implemented. Linux tests cover the engine and core workflows; Windows and
Android execution and physical print proofs remain open release gates.
This is not a finished v1 release. See the [implementation plan](docs/IMPLEMENTATION_PLAN.md),
[progress and remaining work](docs/PROGRESS.md), [development guide](docs/DEVELOPMENT.md)
and [engine commands and limits](docs/STAGE1.md).

## Build (Linux)

Prerequisites: CMake 3.24+, Ninja, a C++20 compiler, GNU Make, Python 3.12,
qpdf, Poppler, and dynamically linked Qt 6.8+ with Core, Gui, Quick,
QuickControls2, Concurrent, Widgets, PrintSupport, Test, and the Qt Quick Dialogs QML module.

```sh
git submodule update --init --recursive
python3 -m venv /tmp/yolopdf-test-venv
/tmp/yolopdf-test-venv/bin/python -m pip install -r tests/requirements.txt
cmake --preset linux -DPython3_EXECUTABLE=/tmp/yolopdf-test-venv/bin/python
cmake --build --preset linux
ctest --preset linux
build/linux/yolopdf-cli info document.pdf
build/linux/yolopdf-cli render document.pdf page.png --page 1 --dpi 144
build/linux/yolopdf
```

The engine uses MuPDF **1.28.5**, pinned by submodule commit, and builds its
bundled dependencies statically. Qt remains dynamically linked. Qt PDF is not
used. Input PDFs are opened without writes. `info` emits JSON and inventories
font and image **resources**, including nested Forms. Occurrence selection and
inline-image tracing are available through `images`.

## License

AGPL-3.0-only; see [LICENSE](LICENSE). Redistribution of binaries must include
or provide the corresponding source under the applicable license terms,
including this project's build scripts and pinned dependencies. MuPDF is AGPL;
Qt is used under LGPLv3 with dynamic linking. The bundled dependencies retain
their own licenses. Packaging must ship their notices and satisfy their terms.

Outside code contributions are currently closed; see
[CONTRIBUTING.md](CONTRIBUTING.md) for the ownership policy.
