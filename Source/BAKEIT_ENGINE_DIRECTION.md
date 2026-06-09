# BakeIT Engine Direction

## Purpose

This document defines the engine direction for BakeIT.

BakeIT is not a sculpting app.
BakeIT is a gameplay-first volumetric interaction game where the player must be able to:

- cut bread, cake, dough, and layered food
- add icing, cream, filling, toppings, and decorations
- smooth, merge, trim, and shape food interactively
- get immediate visual feedback with no hitching during the action

The engine should be built around that requirement first.

## Core Rule

The interaction path must never block on optimization.

During gameplay:

- editing must feel immediate
- brush/cut response must be visible right away
- no frame hitch on drag
- no release freeze

Optimization is allowed only:

- after the action
- in the background
- while idle

If there is a conflict between:

- perfect compactness
- perfect memory usage
- perfect final-quality rebuild

and

- immediate responsive interaction

the interaction wins.

## Product Priorities

Priority order for BakeIT:

1. Realtime editing feel
2. Stable visual result during interaction
3. Correct settled result after interaction
4. Background optimization
5. Long-range rendering/Lod/GI optimization

This means:

- drag path must be simple
- release path must be asynchronous
- idle path can do heavier cleanup

## What BakeIT Is Not

The engine should not be designed like:

- a DCC sculpting tool
- a CAD boolean system
- an offline SDF baker
- a hyper-optimized sparse renderer that sacrifices edit feel

The player should never feel:

- "the game paused because I touched the cake"
- "the icing appears half a second later"
- "cutting is technically correct but feels laggy"

## Rendering And Edit Model

BakeIT should use a 3-phase interaction model.

### Phase 1: Drag / Live Interaction

Goal:

- immediate response

Allowed:

- local compute dispatch around the brush
- broad temporary preview
- approximate active block set
- overdraw
- temporary extra support blocks

Not allowed:

- full chunk readback each sample
- strict CPU active-surface filtering each sample
- full compact instance rebuild each sample
- full `lod1` rebuild each sample
- command buffer rerecord pressure in the interaction loop

Rule:

- while dragging, update the SDF as fast as possible
- preview may be less optimized
- preview must be responsive

### Phase 2: Release / Settle

Goal:

- transition from preview to correct final state without freezing

Allowed:

- full resolve compute
- strict surface scan
- active-instance compaction
- lod rebuild

But:

- it must happen asynchronously or incrementally
- never as one giant blocking step on mouse release

Rule:

- keep showing the preview result
- background-settle the correct result
- swap in optimized data when ready

### Phase 3: Idle / Optimization

Goal:

- recover performance and memory

Allowed:

- atlas cleanup
- brick compaction
- free-list cleanup
- lod rebuild
- chunk maintenance
- background validation

This is where expensive optimization belongs.

## Recommended Runtime Architecture

### 1. Source Of Truth

Long-term source of truth should be:

- gameplay edit operations
- or a stable chunk-local field state that can be recomputed safely

Do not make live rendering depend on CPU rebuilding visibility every brush sample.

### 2. Fast Preview Representation

During editing, use a preview-friendly representation:

- fixed or broad instance coverage for the editable chunk
- shader decides block validity from `blocks[]`
- avoid CPU-driven "which cubes exist" per sample

This is less optimal, but correct for interaction.

### 3. Background Compact Representation

After release, build:

- compact `fineActiveIds`
- strict renderable block list
- coarse `lod1`

Then replace preview rendering with the optimized result.

### 4. Chunk Ownership

The editable chunk should be treated as:

- a high-priority interaction workspace

Snapshot/frozen chunks can be:

- optimized harder
- compacted aggressively
- LOD-managed more strictly

So the editable chunk and settled chunks should not be forced to obey the same performance policy.

## SDF Editing Rules

### Broad Recompute, Narrow Storage

For smooth operations:

- recompute extent must be broad enough to cover influence
- storage/rendering extent should stay narrow

Do not confuse:

- blocks that must be recomputed
- blocks that must be stored
- blocks that must be rendered

These are different sets.

### Temporary Support Is Fine During Editing

During drag:

- some temporary support allocation is acceptable
- extra blocks are acceptable
- slight overdraw is acceptable

During settle/idle:

- shrink to true renderable blocks
- compact storage

### Correctness Goal

For BakeIT, "correct" means:

- cuts feel believable
- icing additions appear where the player expects
- smoothing feels continuous
- result stabilizes cleanly after interaction

Not:

- mathematically minimal block set every frame

## Normals Direction

Current lesson:

- normal quality is limited by field quality
- do not over-invest in tiny shader hacks if the underlying field is under-resolved

Practical rule:

- use the best stable normal path available for gameplay
- improve field resolution/support where needed
- do not let perfect normal math block interaction work

If a choice must be made:

- stable believable lighting during gameplay
beats
- mathematically ideal but unstable normal generation

## LOD Direction

LOD is important, but not in the drag path.

Rules:

- live editable chunk: prioritize responsiveness and correctness
- settled chunks: may use stronger LOD optimization
- future large-scene direction: chunked source + render cache / clipmap style thinking

Do not let LOD rebuilding sit in the interaction loop.

## Immediate Technical Decisions

These should guide the next steps in the current codebase.

### Keep

- local compute dispatch around the brush
- proxy-cube rendering of bricks
- deferred render-pass rebuild trigger (`renderPassDirty`)
- asynchronous/background settle concept

### Remove From Drag Path

- strict renderable surface scan
- full fine active-id rebuild
- full `lod1` rebuild
- any full-chunk resolve done immediately on release if it blocks frame flow

### Move To Background

- full chunk resolve after release
- strict block filtering
- compact instance list rebuild
- coarse lod rebuild

## Concrete Near-Term Plan

### Stage 1

Make editing always feel responsive.

Target:

- brush drag never freezes
- shape appears immediately while dragging
- release does not hitch

Implementation direction:

- live preview uses broad/fixed instance coverage
- compute updates only the brush-affected region
- no blocking CPU visibility rebuild in drag path

### Stage 2

Make release settle invisible.

Target:

- release starts background settle
- preview remains visible
- final compact result swaps in when ready

Implementation direction:

- phase the settle work across multiple frames
- no one-frame full pipeline stall

### Stage 3

Optimize settled chunks.

Target:

- compact rendering after interaction
- lod for non-editing chunks
- memory cleanup in idle

### Stage 4

Improve field quality where gameplay needs it.

Target:

- better cuts
- better icing thickness / smooth merges
- fewer topology artifacts in important food interactions

Implementation direction:

- support bands where needed
- adaptive detail later if justified

## Engineering Decision Filter

Before adding any new system, ask:

1. Does this improve drag responsiveness?
2. If not, can it be moved to release or idle?
3. Does this add CPU/GPU synchronization to the drag path?
4. Does this make the player wait after a brush action?
5. Is this solving a real BakeIT gameplay need, or just making the renderer academically cleaner?

If the answer hurts interaction, it should not live in the drag path.

## Final North Star

The perfect BakeIT engine is not the one with the most elegant sparse structure.

It is the one where:

- cutting feels instant
- icing feels direct
- decorating feels smooth
- the player never notices the engine struggling

That is the standard every edit-path decision should be measured against.
