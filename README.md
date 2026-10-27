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
no frame of latency. The generated frames are queued on the GPU right behind
the real frame, before the game's next frame, so they don't wait for it.

## Status: stage 1, the probe

This release does not warp anything yet. The extra frames are copies of the
last real frame (or a tinted copy, to see them), and the layer measures:

- what each extra frame costs on the GPU (timestamp queries),
- whether the frames reach the screen at a steady rate on the 60 Hz panel,
- how long the game waits for the extra presents.

These numbers decide whether stage 2, the real reprojection, is worth
building on the Adreno 740.

## How it works

On every present of the game the layer copies the game's image into a
history image, presents the game's image, then acquires `FRAMEWARP_MULT - 1`
more images from the same swapchain, draws the history image into them and
presents them. The swapchain runs in FIFO mode, so each present shows for one
refresh: on a 60 Hz panel `FRAMEWARP_MULT=3` shows 60 frames per second from
20 real ones and holds the game at 20.

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
| `FRAMEWARP_TAPS` | `1` | `9` reads a 3x3 footprint per pixel, to measure a pass as heavy as a warp |
| `FRAMEWARP_TINT` | `0` | 0 to 1, tints the generated frames magenta to see them |
| `FRAMEWARP_LOG` | `~/framewarp.log` | where the measurements go |
| `FRAMEWARP_LOG_EVERY` | `2` | seconds between two log lines |

A log line looks like this:

```
real 20.0 fps (worst gap 52.1 ms), shown 60.0 fps, missed 0 | GPU copy 0.30 ms, generated frame 0.90 ms (max 1.40) | CPU in extra presents 33.0 ms (max 35.0)
```

`real` is what the game renders, `shown` what reaches the swapchain, `missed`
counts extra frames that could not be shown, and the GPU times are what the
layer costs per real frame (copy) and per generated frame.

## Build

```
scripts/build.sh out
```

It needs CMake, a C++17 compiler, the Vulkan headers and `glslangValidator`.
CI builds the aarch64 package in Fedora 44, the base of ArmadaOS.

## License

MIT.
