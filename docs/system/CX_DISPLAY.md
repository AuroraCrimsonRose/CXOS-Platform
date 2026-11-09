# Display
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: the server, layers and springs, and the trusted UI are built (2.3 to 2.6).** The display server, compositor, animations, trusted UI
> and the path to Vulkan. Compared against NT5, Haiku, Serenity, Redox and
> macOS.

---

## 1. Inherited

- **The kernel draws.** The `SYS_FB_*` syscalls fill rectangles, draw lines and draw
  text in the kernel, straight onto the screen; no back buffer
  (`kernel/drivers/video/fb.c`).
- **The resolution is fixed at boot** by the firmware (VBE or GOP).
- **The window code runs inside one program.** `os/std/gui.xfxn` has a layer
  compositor with damage rectangles, used by whichever program holds
  `GRANT_FRAMEBUFFER`; two such programs draw over each other.
- **One grant decides who reads the keyboard and mouse.** `SYS_INPUT_READ`,
  `SYS_INPUT_POLL` and `SYS_MOUSE_READ` need `GRANT_INPUT` (roadmap 1.7a): the
  shell holds it and hands it to the program of a foreground `run` line that
  reads its keys; a background process never holds it. Until the display
  server routes input to the focused window, whoever holds it reads every key.

## 2. How the others do it

| | Who draws | Where the window system runs | Lesson |
|---|---|---|---|
| NT5 | The kernel: GDI (`windows/core/ntgdi/gre`), USER (`windows/core/ntuser/kernel`) | Kernel (win32k) | Moved into the kernel for speed; became one of the largest sources of Windows kernel vulnerabilities. Vista added a user-mode compositor. The mirror driver (`drivers/video/ms/mirror`) carried remote desktop. |
| Haiku | The server, from drawing commands | app_server | Drivers split: a tiny kernel part and an accelerant loaded into the server (`src/add-ons/accelerants`). Server-side drawing makes the server a choke point. |
| Serenity | The client, into a shared buffer | WindowServer | `set_window_backing_store` passes the buffer once; then only `invalidate_rect` / `did_finish_painting`. `Compositor::compose` redraws damage into a back buffer and flips. Solid and see-through regions tracked separately. Server-drawn frames. A virtual screen backend. |
| Redox | The client | Orbital, user space | No display code in the kernel beyond a debug console; buffers shared through `FMAP`. |
| macOS | The client, into IOSurfaces | WindowServer | Not in the XNU tree. Apps can send layer trees the compositor animates. Screen recording needs permission. |

All of them converged on: apps draw into their own shared buffers, a
user-space compositor owns the screen, the kernel only manages hardware.

## 3. The kernel's part

- The screen's memory becomes a memory-object handle given to one holder, the
  display server. The `FB_OP` drawing calls stay for early boot and recovery
  and go in `TRANSITIONAL_CODE.md`.
- Keyboard and mouse become handles held only by the display server.
- The kernel can always take the screen back to show a panic.

**Built (2.3):** `SYS_SCREEN_OPEN` gives the screen, as a `KMEM_SCREEN`
memory object mapped write-combining, to one holder at a time
(`kernel/drivers/video/fb.c`). While it is held the kernel's console draws
into its RAM copy only; when the object's last reference goes, the screen
comes back and is repainted from the copy. `panic_begin` takes it back for
good.

**Built (2.4):** `SYS_INPUT_OPEN` gives the keyboard and the pointer together
as one handle, to one holder at a time, and that handle cannot be passed on
(`kernel/drivers/input/inputdev.c`). While it is held every key and movement
goes to it and the old calls refuse with `E_BUSY`; closing it gives them
back. Movements with the same buttons merge into the latest unread one. Key
releases are not reported yet.

## 4. The display server

- **Apps draw their own windows** into two or three buffers shared as sealable
  memory objects. An app **commits** a buffer with its changed rectangles; the
  server owns it until it hands it back, and the app cannot write to it
  meanwhile. That prevents tearing and pixels changing as they are composed;
  Wayland relies on apps following that rule, here the kernel enforces it.
- **Compositing** redraws only damage into a back buffer in RAM, then copies
  those regions to the screen. Reading video memory is slow, so it never
  composes on the screen. The layer compositor in `gui.xfxn` is the core.
- **Frame pacing:** the server tells an app when to draw the next frame through
  the wait queue. A 60 Hz timer paces frames until a driver reports vertical
  blank.
- **Renderer interface** with two back ends, software and Vulkan. Effects and
  animations work on both; only quality differs.
- **Server-drawn window frames.** A frozen app's window can still be moved and
  closed; the server pings each app and greys a window that stops answering.
  The title shows the verified signer.
- **Apps survive a compositor crash.** They hold their buffers and layers as
  handles; a restarted compositor takes them back.

**Built (2.4):** `os/std/wm.xfxn` (windows, stack, frames, focus, routing;
no system calls but the buffers' own, so it can be tested alone),
`os/std/dserver.xfxn` (the loop and the protocol), `os/std/window.xfxn` (the
client's side), `os/apps/displayd.xfxn`, `os/apps/wdemo.xfxn`.

- *Commit, enforced.* A client has two buffers. `SYS_MEM_LEND` makes a
  **lease** on a buffer that has no writable mapping: while the lease exists
  nobody can map the buffer writable, its maker included. A commit unmaps,
  lends, and moves the lease to the server by IPC; the server checks it is on
  that buffer (`SYS_MEM_LEASE_CHECK`), and closing it at the next commit hands
  the buffer back. Unlike a seal it ends, and the lease cannot be copied.
  A lease is on anonymous memory only.
- *Connecting.* A client is started by the server (`run +screen +spawn
  +endpoint +disk displayd [client...]`) and finds a badged send handle in slot
  3. There is no name service yet to find a server by.
- *Frames* are the server's: a title bar that drags, a close box, a colour for
  focus. Ctrl+N and Ctrl+Q are the server's keys and no client sees them.
- *Not answering.* A client with an event unread and no call waiting for two
  seconds goes grey, can still be moved, and its close box ends it. Any other
  client's close box only asks.
- *Not built:* frame pacing beyond one frame event per commit, surviving a
  compositor crash, renderers other than software.

## 5. Transparency and effects

- Windows carry premultiplied alpha. Apps mark which regions are solid, so the
  compositor still skips hidden content under them.
- Frosted-glass blur is a compositor effect. **Effect tiers:** the software
  renderer does transparency, shadows and fades; the GPU renderer adds blur.
  Apps ask for an effect and it degrades cleanly.

## 6. Animation

- **Retained layers.** An app sends a tree of layers, each with a buffer,
  position, opacity, scale and transform, and animations on them. The
  compositor runs them every frame at its own priority, so a busy app still
  animates smoothly and scrolling needs no redraw.
- **Springs, retargetable mid-flight**, carrying velocity into the new target.
- **System animations** (open, close, minimise, workspace switch) in the
  server.
- **Reduce motion** enforced by the compositor for every app.
- X Visual's layout engine produces the layer tree.
- No damage means no composition: an idle screen costs no CPU.

**Built (2.5):** `os/std/scene.xfxn` holds the springs (Q16.16, semi-implicit
Euler at fixed 1 ms substeps, so the result depends on time and not on the
frame rate; a stall counts for 250 ms at most, and a spring still running at
10 s is put where it was going) and the retained tree (64 nodes, depth 4, 16
per client). `ds_layer` has a signed position, an opacity, a 2×2 matrix drawn
nearest-neighbour through its inverse, and a clip. A new target keeps the
velocity. A client asks through `win_layer_add`, `win_layer_set` and
`win_layer_free`; every number is checked in `wm_layer_set`, and a client's
layers are clipped to its own content box.

What input acts on changes at once; the springs only carry the picture. A
window's x, y and workspace are true from the moment they change, and a
closing window is a ghost: no client, no input, no focus, its buffers and
leases held until the fade ends, and reaped early when a slot or a node is
needed. With no spring active `wm_animate` does nothing at all.

Limits: a layer's buffer does not change (new pixels are a new layer); no
filtering; reduce motion is a flag (`displayd --reduce-motion`, Ctrl+R) and is
not saved anywhere until Settings exists; the USB keyboard is still polled, so
the server's loop wakes every 16 ms and composes nothing. Until the trusted UI
(step 6) the server holds Ctrl+N (next window, or back from minimised), Ctrl+B
(minimise), Ctrl+W (next workspace), Ctrl+E (send the window on) and Ctrl+R, as
it holds Ctrl+Q.

## 7. Input and trusted UI

- Input goes only to the focused window. Global hotkeys need a capability.
- A key combination no app can intercept opens a screen only the system draws;
  consent prompts use it and nothing can draw over it.
- Screen capture, full screen and overlays each need a capability.

**Built (2.4):** keys go to the focused window only; the pointer goes to the
window under it, or for as long as a button is down to the one the press
landed in. A press raises and focuses.

**Built (2.6):**

- *The combination.* Ctrl+Alt+Del is recognised in the kernel's two keyboard
  decoders (PS/2, USB), because above them a key is an ASCII-like code and the
  combination cannot be seen. It travels as `INPUT_EVENT_ATTENTION`, a flag
  beside the input ring and delivered ahead of it, so a keyboard flood or a
  server that has stopped reading cannot lose it. It goes only to the holder
  of the input handle, and is never a key.
- *The system's screen.* While it is up the compositor's stack holds that one
  surface and nothing else: no client layer exists to draw over it, keys and
  the pointer reach only the server, and a press in progress is ended first so
  no client is left holding a button. It lists the programs running; a number
  ends one, Esc goes back.
- *Consent.* `DS_OP_CONSENT` asks for one capability and is answered when the
  user has, on the same screen, naming the asker's signer. It does not take
  keys for its first 500 ms, counted from the first clock tick it is up for,
  so a key already on its way answers nothing. A refusal is kept for the life
  of the client, one ask per client is outstanding, and the rest wait in
  order. The combination does not take a question down. Waiting on the user is
  not counted as hanging.
- *Capabilities,* per client and asked for at the moment of use:
  `WM_CAP_CAPTURE` (`DS_OP_CAPTURE`, into the caller's memory, composing first
  so that stale pixels of the system's screen cannot be read, and refused while
  that screen is up) and `WM_CAP_OVERLAY` (`WM_F_OVERLAY`: above ordinary
  windows, never focused, never pressed, not in the Ctrl+N order). A red mark
  is on the screen for two seconds after any capture, and it is in the frame
  captured.
- *The signer,* from the kernel (`process_info.trust` and `.signer`, the first
  eight bytes of the SHA-256 of the key that actually verified the image), in
  the title bar before the client's own title and in its own colour:
  `[System]`, `[Publisher xxxxxxxx]`, `[Unknown xxxxxxxx]`, `[Unsigned]`. A
  long title cannot push it away. A `--dev` build reports what it found, not
  the platform trust it admits an unsigned image at.
- *Not built,* and why: global hotkeys and full screen, because neither feature
  exists to guard; publishers' names, because there are no publishers (the
  fingerprint stands in). Whoever holds the input handle receives the
  combination: that it is the real server rests on who is given `GRANT_INPUT`,
  which this step does not change.

## 8. Drivers and the GPU path

1. The firmware-set framebuffer (works on the rig today through the W5000's
   video BIOS).
2. **Bochs display** (QEMU std VGA): runtime resolution changes through I/O
   ports; Serenity has a reference.
3. **virtio-gpu 2D:** explicit flush, real frame timing.
4. **Venus over virtio-gpu:** Vulkan passed to the host GPU, so the guest
   driver mostly serialises calls. Mesa's Venus driver is MIT-licensed: a Port.
   Needs threads, futex, `mmap`, TLS and a C runtime.
5. **Native AMD** for the W5000 (GCN 1.0), much later.

**Vulkan is the only GPU API.** OpenGL, if needed, through Zink. Shaders from
the X Graphics Language compiled to SPIR-V. **Explicit sync from day one:**
fences are waitable handles and buffers move with explicit fences.

Drivers follow Haiku's split: a tiny kernel part, the rest in user space
(`kernel/CX_DRIVERS.md`).

## 9. Fixed now because they are painful later

Logical coordinates with a scale factor (Serenity `MultiScaleBitmaps`);
fractional scaling; one 32-bit pixel format with alpha in the protocol,
converted only when written to the screen; sRGB declared, leaving room for
HDR; monitor IDs in the protocol from the start; presentation timestamps;
accessibility structure in the protocol.

## 10. Testing

A virtual screen backend runs the server without hardware; frames compare
against saved reference images in the Linux CI.

**Built (2.3):** `ds_screen_virtual` in `os/std/display.xfxn` is that
backend, in both formats. The boot's X tests compose a fixed scene onto it
and check pixels worked out by hand, then the whole frame's FNV-1a
fingerprint (the "reference image"; a deliberate change prints the new one
to pin). The same test checks that only the damage is written.

## 11. Order

After kernel Phase 1 steps 1–3:

1. The screen as a memory object; input as handles; panic takeover.
2. Server with the software renderer, back buffer, damage, virtual screen.
3. Windows: commit and hand-back, frames, focus, input, transparency.
4. Layer trees, springs, system animations, reduce motion.
5. Trusted UI.
6. Bochs, then virtio-gpu 2D; monitors and scaling.
7. After x86-64: Venus and the Vulkan back end; blur.
8. Native AMD.
