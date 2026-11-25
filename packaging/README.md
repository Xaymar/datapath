# Packaging

Recipes for package managers whose package definitions live in someone else's
repository. Both need a published `vX.Y.Z` GitHub Release first, and both go
upstream as pull requests opened by a maintainer.

Everything else (PyPI, npm, crates.io, the prebuilt C/C++ archives on GitHub
Releases) is built and published by [`.github/workflows/release.yml`](../.github/workflows/release.yml).

## vcpkg: `ports/xaymar-datapath`

The port name uses vcpkg's `GitHubOrg-GitHubRepo` form. Try it as an overlay
port before submitting (this builds the tag named in `vcpkg.json`):

```sh
vcpkg install xaymar-datapath --overlay-ports=packaging/vcpkg/ports
vcpkg install xaymar-datapath:x64-linux-dynamic --overlay-ports=packaging/vcpkg/ports
```

For a new version:

1. Set `version-semver` in `vcpkg.json`.
2. Set `SHA512 0` in `portfile.cmake`, run the install above, and copy the
   SHA512 that vcpkg reports into `portfile.cmake`.
3. In a fork of microsoft/vcpkg: copy the port to `ports/xaymar-datapath`, run
   `vcpkg format-manifest ports/xaymar-datapath/vcpkg.json`, commit, run
   `vcpkg x-add-version xaymar-datapath`, commit again, and open the PR.

## Conan Center: `conan/recipes/xaymar-datapath`

Same name as the vcpkg port.

Laid out as in conan-center-index (`config.yml`, `all/conanfile.py`,
`all/conandata.yml`, `all/test_package`). The release workflow runs
`conan create` against each release's source tarball.

For a new version:

1. Add the version to `config.yml` and to `all/conandata.yml`, with the
   SHA-256 of `https://github.com/Xaymar/datapath/archive/refs/tags/vX.Y.Z.tar.gz`
   (`curl -L <url> | sha256sum`).
2. `conan create packaging/conan/recipes/xaymar-datapath/all --version X.Y.Z --build=missing`
3. In a fork of conan-io/conan-center-index: copy the recipe folder to
   `recipes/xaymar-datapath` and open the PR.
