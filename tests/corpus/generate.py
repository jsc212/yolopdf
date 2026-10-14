#!/usr/bin/env python3
"""Generate license-clean PDF fixtures using tests/requirements.txt.

All page content is original. Noto Sans (SIL OFL) and Droid Sans Fallback
(Apache-2.0) come from MuPDF's pinned tree; see the adjacent corpus README.
"""
from pathlib import Path
from io import BytesIO
import struct
import zlib
import sys


def stream(body, attributes=b""):
    return b"<< /Length %d " % len(body) + attributes + b">>\nstream\n" + body + b"\nendstream"


def write_pdf(path, objects):
    output = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, obj in enumerate(objects, 1):
        offsets.append(len(output))
        output.extend(f"{number} 0 obj\n".encode() + obj + b"\nendobj\n")
    start = len(output)
    output.extend(f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode())
    for offset in offsets[1:]:
        output.extend(f"{offset:010d} 00000 n \n".encode())
    output.extend(f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n".encode())
    path.write_bytes(output)


def png(path, width, height, rgba):
    def chunk(name, body):
        return struct.pack(">I", len(body)) + name + body + struct.pack(">I", zlib.crc32(name + body))
    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def build_test_font(destination):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    codes = sorted(set(map(ord, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,!?éèäöüßçàâΩ中日文")))
    order = [".notdef"] + [f"u{c:04X}" for c in codes]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({c: f"u{c:04X}" for c in codes})
    glyphs = {}
    for i, name in enumerate(order):
        pen = TTGlyphPen(None)
        if name != "u0020":
            width = 350 + i % 5 * 35
            pen.moveTo((50, 0)); pen.lineTo((width, 0)); pen.lineTo((width, 650))
            pen.lineTo((50, 500 + i % 4 * 40)); pen.closePath()
        glyphs[name] = pen.glyph()
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics({name: (600, 50) for name in order})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({"familyName": "YoloTest", "styleName": "Regular", "uniqueFontIdentifier": "YoloTest-Regular", "fullName": "YoloTest Regular", "psName": "YoloTest-Regular"})
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupPost(); builder.setupMaxp()
    builder.save(destination / "YoloTest-Regular.ttf")


def font_pdf(path, font_path, text, supported, composite=False, extra=b""):
    from fontTools.ttLib import TTFont
    from fontTools.subset import Options, Subsetter
    font = TTFont(font_path)
    options = Options(); options.layout_features = []
    subset = Subsetter(options); subset.populate(unicodes=map(ord, supported)); subset.subset(font)
    data = BytesIO(); font.save(data); data = data.getvalue()
    cmap = font.getBestCmap(); upm = font["head"].unitsPerEm
    base_name = "ABCDEF+" + (font["name"].getDebugName(6) or "DroidSansFallback")
    cff = "CFF " in font
    if composite:
        codes = {c: i + 1 for i, c in enumerate(sorted(cmap))}
        encoded = b"".join(codes[ord(c)].to_bytes(2, "big") for c in text)
        mappings = [(code, cp) for cp, code in codes.items()]
        widths = " ".join(f"{code} [{round(font['hmtx'][cmap[cp]][0] * 1000 / upm)}]" for cp, code in codes.items())
        cid_map = b"\0\0" + b"".join(font.getGlyphID(cmap[cp]).to_bytes(2, "big") for cp in sorted(cmap))
        pdf_font = f"<< /Type /Font /Subtype /Type0 /BaseFont /{base_name} /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 9 0 R >>".encode()
        descendant = f"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /{base_name} /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor 7 0 R /DW 1000 /W [{widths}] /CIDToGIDMap 10 0 R >>".encode()
        descriptor_number, file_number = 7, 8
    else:
        encoded = text.encode("cp1252")
        mappings = [(ord(chr(cp).encode("cp1252")), cp) for cp in cmap if chr(cp).encode("cp1252", "ignore")]
        widths = []
        for code in range(32, 256):
            try: glyph = cmap.get(ord(bytes([code]).decode("cp1252")))
            except UnicodeDecodeError: glyph = None
            widths.append(str(round(font["hmtx"][glyph][0] * 1000 / upm)) if glyph else "0")
        subtype = "Type1" if cff else "TrueType"
        pdf_font = f"<< /Type /Font /Subtype /{subtype} /BaseFont /{base_name} /Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 255 /Widths [{' '.join(widths)}] /FontDescriptor 6 0 R /ToUnicode 8 0 R >>".encode()
        descriptor_number, file_number = 6, 7
    to_unicode = b"/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n/CMapName /YoloUnicode def\n/CMapType 2 def\n1 begincodespacerange\n" + (b"<0000> <FFFF>" if composite else b"<00> <FF>") + b"\nendcodespacerange\n"
    to_unicode += f"{len(mappings)} beginbfchar\n".encode()
    for code, cp in mappings:
        to_unicode += f"<{code:0{4 if composite else 2}X}> <{chr(cp).encode('utf-16-be').hex().upper()}>\n".encode()
    to_unicode += b"endbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n"
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 400 200] >>",
        b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>", pdf_font,
        stream(b"BT /F1 18 Tf 20 130 Td <" + encoded.hex().encode() + b"> Tj 0 -40 Td <" + encoded.hex().encode() + b"> Tj ET\n" + extra)]
    if composite: objects.append(descendant)
    file_key = "FontFile3" if cff else "FontFile2"
    objects.extend([f"<< /Type /FontDescriptor /FontName /{base_name} /Flags 32 /FontBBox [-1000 -1000 2000 2000] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /{file_key} {file_number} 0 R >>".encode(), stream(data, b"/Subtype /OpenType " if cff else b""), stream(to_unicode)])
    if composite: objects.append(stream(cid_map))
    write_pdf(path, objects)


def stage1(destination, basic, nested):
    build_test_font(destination)
    font_pdf(destination / "subset-latin.pdf", destination / "YoloTest-Regular.ttf", "Hello éééé", "Helo éèäüàç")
    # The embedded program has Omega, but WinAnsiEncoding cannot address it.
    font_pdf(destination / "narrow-encoding.pdf", destination / "YoloTest-Regular.ttf", "Hello éééé", "Helo éèΩ")
    from fontTools.ttLib import TTCollection, TTFont
    collection = TTCollection()
    collection.fonts = [TTFont(destination / "YoloTest-Regular.ttf") for _ in range(2)]
    for record in collection.fonts[1]["name"].names:
        if record.nameID in (1, 3, 4, 6):
            record.string = "YoloAlternate".encode(record.getEncoding())
    collection.save(destination / "two-faces.ttc")
    droid = Path(__file__).resolve().parents[2] / "third_party/mupdf/resources/fonts/droid/DroidSansFallback.ttf"
    font_pdf(destination / "subset-cjk.pdf", droid, "中文中文", "中文日", composite=True)
    noto = Path(__file__).resolve().parents[2] / "third_party/mupdf/resources/fonts/noto/NotoSans-Regular.otf"
    font_pdf(destination / "subset-cff.pdf", noto, "Hello éééé", "Helo éèäüàç")
    png(destination / "red-alpha.png", 2, 2, bytes([255, 0, 0, 128]) * 4)
    png(destination / "blue.png", 2, 1, bytes([0, 0, 255, 255]) * 2)
    (destination / "vector.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20"><rect width="40" height="20" fill="#ff0000"/><circle cx="20" cy="10" r="5" fill="#0000ff"/></svg>')
    from PIL import Image
    frames = [Image.new("RGB", (2, 2), color) for color in ("red", "blue")]
    frames[0].save(destination / "frames.gif", save_all=True, append_images=frames[1:], duration=100, loop=0)
    jpeg = BytesIO(); Image.new("RGB", (3, 2), "red").save(jpeg, format="JPEG"); jpeg = jpeg.getvalue()
    (destination / "original.jpg").write_bytes(jpeg)
    jpeg_pdf = list(basic)
    jpeg_pdf[5] = stream(jpeg, b"/Type /XObject /Subtype /Image /Width 3 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode ")
    write_pdf(destination / "jpeg.pdf", jpeg_pdf)
    tj = list(basic); tj[4] = stream(b"BT /F1 18 Tf 24 TL 30 140 Td [(Hello) 100 ( yolopdf)] TJ ( NEXT) Tj (Quote one) ' 2 1 (Quote two) \" ET")
    write_pdf(destination / "positioned-text.pdf", tj)
    mixed = list(basic)
    mixed[2] = b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R /F2 7 0 R >> >> /Contents 5 0 R >>"
    mixed[4] = stream(b"BT /F1 18 Tf 30 140 Td 1 0 0 rg (Hello ) Tj /F2 18 Tf 0 0 1 rg (World) Tj /F1 18 Tf 0 -40 Td 0 g (Unchanged line) Tj ET")
    mixed.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier-Bold >>")
    write_pdf(destination / "mixed-style.pdf", mixed)
    marked = list(basic); marked[4] = stream(b"/Span << /MCID 0 >> BDC BT /F1 18 Tf 30 140 Td (Hello yolopdf) Tj ET EMC")
    write_pdf(destination / "marked.pdf", marked)
    gs_font = list(basic)
    gs_font[2] = b"<< /Type /Page /Parent 2 0 R /Resources << /ExtGState << /GS1 << /Font [4 0 R 18] >> >> >> /Contents 5 0 R >>"
    gs_font[4] = stream(b"/GS1 gs BT 30 140 Td (Hello yolopdf) Tj ( NEXT) Tj ET")
    write_pdf(destination / "graphics-state-font.pdf", gs_font)
    rotated = list(basic)
    rotated[4] = stream(b"q 0 1 -1 0 180 20 cm BT /F1 18 Tf 0 0 Td (Hello yolopdf) Tj ET Q")
    write_pdf(destination / "rotated-text.pdf", rotated)
    actual = list(basic); actual[4] = stream(b"/Span << /ActualText (Alternative text) >> BDC BT /F1 18 Tf 30 140 Td (Hello) Tj ET EMC")
    write_pdf(destination / "actual-text.pdf", actual)
    modes = list(basic); modes[4] = stream(b"BT /F1 18 Tf 3 Tr 30 140 Td (Hidden) Tj 4 Tr 0 -30 Td (Clip) Tj ET")
    write_pdf(destination / "unsupported-modes.pdf", modes)
    image_only = list(basic); image_only[4] = stream(b"q 30 0 0 30 20 20 cm /Im1 Do Q")
    write_pdf(destination / "scanned.pdf", image_only)
    repeat = list(basic); repeat[4] = stream(b"q 20 0 0 20 20 20 cm /Im1 Do Q q 20 0 0 20 100 20 cm /Im1 Do Q q 20 0 0 20 180 20 cm /Im1 Do Q")
    write_pdf(destination / "shared-image.pdf", repeat)
    inline = list(basic); inline[4] = stream(b"q 20 0 0 20 20 20 cm BI /W 1 /H 1 /CS /RGB /BPC 8 ID \xff\x00\x00 EI Q")
    write_pdf(destination / "inline-image.pdf", inline)
    nested_image = list(nested); nested_image[3] = stream(b"q 30 0 0 30 20 10 cm /Im1 Do Q", b"/Type /XObject /Subtype /Form /BBox [0 0 100 50] /Resources << /XObject << /Im1 8 0 R >> >> ")
    nested_image.append(basic[5]); write_pdf(destination / "nested-image.pdf", nested_image)
    type3 = list(basic); type3[3] = b"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 600 700] /FontMatrix [.001 0 0 .001 0 0] /CharProcs << /A 7 0 R >> /Encoding << /Differences [65 /A] >> /FirstChar 65 /LastChar 65 /Widths [600] /Resources << >> >>"
    type3[4] = stream(b"BT /F1 18 Tf 30 140 Td (A) Tj ET"); type3.append(stream(b"600 0 0 0 600 700 d1 0 0 600 700 re f"))
    write_pdf(destination / "type3.pdf", type3)
    form = list(basic)
    form[0] = b"<< /Type /Catalog /Pages 2 0 R /AcroForm 9 0 R >>"
    form[2] = b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> /XObject << /Im1 6 0 R >> >> /Contents 5 0 R /Annots [7 0 R] >>"
    form.extend([
        b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /V (Alice) /Rect [50 50 200 75] /P 3 0 R /F 4 /DA (/Helv 12 Tf 0 g) /AP << /N 8 0 R >> >>",
        stream(b"BT /F1 12 Tf 5 5 Td (Alice) Tj ET", b"/Type /XObject /Subtype /Form /BBox [0 0 150 25] /Resources << /Font << /F1 4 0 R >> >> "),
        b"<< /Fields [7 0 R] /DR << /Font << /Helv 4 0 R >> >> /DA (/Helv 12 Tf 0 g) >>",
    ])
    write_pdf(destination / "form.pdf", form)
    multiform = list(nested)
    multiform[0] = b"<< /Type /Catalog /Pages 2 0 R /AcroForm 11 0 R >>"
    multiform[2] = b"<< /Type /Page /Parent 2 0 R /UserUnit 2 /Contents 6 0 R /Annots [9 0 R] >>"
    multiform[6] = b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 150] /CropBox [10 20 90 130] /Rotate -90 /Contents 6 0 R /Annots [10 0 R] >>"
    multiform.extend([
        b"<< /FT /Tx /T (Name) /V (Alice) /Kids [9 0 R 10 0 R] /DA (/Helv 12 Tf 0 g) >>",
        b"<< /Type /Annot /Subtype /Widget /Parent 8 0 R /P 3 0 R /Rect [20 20 100 40] /F 4 /AP << /N 12 0 R >> >>",
        b"<< /Type /Annot /Subtype /Widget /Parent 8 0 R /P 7 0 R /Rect [20 20 80 40] /F 4 /AP << /N 12 0 R >> >>",
        b"<< /Fields [8 0 R] /DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 12 Tf 0 g) >>",
        stream(b"BT /F1 12 Tf 5 5 Td (Alice) Tj ET", b"/Type /XObject /Subtype /Form /BBox [0 0 80 20] /Resources << /Font << /F1 5 0 R >> >> "),
    ])
    write_pdf(destination / "multi-widget-form.pdf", multiform)
    linked = list(nested)
    linked[2] = b"<< /Type /Page /Parent 2 0 R /UserUnit 2 /Contents 6 0 R /Annots [8 0 R] >>"
    linked.append(b"<< /Type /Annot /Subtype /Link /Rect [10 10 80 40] /P 3 0 R /Dest [7 0 R /Fit] >>")
    write_pdf(destination / "linked-pages.pdf", linked)
    vertical = (destination / "subset-cjk.pdf").read_bytes().replace(b"/Identity-H", b"/Identity-V")
    # Identity-H and Identity-V have equal byte lengths, preserving xref offsets.
    (destination / "vertical.pdf").write_bytes(vertical)
    complex_text = list(basic)
    complex_text[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 7 0 R >>"
    complex_text[4] = stream(b"BT /F1 18 Tf 30 140 Td (A) Tj ET")
    complex_text.append(stream(b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def /CMapName /Arabic def /CMapType 2 def 1 begincodespacerange <00> <FF> endcodespacerange 1 beginbfchar <41> <0627> endbfchar endcmap CMapName currentdict /CMap defineresource pop end end"))
    write_pdf(destination / "complex-script.pdf", complex_text)
    unknown = list(basic); unknown[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /YoloFontNotInstalled >>"
    write_pdf(destination / "nonembedded-font.pdf", unknown)
    conformance = list(basic)
    conformance[0] = b"<< /Type /Catalog /Pages 2 0 R /Metadata 7 0 R /StructTreeRoot 8 0 R >>"
    conformance.extend([stream(b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description xmlns:pdfaid="http://www.aiim.org/pdfa/ns/id/" pdfaid:part="2" pdfaid:conformance="B"/></rdf:RDF></x:xmpmeta>', b"/Type /Metadata /Subtype /XML "), b"<< /Type /StructTreeRoot /K [] >>"])
    write_pdf(destination / "conformance-declarations.pdf", conformance)
    signature = list(basic)
    signature[0] = b"<< /Type /Catalog /Pages 2 0 R /AcroForm << /SigFlags 3 /Fields [7 0 R] >> >>"
    signature.append(b"<< /FT /Sig /T (Signature) /V << /Type /Sig /ByteRange [0 0 0 0] /Contents <> >> >>")
    # A structural signature marker, not a cryptographically signed document.
    write_pdf(destination / "signature-marker.pdf", signature)
    import subprocess
    subprocess.run(["qpdf", "--encrypt", "test-user", "test-owner", "256", "--",
                    str(destination / "basic.pdf"), str(destination / "encrypted.pdf")], check=True)


def generate(destination):
    destination.mkdir(parents=True, exist_ok=True)
    basic = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 300 200] >>",
        b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> /XObject << /Im1 6 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream(b"0.2 0.4 0.6 rg 20 20 40 30 re f\nBT /F1 18 Tf 30 140 Td (Hello yolopdf) Tj ET\nq 20 0 0 20 100 50 cm /Im1 Do Q"),
        stream(bytes([255, 0, 0]), b"/Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8 "),
    ]
    write_pdf(destination / "basic.pdf", basic)
    nested = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 7 0 R] /Count 2 /MediaBox [0 0 300 200] /Rotate 90 /Resources << /XObject << /Shared 4 0 R >> >> >>",
        b"<< /Type /Page /Parent 2 0 R /UserUnit 2 /Contents 6 0 R >>",
        stream(b"BT /F1 12 Tf 10 20 Td (Shared text) Tj ET", b"/Type /XObject /Subtype /Form /BBox [0 0 100 50] /Resources << /Font << /F1 5 0 R >> >> "),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>",
        stream(b"q /Shared Do Q"),
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 150] /CropBox [10 20 90 130] /Rotate -90 /Contents 6 0 R >>",
    ]
    write_pdf(destination / "nested.pdf", nested)
    # The resource graph contains a cycle, while executed content does not.
    cyclic = list(nested)
    cyclic[3] = stream(b"", b"/Type /XObject /Subtype /Form /BBox [0 0 100 50] /Resources << /XObject << /Self 4 0 R >> >> ")
    write_pdf(destination / "cyclic-resources.pdf", cyclic)
    font_path = Path(__file__).resolve().parents[2] / "third_party/mupdf/resources/fonts/noto/NotoSans-Regular.otf"
    font = font_path.read_bytes()
    embedded = list(basic)
    embedded[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+NotoSans /Encoding /WinAnsiEncoding /FontDescriptor 7 0 R >>"
    embedded.extend([
        b"<< /Type /FontDescriptor /FontName /ABCDEF+NotoSans /Flags 32 /FontBBox [-1000 -1000 2000 2000] /ItalicAngle 0 /Ascent 1069 /Descent -293 /CapHeight 714 /StemV 80 /FontFile3 8 0 R >>",
        stream(font, b"/Subtype /OpenType "),
    ])
    write_pdf(destination / "embedded.pdf", embedded)
    (destination / "invalid.pdf").write_bytes(b"This is not a PDF.")
    stage1(destination, basic, nested)


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
