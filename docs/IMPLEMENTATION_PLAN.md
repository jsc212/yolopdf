# yolopdf — Implementation Plan

## Context

`jsc212/yolopdf` is an empty repository (README only, branch `claude/loving-sagan-85bdyt`). The goal is a lightweight, offline PDF editor for Fedora, Windows and Android. All three platforms share one engine. The editor is built around four tasks: **edit text, work with images, apply stamps, organize pages**. The riskiest requirement is **replacing existing text while keeping the original font**. That work is de-risked first, as a headless engine spike, before most of the UI is built.

**Deliverable of this planning task:** after approval, write this plan to `docs/IMPLEMENTATION_PLAN.md`, commit it, and push it to `claude/loving-sagan-85bdyt`. No application code is written until the user asks.

### Licensing decision (user answer: "start with open-source MuPDF, maybe switch later")

This works, with the following conditions built into the plan:
- **Release yolopdf under AGPL-3.0** from the first commit. Any binary distributed while on open-source MuPDF must come with its source. Versions already released stay AGPL permanently.
- **Switching to a commercial MuPDF license later** needs no code changes, but the project must own copyright to all of its code. Until that option is ruled out, either accept no outside contributions or require a CLA. Add a `CONTRIBUTING.md` that says this.
- **Switching to a different engine** (for example PDFium (BSD) plus qpdf (Apache)) is only practical if MuPDF stays behind an engine boundary. Rule: **only `src/engine/mupdf/` may include MuPDF headers.** Every other layer uses yolopdf's own types. The engine contract test suite (Stage 1+) is what makes a later port verifiable. Be honest about the limit: the font-preserving text editor is engine-specific work and would have to be re-implemented. The boundary contains that cost but does not remove it.
- Qt is used under LGPLv3 with dynamic linking, which is compatible. Qt PDF is **not** used.

---

## Architecture

```
QML UI (desktop layout | mobile layout, shared components)
        │  Q_PROPERTY / signals / async jobs (QFuture)
C++ app core  (Qt Core, no MuPDF types)
  DocumentSession, TabModel, PageThumbnailModel, Selection, Tools,
  CommandStack (UI-level), StampLibrary, FontLibrary, Recovery, SafeSaver, Units
        │  engine interfaces (IDocument, IPage, ITextEditor, IImageOps, IStampOps, IPageOps)
Engine (src/engine/mupdf) — the only place MuPDF is used
        │
MuPDF (pinned tag, git submodule, built static) + FreeType/HarfBuzz bundled with MuPDF
Platform layer: FontLocator, FileAccess (SAF), Clipboard, Print, OpenWith intents
```

**Threading.** `fz_context` is not thread-safe. Each open document is owned by one **engine thread** that performs every mutation. Rendering works like this: the engine thread builds an `fz_display_list` per page, and a worker pool rasterizes those lists with cloned contexts (`fz_clone_context` plus lock callbacks). The UI never blocks on the engine.

**Undo/redo backbone.** Use MuPDF's document journal (`pdf_enable_journal`, `pdf_begin_operation` / `pdf_end_operation`, `pdf_undo` / `pdf_redo`). It records changes at the object level, so it covers page grafting, content-stream rewrites and annotations uniformly. An app-level `CommandStack` adds labels, selection restore, and **cross-document composite commands**. Moving pages between tabs touches two journals, and one undo must reverse both.

**Units.** Internally everything is in PDF points. `mm = pt × 25.4 / 72`. Every physical-size calculation also honours the page's `/UserUnit` and `/Rotate`.

### Repository layout (created in Stage 0)

```
CMakeLists.txt, CMakePresets.json        # linux, windows-msvc, android-arm64/x86_64 presets
third_party/mupdf/                       # submodule, pinned to current stable 1.2x tag
cmake/BundleMuPDF.cmake                  # builds MuPDF libs per platform
src/engine/            api/ (interfaces, value types)   mupdf/ (implementation)
src/core/              session, commands, stamps, fonts, recovery, saving, units
src/platform/          linux/ windows/ android/ (FontLocator, FileAccess, Clipboard, Print)
src/ui/qml/            components/ desktop/ mobile/
src/app/main.cpp
tools/yolopdf-cli/     # headless engine driver: inspect, replace-text, insert-pages, place-stamp…
tests/engine/          # contract + golden tests (Qt Test / CTest)
tests/corpus/          # small license-clean fixture PDFs + generator scripts
android/               # manifest, Java glue (SAF, intents, PrintManager, clipboard)
packaging/flatpak/  packaging/windows/
docs/
.github/workflows/     # CI matrix: Linux, Windows, Android
```

---

## Stage 0 — Scaffolding (small)

- CMake project for Qt 6 (current LTS, at least 6.8), C++20, with presets for the three targets. Add MuPDF as a submodule at a pinned tag and build it static: use MuPDF's Makefile on Linux, and a CMake wrapper or vcpkg port for MSVC and Android NDK, whichever builds first.
- Add `LICENSE` (AGPL-3.0), `CONTRIBUTING.md` (CLA/ownership note) and `README`.
- Set up GitHub Actions to build Linux, Windows and Android (arm64 APK) **from day one**, and to run CTest on Linux and Windows.
- Create the `yolopdf-cli` skeleton: `info <pdf>` prints pages, fonts and their embed/subset status, and image XObjects.

**Done when:** all three CI targets build, and `yolopdf-cli info` runs on Linux and Windows.

---

## Stage 1 — Prove the difficult operations (engine only, driven by CLI and tests)

### 1A. Font-preserving text replacement (the go/no-go spike)

Pipeline, implemented in `src/engine/mupdf/text/`:

1. **Trace text.** Write a custom content-stream processor (`pdf_processor` subclass, `TextShowTracer`). For each glyph shown by `Tj/TJ/'/"` it records: content stream and operator index, font resource name and font object, `Tf` size, `Tm`, CTM, `Tc/Tw/Tz/TL/Ts/Tr`, fill colour space and values, char code, and computed origin. It descends into Form XObjects and records the nesting path.
2. **Build runs.** Use `fz_stext_page` (accurate bboxes, preserve whitespace) for Unicode and line grouping. Join stext chars to traced glyphs by matching origins within a tolerance. Group the results into **EditableRuns**: one line of consecutive glyphs. Each run keeps a list of **style segments** (font, size, colour, Tc/Tw/Tz) so mixed formatting survives.
3. **Classify each run** and show the reason in the UI:
   - *Editable with original font*
   - *Needs font*, because glyphs are missing from the subset
   - *Not editable*: Type3 font, clip or invisible render mode (Tr 3–7), vertical writing mode, complex-shaping script (Arabic, Indic: later), no Unicode mapping, text inside an annotation appearance
   - *No text*: outlined text or a scanned page (the page has drawings or images but no stext chars)
4. **Coverage check for the new string**, per font type:
   - **Type0 / Identity-H:** reverse the `ToUnicode` CMap to get Unicode → CID, then use `CIDToGIDMap` to get the GID. Confirm the glyph actually exists in the embedded program with FreeType via `fz_font`. If the embedded font has a usable cmap, it can also supply glyphs that are not yet in `ToUnicode`. In that case add the entries to `ToUnicode` and `/W`.
   - **Simple TrueType / Type1:** map code ↔ glyph through `/Encoding` + `/Differences`, check the glyph exists, and take widths from `/Widths`. If a code is unused, patch in its width from the font's metrics. This only adds data, so existing text renders exactly as before.
   - If any character is uncovered, the run is **Needs font**.
5. **Font fallback chain. The font is never changed silently.**
   1. Use the original embedded font.
   2. Find the exact installed font by `BaseFont` without the `ABCDEF+` subset tag, then by family, weight and italic from `FontDescriptor`. Lookup goes through `FontLocator`: fontconfig on Linux, DirectWrite on Windows, `/system/fonts` on Android.
   3. Let the user import a font file into the app's FontLibrary.
   4. Offer a substitute explicitly, with a side-by-side preview and a "Substitute: X" label.

   A new font is embedded as Type0/Identity-H (`pdf_add_cid_font`) and subset on save (`pdf_subset_fonts`). Substitution applies to the whole run. Fonts are never mixed inside a word.
6. **Layout.** Compute the advance width from the font widths plus Tc/Tw/Tz. Compare it with the original run width and with the free space before the next run on the line.
   - On **overflow**, show a flag and three explicit choices: widen the box (warn if it would overlap neighbours), wrap onto the next line (only if that area is empty in v1), or use a smaller size the user chooses.
   - When the user edits across segments, an inserted character takes the style of the character before it.
7. **Write the change.** All of this happens inside one journal operation.
   - **Remove** exactly the run's glyphs with a content-stream filter (`pdf_filter_page_contents` with the sanitize filter's `text_filter` callback, matched by operator and glyph index or exact origin). Glyphs removed in the middle of a `TJ` are replaced by positioning adjustments, so neighbouring glyphs stay where they are. **No white rectangles.**
   - **Emit** `q BT /Fn size Tf Tc Tw Tz Ts Tr <colour> <Tm × CTM> Tm [ … ] TJ ET Q`. It reuses the original font resource and colour-space resources. Existing content is wrapped in balanced `q … Q` so the appended stream starts from a clean graphics state.
   - **Marked content:** wrap the new text in the same `/Span`/MCID marked-content context when the original had one.
   - **Shared Form XObjects:** if the run lives in a Form XObject used on several pages, **copy it for this page first**. Editing one page must not change the others.
   - Known v1 limit: appended text draws on top. If something used to overlap the text from above, the stacking order can change. The verifier detects this case.
8. **Verify before committing.** This is automatic and also used by the tests.
   - Re-extract the stext. The new string must appear at the expected baseline with the expected font.
   - Render the page before and after. **Outside the edited bbox, the two renders must match pixel for pixel.**
   - If either check fails: `pdf_undo`, then report "This text can't be edited safely" with the reason.
9. **Preview.** Apply inside a journal operation, render, then commit or undo. Previews are debounced at about 150 ms per keystroke.

**Go/no-go criteria for 1A**, on the corpus and on the user's real documents (invoices, brochures):
- Zero unintended pixel changes outside edited areas.
- Edited files pass `qpdf --check`.
- Edited files render and extract the new text correctly in **Poppler (`pdftoppm`/`pdftotext`) and PDFium**, not only MuPDF.
- Every unsupported run is classified as unsupported. None fails silently.
- Accents (de/fr) and CJK work when the glyphs exist, and are correctly reported as *Needs font* when they do not.

### 1B. Image occurrences

- Write an `ImageOccurrenceTracer` processor. It records every `Do` of an image XObject and every inline image (`BI`) with its CTM, resource name, Form XObject path and operator index. Each one becomes an **ImageOccurrence**, which is the unit of selection.
- **Per-occurrence editing:**
  - Move, resize and rotate by rewriting only that occurrence's `cm`: wrap it in `q … cm … Q` through the content filter.
  - Delete by dropping only that `Do`.
  - Replace by adding a new XObject under a new name and pointing this one `Do` at it.
  - **Shared image streams are never overwritten**, so other placements stay the same.
- **Copy at original resolution:** decode the `fz_image` and keep its SMask alpha. Export JPEG streams as-is, or as PNG.
- **Rectangular area copy:** render the clip region at a chosen DPI (default 300).
- **Insert image:** `pdf_add_image` plus an appended `q w 0 0 h x y cm /ImN Do Q`.

### 1C. Exact-size stamps

- **Create:** a Stamp annotation (`PDF_ANNOT_STAMP`) with a custom `/AP /N` Form XObject.
  - PNG keeps its alpha as an SMask.
  - SVG goes through MuPDF's SVG document handler into a **vector** Form XObject.
  - GIF uses the selected frame, decoded with `QImageReader`, as a PNG.
- **Size:** `/Rect` comes from millimetres, adjusted for `/UserUnit`. Rotation and the counter-rotation for page `/Rotate` go in the AP `/Matrix`. Opacity uses `/CA` and an ExtGState.
- A private `/YoloStamp << /Id /WidthMm /HeightMm /Rot /SizeLocked >>` dict allows re-editing.
- **Flatten on export:** `pdf_bake_document`.
- **Check:** a 40 × 20 mm stamp measures 113.39 × 56.69 pt on pages with mixed `/UserUnit` and `/Rotate`. When printed at actual size it measures 40 × 20 mm.

### 1D. Page insertion

- Insert with `pdf_graft_map` + `pdf_graft_mapped_page` at an index, inside one journal operation for multiple files.
- Also: delete, `pdf_rearrange_pages` (reorder/duplicate), `/Rotate`, blank page from `pdf_add_page` with a chosen MediaBox, and extract to a new document.
- **Merging form fields:** graft field objects into the target `/AcroForm /Fields`. Rename fields that collide.
- Original page sizes are always kept.
- Source files are opened read-only and never written.

### 1E. Early Android build

- A minimal QML app shows page 1 of a PDF opened through the system picker.
- It runs the 1A replacement on a fixture and saves through SAF.
- `yolopdf-cli` is built for arm64 and run over `adb` against the corpus.

**Stage 1 done when:** the golden tests for 1A–1D pass on Linux and Windows CI, the Android build runs on a device or emulator, and the 1A go/no-go criteria are met. If 1A fails badly, revisit the scope of text editing before Stage 2.

---

## Stage 2 — Working foundation

- **Shell:** QML desktop layout with tabs and a compact toolbar at the top, thumbnails on the left, the page in the centre, and a right properties panel that auto-hides. Toolbar modes: **Select, Edit text, Add text, Image, Stamp, Annotate.**
- **Viewer:** continuous scroll, zoom (fit width, fit page, actual size), tiled rendering with a cache, and a virtualised thumbnail `ListView`.
- **Open/save:**
  - **Safe save on desktop:** write the full new file to a temp file in the target directory, reopen it to verify it parses and has the same page count, then do an atomic replace (`QSaveFile` semantics). The original is untouched until that succeeds.
  - **Safe save on Android:** write to the cache directory, verify, then stream to the SAF URI opened in `"rwt"` mode. Keep the cached copy until the copy completes.
  - Save As works on all platforms.
  - Encrypted files keep their encryption (`PDF_ENCRYPT_KEEP`).
  - **Signed documents** are always saved incrementally.
- **Password and signatures:**
  - Prompt for a password using `pdf_needs_password` / `pdf_authenticate_password`.
  - Show permission restrictions using `pdf_has_permission`.
  - Detect signed documents (`/SigFlags`, signature fields with `/V`). Before the first edit, show a dialog explaining that the signature will show as modified.
- **Undo/redo:** use the journal and the CommandStack, with Ctrl+Z / Ctrl+Shift+Z and toolbar buttons.
- **Crash recovery:**
  - After each operation (throttled to at most every 10 s), write a recovery snapshot (`pdf_save_snapshot` / journal) to the app's recovery directory, along with the original's fingerprint (path or URI, size, mtime, hash).
  - On launch, offer to restore.
  - If the original changed since the snapshot, restore as an untitled copy.
  - Delete the snapshot on a clean save or close.
- **Search** (`fz_search_stext_page`), text selection, and copy (`fz_copy_selection`).

**Done when:** files reopen identically in yolopdf, Poppler and PDFium. A forced kill during a save leaves the original intact (tested by killing the save mid-write in CI). Recovery restores the unsaved edits.

---

## Stage 3 — Pages and drag-and-drop

- Multi-select pages with Ctrl/Shift-click or touch long-press. Actions: delete, duplicate, rotate, reorder, extract to a new PDF, and **move/copy to another open tab** (a composite cross-document command).
- **Insert pages** dialog: choose a file, pick a page range (for example "1-3,7"), and set the position.
- **Blank page** with a size picker (A4, Letter, A5, custom mm) that defaults to the current page's size.
- **Drag-and-drop rules**, encoded in a `DropRouter` covered by unit tests:

| Drop target | Behaviour | Overlay |
|---|---|---|
| Overview, between thumbnails | Insert at that index; show the insertion marker line | "Insert pages here" |
| Overview, below the last thumbnail | Append | "Insert pages here" |
| Overview, several PDFs | One undoable operation in drop order; the overlay lists the files in order | "Insert pages here" |
| Main view | Open in a new tab, **never merge** | "Open as document" |
| No document open (anywhere) | Open | "Open as document" |
| Dragging existing thumbnails | Reorder the selected pages | marker only |

- On Android, use explicit **Open document** and **Insert pages** buttons that go through the SAF picker, plus "Open with" / share intent filters for `application/pdf`.

**Done when:** each table row is covered by an automated QML or Qt Quick Test, and the undo of a multi-file drop restores the exact previous page list.

---

## Stage 4 — Images and stamps (UI)

- **Image tool:** two explicit selection modes, **Select image object** and **Select area**.
  - Actions: copy (an internal clipboard that carries the engine object, so pasting into another tab grafts the original image, plus a PNG on the system clipboard), export to file, insert from file, move, proportional resize (Shift releases the aspect lock), rotate and delete.
  - The properties panel shows size in mm.
- **Stamp collection** in `StampLibrary`:
  - Storage: `<AppData>/stamps/<uuid>/{stamp.json, artwork.(png|svg|pdf)}`. `stamp.json` holds name, `widthMm`, `heightMm`, `aspectLocked`, `sizeLocked`, source type and GIF frame.
  - Creation wizard: **choose file → set size in mm (aspect locked) → name → save.** SVG shows a conversion preview, and GIF shows a frame picker.
  - Placement: a ghost preview follows the cursor or finger, and a click or tap places the stamp. The tool **stays active until Esc or Done**.
  - Adjust: move, rotate, duplicate and opacity. Size is locked by default.
  - **Apply to selected pages:** same offset from a chosen corner of each page's visible (CropBox, rotated) box.
  - **Export and import** the collection as a `.yolostamps` zip.
  - Signature images are ordinary stamps.
- **Measurement and alignment:**
  - Numeric X/Y/W/H in mm.
  - Snapping guides to page edges and centre and to other objects.
  - Arrow keys nudge 0.5 mm, and Shift+arrow nudges 5 mm.
- **Annotate mode:** highlight (quads from the text selection), sticky-note comments, arrows (Line with `/LE`), and rectangles. Appearances are generated with `pdf_update_annot`.
- **Export option:** "Flatten stamps".

**Done when:** transparency is preserved, a stamp measures correctly in an independent viewer and in a printed proof, and editing or deleting one occurrence of a shared image leaves the others unchanged.

---

## Stage 5 — Text-editing release

- **Edit text mode:**
  - Hovering highlights a run, colour-coded by its classification.
  - A click opens an inline editor over the run with the original font rendered.
  - The live preview comes from 1A.9.
  - Apply or Cancel.
- **Properties panel:** font name and status (Original / Installed match / Imported / **Substitute**), size, colour and overflow indicator, with the overflow choices from 1A.6.
- **Missing-glyph flow:** list the missing characters, then offer, in order: installed exact match, import font, or choose a substitute.
- **Add text:** click to create a point text box, or drag to create an area box. Pick font (system or imported), size, colour and alignment. New text is embedded as Type0 and subset on save.
- **Unsupported runs and pages** show a clear tooltip or banner, for example "Scanned page: text can't be edited directly (OCR planned)".

**Done when:** on the real-document corpus, the supported cases preserve appearance (the verifier passes) and every unsupported case is identified with a reason.

---

## Stage 6 — Platform refinement and release

- **Android:**
  - Collapsible page overview and a bottom-sheet properties panel.
  - Touch targets of at least 48 dp, pinch zoom, and long-press multi-select.
  - Clipboard through JNI `ClipData` with a `FileProvider` URI.
  - Printing through `PrintManager`, which receives the saved PDF.
- **Desktop print:** print preview with **Actual size** (default for documents with stamps), fit-to-page, and paper-size selection. Pages are rasterized at printer DPI through `QPrinter`.
- **Accessibility:** full keyboard operation, `Accessible.name` and `role` on every control, a visible focus ring, and high-contrast and dark themes.
- **Packaging:**
  - Flatpak manifest, which builds MuPDF as a module (Flathub-ready).
  - Windows installer (WiX/MSI or NSIS) with a `.pdf` "Open with" association.
  - Android: signed APK and AAB.

**Done when:** the same scripted operation list (open, insert pages, edit text, place stamp, save) produces equivalent output on all three platforms. Compare by page structure and by render diff.

---

## Later releases (out of v1 scope)

True redaction (`pdf_redact_page` with a dedicated save that removes history), OCR (searchable text layer), form filling, compression and downsampling with preview, page numbers and date stamps, paragraph and multi-column reflow, complex-script shaping (HarfBuzz), and certificate-based digital signing.

## Key risks and mitigations

| Risk | Mitigation |
|---|---|
| Subset fonts lack the needed glyphs | Coverage check, explicit fallback chain, never silent |
| Text lives in shared Form XObjects | Copy-on-write the form per page before editing |
| Edit changes untouched content | Pixel-diff verifier with automatic undo on failure |
| PDF/A or Tagged PDF conformance broken by edits | Detect PDF/A in XMP and warn; preserve marked content around new text |
| MuPDF API changes between versions | Pin the submodule tag; upgrade deliberately with the contract tests |
| Wanting a different license or engine later | Strict engine boundary, contract tests, CLA/ownership policy |
| Android file access (SAF) edge cases | Cache-then-copy save, URI permission persistence, tested in Stage 1E |

## Verification strategy (all stages)

- **Corpus:** `tests/corpus/` contains small, license-clean, generated fixtures. A fixture generator script uses DejaVu/Noto fonts to produce:
  - subset TrueType and CID fonts
  - de/fr accents and CJK
  - rotated pages and rotated text
  - Type3 fonts
  - outlined text
  - a scanned image-only page
  - an encrypted file and a signed file
  - mixed page sizes and `/UserUnit`
  - one image XObject used three times
  - text inside a shared Form XObject

  A separate **private real-document corpus** (brochures, invoices, scans) is run locally by `tests/run-real-corpus.sh` and never committed.
- **Golden tests (CTest):** each engine operation is run through `yolopdf-cli` or directly, then checked:
  1. `qpdf --check` passes
  2. Poppler `pdftotext` contains the expected text
  3. Poppler and PDFium renders match the golden PNGs within tolerance
  4. Pixel-exact match outside edited regions
  5. Byte-identical source files after the operation
- **UI tests:** Qt Quick Test for the DropRouter, the tool modes and undo/redo sequences.
- **CI:** Linux and Windows run the full engine and UI tests. Android builds and runs `yolopdf-cli` corpus tests on an emulator.
- **Manual release checklist:** open each exported file in an independent viewer (Firefox/pdf.js, Evince/Okular, Adobe Reader), print a 40 × 20 mm stamp and measure it, and kill the app mid-edit to check recovery.

## Immediate next step after approval

Create `docs/IMPLEMENTATION_PLAN.md` containing this plan. Commit it with message "Add implementation plan" and the required attribution trailers, then run `git push -u origin claude/loving-sagan-85bdyt`. Do not create a PR unless asked.
