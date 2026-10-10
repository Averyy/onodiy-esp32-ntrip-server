#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Write the release manifest (manifest.json) that stations read in their
daily update check.

The release workflow (.github/workflows/build.yml) runs this on a tag build:

  python3 tools/release_manifest.py --version v0.3.0 \\
      --app onocoydiy-esp-um982.bin --gnss firmware/um982/UM982_R4.10Build18975.pkg \\
      --sdkconfig-json sdkconfig.json --repo owner/name --out manifest.json \\
      [--sign-key key.pem] [--move-to owner/new-name]

The manifest lists every file the station may download, by release asset
name (the file's base name), with its size and SHA-256 (format and rules:
components/fw_update/priv_include/manifest.h). Without --sign-key it is
written plain; with one it is wrapped in the signed envelope
{"payload": base64, "signature": base64 DER ECDSA P-256/SHA-256}, signed
with `openssl dgst -sha256 -sign`. A station without a manifest key accepts
both; one built with a key accepts only a manifest signed with it.

Checks, so a release the stations would refuse fails here instead:
  - the version is a release version the firmware accepts (vMAJOR.MINOR.PATCH
    with an optional -PRE, at most 31 characters);
  - the app image is an ESP-IDF app whose description names this version and
    this project;
  - each receiver package is named <MODEL>_<VERSION>.pkg with "Build<N>" in
    the version, the first "Build" of the version is followed by the build
    number (the station reads it there), and the package matches the
    SHA256SUMS next to it when there is one;
  - with --sdkconfig-json (the build's build/config/sdkconfig.json): the build
    is not the QEMU development build (sdkconfig.qemu); it
    reads releases from --repo under the manifest name written here (or from
    --move-to: the release that moves stations to another repository); a
    build with a manifest key gets a signed manifest, and the signing key is
    one of its keys; the manifest fits the build's size limit; and none of
    the bench-only options is on (the bench console, the receiver byte
    capture, and receiver flashing, which stays off in releases until the
    receiver side has passed on hardware: drop UM98X_FLASH_ENABLE from
    BENCH_ONLY then, on purpose).

Standard library only, plus the openssl command for signing.
"""

import argparse
import base64
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

PROJECT = "onocoydiy-esp-um982"
SCHEMA = 1
MAX_GNSS = 4  # FW_MANIFEST_MAX_GNSS
UINT32_MAX = 0xFFFFFFFF

# esp_image_header_t (24 bytes) + esp_image_segment_header_t (8), then esp_app_desc_t:
# magic_word, secure_version, reserv1[2], version[32], project_name[32], ...
IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432

# DER SubjectPublicKeyInfo of a P-256 key, up to the 65-byte uncompressed point.
P256_SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")

NUM = r"(?:0|[1-9][0-9]*)"
IDENT = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
VERSION_RE = re.compile(rf"^v({NUM})\.({NUM})\.({NUM})(?:-{IDENT}(?:\.{IDENT})*)?$")
ASSET_RE = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9._-]*$")
PKG_RE = re.compile(r"^(?P<model>[A-Za-z0-9]+)_(?P<version>[!-~]*Build(?P<build>[0-9]+)[!-~]*)\.pkg$")
REPO_RE = re.compile(r"^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$")

# Kconfig symbols (sdkconfig.json names, without CONFIG_) a release must not have on.
BENCH_ONLY = ("BENCH_CONSOLE", "UM98X_FLASH_BYTE_CAPTURE", "UM98X_FLASH_ENABLE")


class Fail(Exception):
    pass


def check_version(v):
    m = VERSION_RE.match(v)
    if not m or len(v) > 31 or any(int(x) > UINT32_MAX for x in m.groups()):
        raise Fail(f"{v!r} is not a release version (vMAJOR.MINOR.PATCH[-PRE], at most 31 characters)")


def check_asset_name(name):
    if not ASSET_RE.match(name) or len(name) > 95:
        raise Fail(f"{name!r} cannot be a release asset name ([A-Za-z0-9._-], not starting with '.', at most 95)")


def file_entry(path):
    data = path.read_bytes()
    if not data or len(data) > UINT32_MAX:
        raise Fail(f"{path}: empty or too large")
    check_asset_name(path.name)
    return {"asset": path.name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}, data


def cstr(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def app_entry(path, version):
    entry, data = file_entry(path)
    if len(data) < APP_DESC_OFFSET + 80 or data[0] != IMAGE_MAGIC:
        raise Fail(f"{path}: not an ESP-IDF app image")
    magic, = struct.unpack_from("<I", data, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        raise Fail(f"{path}: no app description where ESP-IDF puts it")
    img_version = cstr(data[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48])
    project = cstr(data[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80])
    if project != PROJECT:
        raise Fail(f"{path}: built as project {project!r}, not {PROJECT!r}")
    if img_version != version:
        # The station refuses an image whose description does not name the
        # manifest's version (a dev build is "<describe>+dev").
        raise Fail(f"{path}: the image says version {img_version!r}, the release is {version!r} "
                   "(build the tag with -DRELEASE_VERSION)")
    return entry


def read_sums(directory):
    sums = directory / "SHA256SUMS"
    if not sums.is_file():
        return None
    out = {}
    for line in sums.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2:
            out[parts[1].lstrip("*")] = parts[0].lower()
    return out


def gnss_entry(path):
    m = PKG_RE.match(path.name)
    if not m:
        raise Fail(f"{path}: receiver packages are named <MODEL>_<VERSION>.pkg with Build<N> in the version")
    model, version, build = m.group("model"), m.group("version"), int(m.group("build"))
    # The station reads the build number right after the first "Build" of the
    # version and refuses the whole manifest when it is missing or another one.
    first = re.match(r"[0-9]+", version[version.find("Build") + len("Build"):])
    if first is None or int(first.group()) != build:
        read = f"build {int(first.group())}" if first is not None else "no build number"
        raise Fail(f"{path}: stations read {read} after the first \"Build\" in {version!r}, not {build}")
    if len(model) > 15 or len(version) > 47 or not 1 <= build <= UINT32_MAX:
        raise Fail(f"{path}: model, version or build out of range")
    entry, _ = file_entry(path)
    sums = read_sums(path.parent)
    if sums is not None and sums.get(path.name) != entry["sha256"]:
        raise Fail(f"{path}: SHA-256 {entry['sha256']} is not the one in {path.parent / 'SHA256SUMS'}")
    return {"model": model, "version": version, "build": build, **entry}


def openssl(*args, data=None):
    r = subprocess.run(["openssl", *args], input=data, capture_output=True)
    if r.returncode != 0:
        raise Fail("openssl " + " ".join(args[:2]) + ": " + r.stderr.decode(errors="replace").strip())
    return r.stdout


def public_point(key):
    der = openssl("pkey", "-in", str(key), "-pubout", "-outform", "DER")
    if len(der) != len(P256_SPKI_PREFIX) + 65 or not der.startswith(P256_SPKI_PREFIX):
        raise Fail(f"{key}: not a P-256 (prime256v1) private key")
    return der[len(P256_SPKI_PREFIX):]


def sign(payload, key):
    with tempfile.TemporaryDirectory() as tmp:
        p = pathlib.Path(tmp, "payload")
        s = pathlib.Path(tmp, "sig")
        pub = pathlib.Path(tmp, "pub.pem")
        p.write_bytes(payload)
        openssl("dgst", "-sha256", "-sign", str(key), "-out", str(s), str(p))
        pub.write_bytes(openssl("pkey", "-in", str(key), "-pubout"))
        openssl("dgst", "-sha256", "-verify", str(pub), "-signature", str(s), str(p))
        return s.read_bytes()


def key_hex(text):
    h = re.sub(r"[\s:]", "", text or "").lower()
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", required=True, help="release tag, e.g. v0.3.0")
    ap.add_argument("--app", required=True, type=pathlib.Path, help="the app image (.bin) of the tag build")
    ap.add_argument("--gnss", type=pathlib.Path, nargs="*", default=[], help="receiver packages (.pkg)")
    ap.add_argument("--out", required=True, type=pathlib.Path, help="manifest file to write")
    ap.add_argument("--sign-key", type=pathlib.Path, help="P-256 private key (PEM): write a signed manifest")
    ap.add_argument("--sdkconfig-json", type=pathlib.Path, help="the build's config/sdkconfig.json")
    ap.add_argument("--repo", help="owner/name the release is published on")
    ap.add_argument("--move-to", help="owner/name of another repository the build reads releases from: "
                    "the release that moves stations there")
    args = ap.parse_args()

    try:
        check_version(args.version)
        cfg = json.loads(args.sdkconfig_json.read_text()) if args.sdkconfig_json is not None else None
        if cfg is not None:
            qemu = [k for k in ("NETWORK_QEMU_ETHERNET", "ETH_USE_OPENETH") if cfg.get(k)]
            if qemu:
                raise Fail("this is the QEMU development build (CONFIG_" + ", CONFIG_".join(qemu) +
                           "): it has no Wi-Fi and never runs on a station")
            bench = [s for s in BENCH_ONLY if cfg.get(s) is True]
            if bench:
                raise Fail("bench-only options are on in this build: " + ", ".join("CONFIG_" + s for s in bench) +
                           " (build the release from sdkconfig.defaults alone)")
        manifest = {
            "schema": SCHEMA,
            "version": args.version,
            "app": app_entry(args.app, args.version),
            "gnss": [gnss_entry(p) for p in args.gnss],
        }
        if len(manifest["gnss"]) > MAX_GNSS:
            raise Fail(f"at most {MAX_GNSS} receiver packages")
        models = [(g["model"], g["build"]) for g in manifest["gnss"]]
        if len(set(models)) != len(models):
            raise Fail("the same model and build twice")
        names = [manifest["app"]["asset"]] + [g["asset"] for g in manifest["gnss"]] + [args.out.name]
        if len(set(names)) != len(names):
            raise Fail("two release assets with the same name")
        check_asset_name(args.out.name)

        payload = (json.dumps(manifest, indent=2) + "\n").encode()
        if args.sign_key is not None:
            point = public_point(args.sign_key)
            envelope = {
                "payload": base64.b64encode(payload).decode(),
                "signature": base64.b64encode(sign(payload, args.sign_key)).decode(),
            }
            text = (json.dumps(envelope, indent=2) + "\n").encode()
        else:
            point = None
            text = payload

        if cfg is not None:
            build_repo = cfg.get("FW_UPDATE_GITHUB_REPO", "")
            if args.move_to:
                if args.repo is not None and args.move_to.lower() == args.repo.lower():
                    raise Fail(f"--move-to {args.move_to!r} is the repository this release is published on")
                if build_repo.lower() != args.move_to.lower():
                    raise Fail(f"this build reads releases from {build_repo!r}, not {args.move_to!r} (--move-to)")
                print(f"note: moving release: stations that install it read releases from {build_repo}")
            elif args.repo is not None and build_repo.lower() != args.repo.lower():
                raise Fail(f"this build reads releases from {build_repo!r}, but it is being released on "
                           f"{args.repo!r}: set CONFIG_FW_UPDATE_GITHUB_REPO")
            if not REPO_RE.match(build_repo) or build_repo.split("/")[1] in (".", ".."):
                raise Fail(f"CONFIG_FW_UPDATE_GITHUB_REPO {build_repo!r} is not owner/name: stations would not check")
            if cfg.get("FW_UPDATE_MANIFEST_ASSET") != args.out.name:
                raise Fail(f"the build looks for {cfg.get('FW_UPDATE_MANIFEST_ASSET')!r}, not {args.out.name!r}")
            limit = int(cfg.get("FW_UPDATE_MANIFEST_MAX_KB", 8)) * 1024
            if len(text) > limit:
                raise Fail(f"manifest of {len(text)} bytes, the build reads at most {limit}")
            keys = [key_hex(cfg.get(k)) for k in ("FW_UPDATE_MANIFEST_KEY_1", "FW_UPDATE_MANIFEST_KEY_2")]
            keys = [k for k in keys if k]
            if keys and point is None:
                raise Fail("this build accepts signed manifests only (CONFIG_FW_UPDATE_MANIFEST_KEY_1/_2): "
                           "pass --sign-key (the MANIFEST_SIGNING_KEY secret in CI)")
            if keys and point.hex() not in keys:
                raise Fail("the signing key is not one of the build's manifest keys")
            if point is not None and not keys:
                print("note: signed, but this build has no manifest key: stations do not check the signature")

        args.out.write_bytes(text)
    except (Fail, OSError, ValueError) as e:
        print(f"release manifest: {e}", file=sys.stderr)
        return 1

    form = "signed" if point is not None else "plain (no signing key)"
    print(f"{args.out}: {args.version}, {form}")
    print(f"  {manifest['app']['asset']}: {manifest['app']['size']} bytes, sha256 {manifest['app']['sha256']}")
    for g in manifest["gnss"]:
        print(f"  {g['asset']}: {g['model']} build {g['build']}, {g['size']} bytes, sha256 {g['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
