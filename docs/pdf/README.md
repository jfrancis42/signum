<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Jeff Francis, N0GQ -->
# Signum's PDF documents

One toolchain and one look for every PDF Signum builds from its Markdown documents (for example the user guide): a title page, a table
of contents, numbered sections, headers and footers with the version and "page N of M", boxed code
(Python highlighted), tinted tables with column widths from their content.

| File | What |
|---|---|
| `build-pdf.sh` | `docs/pdf/build-pdf.sh -o OUT.pdf [--title T] [--subtitle S] [--version V] [--date D] IN.md [IN2.md ...]`: the title and subtitle from the first document's YAML front matter, the version from `VERSION` |
| `style.tex` | the look (included after pandoc's LaTeX preamble): fonts, colours, headings, title page, headers and footers, code boxes, tables |
| `filter.lua` | pandoc Lua filter: code blocks into the style's boxes, table column widths |

pandoc's own LaTeX template is used (no third-party template), so pandoc 3.1 and later work alike;
LuaLaTeX sets the text. Fonts: TeX Gyre Pagella (text), TeX Gyre Heros (headings), DejaVu Sans Mono
(code), found by file name.

`make docs` builds every document into `docs/`, where they are checked in: the user guide (every page in `docs/md/`, in the
order of the index in `docs/md/README.md`, so a page added there is in the guide), the WebSocket API
(`src/web/README.md`), and the man page as PDF and text (groff).

A new document: write it in Markdown with a front matter (`title:`, `subtitle:`), add a make target that
calls `build-pdf.sh`. `<!-- ... -->` comments do not appear in the PDF.

## Packages

| Distribution | Packages |
|---|---|
| Debian 13, Ubuntu, Raspberry Pi OS | `pandoc texlive-luatex texlive-latex-recommended texlive-latex-extra texlive-fonts-recommended texlive-pictures fonts-texgyre fonts-dejavu-core lmodern` (tested: Debian 13, pandoc 3.1.11) |
| Arch | `pandoc-cli texlive-luatex texlive-latexextra texlive-fontsrecommended texlive-pictures ttf-dejavu` (tested: pandoc 3.10) |
| Fedora | `pandoc texlive-luatex texlive-collection-latexrecommended texlive-collection-latexextra texlive-tex-gyre texlive-pgf dejavu-sans-mono-fonts` (not tested) |

On Debian the TeX Gyre OpenType files are in `fonts-texgyre` (not in `tex-gyre`).
