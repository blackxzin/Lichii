# Ocarina of Time on an RX 580 (Linux)

Branch `amd/oot-rx580`: Ocarina of Time (`0400486024DEC000`) on a Polaris GPU without cooperative matrix or FP16. Work in progress.

## Test machine

| Part | Spec |
|---|---|
| GPU | AMD Radeon RX 580 2048SP, 8 GB (Polaris10, GCN4) |
| Driver | RADV, Mesa 26.2.5 |
| GPU features | no `shaderFloat16`, no `VK_KHR_cooperative_matrix`, no `VK_EXT_mesh_shader`, subgroup size 64, `shaderInt16` |
| ReBAR | off (256 MiB host-visible VRAM heap) |
| CPU | AMD Ryzen 5 4600G (6 cores, 12 threads) |
| RAM | 16 GB, plus 4 GB zram and an 8 GB swapfile |
| OS | Arch Linux, kernel 7.2 |
| Build | clang, Ninja, ccache, Release, 4 to 6 jobs (12 jobs runs out of memory) |

## Settings used

| Setting | Value |
|---|---|
| Console mode | Handheld |
| `gpu_accuracy` | Low |
| `resolution_setup` | 1x |
| `scaling_filter` | Bilinear |
| `anti_aliasing` | None |
| `oot_anti_aliasing` | No sharpening |
| `frame_gen` | Off |
| Built-in patch "Max Resolution" | disabled |
| Environment | `MESA_SHADER_CACHE_MAX_SIZE=4G` |

## Changes on this branch

| Commit | Change |
|---|---|
| `087e222791` | Build the two stalling compute shaders unoptimized on RADV too (#200): 125 s and 221 s compiles drop to 2 to 4 s |
| `03bc7be930` | Swap in the optimized build of those shaders in the background |
| `a9748dd0cd`, `4077283b61` | Replace the movie network by an input copy (port of #721) |
| `562c0de1b8` | Let RADV keep its own pipeline cache (port of Eglantine #714) |
| `e2d08b24d6` | MADV_COLD instead of MADV_PAGEOUT for cold guest memory (#756) |
| `a15151ef32`, `77233e8663`, `4c36472f26`, `a05c983d3c`, `e446b43601` | Ports of #699, #700, #718, #715, #752 |
| `8aa88353f5` | Enable `pipelineCreationCacheControl` when available |
| `4425110679` | "Occlusion Off" as an opt-in built-in patch (`EDEN_NX2_OCCLUSION_OFF=1`) |
| `4677d5b771`, `9b2bf8b11f`, `ac826e8613`, `3de8f49798` | `EDEN_DKB_FRAME_TRACE` also reports GPU time outside draws and dispatches, the call that ended each render pass, emulated mesh steps and image copies |
| `57dc35e6c0` | Create colour images without DCC on RADV before GFX10: Lichii keeps images in `GENERAL`, where pre-GFX10 RADV never compresses, but every copy back from `TRANSFER_*` decompressed the whole image |

## Results

| Scene | nx2 `bf4c42e223` | This branch |
|---|---|---|
| Title screen | ~10 fps | 30 fps (locked) |
| Intro cutscene | stalls minutes compiling | 30 fps |
| Kokiri Forest gameplay (centre) | ~9.5 fps | 13.8 fps |

In gameplay the GPU is fully busy while the game renders at 864x480 (its dynamic resolution minimum). After the DCC fix a Kokiri frame takes ~75 ms on the GPU: ~26 ms of draws and game compute, ~8 to 12 ms of emulated mesh shaders (no `VK_EXT_mesh_shader` on Polaris), up to ~12 ms of depth copies between 864x480 and 854x480 aliases, and ~12 ms idle between submits.

## Known problems

- With 16 GB RAM the kernel OOM killer can close the emulator in gameplay (about 6 GB resident plus 4 to 5 GB shared memory). Use a swapfile and close heavy apps.
- The community "45K" shader cache preloads ~23K pipelines and needs more RAM than this machine has.
