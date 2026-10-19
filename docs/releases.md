# Releases and updates

How a release is cut, what a station trusts, and what that does not cover.
Back to the [README](../README.md).

## Cutting a release

Tag the commit and push the tag:

```sh
git tag v0.3.0
git push origin v0.3.0
```

CI builds the tag as release `v0.3.0` (`-DRELEASE_VERSION`), runs the host tests, then
creates the GitHub Release with:

- `onodiy-esp32-ntrip-server.bin`, the app image stations install;
- the receiver package(s) from [`firmware/um982/`](../firmware/um982/) (checked against
  its `SHA256SUMS`);
- `manifest.json`, written by [`tools/release_manifest.py`](../tools/release_manifest.py):
  the version, and every file with its size and SHA-256;
- `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` and an `esptool`
  command line in the release notes, for the first flash of a new board, and
  `THIRD_PARTY_NOTICES.md`.

The release is published only once every file is attached. Stations read only the
release GitHub marks Latest; the job gives that mark to the highest `vX.Y.Z`, so a
lower tag pushed later (a fix on an older line) is published without it and offered
to no station. A tag with a suffix (`v0.3.0-rc.1`) becomes a pre-release, which
stations never offer (install it as a file upload). To redo a release, delete it and
its tag on GitHub first. The job needs only `contents: write`. After publishing, it
follows the same download redirects a station does and fails if one breaks a rule
the firmware enforces (skipped while the repository is private).

## What a station trusts

The repository must be public: stations download without
a token. A release is trusted because of where it comes from:

- every request is HTTPS, with the server certificate checked against the CA bundle and
  the dates of every certificate in its chain checked against the station's clock (no
  update transfer starts before the clock is set);
- it goes only to the releases of the repository set at build time
  (`CONFIG_FW_UPDATE_GITHUB_REPO`, `Averyy/onodiy-esp32-ntrip-server` by default).
  Every redirect must stay on GitHub's release hosts (`github.com`,
  `release-assets.githubusercontent.com`, `objects.githubusercontent.com`), over https,
  without user info, and on `github.com` inside that repository's `/releases/`, or the
  download stops before it connects;
- the manifest names each file with its size and SHA-256, and an app image or receiver
  package is used only if both match (the app image is also checked in flash, and must
  carry the manifest's version and this project's name).

No signing keys, no secure boot. Updates still start only when you click Update, and a
self-built image can always be installed with a file upload.

## Keep the repository's name

The name is what stations trust. While stations are
in use, never rename or delete the account that owns the repository, and never
rename, transfer or delete the repository. When an account is renamed or deleted,
GitHub lets anyone register its old name. Whoever does, and creates a repository of
the same name, can publish a release that stations offer like any other (built from
this public source), and a station cannot tell the difference. A rename also stops
updates by itself: stations refuse GitHub's redirect to the new name
(`untrusted_host`). To move to another repository:

1. Create the new repository and publish a release there.
2. In the old repository, set `CONFIG_FW_UPDATE_GITHUB_REPO` to the new name and the
   repository variable `RELEASE_MOVE_TO` to the same name, then tag a release. The
   release job publishes that build, which reads the new repository, and refuses one
   that reads any other.
3. Keep the old repository and its account (archive it; never rename or delete it).
   Stations that miss the moving release still read it.

## What this does not cover

Without a signing key, TLS is all that protects the
manifest on its way from GitHub. Certificate revocation is not checked, and the CA
bundle is fixed in each image, so a root certificate distrusted later stays trusted
until a newer firmware is installed. Whatever CI builds from a tag is what stations
are offered, so the CI build image and actions are part of the trust: the build image
is pinned by digest and each action by commit SHA (its version tag in a comment).

## Forks

Set `CONFIG_FW_UPDATE_GITHUB_REPO` to your repository; the release job
refuses to publish a build that reads releases from another one. Signing is optional:
put your public key in `CONFIG_FW_UPDATE_MANIFEST_KEY_1` (130 hex digits; `_2` is for
rotating keys) and the private key in the `MANIFEST_SIGNING_KEY` repository secret, and
CI signs `manifest.json` (`{"payload": ..., "signature": ...}`, ECDSA P-256/SHA-256).
A build with a key accepts only manifests signed with it and refuses unsigned ones;
the release job refuses to publish an unsigned manifest for such a build.

```sh
openssl ecparam -name prime256v1 -genkey -noout -out manifest-key.pem   # keep it secret
openssl ec -in manifest-key.pem -pubout | openssl ec -pubin -text -noout  # the pub: hex
```

Manifest format and rules: `components/fw_update/priv_include/manifest.h`.
