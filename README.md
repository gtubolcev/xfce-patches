# XFCE-patches

Extra features for XFCE components on **Xubuntu 26.04 (resolute)** (and ports to
**24.04 (noble)**), packaged as
drop-in replacement `.deb`s built by GitHub Actions.

Docs: <https://docs.xfce.org/start> · <https://docs.xubuntu.org/> · upstream code: <https://gitlab.xfce.org>

## Layout

```
packages/<source-package>/
    BASE            upstream tag the patches are based on (e.g. thunar-4.20.7)
    patches/        *.patch (git format-patch, DEP-3 friendly) + series
    <codename>/     optional port to an older Ubuntu release (e.g. noble),
        BASE            with its own BASE and patches/
        patches/
scripts/
    setup-dev.sh        one-time: enable deb-src, install toolchain (sudo)
    fetch-upstream.sh   clone upstream git at the installed version -> src/<pkg>
    export-patches.sh   commits on src/<pkg> (branch work) -> packages/<pkg>/patches
    build-package.sh    apt-get source + our patches -> .deb (used by CI too)
src/                upstream working clones (git-ignored)
```

## Workflow

```bash
sudo scripts/setup-dev.sh                 # once
scripts/fetch-upstream.sh thunar          # clone gitlab.xfce.org/xfce/thunar @ thunar-4.20.7
cd src/thunar && meson setup build && ninja -C build   # hack, test, git commit
scripts/export-patches.sh thunar          # -> packages/thunar/patches/
scripts/build-package.sh thunar           # optional local .deb build -> out/
git push                                  # CI builds debs-thunar artifact
git tag v0.1.0 && git push --tags         # CI attaches all .debs to a release
```

Packages get the version suffix `+xfcepatches1` (e.g. `4.20.7-1+xfcepatches1`), so they
sort above the archive version. When Ubuntu ships a newer version, rebase the
patches (`fetch-upstream.sh` on the new tag, `git rebase`, `export-patches.sh`).
To pin our build: `sudo apt-mark hold <pkg>`.

### Older Ubuntu releases

Xubuntu 24.04 (noble) ships older Xfce versions, so the patches are ported
there separately: a branch named after the release in the same upstream
clone, based on noble's upstream tag (e.g. a worktree at `src/<pkg>-noble`).

```bash
git -C src/xfce4-settings worktree add -b noble ../xfce4-settings-noble xfce4-settings-4.18.4
mkdir -p packages/xfce4-settings/noble && echo xfce4-settings-4.18.4 > packages/xfce4-settings/noble/BASE
# cherry-pick the commits from work, fix conflicts, build, test
scripts/export-patches.sh xfce4-settings noble   # -> packages/xfce4-settings/noble/patches/
```

CI builds every package for 26.04 and, if `packages/<pkg>/<codename>/` exists,
also in an `ubuntu:<codename>` container; a release gets the `.deb`s of all of them.

A manual run (Actions → build → Run workflow) with a package name builds any
source package, patched or not.
