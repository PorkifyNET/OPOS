# Spreadsheet: the OPOS spreadsheet

After Writer, the other office program people expect is a spreadsheet:
budgets, lists, marks, invoices, anything with rows of numbers and sums.
People also receive Excel workbooks (`.xlsx`) and CSV exports and need to
open, change and send them back. **Spreadsheet** is an official Store
program (`sdk/programs/spreadsheet.c`, category Office, `official: yes`),
built like Writer: one SDK program, no kernel changes, files opened with
`opos_pick_file()` and saved with `opos_save_file()` (OPOS 0.6.3 or newer),
and `OPOS_WINDOW_ASK_CLOSE` for "Save changes?" on close (OPOS 0.6.6).

## Files

| File | What is in it |
| --- | --- |
| `spreadsheet.c` | the workbook model (sparse cells in a hash table per sheet, styles, undo), editing, drawing, input |
| `sheet_num.h` | reading and writing numbers without floating-point `printf`, dates (1900 system), Office number format codes |
| `sheet_formula.h` | tokenizer, parser, evaluator, about 90 functions, rewriting references |
| `sheet_xlsx.h` | `.xlsx` reading and writing, CSV |
| `ooxml.h` | shared with Writer: whole files, a byte buffer, ZIP, a small XML reader |

## Design

- **Cells** are stored sparsely, keyed by `row * 16384 + column`, so a
  sheet has Excel's size (1,048,576 rows, columns A to XFD) and costs only
  what is filled in. A cell keeps what was typed (`input`), its computed
  value and a style number.
- **Formulas** are compiled to a small tree the first time they are
  computed; references become absolute positions. After every change all
  formulas are computed again, top to bottom and left to right, so long
  chains going down a column stay shallow. Chains deeper than 160 cells are
  finished in further passes; true loops show `#CYCLE!`.
- **References move** by rewriting formula text with the same tokenizer:
  pasting and filling move relative references, inserting or deleting rows
  and columns moves every reference to them (deleted ones become `#REF!`),
  renaming a sheet renames references to it.
- **Undo** records the old state of each changed cell (or a whole sheet,
  for inserting and deleting rows and columns), up to 200 steps or 64 MB.
- **Formulas Spreadsheet cannot compute** (functions it does not have,
  defined names) keep the value saved in the file, and are saved again
  unchanged.

## Steps

1. **Cells, formulas, `.xlsx` and CSV** (this step)
   - Sheets with tabs (add, rename, delete), zoom 50–200%.
   - Numbers, text, TRUE/FALSE, dates and times; typing `12%`, `$5` or
     `2026-11-16` picks a matching format.
   - Formulas with `+ - * / ^ & %`, comparisons, references across sheets,
     ranges and whole columns; about 90 functions (maths, logic, text,
     dates, lookups, `SUMIF`/`COUNTIF`/`…IFS`).
   - Pointing at cells with the mouse or the arrows while typing a formula.
   - Number formats (General, number, currency, percent, date, time, text,
     more or fewer decimals, any Office format code from a file), bold,
     italic, underline, strikethrough, text and fill colours, alignment,
     column widths (drag, or double-click to fit).
   - Cut, copy, paste (between programs as tab-separated text), fill down
     and right, insert and delete rows and columns, sort, AutoSum, the sum,
     average and count of the selection.
   - `.xlsx` open and save; CSV open (comma, semicolon or tab) and save.
2. **More of the sheet**: merged cells, borders, wrapped text, row
   heights, freeze panes, find and replace, the fill handle with series
   (1, 2, 3 and Monday, Tuesday), more functions.
3. **Charts**: column, bar, line and pie charts from a selection, kept in
   `.xlsx` files.
4. **Printing and more files**: PDF export, page setup, `.ods`, opening a
   workbook from Files.

## Testing

- `spreadsheet --print FILE` prints every cell's address, input and shown
  value; `spreadsheet --convert IN OUT` converts between `.xlsx` and CSV.
  Both run on Linux too, so formulas and files can be checked on the build
  machine.
- In QEMU: install from a test Store, type and format cells, save, open
  the file again, and check the close prompt.
