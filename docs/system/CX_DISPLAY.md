# Display
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN.** The display server, compositor, animations, trusted UI
> and the path to Vulkan. Compared against NT5, Haiku, Serenity, Redox and
> macOS.

---

## 1. Inherited

- **The kernel draws.** `FB_OP_*` syscalls fill rectangles, draw lines and draw
  text in the kernel, straight onto the screen; no back buffer
  (`kernel/drivers/video/fb.c`).
- **The resolution is fixed at boot** by the firmware (VBE or GOP).
- **The window code runs inside one program.** `os/std/gui.xfxn` has a layer
  compositor with damage rectangles, used by whichever program holds
  `GRANT_FRAMEBUFFER`; two such programs draw over each other.
- **Any program can read the keyboard and mouse.** `SYS_INPUT_READ` and
  `SYS_MOUSE_READ` need no capability. Fixed early, before the display server
  (roadmap Phase 0).

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

## 7. Input and trusted UI

- Input goes only to the focused window. Global hotkeys need a capability.
- A key combination no app can intercept opens a screen only the system draws;
  consent prompts use it and nothing can draw over it.
- Screen capture, full screen and overlays each need a capability.

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
