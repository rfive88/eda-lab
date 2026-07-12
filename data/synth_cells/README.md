# data/synth_cells/

Hand-written cell LEF fixtures for the netlistgen Stage B/D tests. Unlike the
Nangate45 set under `data/nangate45/` (a real vendor library), these are
tiny, deliberately-shaped cells that exercise specific corners of the
statistical cell-mix machinery.

## `twobucket.lef`

Two connectivity-only cells, loaded against `../nangate45/Nangate45_tech.lef`
(same `SITE` and `metal1` layer):

| MACRO   | Signal pins        | Pin-count bucket |
|---------|--------------------|------------------|
| `BUFB2` | 1 in + 1 out = 2   | 2 (index 0)      |
| `AND3B4`| 3 in + 1 out = 4   | 4 (index 2)      |

Buckets 3, 5 and 6+ are intentionally empty. This lets
`test/netlistgen_stageb_test.cpp` confirm that `generateSynthetic()` fails
fast at spec-build time when a requested pin-count bucket has no matching
master, while a distribution that only weights the two populated buckets
succeeds. Both cells carry `VDD`/`VSS` power/ground pins so the tests also
verify signal-pin counting excludes them.

`twobucket.lef` deliberately contains **no sequential cells** (no `USE
CLOCK` pin, no clock-named pin), which the empty-sequential-class fail-fast
test relies on.

## `dff_seq.lef`

One flip-flop-like cell, added in Stage D:

| MACRO    | Signal pins               | Class      |
|----------|---------------------------|------------|
| `DFFSYN` | `D`, `CK` (in) + `Q` (out)| sequential |

Its `CK` pin is declared `USE CLOCK`, so it classifies as sequential via the
`dbSigType::CLOCK` path directly (no clock-pin-name fallback needed). Stage D
made `sequential_ratio > 0` mandatory in statistical mode, so success-path
tests against the twobucket fixture load this file alongside it to populate
the sequential class; `twobucket.lef` itself stays sequential-free on
purpose.

Geometry (`SIZE`, `PORT` rects) is nominal — these cells exist for
connectivity classification only, never placement or routing.
