# The Toolkit
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGNED 2026-11-11 (roadmap 2.8a), building in 2.8a1-2.8a5.**
> One client toolkit in `os/std` (`tk*.xfxn`) that every base app draws
> with: a retained widget tree in logical units, damage per widget, the
> whole base widget set, a gallery as witness. The decisions behind it are
> in the roadmap's Phase 2 item 8; this document is how.

---

## 1. What the reference trees do (surveyed 2026-11-11)

| | Where the widget lives | Repaint | Layout | Focus |
|---|---|---|---|---|
| **SerenityOS LibGUI** | in the client, a C++ object tree | `Window::update` (`LibGUI/Window.cpp`) queues a rect unless a pending one already contains it, and sends the batch once per event-loop turn; the server answers with `MultiPaintEvent`, `Window::handle_multi_paint_event` paints each rect from the main widget down and flips | `BoxLayout::preferred_size` / `run` (`LibGUI/BoxLayout.cpp`): each child's preferred size clamped to min/max, `Grow` as a special size, spacing between, margins around | `Widget::focus_next_widget` (`LibGUI/Widget.cpp`): an ordered list of the window's focusable widgets, the next one or the first |
| **Haiku Interface Kit** | the `BView` is a proxy; drawing is sent to app_server, which owns the pixels | `BView::Invalidate(BRect)` (`src/kits/interface/View.cpp`) sends `AS_VIEW_INVALIDATE_RECT` over the window's link; the server asks back with an update | `BLayout`/`BGroupLayout` | per window, `BView::MakeFocus` |
| **NT user32** | one kernel window (`HWND`) per control; behaviour in a per-class window procedure, e.g. `ButtonWndProcWorker` (`windows/core/ntuser/client/btnctl.c`), a switch over messages | update region per `HWND`, `WM_PAINT` coalesced | none in user32 (dialog templates give fixed positions) | the dialog manager, `IsDialogMessageW` (`client/dlgmgr2.c`): Tab order, the default button on Enter, Cancel on Esc, mnemonics |

(NT5's sources are no longer on disk; the NT rows come from the graph's
symbols and their places, not from reading the bodies.)

Input methods:

- **Haiku:** the input method sends `B_INPUT_METHOD_CHANGED` with the whole
  composition string, its clauses, a selection and a `be:confirmed` flag;
  `BTextView::_HandleInputMethodChanged` (`src/kits/interface/TextView.cpp`)
  deletes the previous inline text, inserts the new, and on confirm feeds the
  characters through `KeyDown` as if typed (`textview_support/InlineInput.h`
  holds the inline range).
- **Windows TSF** (`windows/advcore/ctf`, e.g. `sdk/mark/compose.cpp`): the
  input method edits the application's text through edit sessions on a text
  store (`ITextStoreACP`), a far larger interface.

**Taken:**

- Retained widgets in the client, as Serenity: our windows are client-drawn
  buffers lent to the server (`std/window.xfxn`), so Haiku's server-side
  views would mean a drawing protocol we do not have, and NT's object per
  control would put every check box in the server's tables.
- NT's shape for behaviour: a fixed set of kinds, each one a case of a
  switch (`tk_kind_*`), not a class hierarchy. A custom widget is the kind
  `TK_CUSTOM` with a draw and an event callback.
- Serenity's coalescing (drop a rect another contains) and its layout model
  (preferred, min, max, grow; spacing; margins), on our `ds_damage`.
- NT's dialog manager as a toolkit service: Tab order from the tree, a
  default and a cancel button per dialog, mnemonics later (2.8a4, they are
  per locale).
- Haiku's input-method model, not TSF's: the method owns a composition
  string and tells the field "here is the preedit, the caret, the clauses"
  or "commit this"; the field never hands its text to the method. Enough
  for dead keys, compose sequences and CJK candidates; reconversion (TSF's
  reason for the text store) is not a goal.

## 2. Where text is drawn: the client (decided 2.8a)

Windows are client-drawn, so the toolkit rasterises. The rasteriser
(2.8a′) is a library in `os/std`, linked into every client and into
displayd for its own frames and trusted screens. `CX_FONTS.md` item 4 is
updated to say so. A glyph cache is per process; sharing one across
processes waits until it is measured to matter.

## 3. The tree

A window's widgets live in one `tk_ui`: a fixed pool of `TK_NODES_MAX`
nodes, addressed by `tk_id` (index plus a generation, so a stale id is
refused, as handle tables do), linked parent / first child / next
sibling. Per node:

- **kind** (`TK_LABEL`, `TK_BUTTON`, ... `TK_CUSTOM`), **flags** (visible,
  enabled, focusable, checked, pressed, hovered, mixed, dirty, ...), a
  **value** (slider, progress, selection) and its range;
- the **layout request** (preferred, min, max in logical units; grow;
  alignment) and, after layout, the **box** in logical and in device pixels;
- **accessibility**: role (from the kind unless set), name (a message key
  from 2.8a4; text until then), description, state (from the flags). The
  accessibility tree is this tree (`CX_ACCESSIBILITY.md` §2 item 1);
- text through a **font handle and a size** (`tk_font`), from the first
  day, so outline fonts change no widget;
- a **command** (2.8a2): what activating it does, so a button, a menu item
  and a shortcut are one command.

No allocation per widget: the pool is part of the `tk_ui`, and text is
copied into the ui's own arena (a widget never points at a caller's
buffer, so a freed string cannot be drawn).

## 4. Units and scale

Everything an app gives is in **logical units**. A window has a **scale in
quarters** (4 = 100 %, 6 = 150 %, 8 = 200 %, any quarter from 4 to 16). A
box's device edges are each rounded on their own, `dev(e) = (e * q + 2) /
4`, never its width, so two widgets that meet in logical units meet in
pixels at every scale and no gap or overlap appears at 125 % (the
fractional-scale bug of systems that round sizes). Hairlines (borders,
focus rings) are a whole number of device pixels, at least 1. Fonts are
asked for by device pixel size. The gallery shows 100, 150 and 200 %.

## 5. Layout

Two passes over the tree, each a plain walk:

1. **measure**, children first: a leaf's preferred size from its kind
   (text extent plus padding), a container's from its children (Serenity's
   `BoxLayout::preferred_size`, our rules: preferred clamped to min/max,
   spacing between, padding around);
2. **arrange**, parents first: a row or column hands out its length by
   preferred size, then the rest to children with `grow`, in proportion;
   a grid sizes each column and row to its largest cell.

Containers: **row**, **column**, **grid**, **stack** (children on top of
each other; tabs and dialogs). Spacing and padding come only from the
design scale (`TK_SP_1`... = 4, 8, 12, 16, 24, 32 logical units); an app
cannot give a raw number, which is how the scale is enforced. Mirrored
layouts (right-to-left, 2.8a4) reverse a row's order in arrange only.

## 6. Damage and the frame loop

A change to a node marks it dirty and adds its device box to the ui's
`ds_damage` (a rect another already holds is dropped, as Serenity's
`Window::update`). A layout change damages the old and the new box. The
loop:

```
tk_run(ui): forever
    win_wait -> event -> dispatch (sec. 7)
    if damage and no commit in flight:
        paint: walk the tree; for each visible node whose box meets the
               damage, clip to (box ∩ damage ∩ parent's clip), draw its kind
        win_commit_rects(damage, at most DS_RECTS_MAX: merged to fit)
        in flight until WM_EV_FRAME
```

One commit in flight, paced by `WM_EV_FRAME`, so a burst of changes
becomes one frame. Paint is the only code that touches pixels; event
handling only changes state and marks damage. A **headless** ui renders
into a plain `ds_surface` with events fed by the caller: the self-tests
drive widgets that way with no server, and the gallery under the server is
the end-to-end test.

## 7. Events, focus, keyboard

- **Pointer:** hit-test the tree top down (last child drawn on top wins);
  a press captures the node until release, so a button activates only if
  released over itself (the usual rule; from memory, not read in a tree). Hover
  sets and clears a flag.
- **Keys** go to the focused node, then up the parents until one takes it;
  what nothing takes goes to the ui's command table (2.8a2).
- **Focus:** the focus order is the tree order of nodes that are visible,
  enabled and focusable (Serenity's ordered list). Tab and Shift+Tab move
  it, wrapping; arrow keys move within a radio group, list or menu.
  Activate is Space on buttons, check boxes, switches; Enter is the
  dialog's default button; Esc its cancel. Every widget a pointer can use,
  the keyboard can (`CX_ACCESSIBILITY.md` §2 item 2): checked in 2.8a5.
- **Today's key events** are one character per press (`WM_EV_KEY`, a: the
  code from `SYS_INPUT_READ`), with no release and no modifiers apart from
  what Ctrl folds into a control code. Enough for 2.8a1 (Tab is 9, Shift+Tab
  is not reachable yet). **2.8a2 changes the ABI**: `input_event` and
  `WM_EV_KEY` carry a key code, the modifiers, press or release and the
  character apart, which the keybinds (Super, Ctrl+Shift+Z, Alt+Tab)
  need.

## 8. The theme and the design system

One theme record of **tokens**, light and dark: surface, surface raised,
text, text muted, accent, accent text, border, focus ring, danger,
disabled; one radius, one shadow, the spacing scale, the type sizes. A
widget draws only with tokens; an app chooses colours only by token. High
contrast is a third token set (2.8a5). The theme follows the system
setting and can be forced per ui (the gallery does).

## 9. The widget set and the sub-steps

| Step | What | Witness |
|---|---|---|
| **2.8a1** | the core (tree, layout, scale, damage, loop, focus, theme), and label, button, check box, radio, switch, slider, progress bar; gallery v1 | headless self-tests in `xtest` (layout at three scales, damage, focus order, activation by pointer and key) and the gallery under displayd; `2.8a1.sab` |
| **2.8a2** | key events with codes and modifiers (ABI); commands and keybinds (registered, clashes refused, the three fixed keys); text field with selection, the clipboard (read only on a paste), the input-method interface with a dead-key method | key and clash tests; composition test |
| **2.8a3** | scroll view, list, tree, table, tabs, menu and context menu, tooltip, the dialog with standard buttons, the text grid | gallery pages |
| **2.8a4** | localisation: message keys, `cxk` string tables (a Fluent subset), CLDR plural rules, the pseudo-locale, our UAX #9 run against Unicode's conformance files, mirrored layouts | pseudo-locale gallery boot |
| **2.8a5** | the inspector and the automation API over the tree; boot checks (an interactive widget with no name, one focus cannot reach, clipped text, two commands on one key); high contrast; `gui.xfxn` retired | each check sabotaged |

The gallery (`os/apps/gallery.xfxn`) shows every widget in every state
(normal, hovered, pressed, focused, disabled, checked, mixed), light and
dark, at 100, 150 and 200 %; each step adds its widgets to it (Serenity's
WidgetGallery and GTK's widget-factory are the precedent). Fonts are the
v1 bitmap XFNT until 2.8a′, drawn at an integer multiple for the larger
scales.
