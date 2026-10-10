# Fork features and fixes

This guide catalogs [kyrie25/linux-wallpaperengine](https://github.com/kyrie25/linux-wallpaperengine),
branch `ii-puppet-multimonitor`, compared with
[Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine),
branch `main`. The comparison was checked on 2026-10-10 against upstream
`b016d7d1fdcf4e5fd2f9c9fa420a8aaa07fee02d`, with fork implementation through
`e93bb91c18bbadf4d45a9f9c35a07b775ab03ee2`.

The catalog includes inherited fork work, locally developed repairs, and adapted
ports absent from that baseline. Upstream already supports scene/video/web
wallpapers, output selection/spanning, MPRIS, and QuickJS scripting; those broad
capabilities are not new here. This fork extends their compatibility and connects
them to a Wayland Quickshell setup. Windows Wallpaper Engine is still required
for its assets, and compatibility remains partial.

## Output layout and live controls

- Horizontal and vertical crop alignment can be configured per output or span
  group with `--align-x left|center|right` and `--align-y top|center|bottom`.
  Alignment follows the selected scaling mode without squashing the scene into
  a different aspect ratio when using fit/fill.
- `--anti-aliasing 0|2|4|8` exposes the multisample setting.
- `--control-file <path>` polls JSON for live FPS, volume, scaling, and alignment.
  FPS is clamped to 1-240, volume to 0-128, and alignment to two normalized values
  from 0 to 1. Invalid/unsupported live settings are rejected. A separate process
  and file per output allow independent updates without rebuilding the scene.
- Native scene and web audio can change volume live. CEF volume targets
  PulseAudio streams owned by the renderer session, including media elements and
  Web Audio, rather than changing unrelated applications' audio.
- Screenshot delay accepts the longer range needed for rendering checks, and
  frame diagnostics advance only after a successful EGL swap, allowing stable
  comparisons at actual rendered frames.

Example live-control payload:

```json
{"fps":30,"volume":0,"scaling":"fill","alignment":[0.5,0.5]}
```

The [dots-hyprland integration](https://github.com/kyrie25/dots-hyprland/blob/main/docs/fork-features.md)
owns wallpaper selection, monitor-local fullscreen/tiled rules, global
other-application audio rules, startup grace, individual mute, supervision,
and atomic control-file replacement. Those policies belong to the shell, not
to a KDE plugin or an embedded Quickshell runtime inside this renderer.

## Puppet animation and composition

- Puppet warp adds bounded MDLV mesh, MDLS skeleton, and MDLA animation parsing,
  CPU skinning, four bone influences, animation-layer selection/rates/visibility,
  and multi-animation handling. Older world-anchored rigs and local rigs have
  separate transform handling.
- Autosize considers animated bounds so limbs do not disappear outside the
  canvas. Bone rotations respect the authored coordinate convention; flattening
  the presented depth avoids unwanted orthographic clipping. Editor crop offsets
  are not applied as a second runtime displacement.
- The generic rig adds animation-layer blending, transitions, physics, IK,
  root-motion handling, and script playback controls.
- Authored image effects run before the final puppet deformation/presentation.
  Padded source UVs and already-cropped intermediate UVs are treated separately;
  image tint/alpha is applied once, preserving effects and transparency.
- MDMP0001 position morphs include axis/radial bone modifiers, morph alpha, and
  per-bone alpha. Position-only targets retain displacement; modifier matrices
  are shared within each target/frame instead of recomputed per vertex.
- Signed/integer puppet part ordering, baked-frame transitions, and hidden-track
  restoration preserve authored layer composition.
- MDLV23 clipping supports normal, inverted, soft, additive, AtTargets, and
  nested masks. Reordered ancestors resolve after building the draw list, with
  shared scene mask buffers rather than accidentally selecting draw zero.
- An optional second puppet mesh supplies authored texture/blend-map channels,
  including float4 UVs, multiple blend rows, straight-alpha coverage, direct,
  offscreen, and double-buffered composition. This does not implement arbitrary
  additional visible meshes.
- Optional invalid rig, clipping, or channel data falls back to the available
  static/ordinary mesh rather than losing an otherwise usable image. Mesh
  section framing respects declared layouts and does not treat `MDLS` bytes
  inside material names as skeleton markers.

## Scene transforms, text, and drift repairs

- Images and text share parent-chain transforms. Scripted vectors preserve their
  type and ownership; script return values are detached from retained initial
  values, avoiding repeated mutation of a script's transform baseline.
- Image, particle, and text parallax honors authored zero, signed, and per-axis
  depths instead of substituting a default. Wayland pointer coordinates stay
  local to their output, including simultaneous monitors.
- Text follows its image parent's parallax with consistent anchoring and Y-axis
  mapping. The fixes address transform/script/parallax causes of drift rather
  than relying on a fixed text offset to compensate for it.
- Text adds shaped layout through HarfBuzz, MSDF glyph rendering, color emoji,
  alignment, padding, sizing, font/layout rules, and effect support within the
  existing glyph pipeline. Flexible text property/default parsing avoids
  rejecting otherwise valid scenes.
- Image/text scripts are registered as native QuickJS property scripts, including
  group/image ownership and initialization after scene construction. This
  replaces the earlier limited text-script shim and restores dynamic clock/media
  text and property-driven placement.
- Effect conditions and live shader constants control actual render passes;
  ambient/skylight, brightness, color, and alpha follow current values. Blend-aware
  depth writes and scene-target alpha preserve composition, with an opaque scene
  root and transparent intermediate targets.

## Native SceneScript extensions

- Typed Vec2/Vec3/Vec4 adapters, vector subtraction, typed property writes,
  defaults, imports, scene settings, and deferred initialization connect native
  image/text modules to their layers.
- Timers preserve their module receiver and `thisLayer`, support cancellation
  and self-cancellation, and defer newly scheduled nested callbacks to a later
  frame. Cancellation affects the requested timer, fixing a defect in the
  reference implementation.
- `registerAudioBuffers()` supports module-scope registration and 16/32/64-bin
  Float32Array stereo/combined data with detached backing arrays. Listener
  ownership prevents stale callback registrations.
- Local storage persists script data. Layer APIs support creation, lookup,
  enumeration, ordering, detached initial configuration, deferred parent/child
  destruction, and cleanup after failed construction.
- Media callback dispatch snapshots recipients. Layers created by a callback
  receive their startup event without recursively extending the same dispatch
  or replaying initialization to old modules.
- Material and effect owners expose shader-typed constants, live uniform updates,
  and effect visibility; puppet handles expose animation controls.
- Per-image atlas handles support play, pause, stop, seek, playback rate, and
  timeline joining without altering other images' default encoded timeline.
- Partial camera eye/center/up/zoom transforms and rectangular 2D cursor events
  include drag capture, propagation blocking, and callback owner restoration.
  These are partial APIs, not a complete 3D camera or geometry hit-test engine.

## Media metadata and album artwork

- MPRIS selection favors the active playing session and responds to player
  appearance/disappearance and metadata replacement, rather than retaining a
  stale player or old fields. Media widget text/artwork updates together.
- Album art supports local files, HTTP downloads, and WebP decoding. Clearing,
  replacement, unavailable art, and recovery reset the appropriate state without
  losing the previous cover before scripts can use it.
- A player starting after wallpaper initialization can populate the previously
  unavailable media texture; its fallback no longer makes that texture
  permanently unselectable.
- Artwork/image uploads retain GL pixel-unpack state and do not corrupt unrelated
  uploads. Authored image bounds are preserved when the replacement texture has
  different dimensions, preventing unintended shrinking/borders in media cards.

## Custom images and animated textures

- Bundled default image paths and user-selected external image properties reach
  the actual material texture. Passthrough/property-backed texture handling no
  longer leaves supported custom images blank.
- External image decoding supports static images, GIF disposal/frame delays,
  transparency, and JPEG EXIF orientation. Packed textures retain authored
  orientation, dimensions, and UVs; EXIF rotation applies only to external images.
- GIF/video replacements retain authored layer bounds instead of replacing the
  layer's canvas dimensions with the source media size.
- Embedded video advances by encoded timestamps rather than render/update count,
  fixing custom videos playing too fast. Default encoded timelines stay shared
  unless a script explicitly creates independent atlas playback.
- Memory-backed video retains timed software decoding on NVIDIA. Texture metadata
  bitmasks are parsed without treating combined flags as unrelated formats.

## Sound and audio response

- Authored `startSilent`, looping, scripted volume, and play/stop controls are
  respected. Sound objects intended to start silent no longer all play at once,
  preventing overlapping songs in wallpapers that select tracks through scripts.
- Stereo spectrum analysis and processing provide native-style channel data and
  average reduction for combined spectra, with owned listener lifetimes.
- Mono-to-stereo resampling reserves the complete output allocation, fixing the
  adapted pipeline's memory-sizing defect while retaining existing audio policy
  and stream ownership.

## Parsing, Wayland, web lifecycle, and development

- Authoring JSON permits comments and trailing commas. Flexible vectors,
  optional orthographic camera settings, text padding/defaults, and wrapped user
  properties (including parallax depth) accept additional valid authored scenes.
  Memory streams enforce bounded seeks and puppet sections enforce their bounds.
- Wayland output handling responds to lifecycle changes. Bounded event dispatch
  and frame pacing avoid unbounded waiting in the render loop; reconnect testing
  is separate from compositor-specific physical hotplug guarantees.
- CEF starts with the actual wallpaper viewport, and callback detachment/browser
  shutdown preserves ownership rather than leaving callbacks to destroyed data.
- The build adds curl, HarfBuzz, and pinned msdfgen core support. msdfgen's MIT
  license is retained in installation. A Nix development flake supplies the
  original developer environment; its historical dependency list is not a
  verified substitute for the current Arch package build.
- New unit/fixture coverage exercises the authored data, media uploads, text,
  spectra, GIF/orientation, live controls, pacing, and puppet/script contracts.

## Verification and limits

The published Release implementation passed **1,551 assertions in 99 cases**.
Run the renderer's tests from a configured `BUILD_TESTING=ON` build using
`output/tests`. The dotfiles repository contains isolated real Wayland tests for
two outputs, live controls, native scripts/materials/layers, scene/web audio,
custom media, and artwork. Puppet checks include 48 texture-channel captures,
34 morph/alpha/order cases, 44 clipping cases, and 20 mesh-layout cases.

KAngel widget/title placement remained stable at successful rendered frames 5
and 900; early/late/replaced/cleared/recovered/local/WebP/HTTP artwork was checked.
Earlier diagnostic counters measured loop iterations; current verification uses
successful swaps. See [port-adaptations.md](port-adaptations.md) for detailed
checks, adaptation decisions, and source pins.

Compatibility remains incomplete: ordinary additional visible meshes,
normal/tangent morph rendering, full 3D/lighting/camera paths, broad particle
additions, and HDR are not implemented by this adaptation. Native SceneScript
and text effects remain partial; cursor picking uses rectangles. Three isolated
Hyprland virtual-output reconnect cycles passed, but physical hotplug and every
hidden-compositor case remain unverified. KAngel has open/closed eye states in
Linux captures, but its Windows blink cadence and hair motion are not established
as matching. No universal Workshop or Windows parity is claimed.

## Attribution and related documentation

- Almamu and upstream contributors provide the base renderer. The initial puppet
  warp implementation by Matthew Snyder is retained in this fork's history.
- Generic code was adapted from
  [WallpaperEngineLover/linux-wallpaperengine-kde](https://github.com/WallpaperEngineLover/linux-wallpaperengine-kde/tree/9f03e93cd0dac98f1a6bd3433146f2a7097e9eff)
  and [sgtaziz/linux-wallpaperengine](https://github.com/sgtaziz/linux-wallpaperengine/tree/ebe278ffcb84af0d37d94c8883a04d1315f0eb54).
  Both are GPL-3.0; detailed component attribution is in
  [port-adaptations.md](port-adaptations.md).
- KDE/Plasma integration, X11/Xwayland port changes, and V8 migration were
  excluded. Adapted renderer features use native Wayland/DE-independent paths;
  the dotfiles Arch package disables X11 discovery.
- [dots-hyprland fork guide](https://github.com/kyrie25/dots-hyprland/blob/main/docs/fork-features.md)
  documents the settings UI, supervisor, dependency packaging, and desktop
  features independently of renderer internals.
- [OS Waves investigation](OS-Waves-rendering-issues.md) is a historical
  pre-fix report, not the current support matrix.
