# run/

Working directory for manual test runs and any command that writes output
files (`out.def`, `out.odb`, logs, dumps, etc.). Everything in here except
this README and `.gitkeep` is git-ignored — see the standing convention in
the top-level `CLAUDE.md`.

```bash
cd run
../build/hello_odb ../data/nangate45/Nangate45_tech.lef \
                   ../data/nangate45/Nangate45_stdcell.lef \
                   ../data/gcd_nangate45.def
```
