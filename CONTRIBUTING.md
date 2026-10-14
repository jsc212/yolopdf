# Contributions and ownership

yolopdf is currently licensed under AGPL-3.0-only and uses open-source MuPDF.
Distributed builds must comply with the AGPL and all bundled dependency
licenses, including providing the corresponding source and build instructions.

To preserve the option of purchasing a commercial MuPDF license and relicensing
future yolopdf versions, **outside code contributions are not accepted yet**.
Issues, bug reports, and discussion are welcome. Please do not submit patches
until the maintainer has established an appropriate contributor agreement.
No CLA is currently offered or implied. Earlier AGPL releases retain their
license; a future license change would apply only where rights permit it.

Development rules:

- Only files under `src/engine/mupdf/` may include MuPDF headers. Public engine
  interfaces use yolopdf value types.
- Keep the text safety verifier enabled. Further UI work does not establish the
  pending Windows, Android, private-document or printed-proof release gates.
- Run the existing CTest suite before proposing changes.
- Keep personal documents, credentials, signing keys, and generated build
  artifacts out of Git.

See [the implementation plan](docs/IMPLEMENTATION_PLAN.md) for scope and gates.
