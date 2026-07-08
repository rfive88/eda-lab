# third_party/

Vendored dependencies.

- `openroad/` — the OpenROAD git submodule, pinned to `a3d4865`. Only the
  `odb` and `utl` targets are linked (via
  `add_subdirectory(... EXCLUDE_FROM_ALL)`); never build the top-level
  `openroad` target. Do not edit anything inside the submodule.
