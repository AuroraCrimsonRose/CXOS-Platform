# The Toolkit
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGNED 2026-11-11 (roadmap 2.8a), building in 2.8a1-2.8a5.**
> One client toolkit in `os/std` (`toolkit.xfxn`, `toolkit_window.xfxn`; its functions are `tk_`, X having one namespace) that every base app draws
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
layouts (right-to-left, 2.8a4) are not a second layout: the window is
mirrored where it meets pixels and the pointer (§10).

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

**What waits on the time (2.8a3).** The ui keeps no clock of its own: the
loop gives it the time (`tk_tick(ui, now_ms)`) and asks how long until it
has something to do (`tk_due`: 0 now, `TK_NONE` never), and waits that
long and no longer. `DS_OP_WAIT` takes a timeout in ms in its flags (0
for none); the server's tick hands a waiter whose time ran out with no
event `WM_EV_TIMER` once (`wm_client.wake_at`, cleared when it fires and
set again by the next wait; a wait an event answered is no longer one). Only the tooltip waits on the time today; a caret's blink
and key repeat in an app would go the same way. This is Serenity's
`Core::Timer` folded into the one wait the loop already makes, without a
second thread or a timer object (`Application.cpp`'s tooltip timers are
Core timers).

## 7. Events, focus, keyboard

- **Pointer:** hit-test the tree top down (last child drawn on top wins);
  a press captures the node until release, so a button activates only if
  released over itself (the usual rule; from memory, not read in a tree). Hover
  sets and clears a flag.
- **Keys** go to the focused node; what it does not take goes to the
  dialog manager and then the ui's command table (2.8a2; below).
- **Focus:** the focus order is the tree order of nodes that are visible,
  enabled and focusable (Serenity's ordered list). Tab and Shift+Tab move
  it, wrapping; arrow keys move within a radio group, list or menu.
  Activate is Space on buttons, check boxes, switches; Enter is the
  dialog's default button; Esc its cancel. Every widget a pointer can use,
  the keyboard can (`CX_ACCESSIBILITY.md` §2 item 2): checked in 2.8a5.
- **Key events (2.8a2, ABI 41).** `input_event` and `WM_EV_KEY` carry a
  **key code**, the **modifiers** and the **character** apart, for presses
  and releases (`CX_SYSCALLS_IPC.md`, `input_event`). Codes are USB HID
  usages (page 7, `KC_*`), so PS/2 set-1 scancodes are translated and a USB
  report needs nothing. Serenity's `KeyEvent` (Kernel/API/KeyCode.h) has the
  same parts (key, scancode, code point, a flags word with the modifiers and
  `Is_Press`); we keep one word of mods with the flags in it: `KEYMOD_SHIFT
  CTRL ALT SUPER` (held), `CAPS NUM` (locks), `KEYEV_RELEASE`, `KEYEV_REPEAT`
  (typematic). The modifier keys are events themselves. `ch` is the code
  point the key types under the kernel's US layout, or 0 (releases, Enter,
  Tab, Backspace, Esc, arrows); Ctrl, Alt and Super do not change it, so an
  app sees Ctrl+S as `KC_S`, `'s'`, `KEYMOD_CTRL`. Caps Lock turns letters
  only, and only with nothing or Shift held (as Serenity's
  `get_char_from_character_map`, Kernel/Devices/Input/Management.cpp). Num
  Lock starts on; off, the keypad keys are the navigation keys' codes. A
  held modifier's typematic repeats give no event. One keymap state serves
  every keyboard (PS/2 and USB alike); USB has no typematic yet.
- **The system's keys** are the window server's (`wm_system_key`) or the
  kernel's, and never reach a client, pressed, held or let go: **Super**
  alone or with anything (the launcher's from 2.8c; Super+B minimise, W
  next workspace, E send, R reduce motion), **Alt+Tab** (Shift allowed, held
  it goes on round), **Alt+F4** (asks the window to close, as its close box
  does), and the **three fixed keys**: Ctrl+Alt+Delete (the kernel's
  attention, once a press: the trusted screen), Ctrl+Alt+F2 (end the
  session, back to the console), Super+Ctrl+Enter (the screen reader,
  counted as asked until 2.8c has one). The old Ctrl+Q/N/B/W/E/R are gone:
  they were keys an app needs. The trusted screen takes presses only, and
  only without Ctrl, Alt or Super, by code (Y, N or Esc, Esc for the next,
  1 to 9 to end a client).
- **Dispatch in the toolkit:** `tk_key(ui, code, ch, mods)`. A release is
  nobody's. A press goes to the input method if a text field has the
  focus (§9), then to the focused widget (only with no Ctrl, Alt or Super
  held), then to the dialog manager (Tab, Shift+Tab, Enter: the default
  button, Esc: the button flagged `TK_F_CANCEL`), then to the command table;
  what is left reaches the app as `TK_EV_KEY` with all three parts.
- **Commands** (design goal 8). A command is a name and at most one key
  (`tk_command(ui, name, key, mods)`, its number; `tk_bind` to move it,
  key 0 to unbind; `tk_set_command(ui, widget, cmd)` so a button runs one).
  A key runs a command only with exactly its modifiers held (locks do not
  count). **A clash is refused** (`E_ALREADY_EXISTS`) and the binding
  that was there stays: Haiku's `BWindow::AddShortcut`
  (src/kits/interface/Window.cpp) silently replaces the old shortcut, which
  is how two commands come to fight over a key unseen. **The system's keys
  are refused** (`E_ACCESS_DENIED`; `tk_key_reserved`, and a boot test sweeps
  every code and modifier so the server's and the toolkit's lists cannot
  drift). A key that types (none or only Shift, except F1 to F12) and a
  modifier alone are `E_INVALID_ARGS`. Every ui starts with the standard
  six: copy, cut, paste, select-all, undo, redo on Ctrl+C X V A Z and
  Ctrl+Shift+Z; a focused text field does them itself, otherwise they come
  to the app as `TK_EV_COMMAND` like the app's own.

## 8. The theme and the design system

One theme record of **tokens**, light and dark: surface, surface raised,
text, text muted, accent, accent text, border, focus ring, danger,
disabled; one radius, one shadow, the spacing scale, the type sizes. A
widget draws only with tokens; an app chooses colours only by token. High
contrast is a third token set (2.8a5, §11). The theme follows the system
setting and can be forced per ui (the gallery does).

## 9. The widget set and the sub-steps

| Step | What | Witness |
|---|---|---|
| **2.8a1** | the core (tree, layout, scale, damage, loop, focus, theme), and label, button, check box, radio, switch, slider, progress bar; gallery v1 | headless self-tests in `xtest` (layout at three scales, damage, focus order, activation by pointer and key) and the gallery under displayd; `2.8a1.sab` |
| **2.8a2** | key events with codes and modifiers (ABI); commands and keybinds (registered, clashes refused, the three fixed keys); text field with selection, the clipboard (read only on a paste), the input-method interface with a dead-key method | key and clash tests; composition test |
| **2.8a3** | scroll view, list, tree, table, tabs, menu and context menu, tooltip, the dialog with standard buttons, the text grid; the server's timed wait | headless tests of each, the gallery's pages replayed; `2.8a3.sab` |
| **2.8a4** | localisation: message keys, `cxk` string tables (a Fluent subset), CLDR plural rules, the pseudo-locale, our UAX #9 run against Unicode's conformance files, mirrored layouts | pseudo-locale gallery boot |
| **2.8a5** | the inspector and the automation API over the tree; boot checks (an interactive widget with no name, one focus cannot reach, clipped text, two commands on one key); high contrast; `gui.xfxn` retired (`590485a`) | the gallery audited at three scales and in the pseudo-locale; each check sabotaged (§11) |

**2.8a2, as built (2026-11-12).**

- **Text field** (`TK_TEXTFIELD`): one line of UTF-8, at most 255 bytes,
  in a pool of eight fields a ui. Offsets are bytes on a character's first
  byte, so the arrows, Backspace and Delete step whole characters; Home and
  End; Shift extends the selection; the pointer puts the caret between
  characters and a drag selects. Typing replaces the selection; only whole
  characters that fit go in. One level of undo: a run of typed characters
  is one change, and undo again after an undo is refused (redo puts it
  back). The text is drawn through UTF-8 to code page 437 (the v1 bitmap
  font's), so the Latin-1 letters it has are shown and anything else is
  `?`, until 2.8a′'s fonts. The node's text is the field's accessible name;
  the field's own text is `tk_field_text`.
- **The clipboard** (design goal 7) is the server's: text, at most
  `WM_CLIP_MAX` (4096) bytes, `DS_OP_CLIP_SET` and `DS_OP_CLIP_GET` with a
  page. Only the **focused window's client** may set or read it, and only
  within `WM_GESTURE_MS` (one second) of being **handed a press** (it
  takes a key press, not a release or a repeat, or a pointer button going
  down, not one held); a read **spends** the press, so one paste a press.
  This is the browsers' transient activation (from memory). Serenity's
  ClipboardServer (LibGUI/Clipboard.h) lets any client fetch at any time,
  which is what goal 7 is against. The toolkit asks for a read only for
  the paste command (`ui.clip_want`, served by `toolkit_window.xfxn` with
  `win_clip`), and puts what comes back through `tk_paste_text`, which
  makes line breaks and tabs spaces and drops other control characters and
  bytes that are not UTF-8.
- **Input methods** see a focused field's keys before the field
  (`tk_im_key`; `tk_set_im(ui, TK_IM_NONE or TK_IM_DEAD)`); a method keeps
  its pre-edit text in the field, shown underlined at the caret, and
  commits through the field's insert. The **dead-key method** has Haiku's
  five accents (its keymaps' acute, grave, circumflex, diaeresis, tilde:
  `BKeymap::IsDeadSecondKey`, src/kits/shared/Keymap.cpp), on the keys
  US-International puts them: ' ` ^ ~ ". The next key decides: a letter
  the accent goes on gives the one Latin-1 character (á é í ó ú ý ç, à è ì
  ò ù, â ê î ô û, ã õ ñ, ä ë ï ö ü ÿ, and the capitals Latin-1 has);
  Space or the same accent gives the accent; Esc or Backspace drops it; a
  key with no character, or with Ctrl or Alt, gives the accent and then
  goes on; another accent gives the first and waits with the second;
  anything else gives the accent and then itself. A modifier key alone
  changes nothing; a move of the focus drops it. Haiku applies its dead
  keys in the input_server, for every app at once
  (`KeyboardDevice::_ControlThread`,
  src/add-ons/input_server/devices/keyboard/KeyboardInputDevice.cpp); ours
  is in the toolkit, per ui, so a method can be per field later.
- **The gallery** has a text-field row (dead keys on), and its own keys are
  commands now (Ctrl+D dark, Ctrl+1 2 3 scale), so a focused field types
  them as letters.
- **Witness:** the headless tests (`tt_commands` 4800, `tt_field` 4900,
  `tt_dead` 5000), the server's (`w_system_keys` 5100, `w_keys_agree`
  5150, `w_clipboard` 5200), the gallery replay typing with a dead key and
  copying and pasting through the server (4614-4624), the kernel's
  (`test_ps2kbd.c` in the host tests, the input handle's steps 18 and 19),
  and `tests/sabotage/2.8a2.sab`.

**2.8a3, as built (2026-11-13).**

- **Scroll view** (`TK_SCROLL`): one child, laid out at the size it asks
  for and seen through the view's box, less a bar on each axis it needs
  (`TK_BAR`, 10). A bar's thumb drags; a press on its track pages. The
  view is a focus stop only when nothing in it can take the focus (a
  page of text); then the arrows, Page Up and Down, Home and End scroll
  it. Positions are logical and clamped to the content. The focus
  going somewhere out of sight scrolls every view round it until it is in
  sight, at the next paint (`tk_reveal_now`, Serenity's
  `AbstractScrollableWidget::scroll_into_view`). `tk_scroll_by`,
  `tk_scroll_pos`.
- **List, tree, table** (`TK_LIST`, `TK_TREE`, `TK_TABLE`, rows of
  `TK_ITEM`): one focus stop each, a chosen row (`tk_select` without an
  event, `tk_selected`). Up, Down, Home, End, Page Up and Down choose,
  stepping past disabled rows and stopping at the ends; a choice by key or
  press is `TK_EV_CHANGE` (value the row's place, code its id); Enter is
  `TK_EV_ACTIVATE`. A tree's items nest; a closed item's children are not
  rows. On a tree, as `TreeView::keydown_event` (LibGUI/TreeView.cpp):
  Right opens a closed item or goes to its first child, Left closes an
  open one or goes to its parent, Enter and Space open or close; a press
  on the disclosure does too, and the app hears `TK_EV_EXPAND`. A table's
  first item is its header (its items the cells), never chosen.
- **Tabs** (`TK_TABS`): its children are the pages, a page's text its
  tab's; one page shows. Left, Right, Home, End on the focused tabs, not
  wrapping (`TabWidget::keydown_event`); Ctrl+Tab and Ctrl+Shift+Tab from
  anywhere in it, wrapping. The focus in a page turned away goes to the
  tabs.
- **Menus** (`TK_MENUBAR`, `TK_MENU`, `TK_MENUITEM`; an item with no text
  is a separator): F10 opens the bar's first menu with its first item lit;
  Up and Down wrap past separators and disabled items; Left and Right go
  to the menu beside; Enter and Space choose; Esc, F10 or Alt close
  (Serenity's WindowServer `MenuManager`). An item runs its command
  (2.8a2) if it has one, so a menu item and a key are one action, and
  shows the command's key; else the app hears `TK_EV_ACTIVATE`. A press
  outside closes every menu and goes no further. A **context menu** is a
  `TK_MENU` not in a bar, given to a widget by `tk_set_context`: a right
  press on it or what it holds, Shift+F10 or the Menu key on the focus;
  `tk_popup` opens one anywhere.
- **Tooltips** (`tk_set_tip`, also the widget's accessible description):
  the pointer still over a widget with one for 700 ms shows it under the
  widget (above if there is no room); moving to another while one is up
  switches at once; a press, a key or moving off hides it (Serenity's
  `Application` tooltip show timer). A focus moved by key to a widget with
  a tip shows it at once, so the keyboard reads it too.
- **Dialogs** (`tk_dialog(ui, title, text, buttons)`, `TK_DLG_*`): a
  heading, a text, a body for the app's widgets (`tk_dialog_body`), and
  the buttons asked for, Discard alone at the left and the rest at the
  right, OK last, as Haiku's `BAlert` (src/kits/interface/Alert.cpp) puts
  its default last and Serenity's `MessageBox` names its standard ones.
  Enter presses OK, else Yes, else Close, else the last; Esc Cancel, else
  Close. Open (`tk_dialog_open`), the menus close, what is under it is
  dimmed and out of reach of the pointer and Tab, the app's own commands
  wait (a field in it keeps copy and paste), and the focus goes to its
  body's first widget, else its default. A button closes it and the app
  hears `TK_EV_DIALOG` with the button; closing gives the focus back.
- **Text grid** (`TK_TEXTGRID`): up to 255 by 255 character cells, all
  grids' cells together at most 4096 a ui; a cell is a code point and
  two colours, the theme's or one of 16 (`tk_palette`). Put, write UTF-8
  along a row, clear, scroll up, a cursor drawn reversed. The terminal
  (2.8b) is built on it.
- **The gallery** is three pages in tabs (Controls; Collections, a list,
  a tree and a table each larger than its view; Menus and dialogs, with a
  tooltip, a text grid and a dialog), a menu bar whose Edit menu is the
  standard commands and whose View menu is the gallery's, and a context
  menu on the last two pages.
- **Witness:** the headless tests (`tt_scroll` 5300, `tt_list` 5400 and
  5600, `tt_tree` 5500, `tt_tabs` 5700, `tt_menus` 5800, `tt_tips` 5900,
  `tt_dialog` 6000, `tt_textgrid` 6100), the server's timed wait
  (`w_timed` 6200), the gallery replay through the pages, a menu, a
  tooltip and the dialog (4625-4641), and `tests/sabotage/2.8a3.sab`.

## 10. Localisation and bidirectional text (2.8a4)

**Surveyed 2026-11-13.** Haiku's catalogs are keyed by the English text and
a context (`B_TRANSLATE(string)`, `headers/os/locale/Catalog.h`), and
`BCatalog::GetString` (`src/kits/locale/Catalog.cpp`) walks a chain of
catalogs, the first that has the string winning; plurals go to ICU's
`MessageFormat` (`headers/os/locale/StringFormat.h`, ICU's
`{0, plural, one{...} other{...}}`). Serenity's
`Painter::split_text_into_directional_runs` (`LibGfx/Painter.cpp`) is, by
its own FIXME, "a very simplified version" of UAX #9: no explicit
embeddings or isolates, no brackets, one pass for neutrals. Windows
mirrors a whole window by mirroring its device context
(`SetLayout(LAYOUT_RTL)`, from memory; NT5 is not on disk), drawing text
the right way round inside the mirrored box.

**Taken:** keys, never the English text (Haiku's key is the text, so
changing the English orphans every translation); Haiku's chain as our
fallback (the locale's table, then English, then the key itself, so a
missing string is seen, not blank); Fluent's syntax for the source and its
plural selectors, compiled ahead of time instead of ICU at run time; the
whole of UAX #9, not Serenity's subset, held to Unicode's own test files;
Windows' mirroring of the whole window, not a row reversed in arrange (§5
said so before; a reversed row would leave a check box's box, a slider's
direction, a scroll bar's side and a tree's indent the LTR way round).

**Data, vendored unmodified** (`THIRD-PARTY-NOTICES.md`): Unicode 18.0's
`DerivedBidiClass.txt`, `BidiBrackets.txt`, `BidiMirroring.txt`,
`BidiTest.txt` and `BidiCharacterTest.txt` in `vendor/unicode/`, CLDR 49's
`plurals.xml` in `vendor/cldr/`, both under the Unicode License v3.

**The source: a Fluent subset** (`os/locale/<domain>/<tag>.ftl`, the
domains `toolkit` and one per app). Comments (`#`, `##`, `###`); messages
`key = text`, the key `[a-zA-Z][a-zA-Z0-9_-]*`; text that goes on in
indented lines (Fluent's line breaks kept, each continuation line's indent removed);
attributes (`.tip = ...`, looked up as `key.tip`); placeables: a variable
`{ $name }`, a string literal `{ "{" }` (escapes `\"`, `\\`, `\uXXXX`), and
a select on a number, `{ $n -> [one] ... *[other] ... }`, whose keys are
CLDR plural categories or integers, exactly one marked default. Refused
with the line named: message references, terms, functions, selects on
anything but a variable. `&` before a character marks its mnemonic, `&&`
is an `&`.

**`cxk strings build <src.ftl>... --locale <tag> [--base <en.ftl>...]
[--pseudo] -o <table.xstr>`.** Every source of an app's table at once (the
toolkit's and the app's), so a key is unique across them. With `--base`,
a translation is held to the English: a key English lacks is an error, a
variable it does not have is an error, a missing key is a warning (it falls
back). A select's category keys must be the locale's CLDR categories. The
locale's plural rules come from `plurals.xml`, compiled into the table.
The table is read back through the checker before the command succeeds,
as `cxk font build` does. `--pseudo` writes the pseudo-locale (below).

**XSTR, the table** (`versions.json` `xstr`), little-endian: a 40-byte
header (`XSTR`, version, flags with bit 0 right-to-left, the tag in 16
bytes NUL-padded, the message count, where the index and the plural rules are, the
whole size), an index of 12-byte records sorted by key bytes (key offset
and length, body offset and length), then the bytes. A body is a pattern:
`1 len16 text`, `2 len8 name` (a variable), `3 len8 name count8` and that
many variants (`kind8` 0 a category, 1 an integer; the key; `default8`;
`len16` and a pattern). The plural block is a count and, per category but
`other`, its rule as relations (operand, modulus, `=` or `!=`, ranges)
joined by `and` and `or`, as CLDR writes them. **The checker** (`lc_parse`)
copies the bytes into the table's own buffer first (a table can come from
another process's memory), then refuses anything out of place: a size
that is not the length, an offset or a length past the end, keys not in
strictly rising order (so lookup is a binary search and a key is there
once), text that is not UTF-8, an unknown op, a select with no default or
two, nesting deeper than 4, a plural rule that runs past its block.

**At run time** (`os/std/locale.xfxn`, `lc_`): `lc_parse`, `lc_find`,
`lc_format(table, fallback, key, args, out, cap)` (variables by name;
numbers are integers, written in ASCII digits until CLDR's number formats
come with the first program that needs them), `lc_plural(table, n)`. The
operands are CLDR's with no fraction: n = i = |value|, v = w = f = t = e =
0. **Where a client's tables come from:** the display server, as its font:
clients have no disk (`dsv_launch`), so the server reads
`/System/Locale/<program>-<tag>.xstr` and `-en.xstr` for the program it
starts and hands both over in one read-only memory object
(`WIN_LOCALE_SLOT`); the client copies and checks them
(`tk_win_locale`). The server's locale is its own setting (`displayd
--locale <tag>`, `en` by default) until Settings has a page for it.
**As built:** the object is `u32` the locale's table's length, `u32` the
English one's, then the two, each at most `LC_TABLE_MAX` (16 KiB; either
length 0 for none, and no object at all when a program has no tables); the
server reads them per launch (`dsv_locale_mem`) and keeps no mapping. The
build makes `gallery-en.xstr`, `gallery-qps.xstr` and `plurals.xplt`
(CLDR's samples, for the boot) and stages them in `/System/Locale`
(`tools/cmake/CMakeLists.txt`).

**In the toolkit.** A widget made with a key (`tk_add_key`, `tk_set_tip_key`,
`tk_dialog_key`, the standard dialog buttons and the key names in menus)
keeps the key and shows what it looks up; `tk_set_arg(ui, id, name, n)`
gives it a number to format with. `tk_set_locale(ui, table, fallback)`
looks every key up again (the arena is rebuilt, not grown), takes the
direction from the table, and lays out again. The accessible name is the
looked-up text; `tk_key_of` gives the key. **Mnemonics:** a key's `&x` is
shown underlined and not as `&`; Alt+x, or x alone in an open menu, does
what a press would (a button, a check box, a menu's title), the dialog
manager's step before the command table.

**Bidirectional text** (`os/std/bidi.xfxn`, `bd_`, the tables generated
into `os/std/unibidi.xfxn` by `cxk unicode bidi vendor/unicode`, checked in
and held to the vendored files by a DevKit test, as `cxk_abi.h` is): the
whole of UAX #9 for one paragraph, P2-P3, X1-X10 with isolates and the
overflow counts, W1-W7, N0 with BD16's 63-deep bracket stack (canonical
equivalents of U+2329 and U+232A), N1-N2, I1-I2, L1 and L2, and L4's
mirrored glyphs; at most 256 characters a run. **Its witness is Unicode's:**
`os/xc/bidicheck.xfxn`, built for the host as the compiler's dump
programs are, runs every line of `BidiTest.txt` and
`BidiCharacterTest.txt` in the CI container and must report no failure; the
boot runs a set of hand cases through the same code. Text with no
right-to-left character in a left-to-right ui takes none of it.

**Mirroring.** A right-to-left ui lays out exactly as a left-to-right one
and is mirrored where it meets pixels and the pointer: every shape is drawn
at x' = W - x - w, damage goes out mirrored, a pointer's x comes in
mirrored. Text is the exception: its box is mirrored and its characters are
set in it in UAX #9's visual order, the paragraph's direction the ui's, not
reversed glyph by glyph. A text grid's cells are never mirrored (a terminal
is left to right). The text field sets its text in visual order and puts
the caret at the edge of the character it is before.

**The pseudo-locale** `qps` (the private-use tag Windows' pseudo-locales
use): every text run's letters accented with ones code page 437 has (so
the v1 font shows them), 40% longer (`~` after the text, at least one),
in `[` `]` (a clipped end shows), and right-to-left. Placeables, mnemonic
letters and select keys are left alone. **Witness: the gallery in `qps`**,
through the server, matching the headless ui as the `en` replay does; and
headless: no widget's shown text without a key, no text wider than its
box, and the whole canvas (no font, so shapes only) the mirror image of the
same gallery laid out left to right, the text grid's box aside.

**The checks, as built** (`os/apps/locale_tests.xfxn`, `xtest.xfxn`): the
staged tables read and checked, five kinds of damage refused, lookup,
plurals, the fallback chain and cutting at a character; every
`tk_builtin` key equal to `os/locale/toolkit/en.ftl`; every CLDR sample's
category (`plurals.xplt`); UAX #9 hand cases, 6500-6515, on the machine (the
whole files on the host, `BidiConformanceTests`); text read back off the
canvas through a font whose every glyph names itself (visual order, L4's
mirrored brackets, an underlined mnemonic, the mirrored box); Alt+letter
on a menu title, a label, a button, a dialog's check box, and a letter in
an open menu; the pseudo-locale gallery headless (keyed, unclipped on
every page and menu, mirrored pixel for pixel, the pointer and damage
mirrored, Left and Right swapped but in a text field) and through the
server (`gx_gallery_qps`). The English gallery's replay now loads the same
English table the server hands the gallery.

The gallery (`os/apps/gallery.xfxn`) shows every widget in every state
(normal, hovered, pressed, focused, disabled, checked, mixed), light and
dark, at 100, 150 and 200 %; each step adds its widgets to it (Serenity's
WidgetGallery and GTK's widget-factory are the precedent). Fonts are the
v1 bitmap XFNT until 2.8a′, drawn at an integer multiple for the larger
scales.

## 11. Automation, the inspector, the audit, high contrast (2.8a5)

**Surveyed 2026-11-25.** Serenity's LibGUI has no automation layer: its
tests drive widgets by calling them. Haiku's scripting suite
(`BHandler::ResolveSpecifier` and `GetSupportedSuites`,
`src/kits/app/Handler.cpp`) addresses a window's views by name and index
through messages, and `hey` drives any app with it, asking no permission;
views answer `B_GET_PROPERTY` for "Frame", "Label", "Value". Windows' UI
Automation and Linux's AT-SPI are bridges an app feeds (from memory), and
fall out of step with what is drawn (`CX_ACCESSIBILITY.md` §2 item 1).
**Taken:** Haiku's address by role and name, over our own tree with no
bridge; nothing of its open door: an action here goes in by the input path
a person's does, and reading another program's tree waits for the
assistive-technology grant (Phase B, `CX_ACCESSIBILITY.md` §2 item 3). The
audit's checks are the ones axe-core and Accessibility Insights make of web
pages and Windows apps (from memory: a control's name, keyboard reach,
clipped text), run as a boot test instead of a tool someone remembers to
run.

**The automation API** (`os/std/toolkit_ax.xfxn`, `tk_ax_`), by id:
`tk_ax_find(ui, from, role, name)` (the first in tree order under `from`;
role 0 or name 0 for any), `tk_ax_parent`, `tk_ax_first`, `tk_ax_next`;
`tk_ax_state` (`TK_AX_DISABLED FOCUSABLE FOCUSED CHECKED MIXED PRESSED
SELECTED EXPANDED OFFSCREEN DEFAULT`); `tk_ax_bounds` (where it shows:
device pixels, cut to the canvas, mirrored as the ui is). Actions:
`tk_ax_press` is the pointer going down and up at the middle of where the
widget shows, refused for one disabled or out of reach (under a dialog, on
a page turned away), and whatever covers that point gets the press, as it
would a person's; `tk_ax_focus` is `tk_focus`; `tk_ax_key` and
`tk_ax_type` (ASCII by its US key, anything else as an input method's
character) are key presses. So a widget a test cannot reach is one a
person cannot.

**The inspector.** `tk_inspect(ui, id, out, cap)`: the tree from `id`, a
line a node, two spaces a level: its role, its name in quotes, where it
shows (`x,y wxh`, or `-`), its states as words; then
`damage n: x,y wxh ...`, what the last paint drew (`ui.painted`), as laid
out. Cut short, it keeps whole lines and returns `E_NO_RESOURCES`.
`tk_set_inspect(ui, 1)` outlines every widget that can be seen, one pixel
in the muted token, the focus in the ring's (the gallery's Ctrl+Shift+I,
in its View menu). A program that shows another's tree is the Process
Manager's, with the grant.

**The audit.** `tk_audit_run(ui, &a)` audits the ui as it stands (the
caller visits each page, dialog and menu) and changes nothing it shows
(the focus, its damage, the reveal and a waiting accent are put back):

- **No name:** every widget that takes input but a scroll view (what it
  holds is read) and a tab list (its pages name its tabs; ARIA asks no
  name of one either), a list's or tree's item, a table's cell, a menu bar's
  menu and a tab's page has a non-empty name. A table's rows are named by
  their cells; a menu item with no text is a separator.
- **Focus cannot reach it:** every widget a pointer can use now (enabled,
  to be seen, in the scope, a kind that takes input; a scroll view only
  when it holds nothing that does) must be a stop of Tab, walked round the
  scope by `tk_focus_next` itself, not judged by `tk_focusable`; every row
  a list, tree or table may choose, a stop of Down; with a menu open, each
  item it may light, a stop of Down there; each menu of a bar, a stop of
  Right. What a pointer can use is asked of the tree, so a fault in the
  focus code is seen, not repeated.
- **Clipped text:** every text to be seen fits the room its box gives text
  (a label all of its box; a button less its padding; a check box, radio
  or switch less the control and its gap; a menu item less its padding and
  its key's name).
- **Two things on one key:** no two commands on one key (`tk_bind` refuses
  it; this is there for the day it does not), no Alt command whose letter
  a mnemonic takes first (mnemonics come before commands, §7), no two
  mnemonics that would act on one letter (the first in the tree always
  wins), and in an open menu none between its items.

Findings are counted, each kind's first named (`tk_audit`: `found`,
`count`, `name`, `focus`, `clip`, `keys`, `cmd`).

**High contrast** is the third token set (§8): white text and borders, a
yellow accent and a cyan focus ring on black; `tk_set_contrast(ui, 1)`, and
dark changes nothing while it is on (the gallery's Ctrl+Shift+H). The
ratios are WCAG 2.2's, computed in the toolkit (`tk_contrast`, x 100, from
`tk_srgb_lin`, the sRGB curve x 10^7): high contrast holds every text to
7:1 against what it is drawn on (1.4.6), disabled text to 4.5:1 (WCAG asks
none), the accent, ring, border and track to 3:1 against the surface and
the accent to 3:1 against the track (1.4.11). Light and dark are held to
4.5:1 for text, muted text and the accent's text, and 3:1 for the ring.
**Not held, found here:** light and dark's border (1.53 and 1.92:1) and
track (1.31 and 1.69:1) are under 1.4.11's 3:1, so a check box's outline
and a slider's track are faint in both. Darker ones change the look: a
design call, left for Aurora; high contrast is the answer until then. The
setting is per ui until Settings has a page (2.8c); the theme follows it
then.

**The witness** (`os/apps/ai_tests.xfxn`, in `xtest`): the API (7000:
found, walked, states, a press by pointer, mirrored too and refused under a
dialog, typing, bounds), the audit against what is planted in it (7100:
each kind, the first named, a clean ui clean and unchanged), the inspector
(7200: lines and depths, bounds written by the test's own code, the damage,
a dump cut short, the overlay's pixels), contrast (7300: known ratios, the
three sets' bounds, the set on the canvas), and **the gallery's boot
check** (7500-7899): in English at 100, 150 and 200 % and in the
pseudo-locale (longer, and mirrored), each page, the dialog, each menu of
the bar and the context menu audit clean. Kept to their checks by
`tests/sabotage/2.8a5.sab`: each check blinded, and each defect it is there
for put into the gallery or the toolkit.
