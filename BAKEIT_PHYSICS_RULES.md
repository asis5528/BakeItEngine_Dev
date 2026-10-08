# BakeIT Physics & Object Rules

The agreed design for how objects and physics work in BakeIT. This is the
reference for the chunk/object/physics system — read it before changing
SdfEditor's chunk, split, weld, or physics code. Decided 2026-07-05.

---

## 1. The one core rule: a chunk IS one connected object

> **A chunk = one connected piece of material. Nothing else.**

Like molecules in the real world: if material isn't touching, it isn't the
same object. The 64³ block domain is *invisible storage*, not the object —
the player only ever thinks about objects.

Consequences:

- Draw two spheres that don't touch → **two chunks** (two objects), even if
  drawn in the same domain, in the same stroke.
- Cut through a shape so a part is disconnected → that part **becomes its own
  chunk** automatically. Cutting is not a special feature; it's just the
  connectivity rule firing.
- **Weld is the ONLY way two objects become one** — a deliberate "glue"
  action (like frosting sticking two cake halves together). A stroke can
  never bridge two objects into one: a stamp writes exactly one chunk's
  field (the air-draw lesson). Player rule: **build by drawing, join by
  welding.**

### How connectivity is enforced (the cut-suspect algorithm)

The trick: stay cheap at block level everywhere, and only zoom into voxels
along the knife's path — the only place where the answer can change.

1. **While erasing**, every block the brush touches is marked a **cut
   suspect** (accumulated across strokes — multi-stroke cuts work).
2. **On stroke release** (release path — never during a drag): read the
   block table, prune suspects that went empty (natural barriers) or fully
   interior (solid), then block-level flood-fill with the remaining suspects
   **excluded as barriers**.
3. **One group** → connected no matter what the suspects hide. Done — no
   voxel data ever read. (99% of strokes; ~1 ms.)
4. **Several groups** → a thin cut may have severed the object *inside* a
   suspect block. Read back **only the suspect tiles** (+ their neighbors'),
   flood-fill the material inside each 8³ block, and check whether any
   material component still **bridges the groups face-to-face** (the
   paper-sketch rule: a block is truly cut when no material inside it
   reaches both sides anymore).
   - Bridge exists → deep scratch, still one object. Suspects that proved
     solid are cleared; multi-piece blocks stay suspicious.
   - No bridge → **real cut** → split.
5. **The split**: one island keeps the chunk (cut strokes: the biggest;
   draw strokes: the one overlapping the pre-stroke content); every other
   island moves into a fresh 64³ chunk by table surgery (block-aligned with
   the parent, tiles never move). The new chunk inherits the parent's
   ModelMatrix, so nothing visibly moves at the split moment.
6. **Mixed cut blocks** (the cut ran inside them, both pieces own voxels of
   the same block) are **duplicated**: each side's chunk gets its own copy
   of the tile, cleaned of the other side's voxels (core and pads). Both
   cut faces stay intact, no material lost, no sliver rides along with the
   wrong piece.

Costs scale with the **size of the cut, not the object**: a normal stroke
pays ~1 ms; a real cut pays a small readback (~hundreds of KB) plus a few ms
of CPU, once. Escalation path if ever needed (Quest): a GPU
connectivity-summary pass per suspect block (component labels + per-face
bitmasks, ~50 bytes/block read back instead of 729 voxels).

Remaining granularity note: two shapes **drawn** very close (sub-block gap,
no erasing) still count as one object — there's no cut line to inspect, and
piping two lines of frosting that close *does* merge them. Accepted.

---

## 2. Object states: sketch → physical (one-way)

Every chunk is in exactly one of two states:

| State | Meaning | Behavior |
|---|---|---|
| **Sketch** | Drawn, not yet part of the physical world | Hangs in the air exactly where drawn, forever. Freely editable. |
| **Physical** | Assigned to the physical world | Falls, lands, rests — always behaves physically from then on. |

Rules:

- **Drawing always creates sketch chunks.** Nothing you draw ever moves on
  its own.
- **Press `P` = commit.** Every sketch chunk is promoted to physical,
  immediately: content bounds measured, gravity takes it. Chunks that are
  already physical are untouched — `P` only drains the "unassigned queue".
  `P` is also the natural future hook for any batch physics work (mass
  computation, etc.).
- **Physical is one-way.** Once real, always real. (Editing a resting
  physical object is allowed — it re-settles after the stroke.)
- **Split inheritance: only CUT pieces inherit physical.**
  - **Erase** stroke splits a *physical* object → the cut piece is born
    physical and falls instantly, no `P` needed. This is the core game verb:
    **what you cut off, falls.**
  - **Draw** stroke creates a disconnected island → the island is always
    born *sketch*, even inside a physical chunk's domain. New material never
    moves until the next `P` — drawing stays frustration-free after a
    commit. (A stroke that erased at any point counts as a cut.)
  - "Cut" is a property of the **island**, not of the stroke whose release
    happened to *detect* the separation: cut suspects accumulate across
    strokes, so a severed piece may only be recognized later (even on a draw
    release). An island that touches the eraser's suspect blocks was cut
    free — it inherits physical no matter which stroke found it.
  - Split a *sketch* object → all islands stay sketch and wait for `P`.
- No anchoring special cases: the main cake resting at ground level "becomes
  physical" on `P` and simply keeps sitting there — same rule as everything
  else.

---

## 3. Physics model

Not a general physics engine. The game needs exactly:

1. Cut something off → it falls.
2. Falling things land on other things (ground, table, the cake itself) and
   rest naturally.
3. Later: fluids (syrup, batter) colliding with the same objects.

**Chunk = rigid body; its SDF = its collision shape.** A distance field is a
perfect collision shape for free: distance = penetration depth, gradient =
push-out direction. This is also why fluids will be cheap to collide later —
they sample the same fields.

### Implementation stages

| Stage | What | Status |
|---|---|---|
| 1 | Gravity + ground plane, linear motion, sleep-when-resting | DONE (2026-07-05) |
| 2 | **Connectivity rule + `P` commit** (this doc's sections 1–2) | DONE (2026-07-05) |
| 3 | **Contacts**: a falling piece lands and rests ON other pieces | DONE first pass (2026-07-06): block-granular + vertical-only over the CPU block-table mirrors — the falling chunk's per-column bottom material blocks vs every other chunk's occupancy; tunneling guard (max 2 blocks fall/frame); a moving support carries its rider but never puts it to sleep; edits/splits/welds wake all physical pieces so removed supports re-settle. Voxel-precision push-out along the gradient stays future work (pieces rest up to 1 block proud of the visual surface) |
| 4 | **Rotation**: contacts act at points → torque → pieces tumble and settle on their side | planned |
| 5 | Mass from block occupancy, VR grab/throw, fluids | future |
| 6 | Conduction over the connectivity graph: heat (baking! the sponge cooks from the pan inward), electricity, magnets — the voxel/block connectivity built for splitting doubles as the propagation medium | idea, parked |

Field samples for contacts: the first pass uses the **CPU block-table
mirrors** (already maintained for brush picking - fresher and finer than the
planned coarse-field copy). Revisit with voxel-level readback or a GPU
contact kernel when block-granular resting reads too coarse.

### Golden-rule compliance (BAKEIT_ENGINE_DIRECTION.md)

- Per-frame physics = a handful of field samples + tiny CPU matrix math.
  Never touches the drag path.
- Flood-fill / table readbacks / island extraction = **release-path only**,
  same policy as weld.
- Nothing ever blocks a brush stroke.

---

## 4. Out of scope — permanently

- Joints, ragdolls, stacked towers, general rigid-body stacking.
- Dynamic-vs-dynamic collision (two simultaneously falling pieces) unless
  gameplay ever demands it.
- Free-air drawing / infinite canvas (PARKED — see the air-draw notes; a
  world-aligned chunk-grid canvas is the design if ever needed).
