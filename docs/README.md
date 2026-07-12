# docs/

Project-level documentation for `eda-lab`.

## Structure

```
docs/
├── briefs/    Spike brief markdown files — one per Claude Code implementation session.
└── papers/    Reference paper citations, fetch instructions, and local PDF filenames.
               PDFs are gitignored and must be fetched locally (see papers/README.md).
```

## Conventions

**Briefs** (`docs/briefs/`)  
Each spike brief specifies a single well-scoped implementation task. Claude Code sessions
are pointed at the relevant brief file directly. Briefs are committed in the same session
they are written; they remain in the repo as a permanent record of design decisions.

**Papers** (`docs/papers/`)  
PDFs are **not redistributed** in this repo (copyright). The `papers/README.md` tracks
full citations, DOIs, publicly accessible source URLs, and the expected local filename
for each paper. Use the `curl` commands there to fetch papers to your local checkout.
