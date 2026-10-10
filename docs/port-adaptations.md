# Wayland port adaptations

For the full upstream comparison, including fixes predating these ports, see
[Fork features and fixes](fork-features.md).

These changes adapt DE-independent code from the GPL-3.0 renderer forks below.
They retain this fork's Quickshell integration, per-output process ownership,
media-player selection, remote artwork, custom-image sizing, and timed software
decoding for memory-backed video on NVIDIA.

## Sources and attribution

- [WallpaperEngineLover/linux-wallpaperengine-kde](https://github.com/WallpaperEngineLover/linux-wallpaperengine-kde/tree/9f03e93cd0dac98f1a6bd3433146f2a7097e9eff):
  generic Wayland lifecycle, spectrum analysis, `PuppetRig`, `PuppetPhysics`,
  `PuppetIK`, `PuppetClipping`, `PuppetRootMotion`, `TextLayout`, `GifAnimation`,
  `ImageDecoder`, authoring JSON, and live-control design.
- [sgtaziz/linux-wallpaperengine](https://github.com/sgtaziz/linux-wallpaperengine/tree/ebe278ffcb84af0d37d94c8883a04d1315f0eb54):
  effect conditions, blend-aware depth writes, scene-target alpha preservation,
  texture metadata flags, vector subtraction, web lifetime/viewport fixes,
  `MediaArtwork`, pixel-unpack state preservation, `FramePacer`, bounded puppet
  mesh parsing, native SceneScript layer/camera/material/cursor/texture APIs,
  and timer/audio-buffer semantics.
- Adapted tests preserve their originating project's license. Rendering and
  supervisor integration tests also cover local regressions discovered during
  adaptation. Both source pins are comparison references, not dependencies.
- `src/External/msdfgen` is pinned at
  `6574da1310df433c97ca0fddcab7e463c31e58f8` (v1.12.1), by Viktor Chlumsky and
  contributors, under its MIT license. The installation retains its license.

KDE/Plasma interfaces, X11/Xwayland implementations, and the V8 runtime migration
are excluded. The Arch package explicitly disables X11 discovery.

## Regression checks

Configure `BUILD_TESTING=ON` and run the build's `output/tests` executable. The
suite includes real surfaceless EGL artwork uploads, authoring JSON, native
texture flags, vectors, effect conditions, text shaping, spectra, puppet layer
blending, frame pacing, web callback detachment, GIF timing/disposal, and JPEG
orientation. The evaluated Release build passes 1,557 assertions in 100 cases.

The dotfiles repository's `tests/wallpaperengine/` contains isolated supervisor
and real Wayland renderer tests. They require a running Wayland session; the
scene audio test captures SDL disk output without using speakers. The tests
create temporary wallpapers and do not change the user's wallpaper configuration.

The integration checks cover:

- Simultaneous output dimensions, native Vec2/3/4 ownership, independent live
  FPS, and pending settings applied after a stopped renderer resumes.
- Native module timers: callback ownership, namespace receiver and `thisLayer`,
  creation order, cancellation, self-cancellation, cancellation of another due
  timer, and nested callbacks deferred to the following frame. Cancellation
  targets the requested timer; the reference fork's first-timer cancellation
  defect is deliberately excluded.
- Native `registerAudioBuffers()` at module scope: 16/32/64-bin Float32Array
  channels, combined-spectrum average reduction, and detached backing arrays.
- Packed JPEG textures retaining authored orientation, with EXIF rotation
  applied only to external images. Rotating packed pixels would invalidate
  their authored dimensions and UVs.
- Declared MDLV vertex layouts and bounded section framing, including embedded
  `MDLS` bytes in material names. Padded puppet UVs scale only source textures;
  intermediate effects already crop the padding. Failed optional rigs retain
  a static mesh. The first mesh is rendered, with an optional second mesh for
  authored puppet texture channels.
- Bounded MDMP0001 position deltas, axis/radial bone modifiers, morph alpha,
  and per-bone alpha through a neutral final pass after authored effects.
  Position-only targets retain their displacement; modifiers affect displacement
  without changing the alpha weight, matching the native shader. Modifier
  matrices are inverted once per target/frame, shared by all vertices. The
  34 rendered morph/alpha/order cases also cover radial rules and image tint
  applied once; 44 clipping cases cover the masks and composition paths.
- Stable signed/integer puppet part order, baked-frame transitions and hidden
  track restoration. Nonzero rest-order keys follow the reference loader;
  rendered Windows parity for those keys is not established.
- MDLV23 normal/inverted/soft/additive/AtTargets and nested clipping, including
  reordered ancestors, source/target alpha, padding and effects. Parent draws
  resolve after the command list is built, avoiding the reference's accidental
  draw-zero fallback when a child precedes its parent. Mask buffers are shared
  within the scene. Invalid optional clipping records retain the ordinary mesh.
- The optional second puppet mesh uses float4 UVs, indexed blend-map channels
  and straight-alpha coverage. Its 48 captures cover direct/offscreen and
  double-buffered routes, multiple blend rows, padding, effects, asymmetric
  orientation and malformed optional data. Uniform uploads retain their row
  count; albedo copies use clip-space vertices and source-scaled UVs, while
  subsequent passes use the already-cropped intermediate texture coordinates.
- Encoded custom GIF/video timing and authored image bounds, opaque scene-root
  alpha with intermediate transparency, and CEF startup dimensions/shutdown.
- Native image/text property scripts with typed writes, deferred startup after
  scene construction, imports, script-property defaults, owned timers and audio
  buffers, local storage, and scene settings. Text uses the native engine.
- Puppet animation controls, material/effect owners and shader-typed constants,
  live effect visibility, and per-image atlas playback handles. Independent
  playback does not alter the default shared encoded timeline.
- Script-created layer initialization, ordering, detached initial configuration,
  failed-construction cleanup and deferred parent/child destruction. Startup
  media events address new modules only; event recipients are snapshotted so
  callbacks that create layers cannot recursively extend the same dispatch.
- Partial eye/center/up/zoom camera controls and rectangular 2D cursor events,
  including drag capture, propagation blocking and callback owner restoration.
- Early and late media-player startup, local/WebP/HTTP artwork, replacement,
  clearing, recovery, and zero/signed/per-axis parallax. Full-scene captures at
  frame counters 5 and 900 retain the authored widget/title placement. Historical
  checks before the frame-counter correction counted event-loop iterations;
  current counters advance only after successful EGL buffer swaps.
- Isolated live and legacy supervisors: monitor-local tiled/fullscreen policy,
  global other-app audio policy, individual mute, startup grace, process-session
  cleanup, crash recovery, and simulated output reconnect.

The supervisor leaves EGL vendor selection to GLVND or an explicit caller
override. Compositor captures on the hybrid NVIDIA/Intel setup showed black
output with forced Mesa for both the pre-port and current renderer; automatic
selection and explicit NVIDIA displayed correctly with both builds. Earlier
Intel checks verified internal rendering but did not establish compositor
presentation. NVIDIA startup under VRAM pressure also failed with the previous
staged build and is not established as a new renderer regression.

Keep the existing renderer installed while evaluating a new build. A successful
unit suite does not establish Windows parity for every workshop wallpaper.

## Current boundaries

The generic rig handles layer blending, physics and IK. The first rendered mesh
supports MDMP0001 position morphs, clipping, alpha and draw order. An optional
second mesh supports puppet texture-channel composition, including direct and
offscreen albedo routes. Ordinary additional visible meshes, normal/tangent
morph rendering, full 3D, lighting and camera-path parity remain incomplete.
Native SceneScript coverage remains partial; native text effects are limited to
the existing glyph pipeline, and cursor picking uses 2D rectangles rather than
alpha masks, puppet geometry or 3D hitboxes. Camera controls are partial transforms,
not a complete 3D camera subsystem. CEF live volume updates PulseAudio streams
owned by the renderer session, covering media elements and Web Audio output
while retaining mute and shutdown behavior.
Three isolated Hyprland virtual-output disconnect/reconnect cycles passed; this
does not establish physical output hotplug or every compositor-hidden pacing
case. These checks do not establish universal Windows parity.

The user's Windows recording `nuShNyZ07g.mp4` is an 8.271-second reference at
2558x1440/60fps. KAngel's model has one mesh, no MDMP section and no bone-alpha
tracks; its blink is authored through the masked shake effect (id949), with
bounds .995/.998. Actual Linux captures show both closed and open eye states
while widget/title placement stays fixed at frames 5/900. This does not establish
matching blink cadence or hair movement across platforms.
