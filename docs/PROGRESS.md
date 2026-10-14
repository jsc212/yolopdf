# Implementation progress

This is an in-development preview, not a completed v1 release. The original
[implementation plan](IMPLEMENTATION_PLAN.md) remains the target; implementing
later-stage features does not waive earlier verification gates.

## Implemented

- Qt 6.8+/C++20 scaffolding, pinned MuPDF 1.28.5, Linux/Windows/Android presets,
  desktop and Android CI definitions, and the AGPL/ownership policy.
- Engine tracing and classification, original-font replacement with an automatic
  extraction/render guard, shared Form copy-on-write, explicit imported or exact
  installed fonts, mixed-style line edits, size/colour changes, live preview and
  an original/selected-font comparison.
  Wrapping is restricted to supported single-style horizontal text and empty
  white space. Widening requires an explicit choice.
- Per-occurrence image editing and original-stream clipboard grafting, area
  copying, vector/alpha/GIF/PDF stamps, locked stamp sizes, rotation/duplication,
  page insertion/reordering/extraction and basic form/link merging.
- An engine worker thread, immutable display lists with per-job cloned contexts,
  tiled page rendering, tabs, continuous viewing, thumbnails, search, selection,
  tool modes, desktop properties and a compact mobile layout.
- Journal undo/redo and paired cross-document page moves, atomic desktop saves,
  cached SAF saves, password entry, permission checks, explicit signed-edit
  confirmation and incremental signed saves.
- Recovery snapshots with integrity hashes and native undo history, source-change
  detection, persistent stamp/font libraries, stamp collection ZIP import/export,
  keyboard nudging, alignment guides, image resizing/rotation, high contrast,
  mobile position controls, selection restoration and tested page-drop routing.
- PDF highlights and strikeouts built from text quads, freehand ink markers,
  notes, arrows and rectangles, saved appearances and print flags. Desktop print
  preview includes annotations and offers actual size or fit. Save flattened copy
  bakes annotations into vector page content in a new file, preserving searchable
  text and leaving the editable document/history intact.
- Android SAF/intent/clipboard/printing bridges, debug APK and AAB builds for
  arm64, and an x86_64 test APK. The Qt/NDK build configuration and Bionic entropy
  header handling are validated without altering the pinned vendor source.
- Linux install integration and dependency notices, a Flatpak preview manifest,
  NSIS installer configuration, clean corresponding-source archives including
  recursive MuPDF dependencies, verified Qt source fetching and packaging CI.
- Native Fedora 43/44 package instructions and CI using distribution Qt, explicit
  Qt Wayland/fontconfig dependencies, local desktop installation instructions and
  the matching GNOME/Wayland desktop-file identity.

## Validation

The Linux CTest suite has fifteen suites. It exercises generated fixtures, engine
contracts, safe sessions, native journal recovery, concurrent tile rendering,
composite undo/redo, saved highlight appearances in print copies, mixed print
page geometry, the CLI and QML startup/sample editing. Independent engine tests
use qpdf, Poppler and PDFium. Missing verifiers fail tests; they are not skipped.

Saved highlights, strikeouts and markers display independently in Poppler and
PDFium. Print flags/appearances are checked, Poppler's print conversion includes
markings, and flattened copies match appearances while retaining extracted text.
Real-window tests draw/strike/undo/redo/export, move images, preview imported
fonts without committing, and open encrypted batches in order. Library tests
cover persistence, ZIP round trips and invalid-import rollback. A real saver is
killed mid-write without changing the original; a killed editor's snapshot and
native undo history are restored. Linux installed-layout smoke checks pass.

A headless native Wayland check passes with Qt 6.8.2 and Weston 14 on Debian:
the installed app renders a page, performs the sample engine edit and announces
`org.yolopdf.app` as its Wayland app ID. The
[native Fedora 43/44 CI run](https://github.com/jsc212/yolopdf/actions/runs/38505649682)
passed both jobs with distribution Qt, the fourteen then-existing suites and
installed native Wayland startup. A later exact-font regression adds a fifteenth
suite: a strong desktop family alias must not hide an installed PostScript face,
and an unmatched name must never return a substitute. Linux now uses `fc-list`
instead of ranked `fc-match` results. All fifteen suites pass locally.
The separate local Fedora 44 container could not install packages because the
cloud egress policy denied Fedora repository hosts; native CI supplied the
distribution validation.

Android native/Java compilation and arm64 APK/AAB/x86_64 APK creation pass.
The native driver ran Latin-accent and CJK replacements on an API 29 emulator;
both results passed qpdf and independent Poppler/PDFium text extraction.
The GUI/SAF smoke test remains unverified: this cloud instance has no KVM,
the initial software GPU failed to create an EGL context, and a second backend
stalled during APK installation. Run the bounded harness on a working emulator
or device; native engine success does not establish GUI/file-provider readiness.
A real cryptographic signature proof and physical-printer checks remain pending;
the signature fixture is a field marker.

Recovery uses a compatibility adapter for the pinned MuPDF 1.28.5 journal
format/ABI. Stock snapshot/deserializer behavior required incremental-section
and journal-entry fixes. Keep the version pin; review the adapter and recovery
regressions before upgrading MuPDF. The upstream submodule remains unchanged.

## Remaining requirements and release gates

- Android emulator/device GUI, SAF, clipboard, intent and print validation beyond
  the verified native edits; equivalent-operation output comparison across platforms.
- Windows/MSVC and Flatpak CI execution, installer deployment and production
  signing. Debug artifacts are previews, not store-ready releases.
- Private invoices/brochures/scans: none were available. Broader font/layout
  coverage and a curated golden-image corpus are still needed.
- Actual-size print proofs, including a measured 40 × 20 mm stamp. Tested
  Poppler/PDFium UserUnit raster behavior differs from MuPDF; object dimensions
  alone do not prove physical print size.
- Further touch/keyboard/accessibility audits, mobile interaction tests and the
  manual independent-viewer checklist. Existing UI tests cover core workflows,
  not every control, screen size, assistive tool or complex document.
- Signed release artifacts and immutable Flathub sources after all release gates
  and source/license obligations have been reviewed.
- Family/weight font matching, complex-script shaping, tagged/PDF-A conformance
  preservation, unrestricted paragraph reflow and unsupported PDF structures.

Do not distribute a production release until these gates and license obligations
are satisfied. CI definitions are executable scaffolding, not evidence that
remote jobs have passed. No PR or release is created by the implementation push.
