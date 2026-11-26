#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis, N0GQ
#
# build-pdf.sh: one Markdown document (or several, in order) to one PDF in Signum's document style:
# a title page, a table of contents, numbered sections, headers and footers with the version and the
# page of pages, boxed code, tinted tables (docs/pdf/README.md).
#
#   docs/pdf/build-pdf.sh -o OUT.pdf [--title T] [--subtitle S] [--version V] [--date D] IN.md [IN2.md ...]
#
# The title and subtitle default to the first document's YAML front matter; the version to the
# repository's VERSION file; the date to today. Needs pandoc and LuaLaTeX (README.md: packages).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out= title= subtitle= version= date=
inputs=()
while [ $# -gt 0 ]; do
	case $1 in
	-o) out=$2; shift 2 ;;
	--title) title=$2; shift 2 ;;
	--subtitle) subtitle=$2; shift 2 ;;
	--version) version=$2; shift 2 ;;
	--date) date=$2; shift 2 ;;
	-h|--help) sed -n '4,12p' "$0"; exit 0 ;;
	*) inputs+=("$1"); shift ;;
	esac
done
[ -n "$out" ] && [ ${#inputs[@]} -gt 0 ] || { echo "usage: $0 -o OUT.pdf [--title T] [--subtitle S] [--version V] IN.md ..." >&2; exit 2; }
for t in pandoc lualatex; do command -v $t >/dev/null || { echo "build-pdf: $t not found (docs/pdf/README.md lists the packages)" >&2; exit 3; }; done
front() { sed -n '1,/^---$/{/^'"$1"':/{s/^'"$1"': *//;s/^"//;s/"$//;p}}' "${inputs[0]}" | head -1; }
[ -n "$title" ] || title=$(front title)
[ -n "$subtitle" ] || subtitle=$(front subtitle)
[ -n "$version" ] || version=$(cat "$root/VERSION" 2>/dev/null || echo dev)
[ -n "$date" ] || date=$(date +"%-d %B %Y")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tex_esc() { printf '%s' "$1" | sed 's/[\\&%$#_{}]/\\&/g'; }
{
	echo "\\newcommand{\\doctitle}{$(tex_esc "$title")}"
	echo "\\newcommand{\\docsubtitle}{$(tex_esc "$subtitle")}"
	echo "\\newcommand{\\docversion}{$(tex_esc "$version")}"
	echo "\\newcommand{\\docdate}{$(tex_esc "$date")}"
	cat "$here/style.tex"
} > "$tmp/header.tex"
mkdir -p "$(dirname "$out")"
pandoc "${inputs[@]}" --from markdown-yaml_metadata_block+yaml_metadata_block+gfm_auto_identifiers --to latex --standalone \
	--pdf-engine=lualatex --lua-filter "$here/filter.lua" -H "$tmp/header.tex" --toc --toc-depth=2 --number-sections \
	-V documentclass=article -V fontsize=10pt -V colorlinks=true -M title="$title" \
	--resource-path="$(dirname "${inputs[0]}")" -o "$out"
echo "build-pdf: $out ($(du -k "$out" | cut -f1) KB)"
