# Flatpak development preview

Use a clean committed checkout with initialized recursive submodules:

```sh
python3 tools/package_source.py --output build/flatpak/yolopdf-source.tar.gz --notices build/flatpak/notices
flatpak install --user flathub org.kde.Platform//6.8 org.kde.Sdk//6.8
flatpak-builder --user --install --force-clean build/flatpak-app packaging/flatpak/org.yolopdf.app.json
flatpak run org.yolopdf.app
```

The module builds MuPDF statically from the matching source archive. Qt is
provided dynamically by the KDE runtime. No network or host-filesystem access
is granted; file dialogs use portals. Printing uses the CUPS socket. Source
archives carry SOURCE_COMMITS.json so the build can verify the engine pin even
without Git metadata.

For a Flathub submission, replace the local archive source with a published,
immutable corresponding-source URL and SHA-256 after release gates pass. This
preview manifest is not a Flathub submission or published release.
