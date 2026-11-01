# framewarp-arm64

A Vulkan layer that shows extra frames between the frames a game renders,
built for ARM64 Linux handhelds such as the Retroid Pocket 6 (Snapdragon 8
Gen 2, Adreno 740) with ArmadaOS. The first target is
[Azimuth](https://almost-there-games.itch.io/azimuth) (Forza Horizon 2), which
is GPU-bound at 20-24 fps there.

It makes the extra frames in one of several modes, which can be switched
while the game runs: from its own block motion (`warp`, `camera`), or from the
optical flow of [win-fg](https://github.com/The412Banner/win-fg), the frame
generation DroidDeck ships as DD-FG, either pushed forward (`flow`) or
interpolated like LSFG (`interp`).

## Extrapolation or interpolation

Frame generators such as LSFG, FSR 3 and DLSS-G interpolate: they hold real
frame N until frame N+1 is done and show a frame in between. At 20 fps that
costs at least one real frame, 50 ms, on top of the game's own latency, which
is too much for a racing game.

framewarp extrapolates instead: the real frame goes to the screen as soon as
it is done, and the extra frames that follow it are made from the frames the
game has already rendered. Nothing waits for a future frame, so the layer adds
no frame of latency, only the wait for the next refresh that v-sync has. The
generated frames are queued on the GPU right behind the real frame, before the
game's next frame, so they don't wait for it.

Interpolation still looks better wherever something moves on its own or
appears from behind something else, because it knows where things end up. So
`interp` is there too, for games where one more real frame of latency is
acceptable: at 40-50 real fps it is 20-25 ms, at 20 fps 50 ms.

## How it works

On every present of the game the layer:

1. copies the game's image into a history image;
2. presents the game's image, unchanged and without delay;
3. estimates the motion between this frame and the previous one: block
   matching on a 1/4 size luma copy, with a coarse search on a 1/16 size copy
   first (up to about 128 pixels per real frame), then a 3x3 median filter
   over the blocks (one per 32x32 pixels); then every 4x4 pixels pick, among
   the vectors of their block, its neighbours and zero, the one that matches
   them best, so the edge of the car and the road behind it each keep their
   own motion (and each pixel of the generated frame picks the best of the 4
   nearest of those);
4. acquires `FRAMEWARP_MULT - 1` more images from the same swapchain and
   draws in each the last real frame pushed forward along that motion, a
   third and two thirds of a real frame ahead with `FRAMEWARP_MULT=3`, then
   presents them. Pixels that did not change between the last two real
   frames (the HUD) stay where they are, and pixels whose source would be
   off the screen or on the HUD keep the real frame's pixel instead of
   smearing it.

The swapchain runs in FIFO mode, so each present shows for one refresh: on a
60 Hz panel `FRAMEWARP_MULT=3` shows 60 frames per second from 20 real ones
and holds the game at 20. `FRAMEWARP_MULT=auto` instead generates only as many
frames as the game needs: none where it reaches 60 by itself (intros, menus),
two where it runs at 20. Since the layer holds the game at the rate it picked,
it tries one generated frame less every 10 to 60 seconds and keeps that if the
game keeps up, so a failed try shows as a short stutter.

Because the motion comes from the picture only, the generated frames still
smear a little where something appears from behind something else, motion is
found to 4 pixels per real frame, and anything that changes speed shows it a
frame late.

### Optical flow modes: flow and interp

`FRAMEWARP_MODE=flow` and `FRAMEWARP_MODE=interp` use the optical flow and
frame synthesis of win-fg (MIT, in `third_party/win-fg`): a luma pyramid of
seven levels for the last two real frames, a block search on each level from
the coarsest down, with the camera motion taken out first and an
edge-preserving smoothing of the result, then one flow vector per pixel.

- `flow` pushes the last real frame forward along that flow, like `warp`:
  no added latency. Where the flow at the source doesn't explain the real
  frame from the one before, at the screen edges and on pixels that didn't
  change (the HUD), it keeps the real frame's pixel.
- `interp` holds the new real frame back and first shows frames between the
  previous real frame and it, a third and two thirds of the way with
  `FRAMEWARP_MULT=3`, warping both towards that time and blending them where
  the flow agrees, then the real frame. One real frame of latency.

The flow is solved once per real frame and each generated frame only runs
the synthesis. `FRAMEWARP_FLOW_PRESET` picks how fine the flow is solved (0:
1/2 size, 1: 1/4, the default, 2: 1/8) and `FRAMEWARP_FLOW_MODEL=4` searches
the flow in both directions and trusts it only where they agree (sharper on
complex motion, about twice the work). Upstream win-fg bound its two flow
images to the synthesis the wrong way round, so every warp sampled from the
wrong side; framewarp binds them the right way, and `tests/flow_test.cpp`
checks the result. On the Retroid Pocket 6 with Azimuth at 1280x720 the
flow costs about 1.5-5 ms and each generated frame 0.3-1.5 ms.

`FRAMEWARP_MODE=flow-debug` shows what the flow found instead of using it:
no generated frames, and each real frame in grey with the motion drawn in
colour, the hue for the direction and the strength for the size (full at 48
pixels per real frame), with a colour wheel at the top left as the key.
White means the flow hit its limit of a tenth of the screen; plain grey means
the pixel didn't change. If the colours follow the road and the cars, the
flow is right and the fault is in the synthesis; if they are noise, the
frames don't carry enough to solve it.

### Camera mode

Where the motion per block goes wrong, when the camera turns, on tyre smoke,
or near cars that overtake, the generated frames show it as blocks.
`FRAMEWARP_MODE=camera` trades that for one smooth motion of the whole
picture, like the reprojection of VR headsets: after the block search it
fits a single smooth motion to the blocks that move: pan, zoom and roll, and
perspective, since when driving forward the road close to the camera moves
much faster than the horizon. Blocks that disagree with it (smoke, other
cars) count less and less over a few rounds, and of two starting points it
keeps the one more blocks agree with. Then every 4x4 pixels chooses between
that motion and no motion at all, so the player's car and the HUD stay where
they are while the road and the scenery move with the camera. There are no blocks, but anything
that moves on its own, such as another car, moves with the camera in the
generated frames and catches up at the next real frame.

On the Retroid Pocket 6 in Azimuth (1920x1080 output, `FRAMEWARP_MULT=3`)
version 0.2.0 cost about 1.5 ms of GPU time per real frame in all (copy 0.2
ms, motion 0.9 ms, 0.2-0.3 ms per generated frame), kept a steady 60 frames
per second, and the controls answered as without the layer.

## Install on ArmadaOS

1. Download `framewarp-VERSION-aarch64.tar.gz` from the
   [releases](https://github.com/Ken5998/framewarp-arm64/releases) and extract
   it.
2. In Desktop Mode, run `framewarp/install.sh`. It installs the layer for your
   user only; nothing changes until a game starts with `FRAMEWARP=1`.
3. In Steam, put the variables in front of the game's launch options, for
   example `FRAMEWARP=1 FRAMEWARP_MULT=3 %command%`. Turn off LSFG-VK and the
   Quick Access Menu frame limit for that game.

`framewarp/install.sh --uninstall` removes it.

## Switching modes while playing

`framewarp-mode` (installed in `~/.local/bin`) writes the control file
`~/.config/framewarp/mode`, which the layer reads within a quarter of a
second:

```
framewarp-mode interp        # interpolation
framewarp-mode flow 3        # optical flow, pushed forward, 3 frames per real frame
framewarp-mode camera auto   # camera mode, frames as the game needs
framewarp-mode off           # only the real frames, to compare
framewarp-mode next          # interp -> flow -> camera -> off -> interp
```

Only changes made while the game runs apply, so a file left from another
game doesn't; each game starts in its `FRAMEWARP_MODE`. The number of frames
per real frame can't go above what the game started with (`FRAMEWARP_MULT`,
or `FRAMEWARP_MAX_MULT` with `auto`), since the swapchain only has room for
those. In Gaming Mode run it over SSH or from a terminal in Desktop Mode for
now.

## Settings

| Variable | Default | Meaning |
| --- | --- | --- |
| `FRAMEWARP` | off | `1` turns the layer on for this game |
| `FRAMEWARP_MULT` | `2` | frames shown per real frame, 1 to 4 (60 Hz panel: 3 for a 20 fps game, 2 for 30 fps), or `auto` |
| `FRAMEWARP_MAX_MULT` | `3` | with `auto`, the most frames shown per real frame |
| `FRAMEWARP_REFRESH` | `60` | the panel's refresh rate, for `auto` |
| `FRAMEWARP_FORCE_FIFO` | `1` | forces FIFO presents, so every frame gets its own refresh |
| `FRAMEWARP_MODE` | `warp` | `warp`; `camera` moves the whole picture with one motion (see above); `camera-debug` does the same and colours the generated frames: blue where it keeps the picture still, yellow where the camera moves it less than 2 pixels per real frame; `flow` pushes the frame along win-fg's optical flow; `interp` interpolates with it (one real frame of latency); `flow-debug` draws that flow over the real frames (see above); `copy` repeats the last real frame; `debug` overlays the block motion; `off` shows only the real frames |
| `FRAMEWARP_FLOW_PRESET` | `1` | `flow` and `interp`: flow solved at 1/2 (`0`), 1/4 (`1`) or 1/8 (`2`) size |
| `FRAMEWARP_FLOW_MODEL` | `3` | `flow` and `interp`: `4` searches the flow both ways and trusts it where they agree |
| `FRAMEWARP_CONTROL` | `~/.config/framewarp/mode` | the control file `framewarp-mode` writes |
| `FRAMEWARP_ZERO_BIAS` | `0.05` | 0 to 0.5, how much a block prefers "no motion" over a match that is only slightly better (steadier HUD) |
| `FRAMEWARP_TINT` | `0` | 0 to 1, tints the generated frames magenta to see them |
| `FRAMEWARP_LOG` | `~/framewarp.log` | where the measurements go |
| `FRAMEWARP_LOG_EVERY` | `2` | seconds between two log lines |

A log line looks like this:

```
real 19.9 fps (worst gap 53.6 ms), shown 59.8 fps, missed 0 | GPU copy 0.23 ms, motion 0.40 ms, generated frame 0.30 ms (max 0.40) | CPU in extra presents 17.60 ms (max 26.15)
```

`real` is what the game renders, `shown` what reaches the swapchain, `missed`
counts extra frames that could not be shown, and the GPU times are what the
layer costs per real frame (copy and motion) and per generated frame. With
`auto` the line ends with how many frames it generates per real frame. In
camera mode a second line shows the camera it found, on average per real
frame:

```
  camera per real frame: pan 4.2 px (max 6.2), zoom +0.11%, roll +1.46 deg | blocks that move 67%, of those that agree with the camera 77%
```

## Build

```
scripts/build.sh out
```

It needs CMake, a C++17 compiler, the Vulkan headers and loader, and
`glslangValidator`. `build/motion_test` runs the motion shaders on a pattern
moved by known amounts, everywhere or in one half only, and checks the motion
they find per block and per pixel; it also zooms the pattern and keeps a still
strip at the bottom, like a HUD, or adds perspective, and checks the camera
motion and which pixels camera mode keeps still. `build/flow_test` runs
win-fg's flow on such a pair and checks that the frames it interpolates and
extrapolates are much closer to the pattern moved by that much than a repeat
of the last frame, and that the still strip stays. CI builds the aarch64
package in Fedora 44, the base of ArmadaOS, runs both tests, runs vkcube
through the layer in every mode with the validation layer below it, on Mesa's
software driver, and runs it once more while `framewarp-mode` switches modes
and multipliers every few seconds.

## License

MIT. `third_party/win-fg` is win-fg by The412Banner, MIT, with an
adaptation of the AMD FidelityFX FSR3 optical flow, MIT; see its README for
what framewarp changed.
