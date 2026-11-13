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
contrast is a third token set (2.8a5). The theme follows the system
setting and can be forced per ui (the gallery does).

## 9. The widget set and the sub-steps

| Step | What | Witness |
|---|---|---|
| **2.8a1** | the core (tree, layout, scale, damage, loop, focus, theme), and label, button, check box, radio, switch, slider, progress bar; gallery v1 | headless self-tests in `xtest` (layout at three scales, damage, focus order, activation by pointer and key) and the gallery under displayd; `2.8a1.sab` |
| **2.8a2** | key events with codes and modifiers (ABI); commands and keybinds (registered, clashes refused, the three fixed keys); text field with selection, the clipboard (read only on a paste), the input-method interface with a dead-key method | key and clash tests; composition test |
| **2.8a3** | scroll view, list, tree, table, tabs, menu and context menu, tooltip, the dialog with standard buttons, the text grid; the server's timed wait | headless tests of each, the gallery's pages replayed; `2.8a3.sab` |
| **2.8a4** | localisation: message keys, `cxk` string tables (a Fluent subset), CLDR plural rules, the pseudo-locale, our UAX #9 run against Unicode's conformance files, mirrored layouts | pseudo-locale gallery boot |
| **2.8a5** | the inspector and the automation API over the tree; boot checks (an interactive widget with no name, one focus cannot reach, clipped text, two commands on one key); high contrast; `gui.xfxn` retired | each check sabotaged |

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

The gallery (`os/apps/gallery.xfxn`) shows every widget in every state
(normal, hovered, pressed, focused, disabled, checked, mixed), light and
dark, at 100, 150 and 200 %; each step adds its widgets to it (Serenity's
WidgetGallery and GTK's widget-factory are the precedent). Fonts are the
v1 bitmap XFNT until 2.8a′, drawn at an integer multiple for the larger
scales.
