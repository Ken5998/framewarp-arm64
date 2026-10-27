# framewarp-arm64

A Vulkan layer that shows extra frames between the frames a game renders,
built for ARM64 Linux handhelds such as the Retroid Pocket 6 (Snapdragon 8
Gen 2, Adreno 740) with ArmadaOS. The first target is
[Azimuth](https://almost-there-games.itch.io/azimuth) (Forza Horizon 2), which
is GPU-bound at 20-24 fps there.

## Why not interpolation

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

### Camera mode

Where the motion per block goes wrong, when the camera turns, on tyre smoke,
or near cars that overtake, the generated frames show it as blocks.
`FRAMEWARP_MODE=camera` trades that for one smooth motion of the whole
picture, like the reprojection of VR headsets: after the block search it
fits a single pan, zoom and roll to all the blocks, letting the blocks that
disagree with it (the player's car, the HUD, smoke, other cars) count less
and less over a few rounds, and keeps whichever of two starting points more
blocks agree with. Then every 4x4 pixels chooses between that motion and no
motion at all, so the player's car and the HUD stay where they are while the
road and the scenery move with the camera. There are no blocks, but anything
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

## Settings

| Variable | Default | Meaning |
| --- | --- | --- |
| `FRAMEWARP` | off | `1` turns the layer on for this game |
| `FRAMEWARP_MULT` | `2` | frames shown per real frame, 1 to 4 (60 Hz panel: 3 for a 20 fps game, 2 for 30 fps), or `auto` |
| `FRAMEWARP_MAX_MULT` | `3` | with `auto`, the most frames shown per real frame |
| `FRAMEWARP_REFRESH` | `60` | the panel's refresh rate, for `auto` |
| `FRAMEWARP_FORCE_FIFO` | `1` | forces FIFO presents, so every frame gets its own refresh |
| `FRAMEWARP_MODE` | `warp` | `warp`; `camera` moves the whole picture with one motion (see above); `camera-debug` does the same and colours the generated frames: blue where it keeps the picture still, yellow where the camera moves it less than 2 pixels per real frame; `copy` repeats the last real frame (no motion); `debug` warps and colours the motion it found |
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
  camera per real frame: pan 4.2 px (max 6.2), zoom +0.11%, roll +1.46 deg | blocks that move 67%, that agree with the camera 52%
```

## Build

```
scripts/build.sh out
```

It needs CMake, a C++17 compiler, the Vulkan headers and loader, and
`glslangValidator`. `build/motion_test` runs the motion shaders on a pattern
moved by known amounts, everywhere or in one half only, and checks the motion
they find per block and per pixel; it also zooms the pattern and keeps a still
strip at the bottom, like a HUD, and checks the camera motion and which pixels
camera mode keeps still. CI builds the aarch64
package in Fedora 44, the base of ArmadaOS, runs that test and runs vkcube
through the layer with the validation layer below it, on Mesa's software
driver.

## License

MIT.
