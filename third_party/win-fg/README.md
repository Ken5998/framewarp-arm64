# win-fg (vendored)

The optical flow and frame synthesis of
[Win-FG](https://github.com/The412Banner/win-fg) by The412Banner, MIT
(see `LICENSE`), at commit `fbc69ed6c5bf16b824d0a29bd8b171675101f9dd`
(v0.3.0). It is the frame generation DroidDeck ships as DD-FG. The flow is
an adaptation of the AMD FidelityFX FSR3 optical flow, MIT (see
`NOTICE_FIDELITYFX_OPTICALFLOW.md`).

Only the engine is kept (`src/framegen.*`, `src/record_impl.inc` and the
headers they need, `shaders/`), not win-fg's own layer. framewarp's changes,
each marked `framewarp:` in the code:

- `FrameGen::record()` is split into `recordFlow()`, one flow solve per real
  frame, and `recordSynth()`, one call per generated frame, with alpha and
  an extrapolate flag as push constants instead of a uniform buffer, so
  several generated frames can be in flight;
- `wfg_synth.comp` extrapolates when asked (alpha above 1): it pushes the
  current frame along its flow and blends nothing from the previous one;
- the real frames are sampled in `VK_IMAGE_LAYOUT_GENERAL`;
- `FrameGen::resetScratch()` frees the per-size descriptors before a resize;
- the SPIR-V is compiled at build time instead of a generated table, and
  the log goes to framewarp's log.
