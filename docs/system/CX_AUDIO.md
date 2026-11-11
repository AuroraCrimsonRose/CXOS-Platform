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
7. **Codecs are our own** (Aurora, 2026-11-11, replacing "ported"):
   royalty-free formats only, every decoder written here, each in its own
   process with no grants, as the roadmap's Phase B media plan says. Opus,
   FLAC and Vorbis first; MP3 (its patents have expired); AAC and H.264 are
   out, not deferred.
8. **A graph inside, a small API outside.** The server routes, mixes and
   applies effects as a graph (item 1, the patchbay below); apps see the
   media plan's small calls: play, record with consent, a stream at a
   latency.

### 2a. Where CXOS aims to be better (Aurora, 2026-11-11)

1. **No glitches under load.** The server's work each period is real-time
   with a guaranteed budget; every underrun is charged to the stream that
   missed its deadline and shown in the Process Manager.
2. **Per-app volume and routing:** any app to any device, remembered per
   app, with rules ("headphones plugged in: move these apps").
3. **Routing built in:** virtual devices and a patchbay, so sending one
   app's sound elsewhere needs no third-party driver (VoiceMeeter,
   BlackHole, Loopback elsewhere). Recording another app needs its grant
   and the consent screen (item 4).
4. **Pro latency without a second stack** (no ASIO beside it, no JACK):
   latency per stream (item 2) lets a DAW and a browser share the system.
5. **Bit-perfect without locking the device:** untouched passthrough when
   one stream matches the device's rate and needs no processing; mixing
   resumes when a second sound starts.
6. **Hearing safety and loudness:** optional system-wide levelling to EBU
   R128, new streams faded in, a limiter, a headphone exposure meter.
7. **Restarts lose nothing:** if the server or a driver restarts, clients
   keep their streams and reconnect without noticing.
8. **One clock for sound and picture:** audio timestamps and the
   compositor's frame clock (roadmap 2.8e) share a time base, so video is in
   lip sync; a device's own latency (Bluetooth later) is reported into it.
9. **Accessibility:** mono downmix, balance, alert sounds also shown as a
   screen flash.
10. **Measured in the tests:** a loopback virtual device measures round-trip
    latency and counts underruns in the verification boots, beside item 6's
    bit-exact references, and both are sabotaged.

## 3. Dependencies and order

Needs shared memory, notifications, priority classes and the driver-host
primitives. Then: the graph and server on a virtual device; HDA under QEMU;
virtio-sound; the rig's codec; microphone with its capability; codecs.
