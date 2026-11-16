# Writer: the OPOS word processor

OPOS has Notepad for plain text, but nothing for letters, reports, school
work or CVs: text with headings, bold and italic, lists and pages. People
also receive Word documents (`.docx`) and need to open, change and send
them back. **Writer** is an official Store program (`sdk/programs/writer.c`,
category Office, `official: yes`), built like Picasso and Media Player: one
SDK program, no kernel changes, files opened with `opos_pick_file()` and
saved with `opos_save_file()` (so it needs OPOS 0.6.3 or newer).

## File formats

- **`.docx`** is the main format: Word, LibreOffice, Google Docs and
  WordPad all open it. Writer writes a small, valid Office Open XML package
  (a ZIP with stored, uncompressed parts: `document.xml`, `styles.xml`,
  `numbering.xml`) and reads the parts of other programs' files that it
  understands (deflated with the SDK's `opos_inflate`).
- **`.txt`** (UTF-8; other bytes are read as Windows-1252) opens and saves
  without formatting.
- Anything Writer cannot show yet (pictures, tables, text boxes, comments)
  is left out when a `.docx` opens, and Writer says so. Because opening a
  file never grants writing to it, the first save always asks where to
  save, so the original file is not replaced by accident.

## Steps

1. **Editor and `.docx`** (this step)
   - Pages (A4, 2.54 cm margins) shown one under the other, zoom 50–200%.
   - Paragraph styles Normal, Title, Heading 1–3; bold, italic, underline,
     strikethrough, text size, colour, two fonts (Noto Sans and Noto Sans
     Mono); left, centre, right and justified alignment; bulleted and
     numbered lists with five levels (Tab and Shift+Tab).
   - Typing, selection with mouse and keyboard (double-click a word,
     triple-click a paragraph), cut/copy/paste (formatting kept inside
     Writer, plain text to other programs), undo/redo, line breaks
     (Shift+Enter), tabs.
   - Open and save `.docx` and `.txt`; unsaved-changes question on New,
     Open and Close; page count and word count in the status bar.
   - Kernel: window option `OPOS_WINDOW_ASK_CLOSE` (OPOS 0.6.6). Until
     now the close button removed a program's window before the program
     heard of it, so no program could ask "save changes?". With the option
     the window stays and the program exits by itself; a program that
     does not read its events is still closed when asked again after 3 s.
     On older OPOS versions Writer closes without asking.
2. **Page setup and finding text**
   - Find and replace (Ctrl+F, Ctrl+H).
   - Page size (A4, Letter), orientation and margins, read from and
     written to `sectPr`.
   - Page numbers in the footer, line spacing, paragraph indents, page
     breaks (Ctrl+Enter).
   - Keep the font names of opened documents (shown in Noto Sans, written
     back unchanged); a serif font (Noto Serif) in the package.
3. **Tables and pictures**
   - Tables: insert, type in cells, add and remove rows and columns; read
     and written as `w:tbl`.
   - Pictures (PNG, JPEG) placed in the text, read and written as inline
     `w:drawing` parts.
4. **Sharing**
   - Export to PDF (fonts embedded) for printing elsewhere.
   - Open `.docx` and `.txt` files by double-clicking them in Files
     (file-type association, also useful for Picasso and Media Player).
   - Read `.odt` and `.rtf`.

Spreadsheets and presentations are separate programs and plans.
