# SESSION_NOTES — BC-250 mesh shader fallback

Branch: `bc250-mesh-fallback` (base: `a9ef675`, KytyPS5-Legacy main, upstream base `16b83a0`)
Target: AMD BC-250 (RADV, GFX1013 / GFX10.1) on Bazzite, host `bazzite@192.168.1.7`
Test game: ASTRO BOT (PPSA21564, v01.007.000) at `/home/bazzite/Emulation/storage/kytyps5/PPSA21564-app/eboot.bin`

## 2026-09-29 — Phase A

### Setup

- `git clone https://github.com/Hultwl/KytyPS5-Legacy.git` into
  `C:\Users\kaoti\Code Projects\kytylegacy-testbc250\KytyPS5-Legacy`
- `git checkout -b bc250-mesh-fallback`
- Local tooling gaps: no `rsync` and no `gh` in Git Bash on this PC. Sync will need
  `tar | ssh` or `scp` unless rsync is installed. GitHub searched through the REST API.

### SSH status (resolved later the same day — see "Phase A, on-box")

| Attempt | Result |
| --- | --- |
| `ssh bazzite@192.168.1.7` (first) | `Connection refused` on 22; host answered ping |
| retry ~20 min later | port 22 open; unknown host key (no prior `known_hosts` entry) |
| `-o StrictHostKeyChecking=accept-new` | key accepted, then `Permission denied (publickey,...,password)` |

- Host key recorded on first use: ED25519 `SHA256:gxxswEnaMLhti2YVAUItkEVR87WNx5Izg9Psvb4wrKU`
- No key on this PC is authorised on the box. Generated a dedicated keypair
  `~/.ssh/bazzite_bc250` (ed25519, no passphrase). Public key must be added to
  `~/.ssh/authorized_keys` on the Bazzite box by Brett (password entry is not something
  the agent does).
- Not run yet because of this: Mesa/kernel capture, `vulkaninfo`, game log (A1, A2, A4).

### A3 — Mesa source check (main, `VERSION` = 26.3.0-devel, fetched 2026-09-29)

Files read: `src/amd/vulkan/radv_physical_device.c`, `src/amd/vulkan/radv_instance.c`,
`src/amd/common/ac_gpu_info.c`, `docs/envvars.rst`.

```c
static bool
radv_taskmesh_enabled(const struct radv_physical_device *pdev)
{
   if (RADV_DEBUG(instance, NO_MESH_SHADER))
      return false;
   return pdev->use_ngg && !pdev->use_llvm && pdev->info.gfx_level >= GFX10_3 &&
          radv_compute_queue_enabled(pdev) && !pdev->info.has_taskmesh_indirect0_bug;
}
...
.EXT_mesh_shader = radv_taskmesh_enabled(pdev),
.KHR_fragment_shader_barycentric = pdev->info.gfx_level >= GFX10_3,
```

- Mesh shaders are hard-gated on `gfx_level >= GFX10_3`. GFX1013 reports `GFX10`.
- Second, independent blocker: `ac_gpu_info.c` has
  `/* GFX1013 is known to have broken compute queue */` and returns no compute queue for
  it. RADV task/mesh needs the compute queue (gang submit), so even with the level check
  patched out the feature would stay off.
- Env options that mention mesh shaders: only `RADV_DEBUG=nomeshshader` ("disable mesh
  shader support on GFX10.3+") and `RADV_DEBUG=mesh` (dump). Both only reduce/inspect.
- `RADV_PERFTEST` list (cswave32, dccmsaa, dmashaders, gewave32, localbos, lowlatencydec,
  lowlatencyenc, nggc, nircache, nogttspill, nosam, pswave32, rtcps, rtwave64, sam) and
  `RADV_EXPERIMENTAL` list (bfloat16, emulate_rt, hic, msrtss, sparse, transfer_queue,
  video_decode, video_encode, elf): nothing enables mesh shaders. The old `ext_ms`
  perftest flag no longer exists.
- Conclusion: no environment variable can turn on `VK_EXT_mesh_shader` on GFX10.1.

Expected values from Mesa main source (NOT yet confirmed on the box):

| Item | Expected on GFX1013 | Source |
| --- | --- | --- |
| `VK_EXT_mesh_shader` | no | `radv_taskmesh_enabled` |
| `VK_KHR_fragment_shader_barycentric` | no | `gfx_level >= GFX10_3` |
| `VK_KHR_ray_tracing_pipeline` | yes (native) | `has_image_bvh_intersect_ray` is set for `CHIP_GFX1013` |
| `geometryShader` | yes | unconditional `true` |
| `depthBounds` | yes | unconditional `true` |
| `VK_EXT_image_view_min_lod` | yes | unconditional `true` |
| `multiDrawIndirect` / `drawIndirectCount` | yes / yes | unconditional `true` |

The box runs whatever Mesa Bazzite ships, which may differ from main. Treat the table as a
prediction until `vulkaninfo` is captured.

### A5 — `mesh_shader_enabled` trace

All references:

| Location | Role |
| --- | --- |
| `src/graphics/host_gpu/graphicContext.h:39` | flag + `mesh_shader_properties` |
| `src/graphics/presentation/window/vulkanWindow.cpp:583` | set from extension + `meshShader` feature |
| `vulkanWindow.cpp:595`, `:621`, `:719` | properties query, log line, device feature request |
| `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:594` | `EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled)` |
| `src/graphics/host_gpu/renderer/pipeline/shaders.cpp:227` | `EXIT_NOT_IMPLEMENTED(mesh && !graphics.mesh_shader_enabled)` |
| `src/graphics/host_gpu/renderer/renderDraw.cpp:1046` | workgroup limit check, then `drawMeshTasksEXT` at `:1140` |

The reported message text `!m_graphics.mesh_shader_enabled` matches `pipelineCache.cpp:594`
exactly (the `shaders.cpp` check would print `mesh && !graphics.mesh_shader_enabled`), so
the abort is in `PipelineCache::GetGraphicsPrograms`, before any SPIR-V is compiled.

Path back to the guest:

1. `PrepareProgram` (`src/graphics/shader/shader.cpp:744`) reads `VGT_SHADER_STAGES_EN`.
   `merged = (stages & 0x20) != 0` — bit 5 is `GS_EN`.
2. `GS_EN` clear: ordinary NGG vertex/primitive shader, translated as a host **vertex**
   shader. Not affected.
3. `GS_EN` set: `info.logical_stage = ShaderType::Mesh` (`shader.cpp:765`). This is the
   guest **NGG merged ES+GS geometry shader**. With binary type `kGsFront` the ES front half
   and the `kGsBack` half at `gs_regs.data_addr` are concatenated into one program.
4. The recompiler synthesises the launch state the geometry engine would have provided
   (`Translate.cpp:1096`): wave info in s3, vertex/instance indices, primitive connectivity,
   index fetch through a BDA buffer. `S_SENDMSG(MSG_GS_ALLOC_REQ)` becomes `MeshAllocate`
   → `OpSetMeshOutputsEXT` (`Control.cpp:193`, `spirvEmitterMesh.cpp`).
5. Draw: six push-constant dwords, then `drawMeshTasksEXT(groups, instances, 1)`.

Tessellation is separate (host tess stages) and asserts `GS_EN` is clear.

Which one this game hit: by elimination the merged ES+GS geometry shader path — it is the
only producer of `ShaderType::Mesh` in this tree. Primitive type, shader hashes and whether
it was a `kGsFront`/`kGsBack` pair need the log from the box.

Properties of the guest program that matter for a fallback:

- It is a workgroup-style program: uses LDS (`Workgroup` storage), `S_BARRIER`, lane
  ordinals, up to `15 * wave_size` threads, wave32 or wave64.
- One workgroup handles `primitives_per_group` input primitives and emits up to
  `max_vertices` / `max_primitives`; only triangle output is supported
  (`m_vgtGsOutPrimType == 2`).
- Outputs used: Position, Parameter (vec4 per location), Layer (per primitive), cull bit.

### A6 — prior work search

GitHub REST search, repos `KytyPS5/KytyPS5` and `Hultwl/KytyPS5-Legacy`, queries: `mesh`,
`geometry shader`, `GFX10 OR BC-250 OR BC250 OR RADV`, `fallback`.

- Legacy fork: 0 issues/PRs for every query.
- Upstream, relevant:
  - PR #313 "graphics: support NGG merged geometry shaders" (closed, not merged as a PR;
    the same design is what is in the tree) — origin of the GS→mesh routing.
  - PR #727 (open) routes NGG **passthrough** programs through the Mesh path too. If it
    lands, more draws will require mesh shaders, not fewer.
  - PR #793 (open) splits oversized mesh draws; fixes issue #624
    (`mesh draw exceeds host workgroup limits: 1x68734`), seen in Astro Bot / Astro's Playroom.
  - PR #741 (open) prunes unused mesh outputs for MoltenVK's 32 KiB output limit.
  - Issue #489 (open) "Could not find suitable device AMD GPU Linux" — reporter is on a
    BC-250 (RADV GFX1013, Mesa 26.2.1). No maintainer reply about a fallback.
  - PR #586 / #559 / #551 (closed) — the barycentric/depthBounds fallback, rejected upstream
    as out of scope; this is why the Legacy fork exists.
- No mesh shader fallback exists or is proposed anywhere. macOS/MoltenVK does have
  `VK_EXT_mesh_shader`, so upstream has never needed one.

Astro Bot data points from upstream issues:

- Issue #624 comment: `Shaders: VS 282 | PS 180 | CS 67 | GS 5` — about five GS programs
  in a session, tied to breakable glass/boxes.
- Issue #835: needs game patches ("non-tiled deferred-lighting renderer", "Disable GI
  probes and lighting shaders"); one scene reported at 7 fps on an RTX 5070 Ti.

### Decisions

- Accepted the Bazzite host key on first use (no prior entry, LAN host named by Brett).
- Did not touch `~/.ssh/config`; the new key is passed with `-i`.
- Phase B not started. Waiting for approval and for SSH access.

### Open items (at that point)

All closed later the same day: key authorised, game path found on the box, captures done.

## 2026-09-29 (later) — Phase A, on-box

Brett installed `~/.ssh/bazzite_bc250.pub`. All commands below use
`ssh -i ~/.ssh/bazzite_bc250 -o IdentitiesOnly=yes -o BatchMode=yes bazzite@192.168.1.7`.

### A1 — versions

| Item | Value |
| --- | --- |
| OS | Bazzite 44.20260929.0 (Kinoite), image `bazzite-deck-44.20260929` |
| Kernel | `7.2.7-ogc1.1.fc44.x86_64` |
| Mesa | `mesa-vulkan-drivers-26.2.2-3.fc44` (RADV 26.2.2) |
| Vulkan | instance 1.4.341, device apiVersion 1.4.354, conformance 1.4.5.3 |
| Device | `AMD BC-250 (RADV GFX1013)`, 1002:13fe, integrated |
| Session | KDE Plasma Wayland (`wayland-0`, Xwayland `:0`) |
| Memory / disk | 7.5 GiB system RAM visible; `/var/home` 54 GiB free (95% used) |
| Tools on host | `vulkaninfo`, `distrobox`, `podman`, `rsync` present; no distrobox created yet |

Captured with `vulkaninfo --summary` and `vulkaninfo` into `~/kyty-bc250/diag/` on the box,
copied to `..\diag\` next to this repo on the PC (`vulkaninfo-summary.txt`,
`vulkaninfo-full.txt`, `versions.txt`). Kept out of the repo (size).

### A2 — confirmed limits (GPU0 section of `vulkaninfo-full.txt`)

| Item | Result |
| --- | --- |
| `VK_EXT_mesh_shader` | **NO** (extension absent, no `meshShader`/`taskShader` feature) |
| `VK_KHR_fragment_shader_barycentric` | **NO** |
| `VK_KHR_ray_tracing_pipeline` | YES (rev 1; `rayTracingPipeline = true`; also `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`) |
| `geometryShader` | YES |
| `depthBounds` | YES |
| `VK_EXT_image_view_min_lod` | YES (`minLod = true`) |
| `multiDrawIndirect` | YES |
| `drawIndirectCount` | YES (`maxDrawIndirectCount = 4294967295`) |

Every row matches the prediction made from Mesa source.

Numbers that matter for a fallback:

| Limit | Value |
| --- | --- |
| Subgroup size | default 64, min 32, max 64; size control required stages: fragment, compute |
| `maxComputeWorkGroupInvocations` / size | 1024 / 1024x1024x1024 |
| `maxComputeSharedMemorySize` | 65536 bytes |
| `maxComputeWorkGroupCount` | 4294967295 x 65535 x 65535 |
| `maxGeometryOutputVertices` | 256 |
| `maxGeometryTotalOutputComponents` | 1024 |
| `maxGeometryInputComponents` / output | 64 / 128 |
| `maxPushConstantsSize` | 256 |
| Transform feedback | yes, 4 buffers, 512 bytes stream data |
| Queues | one graphics+compute+transfer queue; no separate compute queue |

The missing separate compute queue agrees with Mesa's "GFX1013 is known to have broken
compute queue". Compute shaders on the graphics queue work: the emulator built and ran 16
compute pipelines before the abort.

### A4 — game run

Emulator on the box: prebuilt fork build `a9ef675` (same commit as this branch's base) at
`~/Downloads/KytyPS5-2026-09-26-a9ef675-Linux-x86_64/`.

- Brett's own log from 18:22 was already there (16462 lines). Copied, not modified:
  `..\diag\user-run-2026-09-29-1822_kyty.txt`.
- Own run, from an isolated cwd `~/kyty-bc250/run-a4/` so the emulator's relative
  `_SaveData`, `_TempData`, `_DownloadData`, `_PipelineCache` and `_kyty.txt` landed there
  and nothing in the install directory or the game directory was written.
- Arguments copied from `kyty_run.sh`. Environment:
  `XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
  XAUTHORITY=/run/user/1000/xauth_* DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`,
  `timeout 240`, stdin from `/dev/null`.
- **Launching over SSH works.** Run lasted 4 s, exit code 65.
- Log: `..\diag\run-a4\_kyty.txt` (16166 lines), `stdout.txt`.

Result, identical in both runs:

```
--- Build ---
Fork build Hultwl/KytyPS5-Legacy a9ef675
--- Fatal Error ---
Not implemented (!m_graphics.mesh_shader_enabled) in .../pipelineCache.cpp:594
```

Startup lines confirm the device state:

```
Select device: AMD BC-250 (RADV GFX1013)
fragmentShaderBarycentric support: No (fallback)
Vulkan subgroup: default=64 min=32 max=64 stages=0x00000030 size_control=true wave64=true
```

Where it dies:

- Last shader count: `VS 5 | PS 4 | CS 16 | GS 0`. The GS draw is in the first rendered
  frames, about 2 s of guest time in. There is no way to get further in this title without
  the fallback.
- Last markers before the abort: `... WaterStream_0`, `WaitAsyncFluidTr_0`, `FluidBase_0`,
  `Distortion_0`. The draw belongs to the transparent/distortion part of the frame.
- The abort is before the GS program is translated, so the log has no GS hash, primitive
  type or `kGsFront`/`kGsBack` information. Getting those needs a build with one extra log
  line ahead of the check — first thing to do once a build exists.
- Also seen: `Warning: ray tracing is not implemented; skipping compute dispatches
  containing BVH intersection instructions`. The emulator skips RT itself, so the BC-250's
  native RT support is unused.
- No cheat/patch lines in the log. Upstream issue #835 says Astro Bot needs two patches to
  get past the splash on any GPU; that is likely the next blocker after this one.

### State left on the Bazzite box

- `~/kyty-bc250/diag/` — vulkaninfo captures
- `~/kyty-bc250/run-a4/` — logs and emulator scratch dirs from the A4 run
- Nothing else created or changed. No distrobox, no packages, no system config.

### Phase A conclusion

Limits confirmed on hardware, code path confirmed by two logs, no prior fallback work
exists. Waiting for Brett's decision on Phase B.

## 2026-09-29 (evening) — Phase B, step 1: design doc

Brett approved Phase B and asked for the design doc first.

### Code study (read, not changed)

- Recompiler pipeline: `TranslateProgram` / `CompileProgram` in
  `src/graphics/shader/recompiler/ShaderRecompiler.cpp`; fused front+back decode in
  `DecodeFusedProgram`.
- Mesh prolog `Translate.cpp:1096`, `GS_ALLOC_REQ` handling `Control.cpp:193`, export staging
  `spirvEmitterFlow.cpp:440`, epilogue `spirvEmitterMesh.cpp:124`.
- Only compute pipelines request a subgroup size (`shaders.cpp:573`); the mesh path uses the
  driver default (`pipelineCache.cpp` sets `host_subgroup_size = m_graphics.subgroup_size`).
- Compute dispatches already end and resume rendering (`renderCompute.cpp:366`).
- In-tree precedent for generated bridge shaders: `src/graphics/shader/rectListShader.cpp`.

### External references checked

- MoltenVK compiles the vertex stage of tessellated pipelines as a compute kernel (its
  changelog and `MVKPipeline.mm`). Same idea as compute expansion.
- shadPS4 added geometry shader support by translating to Vulkan geometry shaders; that is
  the PS4 legacy ES/GS ring-buffer pipeline, not NGG.

### Output

- `docs/bc250-mesh-fallback.md` — compares compute expansion, vertex+geometry, and five other
  options. Recommends: stage 0 skip-and-log, then compute expansion with fixed output slots.

### Build environment on the box

| Step | Command / result |
| --- | --- |
| Distrobox | `distrobox create --name kyty-build --image registry.fedoraproject.org/fedora-toolbox:44 --yes` |
| Packages (inside the box only) | clang lld ninja-build cmake git glslang pkgconf, X11/Wayland/xkbcommon/ALSA/Pulse/udev/dbus/libdecor/pipewire devel packages, python3, rsync. 187 packages, `dnf rc=0` |
| Source tree | `~/kyty-bc250/src`: `git clone` of the fork at `a9ef675`, `git submodule update --init --recursive --depth 1` (314 MiB) |
| Sync method | `git diff a9ef675 --binary` on the PC, piped over SSH, `git apply` on the box |
| Build | `cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKYTY_BUILD_LAUNCHER=OFF`, target `kyty_emulator`, `--parallel 4` (limited for the 7.5 GiB of RAM) |
| Logs | `~/kyty-bc250/logs/` on the box |

Decision: sync by patch instead of copying the working tree. The PC checkout has
`core.autocrlf=true`, so its files are CRLF on disk; a patch keeps the box tree LF and identical
to what git records. The PC also has no `rsync`.

### Diagnostic change (small, not the fallback)

`pipelineCache.cpp`: before the existing abort, log the GS program's hash and mesh parameters
(`Mesh fallback needed: ...`). Purpose: fill in the "Measured" section of the design doc.

### Baseline build and diagnostic run

- Build: 857 steps, 7 minutes (18:52 to 18:59), `build rc=0`, 138 warnings, no errors.
  Binary `~/kyty-bc250/src/_Build/linux-no-qt/kyty_emulator`; build tree 784 MiB; 51 GiB free after.
- Run from isolated cwd `~/kyty-bc250/run-b0/`, same arguments and environment as the A4 run.
  Exit code 65, same abort (now reported at `pipelineCache.cpp:608` because of the added lines).
- Logs copied to `..\diag\run-b0\`.

Result:

```
Mesh fallback needed: hash=0x4e555b0ebf3b53f8 fused=true code_words=56+852 input_primitive=1
wave=64 threads=256 max_vertices=216 max_primitives=210 primitives_per_group=3
vertices_per_group=3 lds_dwords=3072 scratch_dwords=0 provoking=0
```

Reading: a fused `kGsFront`+`kGsBack` program, point list input, 3 points per group, up to 72
vertices / 70 triangles per point, 4 waves of 64, 12 KiB of LDS. Everything fits the device's
compute limits. It does not fit a host geometry shader (1024 total output components over 72
vertices leaves 14 components per vertex).

Design doc updated with these numbers.

### Status

Stopped for approval of the approach (Phase B step 2). No fallback code written. The only
source change on the branch is the diagnostic log line.

## 2026-09-29 (night) — Phase B, stage 0: skip and log

Brett approved: stage 0, then compute expansion. The force-fallback switch and a second test
title were not approved or named, so neither is included.

### Change

- `pipelineCache.cpp`, `GetGraphicsPrograms`: when the draw needs the mesh stage and the device
  has no mesh shaders, log the program once per hash and return no programs (was: abort).
- `renderDraw.cpp`, `PrepareDrawRenderState`: a draw with no vertex program is dropped; the
  first 64 and every power-of-two count are logged with primitive, index and instance counts.
- Devices with mesh shaders never enter either branch.

### Helper scripts (on the PC, outside the repo, `..\scripts\`)

- `sync-build.sh [targets]` — patch sync plus incremental build in the distrobox.
- `run-game.sh <run-name> [timeout]` — run ASTRO BOT from `~/kyty-bc250/<run-name>/`.

### Result

| Item | Value |
| --- | --- |
| Build | incremental, `build rc=0` |
| Run `run-s0`, 120 s timeout | exit code 124: still running when the timeout ended it. No abort |
| Shaders compiled | VS 26, PS 30, CS 37 when stopped |
| GS draws skipped | more than 65,536 in 120 s |
| Other warnings | only the known "ray tracing is not implemented" line |
| Process | about 2.4 GiB resident, about 210% CPU |
| Frame rate (window title) | 19 fps at frame 1523, 16 fps at frame 2152 |
| What renders | intro sequence: star field with particles, then the light-streak tunnel |

Screenshots (`spectacle -b -n -a`, active window only): `..\diag\run-s0\shot-50s.png`,
`shot-85s.png`.

GS programs seen:

| Hash | Fused | Words | Input | Threads | Max vertices | Max primitives | Per group | LDS dwords |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0x4e555b0ebf3b53f8` | yes | 56+852 | points | 256 | 216 | 210 | 3 | 3072 |
| `0xc739f9614016bed4` | yes | 68+3648 | points | 128 | 80 | 40 | 20 | 4096 |
| `0x2b3be82b8235ac05` | no | 4164 | points | 192 | 190 | 152 | 19 | 128 |

All three take point lists. The third is a single merged binary rather than a front/back pair,
and its 190/152 limits match the program described in upstream PR #741.

Draw sizes seen: `DrawIndexAuto` with 1, 30, 128, 135, 800 points and one instance;
`DrawIndex` with 1 index and 512 instances.

Next blocker after the abort: none reached in 120 s. The missing geometry is whatever these
three programs draw (particle-like effects).

## 2026-09-29 (night) — Phase B, stage 1: emitter

### Design decisions

- The IR stage stays `ShaderType::Mesh`. `ShaderMeshInputInfo::via_compute` selects the host
  encoding and is part of the stage static key.
- Outputs are stored through **device addresses** passed in push constants, not through a
  descriptor. Reason: a new descriptor binding kind would change `DescriptorBindingKind::Count`
  (asserted to be 50) and shift every pixel-stage binding. Device addresses need no layout change.
- Push constants for the compute encoding: the six existing draw dwords, then vertex buffer
  address (6,7), index buffer address (8,9), group count (10). `MeshComputeDrawDwordCount = 11`.
- Vertex record: one vec4 (4 dwords) per entry of `program.info.outputs`, in that order. Layer
  uses the first dword of its slot.
- Slot: `WorkgroupId.y * group_count + WorkgroupId.x`. Vertices at `slot * max_vertices + lane`,
  index triples at `slot * max_primitives + lane`, index values rebased by `slot * max_vertices`.
- Culled or unallocated primitives are not written; the renderer clears the buffers first.

### Files

| File | Change |
| --- | --- |
| `src/graphics/shader/shader.h` | `via_compute`, `ComputeOutputDwords` |
| `src/graphics/shader/shader.cpp` | `via_compute` in the static key |
| `src/graphics/shader/recompiler/ir/ShaderIR.h` | push constant slot constants |
| `backend/spirv/spirvEmitterInternal.h`, `spirvEmitterAnalysis.cpp`, `SpirvEmitter.cpp` | execution model `GLCompute` for the compute encoding |
| `backend/spirv/spirvEmitterModule.cpp` | no mesh capability or execution modes; physical addressing |
| `backend/spirv/spirvEmitterMesh.cpp` | no `Output` arrays; epilogue stores to the buffers |
| `tests/shaderCfgTests.cpp` | `TestMeshExportStorage` also compiles the compute encoding |

### Verification

- `sync-build.sh shader_cfg_tests`: `build rc=0`.
- `shader_cfg_tests` on the box: exit code 0. The test validates the module with SPIRV-Tools
  for host subgroup sizes 32 and 64 and checks: entry point is `GLCompute`, no
  `MeshShadingEXT`, stores go through `OpConvertUToPtr`, push constant dwords 6 to 10 are read.
- The "error" blocks in the test log come from the suite's expected-failure cases, which run
  in forked children.
- Not verified yet: execution on the GPU. That needs stage 2.

## 2026-09-30 (early) — Phase B, stage 2: renderer

### What was built

| Piece | Where |
| --- | --- |
| Device flag `mesh_shader_compute_fallback` (true only without mesh shaders) plus a startup log line | `graphicContext.h`, `vulkanWindow.cpp` |
| `GetGraphicsPrograms`: sets `via_compute`, checks compute limits (invocations, shared memory) instead of mesh limits, logs each program once as `Mesh fallback compute` or `Mesh fallback skipped` | `pipelineCache.cpp` |
| `GetMeshComputePipeline`: compute pipeline for the mesh program, cached by program id next to the real compute pipelines | `pipelineCache.cpp/.h`, `shaders.cpp` (compute pipeline creation now takes a `ShaderStageRuntime`) |
| Graphics pipeline: generated vertex shader + guest pixel shader, one vertex binding of vec4 records, triangle list, push constants for the fragment stage only | `shaders.cpp`, `pipelineCache.cpp` |
| Generated vertex shader (`BuildMeshOutputVertexShader`): attribute N = output N; Position, Parameter locations and Layer replayed; parameter locations follow the pixel shader, unexported ones read as zero | new `src/graphics/shader/meshOutputShader.{h,cpp}` |
| Output buffer: 64 MiB device-local buffer with device address, created on first use | `bufferCache.{h,cpp}` `GetMeshOutputBuffer` |
| Draw path: end rendering, clear the first record and the index region, commit the mesh program's descriptors on the compute point, push 11 dwords, dispatch `(groups, instances, 1)`, barrier to vertex input, bind the buffer as vertex + index buffer, then the usual graphics commit and `drawIndexed` | `renderDraw.cpp` |
| `CommitBindings`: a mesh program bound on the compute point is treated as a compute stage | `descriptors.cpp` |
| Unit test for the generated vertex shader | `tests/shaderCfgTests.cpp` |

Draws that would not fit the buffer or the compute workgroup limits are skipped with a log line
(`Mesh fallback: skipped oversized`). None occurred in ASTRO BOT's intro.

### Verification

1. **Readback (temporary diagnostic, removed before commit).** After each dispatch the output
   buffer was copied to host memory and analysed:
   - program `0xc739...` (points to quads, 20 points per group): a particle system whose real
     triangle count grew 42 → 88 across frames and spilled from group 0 into groups 1 and 2,
     with plausible clip-space positions (w ≈ 450–700). Groups beyond the first therefore work.
   - program `0x4e55...`: only "dummy" allocations in the intro (one vertex, one null
     primitive per group, which is what NGG hardware requires when nothing is emitted).
   - no out-of-range indices in any traced draw.
2. **Opaque test (temporary diagnostic, removed).** With blending and culling forced off for
   fallback draws, the geometry showed on screen as solid quads where the particles are.
3. **Vulkan validation**, run inside the distrobox (`vulkan-validation-layers` installed
   there; the host has none): the first error was a push-constant stage mismatch in the
   fallback's graphics pipeline layout — fixed. The second was a vertex/fragment interface
   mismatch — turned out to be pre-existing (reproduced with the stage 0 build). With
   validation made non-fatal for one run, all three fallback programs created their pipelines
   and drew for 150 s without any error attributed to them; the remaining errors are the
   stock emulator's (interface mismatches on regular pipelines, sampled-image format issues).
4. `shader_cfg_tests`: exit code 0.
5. Normal run `run-s2-final`, 125 s, no abort. Frame rate 13–15 fps (stage 0: 16–19 fps).
   Screenshots `..\diag\run-s2-final\shot-*.png`.

### Known limits after stage 2

- No slicing: a draw whose output exceeds 64 MiB or 65535 instances is skipped.
- Fixed slots: every group submits `max_primitives` triangles; unused ones are degenerate.
- Two buffer fills and one render pass break per GS draw.

## 2026-09-30 — Phase B, stage 3: instance slicing

- `renderDraw.cpp`: the compute encoding now dispatches instances in slices. The slice size is
  the largest instance count whose vertices and indices fit the 64 MiB output buffer and the
  device's `maxComputeWorkGroupCount[1]`. Each slice: clear, dispatch with its own
  `first_instance`, barrier, resume rendering, `drawIndexed`. Graphics bindings, vertex and
  index buffer bindings and dynamic state persist across the render pass break, so only the
  first slice commits them.
- Draws where a single instance does not fit (too many groups) are still skipped with a log
  line. Slicing by group would need a base-group push constant in the mesh prolog, as upstream
  PR #793 does for the mesh path. Not needed by anything seen so far.
- Test: with the slice size temporarily capped at 100 instances, ASTRO BOT's 512-instance
  draws ran as six slices each under Vulkan validation with no error attributed to the
  fallback (the run ends at the pre-existing interface error, as before). Cap removed.
- Normal run `run-s3`, 90 s: no abort, no skipped draws.

## 2026-09-30 — Phase B, stage 4: cost measurement and clean-up

### Method

`..\scripts\fps-sample.sh`: run the current box build for 112 s, screenshot the window at
40, 55, 70, 85 and 100 s; the emulator's window title carries the frame counter and fps.
Screenshots and cropped title bars in `..\diag\fps\`. Same intro sequence every run.

### Results

| Build | fps at 40/55/70/85/100 s | frame at 100 s |
| --- | --- | --- |
| stage 0 (GS draws skipped entirely) | 20 / 20 / 22 / 21 / 21 | 2714 |
| stage 3 (fallback) | 16 / 17 / 17 / 17 / 16 | 2341 |
| stage 3, dispatch kept, `drawIndexed` skipped | 18 / 16 / 16 / 17 / 16 | 2381 |
| stage 3, dispatch and draw both skipped | 14 / 16 / 16 / 17 / 16 | 2357 |
| stage 4 (no buffer clears) | 16 / 16 / 16 / 16 / 17 | 2315 |
| stage 4, whole compute block and draw skipped (only draw preparation, bindings, render pass) | 16 / 17 / 17 / 17 / 16 | 2369 |

Reading: the fallback costs about 14 % of frames over the intro, but none of it is the
fallback's GPU work. Skipping the dispatch, the draw, the clears, or all of them leaves the
frame rate unchanged. The cost is the emulator's ordinary per-draw preparation (descriptor
resolution, render target acquisition, pipeline lookup, bindings) for the roughly 34
geometry shader draws per frame that stage 0 dropped before any of that ran. A native mesh
path on a mesh-capable GPU would pay the same.

### Change kept from the experiments

- Emitter: the compute epilogue now writes an index triple for **every** primitive slot of
  the workgroup (its own primitive when allocated and visible, otherwise the degenerate
  `(base, base, base)`), so the renderer no longer clears anything before the dispatch. The
  two `Fill` calls and their four barriers per draw are gone; one buffer barrier
  (vertex input read → compute write) protects buffer reuse.
- Verified: `shader_cfg_tests` exit 0; validation run inside the distrobox clean for the
  fallback (ends at the pre-existing interface error, as before); normal run `run-s4-final`
  renders the intro as before.

### State on the box

- `~/kyty-bc250/src` holds the branch tree (synced by patch), build in `_Build/linux-no-qt`.
- Run directories `run-*` under `~/kyty-bc250/` hold logs and screenshots of every run.
- Distrobox `kyty-build` has the toolchain, Mesa and the validation layers.
- Nothing on the host outside `~` was changed.
