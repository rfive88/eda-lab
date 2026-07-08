# .devcontainer/

Dev-container definition for working on eda-lab: `Dockerfile` starts from
Ubuntu 24.04 and installs the OpenROAD build dependencies by running the
pinned submodule's `etc/DependencyInstaller.sh`; `devcontainer.json` builds
that image and bind-mounts the repo at `/workspace` for VS Code /
compatible tools.
