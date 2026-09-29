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
