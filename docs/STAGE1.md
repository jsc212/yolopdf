# Stage 1 engine spike

Stage 1 remains open. Linux passes the generated-corpus checks, and the native
Android driver has edited Latin-accent and CJK fixtures on an emulator. Windows
execution, Android GUI/SAF validation, private-document trials and printed stamp
proofs have not closed the plan's gates. Later-stage features are described in
[PROGRESS.md](PROGRESS.md).

## CLI examples

Follow [DEVELOPMENT.md](DEVELOPMENT.md) first. Desktop CLI page numbers are
one-based; run/occurrence IDs are zero-based and must be refreshed after editing.
Inputs are opened without writing. Mutations require a separate output path.

```sh
build/linux/yolopdf-cli text-runs build/linux/tests/corpus/subset-latin.pdf
build/linux/yolopdf-cli replace-text build/linux/tests/corpus/subset-latin.pdf /tmp/edited.pdf --run 0 --text 'éè'
build/linux/yolopdf-cli replace-text build/linux/tests/corpus/subset-latin.pdf /tmp/imported.pdf --text 'Ω' --font build/linux/tests/corpus/YoloTest-Regular.ttf
build/linux/yolopdf-cli font-match document.pdf --run 0
build/linux/yolopdf-cli images document.pdf --page 1
build/linux/yolopdf-cli copy-image document.pdf /tmp/image.png --occurrence 0
build/linux/yolopdf-cli copy-image document.pdf /tmp/image.jpg --occurrence 0 --original-jpeg
build/linux/yolopdf-cli copy-area document.pdf /tmp/area.png --x 10 --y 10 --width 40 --height 20
build/linux/yolopdf-cli delete-image document.pdf /tmp/deleted.pdf --occurrence 0
build/linux/yolopdf-cli replace-image document.pdf /tmp/replaced.pdf artwork.png --occurrence 0
build/linux/yolopdf-cli transform-image document.pdf /tmp/moved.pdf --matrix '100,0,0,50,30,40'
build/linux/yolopdf-cli insert-image document.pdf /tmp/image-added.pdf artwork.png --matrix '100,0,0,50,30,40'
build/linux/yolopdf-cli place-stamp document.pdf /tmp/stamped.pdf artwork.svg --x 10 --y 10 --width 40 --height 20
build/linux/yolopdf-cli flatten-stamps /tmp/stamped.pdf /tmp/flattened.pdf
build/linux/yolopdf-cli insert-pages document.pdf /tmp/merged.pdf other.pdf another.pdf --at 2
build/linux/yolopdf-cli reorder-pages document.pdf /tmp/reordered.pdf --pages '3,1,2,2'
build/linux/yolopdf-cli delete-pages document.pdf /tmp/deleted-pages.pdf --pages 2
build/linux/yolopdf-cli extract-pages document.pdf /tmp/extracted.pdf --pages '1-3,7'
build/linux/yolopdf-cli rotate-page document.pdf /tmp/rotated.pdf --page 1 --degrees 90
build/linux/yolopdf-cli blank-page document.pdf /tmp/blank.pdf --at 1 --width 210 --height 297
```

Traced coordinates and `--matrix` use displayed page points, top-left origin,
with CropBox, rotation and UserUnit applied. Stamp/area sizes and offsets are
millimetres in the same coordinate system. Area copies default to 300 DPI.
`--font-index` selects a collection face. Stamps also accept PNG, a GIF frame
(`--frame`), rotation and opacity.

## Text replacement

The processor traces text-show operators, glyphs, text state, transforms and
Form occurrence paths, then joins glyphs with structured text. `text-runs`
reports operator-level runs; `text-lines` groups adjacent compatible runs into
lines with style segments. `replace-text` is the operator API; `replace-line`
preserves retained characters' styles and gives insertions the preceding style.

```sh
build/linux/yolopdf-cli text-lines document.pdf
build/linux/yolopdf-cli replace-line document.pdf /tmp/edited.pdf --run 0 --text 'Changed text'
build/linux/yolopdf-cli replace-line document.pdf /tmp/wrapped.pdf --run 0 --text 'Longer text' --wrap-width 100
```

`--wrap-width` uses displayed PDF points. Line IDs refer to the first source
run and may have gaps; use the reported ID, not a position in the returned list.
Size changes on lines use physical points. Imported fonts remain explicit and
apply to the entire line; fonts are not silently substituted. Installed-font
lookup checks exact PostScript names. Font collections support a face index.

Coverage uses Encoding/ToUnicode/CIDToGID mappings, font metrics and embedded
FreeType glyphs. The corpus covers simple TrueType/CFF, Type0/Identity-H,
accents, CJK, subset coverage and explicit imports. A usable embedded cmap can
supply missing encoding entries through a private Identity-H resource; shared
font dictionaries remain intact.

Replacement rewrites content in its original stacking and marked-content
position, rather than appending it above the rest of the page. Position-only TJ
adjustments preserve later glyph and line positions. Shared Forms and resources
are copied along the selected occurrence path. A journal transaction verifies
extraction, font/baseline and pixel-exact 144-DPI MuPDF renders outside the
old/new bounds union plus a small glyph margin. Failure rolls back. This fixed-
resolution guard is not proof for every document or resolution.

The UI offers explicit widening, chosen smaller size and restricted wrapping.
Wrapping currently requires single-style, horizontal filled text outside an
existing Form, and verifies empty white space beyond the original line. Other
layouts fail explicitly. Preview is debounced and abandoned without changing
undo history. Imported fonts and stamp assets persist in the application library.

Unsupported cases include Type3, vertical writing, complex shaping/combining
marks, missing Unicode mappings, non-Identity-H composite fonts, invisible or
clipping modes, path clipping, ActualText overrides and tagged Forms needing
structure-tree remapping. Annotation appearances are outside text editing.
Pages without text report a scanned/outlined/empty-page reason.

## Images, stamps and pages

Image editing selects each Do/inline-image occurrence, including nested Forms,
and preserves other placements of shared streams. Copies retain original
resolution and soft-mask alpha; unmasked JPEG streams can be exported unchanged.
The internal image clipboard grafts the original object into another document.
Matrices map the PDF image unit square in displayed coordinates. For upright
image insertion, use a negative displayed Y scale, for example
`--matrix '100,0,0,-50,30,90'` for a 100 × 50 point image at (30,40).

Stamps have custom appearance Forms, alpha, print flags and private physical-size
metadata. PNG/GIF, vector SVG and PDF artwork are supported. Sizing accounts
for UserUnit and rotations. Size is locked by default; movement, rotation,
duplication and opacity are supported. Flattening stamps leaves other
annotations editable. Compositing may differ by one MuPDF/two independent-
renderer colour levels when flattened.

Multi-source insertion is one operation, preserving geometry, basic AcroForm
fields and internal links. Field collisions are renamed. XFA, calculated or
action-bearing fields, duplicate widget pages and excluded link destinations
fail explicitly. Tagged duplication fails. The core supports paired cross-
document move/undo and refuses inconsistent history states across tabs.

## Saving, recovery and printing

Unsigned export clones the reachable catalog graph, subsets fonts on the clone,
writes and reopens it, leaving the live journal intact. Desktop saves use
QSaveFile with no direct-write fallback and detect external source changes.
Android retains a verified cache before streaming to a SAF `rwt` descriptor;
provider writes cannot have desktop filesystem atomicity.

Password entry and encryption preservation are implemented. Signed documents
require explicit confirmation and retain the original signed byte prefix
through incremental saves. This marks edited signatures as modified; a real
cryptographic signature proof remains pending. Recovery stores generation-
paired snapshots/journals with hashes and restores unsaved work as a separate
copy when the source changed. Encrypted journals remain encrypted; passwords
are never persisted. The native journal loader is tied to MuPDF 1.28.5.

Highlights and strikeouts use text selection quads; freehand markers use ink
annotations. Notes, arrows and rectangles have saved
appearances and printable flags. The raster print backend includes annotation
appearances as page pixels and uses physical page geometry. The desktop UI has
print preview with actual-size and fit choices. Save flattened copy bakes annotations into a separate vector PDF without
changing the live document or its undo history.

## Verification limits

Fifteen Linux suites cover engine/core contracts, exact-font lookup, independent exports, native CLI
replay, CLI contracts, the header boundary and QML startup/sample editing.
Independent checks use qpdf, Poppler and PDFium. Generated fixtures and source
render comparisons do not replace private-document trials or curated goldens.
No private corpus was available.

Poppler/PDFium versions tested here use raw-unit raster dimensions on the
UserUnit fixture, while MuPDF applies UserUnit. Tests account for each engine's
measured pixels per physical point without rescaling results. An independent
actual-size view and printed 40 × 20 mm stamp proof remain open.

The app now has a desktop/mobile editing UI, but UI automation and accessibility
coverage are incomplete. Windows/Android CI definitions have not executed here.
Android test hooks and the test-only DocumentsProvider default OFF and must
never ship. That provider is protected by the system MANAGE_DOCUMENTS permission.
An APK build, device operation and physical print proof are separate gates.
