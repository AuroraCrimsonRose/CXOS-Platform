# Audio
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN.** A clean slate: the inherited system has only the PC
> speaker (`kernel/platform/pc/speaker.c`).

---

## 1. How the others do it

- **NT5** mixed in the kernel (`drivers/wdm/audio/filters/kmixer`) on the
  kernel-streaming graph (`drivers/ksfilter/ks`). Vista moved mixing to a user
  process after glitch and security problems.
- **Serenity:** kernel AC97 and Intel HDA drivers (`Kernel/Devices/Audio`);
  AudioServer mixes on its own thread (`Mixer.cpp`); each client shares a ring
  buffer (`AudioServer.ipc`, `set_buffer`).
- **Haiku:** media_server with a node graph (`NodeManager.cpp`) and shared
  buffers (`BufferManager.cpp`); drivers for HDA, AC97, USB, virtio, null.
- **macOS** (public): Core Audio in user space, the device clock driving a
  timed cycle over shared rings; very low latency.
- **PipeWire:** one graph for audio and video, shared memory, real-time
  scheduling.

## 2. Design

1. **A graph engine.** Apps, effects and devices are nodes; links carry
   buffers in shared memory objects. The same engine handles video later.
2. **The hardware clock drives timing.** The server wakes each period at
   real-time priority and asks each app to fill N frames for time T through a
   notification. Latency is chosen per stream: small for games and production,
   large for playback to save power.
3. **The server does format work:** 32-bit float internally, resampling,
   mixing, per-app volume, and switching devices mid-stream without the app.
4. **Privacy:** the microphone needs a capability, and the trusted part of the
   screen shows an indicator while it is in use. Recording another app's audio
   needs its own grant.
5. **User-space drivers** (`kernel/CX_DRIVERS.md`): HDA (QEMU's `intel-hda`,
   and the rig's Realtek codec) and virtio-sound. The DMA ring is shared with
   the server, so samples are not copied.
6. **Testing:** null and virtual devices in CI; mixed output compared bit for
   bit against saved references.
7. **Codecs are ported:** Opus, FLAC, Vorbis first; MP3's patents have
   expired; AAC and H.264 deferred.

## 3. Dependencies and order

Needs shared memory, notifications, priority classes and the driver-host
primitives. Then: the graph and server on a virtual device; HDA under QEMU;
virtio-sound; the rig's codec; microphone with its capability; codecs.
