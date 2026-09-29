# Mesh shader fallback for GPUs without `VK_EXT_mesh_shader`

Status: **design, awaiting approval** — no fallback code written yet.
Branch: `bc250-mesh-fallback`. Target: AMD BC-250 (RADV GFX1013, Mesa 26.2.2).
Background and raw data: [`SESSION_NOTES.md`](../SESSION_NOTES.md).

## 1. Problem

KytyPS5 runs every guest **NGG merged ES+GS geometry shader** as a host mesh shader.
On a device without `VK_EXT_mesh_shader` the first such draw aborts:

```
Not implemented (!m_graphics.mesh_shader_enabled) in .../pipelineCache.cpp:594
```

RADV exposes mesh shaders only on GFX10.3+, and no environment option changes that. ASTRO BOT
issues a GS draw in its first rendered frames, so the title cannot progress at all.

Nothing else in the tree uses the mesh stage today. Upstream PR KytyPS5/KytyPS5#727 (open) would
also route NGG passthrough programs through it, so the set of affected draws is likely to grow.

## 2. What the fallback has to reproduce

The existing mesh path, in order:

| Step | Where | What happens |
| --- | --- | --- |
| Stage selection | `PrepareProgram`, `src/graphics/shader/shader.cpp:744` | `VGT_SHADER_STAGES_EN` bit 5 (`GS_EN`) sets `logical_stage = Mesh`; a `kGsFront` binary is fused with its `kGsBack` half |
| Launch state | `Translate.cpp:1096` | Synthesises what the geometry engine would supply: wave info in `s3`, vertex and instance index, primitive connectivity in `v0`/`v1`, guest index fetch through a BDA read |
| Guest body | whole recompiler | One workgroup = one guest subgroup. Lanes share LDS (`Workgroup` storage), use `S_BARRIER`, ballots and lane ordinals |
| Allocation | `Control.cpp:193` | `S_SENDMSG(GS_ALLOC_REQ)` becomes `MeshAllocate`, storing vertex and primitive counts |
| Exports | `spirvEmitterFlow.cpp:440` | Position/Parameter exports go to per-lane `Private` staging; Layer goes to `Workgroup` staging; the primitive export stores a packed connectivity dword |
| Epilogue | `EmitMeshEntryPoint`, `spirvEmitterMesh.cpp:124` | After a barrier: `OpSetMeshOutputsEXT`, then lane *i* copies its staging into output vertex *i* and output primitive *i* (three 10-bit indices, bit 31 = culled, Layer taken from the provoking vertex) |
| Draw | `renderDraw.cpp:1109` | Six push-constant dwords, then `drawMeshTasksEXT(groups, instances, 1)`; group = `WorkgroupId.x`, instance = `WorkgroupId.y` |

Two properties decide which fallbacks are viable:

1. **The guest program is wave-shaped, not per-vertex or per-primitive.** The role of a lane changes
   inside one program (input vertex, then input primitive, then output vertex), and lanes talk
   to each other through LDS and subgroup operations.
2. **The guest already compacts its own output.** Lane *i* is output vertex *i*; connectivity is
   local to the group. The host only needs somewhere to put the results.

### Measured on the BC-250

From a diagnostic build (`a9ef675` plus one log line), ASTRO BOT, first GS draw:

```
Mesh fallback needed: hash=0x4e555b0ebf3b53f8 fused=true code_words=56+852 input_primitive=1
wave=64 threads=256 max_vertices=216 max_primitives=210 primitives_per_group=3
vertices_per_group=3 lds_dwords=3072 scratch_dwords=0 provoking=0
```

| Property | Value | Meaning |
| --- | --- | --- |
| Program | `kGsFront` (56 words) fused with `kGsBack` (852 words) | A real geometry shader, not a passthrough |
| Input primitive | point list, 3 points per group | Expansion of points into geometry |
| Output | up to 72 vertices and 70 triangles **per input point** | A strip or ribbon, not a simple quad |
| Workgroup | 256 threads = 4 waves of 64 | Fits the 1024-invocation compute limit |
| LDS | 3072 dwords = 12 KiB | Fits the 64 KiB compute shared memory limit |
| Per group | 216 vertices, 210 triangles | Sets the output slot size |

Only this one program is known so far; the run stops at the first GS draw. Upstream logs show
about five GS programs per ASTRO BOT session.

Device limits that bound the designs (from `vulkaninfo`, GPU0):

| Limit | Value |
| --- | --- |
| Subgroup size | default 64, min 32, max 64; size control required for fragment and compute |
| Compute workgroup | 1024 invocations, 65536 bytes shared memory |
| Compute workgroup count | 4294967295 x 65535 x 65535 |
| Geometry shader output | 256 vertices, 1024 total components, 128 components per vertex |
| Push constants | 256 bytes (the emulator uses 128) |
| Queues | one combined graphics+compute queue |

## 3. Approach A — compute expansion

Run the unchanged guest program as a compute shader that writes vertices and indices into
buffers, then draw those buffers with a small generated vertex shader.

### Mapping onto the recompiler

The frontend, IR and every pass stay as they are. The IR stage stays `ShaderType::Mesh`; a
boolean on `ShaderMeshInputInfo` (working name `via_compute`) selects the host encoding. It must
be part of `BuildStageStaticKey` (`shader.cpp:659`).

| Piece | Mesh path today | Compute expansion |
| --- | --- | --- |
| Execution model | `MeshEXT`, SPIR-V 1.4 | `GLCompute` |
| Execution modes | `OutputTrianglesEXT`, `OutputVertices`, `OutputPrimitivesEXT` | `LocalSize` only (already emitted from `threads_num`, `spirvEmitterModule.cpp:714`) |
| Builtins | `WorkgroupId`, `LocalInvocationIndex` | same builtins, same meaning |
| LDS, barriers, subgroup ops | as compute | identical |
| Export staging | `Private` / `Workgroup` arrays | unchanged |
| Output declarations | `DefineMeshOutputs` declares `Output` arrays | declares one storage buffer block (vertex records) and one (indices) |
| Epilogue | `EmitMeshEntryPoint` stores into outputs | same loop; stores go to the buffers at a slot computed from group and instance |
| Draw parameters | `MeshDrawParameter` reads push constants | unchanged, plus two dwords for the slot base |

So the change is confined to `spirvEmitterMesh.cpp`, the three mesh branches in
`spirvEmitterModule.cpp`, and `ExecutionModelForStage`. No second emitter.

One thing gets *better*: compute is the only stage where RADV honours
`requiredSubgroupSize`. The mesh path takes whatever default subgroup size the driver picks;
the compute path can ask for the guest's wave size, reusing `shaders.cpp:573`.

### Output layout

Fixed slots, no compaction, no atomics:

```
slot          = instance * groups + group
vertex record = slot * max_vertices   + lane       (lane < allocated vertices)
index triple  = slot * max_primitives + lane       (lane < allocated primitives, not culled)
index value   = slot * max_vertices + local index
```

The index buffer is zero-filled before the dispatch, so every unused or culled slot is the
degenerate triangle (0,0,0) and rasterises to nothing. Draw count is known on the CPU:
`slots * max_primitives * 3`. No indirect draw is needed.

Fixed slots keep primitive order deterministic, which matters because the ASTRO BOT draw sits in
the transparent part of the frame.

### Mapping onto the renderer

| Piece | Change |
| --- | --- |
| `GetGraphicsPrograms` (`pipelineCache.cpp:594`) | Replace the abort with: if no mesh support, set `via_compute` and check compute limits instead of mesh limits |
| Pipeline objects | One compute pipeline for the guest program, one graphics pipeline made of the generated vertex shader plus the guest pixel shader |
| Generated vertex shader | Built with `SpirvBuilder`, the way `BuildRectListShaders` (`src/graphics/shader/rectListShader.cpp`) already generates tessellation shaders for rect lists. Reads the vertex record, writes Position, each Parameter location, and Layer |
| Descriptors | The guest program's bindings move to the compute bind point; the pixel shader's stay on graphics. `NativeBinding` already puts both in separate groups |
| `ExecutePreparedDraw` (`renderDraw.cpp:1020`) | For `via_compute`: `EndRendering`, zero-fill, bind compute pipeline, push constants, `dispatch(groups, instances, 1)`, buffer barrier, resume rendering, bind graphics pipeline, `drawIndexed` |
| Output buffers | A `StreamBuffer`-style ring with `eStorageBuffer | eVertexBuffer | eIndexBuffer` usage, sized per draw |
| Large draws | Slice the dispatch when a draw exceeds `maxComputeWorkGroupCount` or a buffer budget. Upstream PR #793 does the same slicing for mesh limits and its helper can be reused |

Ending and resuming rendering around a dispatch is already what `DispatchDirect` does
(`renderCompute.cpp:366`), so the scheduler supports it.

### Correctness gaps

| Gap | Severity | Note |
| --- | --- | --- |
| Primitive restart | same as today | Already unimplemented on the mesh path (warning at `renderDraw.cpp:1033`) |
| Output kinds other than Position, Parameter, Layer | same as today | The mesh path already aborts on them |
| Per-primitive Layer becomes per-vertex `gl_Layer` | low | Equivalent as long as the pipeline's provoking vertex matches `mesh.provoking_vertex`; `VK_EXT_provoking_vertex` is present |
| Culled primitives rasterised as degenerates | none visible | Zero-area triangles produce no fragments |
| Render pass interrupted per GS draw | none visible, some cost | Must resume with load, never clear. MoltenVK had exactly this bug in its tessellation pre-pass |
| Interpolation qualifiers on the generated vertex shader | to verify | Vulkan takes them from the fragment inputs; needs a check against the barycentric fallback's pixel inputs |
| Compute shaders cannot use implicit derivatives | none expected | Geometry stage programs do not sample with implicit LOD |

### Cost on the BC-250

Estimates, not measurements — measuring is part of the prototype.

- **GPU work:** the same guest code runs for the same number of invocations as the mesh path
  would. The extra work is one buffer fill, one barrier and a trivial vertex shader.
- **Per-draw overhead:** one render pass break per GS draw. Cheap on a desktop-class AMD GPU
  (not a tiler), but it adds up if a frame has hundreds of GS draws.
- **Memory:** `slots * max_vertices * record size`. For the measured program one slot is
  216 vertices; at an assumed 80 bytes per record (Position plus four parameters — the real
  count is known only after translation) that is about 17 KiB of vertices and 2.5 KiB of
  indices per group of three points. A draw of 3,000 points needs about 20 MiB. Upstream
  reports ASTRO BOT draws with 68,734 to 85,246 instances; if one of those used a program
  this size it would need over 1 GiB, so slicing is required rather than optional.
  The box shares 16 GiB between system and GPU.
- **Wasted rasteriser input:** fixed slots always submit 210 triangles per group, even when
  the guest emits far fewer. Degenerates are cheap but not free; this is the main argument
  for the compaction variant (C2) later.
- **CPU:** one extra pipeline and descriptor commit per GS draw.

## 4. Approach B — classic vertex and geometry pipeline

Re-emit the guest program through host vertex and geometry stages.

### Why it does not map

A host geometry shader is one isolated invocation per input primitive: no memory shared with
other invocations, no barrier, no subgroup. The guest program assumes all three.

| Variant | How it would work | Problem |
| --- | --- | --- |
| B1: whole program inside one geometry shader invocation | Loop over the lanes of the wave, with every register an array of 64 | Cross-lane operations can occur anywhere in divergent control flow. This needs a full vectorising backend: a second emitter, which the fork's README rules out ("no emitter forks") |
| B2: split at the fused boundary — front half as host VS, back half as host GS | LDS writes of the front become VS outputs; LDS reads of the back become `gl_in[]` | The back half is still wave-shaped: it counts and compacts output across lanes before exporting. Splitting it requires pattern-matching compiler output and breaks on any program that does not fit |
| B3: recognise common GS shapes (point to quad, etc.) and hand-write host shaders | Pattern library | Not a translation; every new title is a new pattern |

Hard limits also bite. The device allows 1024 total output components per geometry shader
invocation. The measured ASTRO BOT program emits up to 72 vertices per input point, which
leaves 14 components per vertex: Position plus two and a half parameters. Any program with
three or more parameters cannot be expressed as one host geometry shader on this GPU.

### Correctness gaps and cost

- B2 and B3 silently mis-render anything outside the recognised shape.
- Non-fused merged programs (no `kGsFront`/`kGsBack` split) have no boundary to split at.
- Geometry shaders are the slowest stage on AMD hardware; RADV itself lowers them to NGG with
  LDS round trips.

Not recommended. Listed because it was asked for and because B2 is the first idea most people have.

## 5. Approach C — other options considered

| Option | Verdict | Reason |
| --- | --- | --- |
| C1: **skip the draw** with a one-time warning | **Recommended as stage 0** | Upstream PR #313 described the same escape hatch ("refused with a named reason and dropped"). Costs an afternoon, shows immediately what breaks next, and stays useful as the fallback's own fallback |
| C2: compute expansion with compaction and `drawIndexedIndirectCount` | Later optimisation | Saves rasterising degenerates but needs atomics, and output order becomes non-deterministic, which is wrong for blended passes |
| C3: run the guest program on the CPU | Rejected | There is no IR interpreter in the tree; the SRT walker evaluates only address arithmetic |
| C4: patched Mesa enabling mesh shaders on GFX1013 | Rejected | Two independent blockers in RADV (hardware level gate, broken compute queue), and it would mean replacing the system driver |
| C5: host vertex shader re-running the group per output vertex | Rejected | Same lane-loop problem as B1, multiplied by the vertex count |

### How other projects handled the same class of problem

- **MoltenVK** (Metal has no tessellation control stage): compiles the vertex and tessellation
  control shaders as compute kernels that write buffers, then draws from them. This is approach A,
  in production, and its changelog shows the pitfalls (attachment re-clear on resume, multiple
  patches per workgroup for speed).
- **shadPS4** (PS4): translates guest geometry shaders to Vulkan geometry shaders. That works
  because the PS4 pipeline is the legacy one: ES and GS are separate stages joined by ring
  buffers, and the GS emits with explicit emit/cut messages. The PS5 NGG program is a different
  shape, so the approach does not transfer.
- **KytyPS5 itself**: rect lists are drawn with generated tessellation shaders
  (`rectListShader.cpp`). That is the in-tree precedent for "generate a small host shader to
  bridge a missing primitive feature".

## 6. Comparison

| | A: compute expansion | B: vertex + geometry | C1: skip draw |
| --- | --- | --- | --- |
| Runs arbitrary guest GS programs | yes | no | n/a |
| Recompiler change | one file plus three branches | second backend or pattern matcher | none |
| Renderer change | large (second pipeline, buffers, dispatch in draw path) | medium | trivial |
| Visual result | should match mesh path | partial | geometry missing |
| Risk of silent wrong output | low | high | none (it is loud) |
| Effect on GPUs with mesh shaders | none, gated | none, gated | none, gated |

## 7. Recommendation

**Stage 0: C1 (skip and log). Stage 1: approach A with fixed slots.**

Approach A is the only option that runs the guest program as written, it reuses the compute
infrastructure the emulator already has, and the recompiler change is small. Most of the work
and most of the risk is in the renderer.

## 8. Plan

| Stage | Content | Exit criterion |
| --- | --- | --- |
| 0 | Log the GS program's parameters (done); skip the draw when there is no mesh support and no fallback | ASTRO BOT runs past the abort; next blocker identified; all GS programs and draw sizes listed |
| 1 | Emitter: `via_compute` encoding; unit test next to the existing mesh tests in `tests/shaderCfgTests.cpp`; SPIR-V validates | `kyty_tests` passes |
| 2 | Renderer: output buffers, compute pipeline, generated vertex shader, dispatch and draw, no slicing | GS geometry visible in ASTRO BOT |
| 3 | Slicing for large draws, buffer budget, provoking vertex and Layer checks | No abort on the large-instance draws reported upstream |
| 4 | Measure frame cost on the BC-250; write up | Numbers in `SESSION_NOTES.md` |

### Gating

Same pattern as the barycentric fallback (commit `3f7f874`):

- `graphics.mesh_shader_enabled` keeps its meaning and is still set only from the extension and
  feature query.
- A new `graphics.mesh_shader_compute_fallback` is true only when mesh shaders are absent.
- Devices with mesh shaders take the existing path; none of their code paths or shader keys change.
- A startup line announces the fallback, like `Vulkan barycentric fallback active`.

An override to force the fallback on a mesh-capable GPU would let it be tested against the real
path on other hardware. Worth having, but it is a new option and therefore Brett's call.

## 9. Risks and open questions

| Item | Why it matters |
| --- | --- |
| Only one GS program has been measured | The run stops at the first GS draw; stage 0 will reveal the others and their draw sizes |
| Parameter count of the measured program is unknown | It sets the vertex record size; known once the program is translated in stage 1 |
| ASTRO BOT needs two game patches on every GPU (upstream issue #835) | The title may stop at the next blocker even with a perfect fallback; a second, lighter GS-using title would make results easier to read |
| 7.5 GiB of system RAM visible on the box | Independent of this work, but a likely cause of later failures |
| Compute subgroup behaviour on GFX1013 | Compute pipelines already run on this box; wave64 with full subgroups is reported as supported but not yet exercised by a GS program |
| Upstream churn | PRs #727, #741 and #793 all touch the mesh path; the fallback should stay in as few files as possible to keep rebases cheap |
| Upstream acceptance | Upstream declined the barycentric fallback as out of scope; assume this lives in the Legacy fork |

## 10. Decision needed

1. Approve stage 0 plus approach A, or choose differently.
2. Force-fallback override for testing on mesh-capable GPUs: yes or no.
3. A second test title, if one is available on the box.
