# The VR shell: a second shell around the same session

Step 5a of the order of work (SYSTEMS.md M2, the boundary proof of the game
layer). Decided and built on 2026-10-17 as a spike: it must show that the
session of ADR 0002 runs under a second front end without the game layer
learning about it, not be a finished VR game.

## Context

ADR 0002 says a shell hands `GameSession` a viewpoint (Camera3D) and a
player object, and ADR 0003 says a VR shell sets the viewpoint to its hand.
Until now only the viewer existed, so the claim was untested. The widgets
(HUD, dialogue, subtitles) were `CanvasLayer`s that drew straight into the
window, which a headset cannot show.

## Decision

- **A shell of its own**: `game/vr/vr_shell.tscn` and `vr_shell.gd`. It
  opens the session the way the viewer does, with a `VrRig` as the player
  object and the `XRCamera3D` as viewpoint, calls `step` every frame and
  listens to the same signals. OpenXR stays off in `project.godot`
  (`xr/openxr/enabled=false`), so the viewer never touches XR; the VR shell
  is started with Godot's `--xr-mode on` and its own main scene.
- **Fallback, never a crash**: when no OpenXR session starts (no runtime, or
  `--xr-mode` off) the shell prints `vr: no OpenXR runtime, desktop fallback`
  and runs with the `XRCamera3D` as a plain camera and the viewer's keyboard
  and mouse. A test (`vr_shell_fallback`, without the flag; see Findings) holds that.
- **Rig** (`vr_rig.gd`, a `PlayerRig` subclass): an `XROrigin3D` at the
  `SkydotPlayer`'s feet with the camera and two controllers (default OpenXR
  action map). The headset moves the camera; `yaw` and `pitch` of the player
  object are where the head looks, so saves and `place_at` keep their
  meaning. Left stick moves relative to the head (click: sprint), right
  stick x snap-turns 30 degrees about the head, the right trigger activates
  along the right controller's aim ray, A jumps and accepts, B cancels, the
  left menu button asks for the inventory (logged: there is none yet).
  Controller events become `InputEventAction`s on the InputMap actions that
  `PlayerInput` and the menus already read; `PlayerInput` is reused as is, so
  the desktop fallback plays like the viewer.
- **Pointer**: `GameSession.set_pointer(node)` makes activation and the
  prompt use that node's -Z axis instead of the viewpoint's. The viewer never
  calls it. This is the only change in `game/game/` (with the two places that
  take the node as a parameter: `ScriptBridge.activate_in_view` and
  `ActivationPrompt.target`).
- **Views and hosts**: the HUD, dialogue menu and subtitles are split into a
  view `Control` (`game/ui/*_view.gd`: drawing and, for the dialogue, menu
  input through `ui_*` actions and `activate`) and a host. The viewer's hosts
  (`viewer/hud.gd`, `dialogue_menu.gd`, `subtitles.gd`) stay `CanvasLayer`s
  with the same constructors and look. The VR host, `vr_panel.gd`, puts a view
  in a `SubViewport` on an unshaded quad without depth test: the HUD and
  subtitles are locked to the head under the view, the dialogue menu is put
  1.3 m in front of the head when it opens and stays there.
- **Options**: the VR shell reuses `ViewerSettings` (one parse, one
  `game_config()`), so every game option works in both shells; the viewer-only
  ones (shots, benchmark, debug) are ignored. Entering the place is a short
  copy of the viewer's `_start_game` without shots and benchmark; folding the
  two into one `GameStart` helper is open.

## What moved and why

- `viewer/hud.gd`, `subtitles.gd`, `dialogue_menu.gd` -> `ui/*_view.gd`:
  two hosts need the same drawing; nothing else changed, and a viewer shot
  of the dialogue matches the old one apart from the fire's flicker.
  `DialogueView` no longer knows `PlayerInput`: it emits `engaged(open)` (the
  host blocks the player) and asks a callable whether it listens.
- The viewer's image-space sync -> `viewer/image_space_sync.gd`, used by
  both shells.
- `PlayerRig._init` takes the camera to use.

## Findings

- With `--xr-mode on` and no runtime, Godot itself opens a modal `zenity`
  alert ("OpenXR was requested but failed to start") and waits for a click,
  before any script runs; unsetting `DISPLAY` did not stop it (GTK finds the
  Wayland socket). The test therefore runs without the flag: the OpenXR
  interface then cannot initialise, which is the same branch of the shell.
  Starting with the flag and no runtime needs a launcher that checks for a
  runtime first (open). On this machine the default runtime is SteamVR.
- A lambda connected to an `OpenXRInterface` signal and left connected when
  the shell is freed corrupts the heap at exit (abort with "corrupted size
  vs. prev_size"). The shell connects methods and disconnects them in
  `_exit_tree`.
- Monado with only the qwerty builder (`P_OVERRIDE_ACTIVE_CONFIG=qwerty`
  `QWERTY_ENABLE=1`, started with stdin on a pipe, not `/dev/null`) gives a
  session that reaches begun, visible and focussed. Neither window of a
  running session captures (black), so the picture was not inspected.
- The testpack interior has no floor at its spawn; the flat viewer also
  prints "fell through the world" there once frames render.

## Considered options

- **Move the game options into a shared parser**: more churn in the viewer
  for no gain yet; `ViewerSettings` has no viewer-only dependencies.
- **The VR shell drawing 3D widgets**: a view that draws 2D into a texture
  gives text, bars and menus for free and keeps one drawing code.
- **A `CanvasLayer` host with `XRInterface` compositor layers**
  (`OpenXRCompositionLayer`): sharper text and no render cost in the scene,
  but it needs runtime support and a different host per runtime; the quad
  is the baseline to compare with.
- **Own movement from tracked poses** (walking in the room moves the
  player): later; the head only moves the camera against the origin now.

## Open

- A laser pointer and clicking in menus (the right controller's ray against
  the panel), and a visible activation ray.
- Comfort options: vignette on movement, smooth turn, seated mode, height
  calibration, the image-space grade is applied in the headset as in the
  viewer and its eye adaptation is untested there.
- Steam Frame specifics: the standalone target (Android-like constraints:
  foveation, mobile renderer budget, controller profile), hand tracking and
  eye tracking, and whether a Steam Frame build runs the same GDScript shell.
- Room-scale: the player's capsule does not follow the head, so walking
  physically into a wall is possible.
- The panels are drawn before the compositor effect, so the image space also
  grades them; check in a headset.
