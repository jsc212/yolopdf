# Dependency notices and corresponding source

`tools/package_source.py` creates a matching source tarball from a clean checkout,
including every initialized recursive MuPDF submodule, and collects upstream
license/notice files. It refuses missing or modified submodules and excludes
build outputs, caches, private documents and Git metadata. The output identifies
the root commit and every dependency commit. Run it before distributing binaries:

```sh
python3 tools/package_source.py --output /tmp/yolopdf-source.tar.gz --notices /tmp/yolopdf-notices
```

MuPDF and its enabled bundled components retain their upstream licenses. Inspect
the pinned MuPDF COPYING and thirdparty component notices. Tests embed Noto (SIL
OFL) and Droid (Apache-2.0) fonts; their upstream notices are in the source bundle.
The application fixture uses original geometric glyphs under the app license.

Qt is linked dynamically. Fedora/Flatpak users receive Qt from their system or
runtime package, whose distributors provide Qt source. If redistributing Qt
DLLs on Windows or Qt in an APK, provide the corresponding source for the exact
Qt binaries you ship and preserve their LGPL/GPL notices. The upstream source
for the CI Qt version is https://download.qt.io/archive/qt/6.8/6.8.3/submodules/;
components include qtbase, qtdeclarative and applicable dependencies/plugins.
A link alone is not a substitute for satisfying the distribution obligations.

The installed NOTICE file and dependency-license directory explain this policy.
Release artifacts must be reviewed for enabled components and corresponding
source completeness; preview CI builds are not production releases.
