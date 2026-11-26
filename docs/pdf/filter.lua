-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 Jeff Francis, N0GQ
-- filter.lua: pandoc Lua filter for Signum's PDF documents (docs/pdf/README.md).
--  - code blocks become the style's boxed listings (Python highlighted, the rest plain);
--  - table columns get widths from their content, so short columns stay narrow;
--  - links between the documents of one PDF: page.md#anchor jumps to the anchor, a link to a whole page
--    or to a file outside the PDF stays as plain text.

local function escape_end(s)
  return s:gsub("\\end{fcode}", "\\end {fcode}")
end

function CodeBlock(el)
  if FORMAT ~= "latex" then return nil end
  local lang = el.classes[1]
  local opt = "[]"                       -- always an option: without one, LaTeX looks ahead and a first line
                                         -- starting with # breaks it ("Illegal parameter number")
  if lang == "python" then opt = "[language=Python]"
  elseif lang == "json" then opt = "[]"
  elseif lang == "sh" or lang == "bash" then opt = "[language=bash]" end
  return pandoc.RawBlock("latex", "\\begin{fcode}" .. opt .. "\n" .. escape_end(el.text) .. "\n\\end{fcode}")
end

local function cell_len(cell)
  return #pandoc.utils.stringify(cell.contents or cell)
end

function Table(tbl)
  local n = #tbl.colspecs
  if n == 0 then return nil end
  local maxl = {}
  for i = 1, n do maxl[i] = 3 end
  local function scan(rows)
    for _, row in ipairs(rows) do
      for i, cell in ipairs(row.cells) do
        if i <= n then maxl[i] = math.max(maxl[i], math.min(cell_len(cell), 70)) end
      end
    end
  end
  scan(tbl.head.rows)
  for _, body in ipairs(tbl.bodies) do scan(body.body) end
  local total = 0
  for i = 1, n do maxl[i] = math.sqrt(maxl[i]) + 1; total = total + maxl[i] end
  for i = 1, n do tbl.colspecs[i][2] = 0.98 * maxl[i] / total end
  return tbl
end

function Link(el)
  local t = el.target
  if t:match("^%a[%w+.-]*:") then return nil end          -- http:, mailto:, ...
  local anchor = t:match("^[^#]*%.md#(.+)$")
  if anchor then el.target = "#" .. anchor; return el end
  if t:match("%.md$") or t:match("^%.%./") then return el.content end
  return nil
end
