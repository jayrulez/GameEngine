#!/usr/bin/env python3
# Usage: scripts/pack-samples.py <version> <out-dir>
#
# Packs the sample games for a release, as Data/SampleProjects/Samples.json lists them: for each, a
# zip of what git tracks in its folder less Tools/ (the authoring scripts), its Project.xml at the
# zip's root (the editor extracts it into the folder the user chooses), as sample-<folder>.zip;
# its picture as sample-<folder>.png; and samples.json naming them, with each zip's size and
# SHA-256 and the bytes it unpacks to. The editor of the same
# version reads samples.json from its release (EngineReleaseDownloadUrl) for its Samples tab.
# Run from the repository root, in a checkout (the release workflow's), so only tracked files go.
import hashlib, json, os, shutil, subprocess, sys, zipfile

if len(sys.argv) != 3:
    sys.exit("usage: scripts/pack-samples.py <version> <out-dir>")
version, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
manifest = json.load(open("Data/SampleProjects/Samples.json", encoding="utf-8"))

listed = []
for sample in manifest["samples"]:
    folder = sample["folder"]
    root = f"Data/SampleProjects/{folder}"
    files = subprocess.run(["git", "ls-files", "-z", "--", root], check=True, capture_output=True).stdout.decode("utf-8").split("\0")
    files = sorted(f for f in files if f and not f.startswith(f"{root}/Tools/"))
    if not any(f == f"{root}/Project.xml" for f in files):
        sys.exit(f"{folder}: no Project.xml tracked")
    archive = f"sample-{folder}.zip"
    path = os.path.join(out, archive)
    unpacked = 0
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            name = f[len(root) + 1:]
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0)) # fixed: the same files pack the same
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            data = open(f, "rb").read()
            unpacked += len(data)
            z.writestr(info, data, compresslevel=9)
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    picture = f"sample-{folder}.png"
    shutil.copyfile(sample["picture"], os.path.join(out, picture))
    listed.append({
        "folder": folder,
        "name": sample["name"],
        "description": sample["description"],
        "archive": archive,
        "size": os.path.getsize(path),
        "unpackedSize": unpacked,
        "sha256": digest,
        "picture": picture,
    })
    print(f"{archive}: {len(files)} files, {os.path.getsize(path) // 1024} KB packed, {unpacked // 1024} KB unpacked")

json.dump({"version": version, "samples": listed}, open(os.path.join(out, "samples.json"), "w", encoding="utf-8"), indent=2)
print(f"samples.json: {len(listed)} samples for {version}")
