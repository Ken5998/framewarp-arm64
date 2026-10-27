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
no frame of latency, only the wait for the next refresh that v-sync has. The generated frames are queued on the GPU right behind
the real frame, before the game's next frame, so they don't wait for it.

## How it works

On every present of the game the layer:

1. copies the game's image into a history image;
2. presents the game's image, unchanged and without delay;
3. estimates the motion between this frame and the previous one: block
   matching on a 1/4 size luma copy, with a coarse search on a 1/16 size copy
   first (up to about 80 pixels per real frame), then a 3x3 median filter
   over the blocks (one per 32x32 pixels);
4. acquires `FRAMEWARP_MULT - 1` more images from the same swapchain and
   draws in each the last real frame pushed forward along that motion, a
   third and two thirds of a real frame ahead with `FRAMEWARP_MULT=3`, then
   presents them.

The swapchain runs in FIFO mode, so each present shows for one refresh: on a
60 Hz panel `FRAMEWARP_MULT=3` shows 60 frames per second from 20 real ones
and holds the game at 20.

Because the motion comes from the picture only, the generated frames smear
where something appears from behind something else, at the screen edges and
around the HUD, and anything that changes speed shows it a frame late.

On the Retroid Pocket 6 in Azimuth (1920x1080 output) the first version of
the layer, which drew plain copies, cost 0.2 ms per real frame plus 0.15-0.3
ms per generated frame and kept a steady 60 frames per second.

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
| `FRAMEWARP_MULT` | `2` | frames shown per real frame, 1 to 4 (60 Hz panel: 3 for a 20 fps game, 2 for 30 fps) |
| `FRAMEWARP_FORCE_FIFO` | `1` | forces FIFO presents, so every frame gets its own refresh |
| `FRAMEWARP_MODE` | `warp` | `warp`; `copy` repeats the last real frame (no motion); `debug` warps and colours the motion it found |
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
layer costs per real frame (copy and motion) and per generated frame.

## Build

```
scripts/build.sh out
```

It needs CMake, a C++17 compiler, the Vulkan headers and loader, and
`glslangValidator`. `build/motion_test` runs the motion shaders on a pattern
moved by known amounts and checks what they find. CI builds the aarch64
package in Fedora 44, the base of ArmadaOS, runs that test and runs vkcube
through the layer with the validation layer below it, on Mesa's software
driver.

## License

MIT.
