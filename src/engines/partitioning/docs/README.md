# src/engines/partitioning/docs/

Reference papers for the partitioning engine — background for the flat FM
implementation (Stages 1–2) and, especially, the planned Stage 3 multilevel
work.

**The PDFs in this directory are gitignored and NOT redistributed in this
repo** (they are freely readable at their sources but not freely
redistributable). They are for local reference only. Anyone cloning this repo
must fetch them themselves from the sources cited below, saving each under
the listed local filename.

## Papers

### 1. hMETIS journal paper — `hmetis_karypis1999.pdf`

- **Authors:** George Karypis, Rajat Aggarwal, Vipin Kumar, Shashi Shekhar
- **Title:** Multilevel Hypergraph Partitioning: Applications in VLSI Domain
- **Venue:** IEEE Transactions on Very Large Scale Integration (VLSI)
  Systems, Vol. 7, No. 1, March 1999, pp. 69–79
- **DOI:** 10.1109/92.748202
- **Source URL:** <https://limsk.ece.gatech.edu/course/ece6133/papers/hmetis.pdf>

```bash
curl -L -o src/engines/partitioning/docs/hmetis_karypis1999.pdf \
  https://limsk.ece.gatech.edu/course/ece6133/papers/hmetis.pdf
```

### 2. TritonPart — `tritonpart_bustany2023.pdf`

- **Authors:** Ismail Bustany, Grigor Gasparyan, Andrew B. Kahng, Ioannis
  Koutis, Bodhisatta Pramanik, Zhiang Wang
- **Title:** An Open-Source Constraints-Driven General Partitioning
  Multi-Tool for VLSI Physical Design
- **Venue:** IEEE/ACM International Conference on Computer-Aided Design
  (ICCAD), 2023
- **DOI:** 10.1109/ICCAD57390.2023.10323892
- **Source URL:** <https://vlsicad.ucsd.edu/Publications/Conferences/401/c401.pdf>

```bash
curl -L -o src/engines/partitioning/docs/tritonpart_bustany2023.pdf \
  https://vlsicad.ucsd.edu/Publications/Conferences/401/c401.pdf
```

### 3. FM 1982 — `fm_fiduccia1982.pdf` (not fetchable; ACM-paywalled)

- **Authors:** Charles M. Fiduccia, Robert M. Mattheyses
- **Title:** A Linear-Time Heuristic for Improving Network Partitions
- **Venue:** 19th Design Automation Conference (DAC), 1982
- **DOI:** 10.1145/800263.809204
- **Source:** ACM Digital Library only — there is no clean open host; obtain
  it via ACM Digital Library access and save it here as
  `fm_fiduccia1982.pdf`.

## What informs what

hMETIS → coarsening schemes (HEC/MHEC) and V-cycle refinement; TritonPart →
the algorithm behind `third_party/openroad/src/par` and the
constraints-driven multilevel flow; FM → the base move-based refinement
(already implemented in Stages 1–2).
