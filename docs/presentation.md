# Presentation: the OPOS slide show program

With Writer and Spreadsheet, the third office program people expect is
one for slide shows: talks, lessons, school projects, meetings. People
also receive PowerPoint files (`.pptx`) and need to open, change, show and
send them back. **Presentation** is an official Store program
(`sdk/programs/presentation.c`, category Office, `official: yes`), built
like Writer and Spreadsheet: one SDK program, no kernel changes, files
opened with `opos_pick_file()` and saved with `opos_save_file()` (OPOS
0.6.3 or newer), `OPOS_WINDOW_ASK_CLOSE` for "Save changes?" on close
(OPOS 0.6.6), and `OPOS_WINDOW_FULLSCREEN | OPOS_WINDOW_QUIET` for the
slide show.

## Files

| File | What is in it |
| --- | --- |
| `presentation.c` | the deck model (slides, shapes, rich text), drawing, the editor, the slide show, undo |
| `slide_pptx.h` | `.pptx` reading (with layouts, masters and themes) and writing |
| `ooxml.h` | shared with Writer and Spreadsheet: whole files, a byte buffer, ZIP, a small XML reader |

## Design

- **Positions** are kept in EMU (914,400 per inch), the unit of `.pptx`
  files, so shapes do not move when a file is opened and saved again. A
  slide is drawn at any scale: the editor, the thumbnails and the full
  screen show all use the same drawing code.
- **Text** is paragraphs of UTF-8 with one look per byte (colour, bold,
  italic, underline, strikethrough, size in 1/100 point), like Writer.
  Paragraphs have alignment, a level (0-8), bullets or numbering (ten
  `.pptx` numbering styles), indents, spacing and line spacing in
  PowerPoint's units. Text is laid out for the scale it is drawn at.
- **Placeholders** (title, subtitle, body) show a grey prompt in the editor
  until they have text. Their text shrinks to fit, as in PowerPoint; text
  boxes grow with their text.
- **Opening a file** resolves PowerPoint's inheritance: a placeholder takes
  its position, text look, bullets and spacing from the layout and the
  master (`txStyles`), theme colours go through the master's colour map,
  and `lumMod`, `lumOff`, `tint` and `shade` are applied. Shapes on layouts
  and masters (bars, logos) become read-only decorations drawn under the
  slides. Groups are flattened. Style references (`p:style`) give shapes
  inserted in PowerPoint their theme fill, outline and text colour.
- **Saving** writes a small complete package: one theme, one master, a
  layout per decoration set and kind of slide, and slides whose shapes
  carry all of their look, so they look the same whatever the layout.
  Pictures are kept byte for byte. The theme's fonts are Noto Sans, the
  font Presentation draws with.
- **Undo** keeps whole-deck snapshots (up to 100 steps or 128 MB); typing
  and deleting in one shape are one step.

Not shown yet, and reported when a file is opened: tables, charts,
SmartArt, rotation, custom shapes (drawn as rectangles), picture fills
and cropping, gradient and picture backgrounds (gradients become their
average colour), vertical text and equations. Speaker notes, animations
and transitions are left out without a message, and are not saved.

## Steps

1. **Slides, shapes, text, `.pptx` and the slide show** (this step)
   - New slides from five layouts (title, title and content, section
     header, title only, blank); duplicate, move, hide and delete slides.
   - Text boxes, rectangles, rounded rectangles, ellipses, lines and
     pictures (PNG, JPEG, BMP); move and resize with the mouse (Shift or
     pictures keep the proportions), arrows nudge, Tab picks the next
     shape, bring to front and send to back.
   - Bold, italic, underline, strikethrough, sizes, text colour, alignment,
     bullets and numbering with levels (Tab, Shift+Tab); fill and outline
     colours; slide background colours.
   - Cut, copy and paste of text and shapes (text between programs).
   - The slide show: full screen, from the first slide (F5) or this one
     (Shift+F5); Space, Enter, arrows, Page Down and clicks go on,
     Backspace and Page Up go back, Esc ends it.
   - `.pptx` open and save.
2. **Tables and more shapes**: tables, arrows, stars and other preset
   shapes, rotation, picture cropping, gradient backgrounds.
3. **Speaker notes and the presenter view**: notes kept in `.pptx` files,
   a timer, the next slide.
4. **Transitions, animations and printing**: fade and push transitions,
   appear-on-click animations, PDF export, `.odp`.

## Testing

- `presentation --print FILE` prints every slide's shapes, positions,
  colours and text runs; `presentation --convert IN OUT` opens and saves a
  `.pptx`; `presentation --render FILE PREFIX WIDTH` draws every slide to
  `PREFIX<n>.ppm`. All three run on Linux too. To draw text, `--render`
  needs the fonts at `/share/fonts`: run it in a user-namespace chroot
  (`unshare -r chroot ROOT /presentation ...`) whose `ROOT/share/fonts` is
  a copy of `data/fonts`.
- Round trip: print a file, convert it, print the copy; the two listings
  must match except for the "not shown yet" note.
- In QEMU: install from a test Store, type in the placeholders, add a
  slide with a two-level list, draw a shape and type in it, insert a
  picture, undo and redo, show the slides, save, open a `.pptx` made by
  another program, and check the close prompt.
