# Implementation progress

This is an in-development preview, not a completed v1 release. The original
[implementation plan](IMPLEMENTATION_PLAN.md) remains the target; implementing
later-stage features does not waive earlier verification gates.

## Implemented

- Qt 6.8+/C++20 scaffolding, pinned MuPDF 1.28.5, Linux/Windows/Android presets,
  desktop and Android CI definitions, and the AGPL/ownership policy.
- Engine tracing and classification, original-font replacement with an automatic
  extraction/render guard, shared Form copy-on-write, explicit imported or exact
  installed fonts, mixed-style line edits, size/colour changes and live preview.
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
  keyboard nudging, alignment guides and page drag/drop routes.
- PDF highlights built from selected text quads, notes, arrows and rectangles,
  generated annotation appearances and printable flags. Desktop print preview
  uses raster pages that include annotations and offers actual size or fit.
  The print backend can produce a raster PDF copy with appearances baked into
  page images; a separate UI action for a flattened sharing copy is pending.
- Android source bridges for SAF, PDF intents, clipboard image URI grants and
  PrintManager. These are not yet verified in an Android build or device run.

## Validation

The Linux CTest suite has nine suites. It exercises generated fixtures, engine
contracts, safe sessions, native journal recovery, concurrent tile rendering,
composite undo/redo, saved highlight appearances in print copies, mixed print
page geometry, the CLI and QML startup/sample editing. Independent engine tests
use qpdf, Poppler and PDFium. Missing verifiers fail tests; they are not skipped.

The highlight/print regression checks use MuPDF to reopen saved annotations and
raster print copies. Independent-viewer and physical-printer checks for those
new annotation workflows still need to run. The signature fixture checks field
markers and incremental byte preservation, not real cryptographic validity.

Recovery uses a compatibility adapter for the pinned MuPDF 1.28.5 journal
format/ABI. Stock snapshot/deserializer behavior required incremental-section
and journal-entry fixes. Keep the version pin; review the adapter and recovery
regressions before upgrading MuPDF. The upstream submodule remains unchanged.

## Remaining requirements and release gates

- The requested **strikeout, freehand marker and flattened-sharing-copy UI** are
  pending. Highlights already persist as PDF annotations and the print backend
  includes their appearances. Printing markings is a required release gate.
- Windows/MSVC and Android APK/emulator execution, real-device SAF/intent/
  clipboard/printing checks, and equivalent-output comparison across platforms.
- Private invoices/brochures/scans: none were available. Broader font/layout
  coverage and a curated golden-image corpus are still needed.
- Actual-size print proofs, including a measured 40 × 20 mm stamp. Tested
  Poppler/PDFium UserUnit raster behavior differs from MuPDF; object dimensions
  alone do not prove physical print size.
- Full UI automation for DropRouter/tool modes, selection restoration through
  undo, font-substitution comparison previews, mobile properties refinement,
  resize/rotate handles and a complete keyboard/accessibility/high-contrast audit.
- Stamp/font library workflow tests, save-interruption/crash injection and
  recovery lifecycle tests beyond the current core contracts.
- Flatpak/Windows installers, complete dependency notices and corresponding-
  source bundles, signed APK/AAB release packaging and the manual viewer checklist.
- Family/weight font matching, complex-script shaping, tagged/PDF-A conformance
  preservation, unrestricted paragraph reflow and unsupported PDF structures.

Do not distribute a production release until these gates and license obligations
are satisfied. CI definitions are executable scaffolding, not evidence that
remote jobs have passed. No PR or release is created by the implementation push.
