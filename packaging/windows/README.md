# Windows installer

From an x64 Visual Studio/Qt developer shell, initialize submodules and build
`windows-msvc` as described in docs/DEVELOPMENT.md. Then run:

```bat
cmake --install build\windows-msvc --prefix build\windows-package
cpack --config build\windows-msvc\CPackConfig.cmake -G NSIS
```

Install NSIS 3 before creating the installer. The install step deploys dynamic
Qt libraries and QML plugins; bundled license notices and build instructions
are installed alongside the program. CPack adds an Open with association for
PDFs without replacing the user's default reader.

The CI artifact is a development preview. No production Authenticode certificate
is configured. Any distributed binary must have its matching corresponding
source bundle, including the pinned MuPDF dependencies. LGPL Qt source must
also be available under its applicable license terms; do not omit it when
redistributing Qt binaries. See packaging/licenses/README.md.
