# Stage 1 sample

`stage1-sample.pdf` is the generated `subset-latin.pdf` fixture, produced by
`tests/corpus/generate.py`. Its YoloTest font program and page content are
original to yolopdf and licensed under AGPL-3.0-only. The app's "Open sample"
action replaces its first run with `éè`, renders it, and exposes the verified
output through Save As. It exercises the same engine on desktop and Android
without shipping a private document. The main UI also supports interactive
text editing. This fixture uses geometric test glyphs; their appearance is
intentionally artificial.
