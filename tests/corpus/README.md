# Generated corpus

`generate.py` writes fixtures into the build directory using the pinned test
Python packages and qpdf. Page content, object structures and geometric YoloTest
fonts are original and licensed with yolopdf. Embedded Noto Sans and Droid Sans
Fallback come from the pinned MuPDF tree. Noto uses SIL OFL
(`resources/fonts/noto/COPYING`); Droid uses Apache-2.0
(`resources/fonts/droid/NOTICE`). Preserve both notices.

Fixtures cover subset TrueType/CFF/CID fonts, accents/CJK, coverage/imports,
limited encodings, collections, rotation/positioning, unsupported scripts/modes,
shared Forms/images, inline images, alpha/vector/GIF artwork, AcroForm collisions,
links, mixed geometry/UserUnit and malformed input. Marked-content fixtures cover
sequences crossing graphics save/restore and content streams, `/ActualText`
scope restoration and restrictions inherited by nested Forms. Clipping fixtures
distinguish fully contained rectangular text, clipped glyphs and complex paths,
and verify rollback when an edit crosses the clip boundary. FreeText fixtures
with missing appearance streams exercise recovery and paired page-move history.
High-precision content checks preserve untouched operator bytes and independent
rendering after a text edit.

`encrypted.pdf` uses fixture passwords test-user/test-owner.
`signature-marker.pdf` tests field detection, not cryptographic signatures.
`conformance-declarations.pdf` tests PDF/A and tagged markers, without claiming
actual conformance. A real signed-file proof and curated golden images remain
open gates. Private documents stay outside the repository. A supplied receipt
was used locally to reproduce a marked-content bug; these fixtures contain
original generated content rather than that receipt or its text.
