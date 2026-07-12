# docs/papers/

Reference papers for `eda-lab`. PDFs are **gitignored and not redistributed**
in this repo. Fetch each one locally using the `curl` command provided.

---

## Partitioning

### 1. FM 1982 — `fm_fiduccia1982.pdf`

- **Authors:** Charles M. Fiduccia, Robert M. Mattheyses
- **Title:** A Linear-Time Heuristic for Improving Network Partitions
- **Venue:** 19th Design Automation Conference (DAC), 1982
- **DOI:** 10.1145/800263.809204
- **Source:** ACM Digital Library only (paywalled). Obtain via ACM DL access
  and save as `docs/papers/fm_fiduccia1982.pdf`.
- **Informs:** Base move-based gain structure in partitioner Stages 1–2.

---

### 2. hMETIS — `hmetis_karypis1999.pdf`

- **Authors:** George Karypis, Rajat Aggarwal, Vipin Kumar, Shashi Shekhar
- **Title:** Multilevel Hypergraph Partitioning: Applications in VLSI Domain
- **Venue:** IEEE Transactions on VLSI Systems, Vol. 7, No. 1, March 1999, pp. 69–79
- **DOI:** 10.1109/92.748202
- **Source URL:** <https://limsk.ece.gatech.edu/course/ece6133/papers/hmetis.pdf>

```bash
curl -L -o docs/papers/hmetis_karypis1999.pdf \
  https://limsk.ece.gatech.edu/course/ece6133/papers/hmetis.pdf
```

- **Informs:** Coarsening schemes (HEC/MHEC) and V-cycle refinement for
  partitioner Stage 3 (multilevel k-way).

---

### 3. TritonPart — `tritonpart_bustany2023.pdf`

- **Authors:** Ismail Bustany, Grigor Gasparyan, Andrew B. Kahng, Ioannis
  Koutis, Bodhisatta Pramanik, Zhiang Wang
- **Title:** An Open-Source Constraints-Driven General Partitioning
  Multi-Tool for VLSI Physical Design
- **Venue:** IEEE/ACM International Conference on Computer-Aided Design
  (ICCAD), 2023
- **DOI:** 10.1109/ICCAD57390.2023.10323892
- **Source URL:** <https://vlsicad.ucsd.edu/Publications/Conferences/401/c401.pdf>

```bash
curl -L -o docs/papers/tritonpart_bustany2023.pdf \
  https://vlsicad.ucsd.edu/Publications/Conferences/401/c401.pdf
```

- **Informs:** Algorithm behind `third_party/openroad/src/par/` and the
  constraints-driven multilevel flow; reference destination architecture for
  Stage 3.

---

## Hypergraph Metrics

### 4. Alpert DAC 2010 — `alpert_tangled_dac2010.pdf`

- **Authors:** Tanuj Jindal, Charles J. Alpert, Jiang Hu, Zhuo Li,
  Gi-Joon Nam, Charles B. Winn
- **Title:** Detecting Tangled Logic Structures in VLSI Netlists
- **Venue:** 47th Design Automation Conference (DAC), Anaheim, CA,
  July 13–18, 2010, pp. 603–608
- **DOI:** 10.1145/1837274.1837422
- **Source:** IEEE Xplore / ACM DL (paywalled). Obtain and save as
  `docs/papers/alpert_tangled_dac2010.pdf`.
- **Informs:** `tangle_score` (local Rent exponent) in
  `src/hg_metrics/congestion_metrics` — spike brief C4.

---

### 5. HyperANF — `hyperanf_boldi2011.pdf`

- **Authors:** Paolo Boldi, Marco Rosa, Sebastiano Vigna
- **Title:** HyperANF: Approximating the Neighbourhood Function of Very
  Large Graphs on a Budget
- **Venue:** WWW 2011 (also on arXiv:1011.5599)
- **DOI:** 10.1145/1963405.1963493
- **Source URL:** <https://arxiv.org/pdf/1011.5599>

```bash
curl -L -o docs/papers/hyperanf_boldi2011.pdf \
  https://arxiv.org/pdf/1011.5599
```

- **Informs:** `neighbourhood_function` and `vertex_ball_sizes` in
  `src/hg_metrics/timing_metrics` — spike brief T3. The HyperLogLog
  ball-expansion algorithm is implemented as a static helper in
  `timing_metrics.cpp`.

---

### 6. NESS — `ness_khan2011.pdf`

- **Authors:** Arijit Khan, Nan Li, Xifeng Yan, Ziyu Guan,
  Supriyo Chakraborty, Shu Tao (IBM T.J. Watson)
- **Title:** Neighborhood Based Fast Graph Search in Large Networks
- **Venue:** ACM SIGMOD, Athens, Greece, June 12–16, 2011
- **DOI:** 10.1145/1989323.1989387
- **Source URL:** <https://sites.cs.ucsb.edu/~xyan/papers/sigmod11_ness.pdf>

```bash
curl -L -o docs/papers/ness_khan2011.pdf \
  https://sites.cs.ucsb.edu/~xyan/papers/sigmod11_ness.pdf
```

- **Informs:** Label-weighted information propagation model in
  `src/hg_metrics/congestion_metrics` (`neighborhood_density`) — spike
  brief C3. The `A(u, l)` accumulation formula is adapted for structural
  density scoring on netlists.

---

## Fetching all freely available papers at once

```bash
# Run from repo root
curl -L -o docs/papers/hmetis_karypis1999.pdf \
  https://limsk.ece.gatech.edu/course/ece6133/papers/hmetis.pdf

curl -L -o docs/papers/tritonpart_bustany2023.pdf \
  https://vlsicad.ucsd.edu/Publications/Conferences/401/c401.pdf

curl -L -o docs/papers/hyperanf_boldi2011.pdf \
  https://arxiv.org/pdf/1011.5599

curl -L -o docs/papers/ness_khan2011.pdf \
  https://sites.cs.ucsb.edu/~xyan/papers/sigmod11_ness.pdf
```

Papers requiring institutional access (fm_fiduccia1982.pdf,
alpert_tangled_dac2010.pdf) must be obtained separately via ACM DL or
IEEE Xplore.
