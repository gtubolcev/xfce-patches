# CLAUDE.md

Patches adding features to XFCE components for the user's Xubuntu 26.04 (resolute, XFCE 4.20).
See README.md for layout and the fetch → hack → export → CI workflow.

- Develop in `src/<pkg>` (upstream git clone, branch `work`, based on the tag in `packages/<pkg>/BASE`); never edit `packages/<pkg>/patches` by hand — regenerate with `scripts/export-patches.sh`.
- Patches are applied with `quilt --fuzz=0` on top of the Ubuntu source package, so they must apply cleanly after Ubuntu's own `debian/patches`.
- CI (`.github/workflows/build.yml`) builds every dir in `packages/` inside `ubuntu:26.04`; tags `v*` create a GitHub release with the `.deb`s.
- Follow XFCE coding style (GNU-ish C, GTK3, xfconf for settings); upstream: https://gitlab.xfce.org, docs: https://docs.xfce.org.
- Scratch files go in `_tmp/` (git-ignored), not `/tmp`. No passwordless sudo — ask the user to run `! sudo ...`.
