# APK preflight (Step 02)

Run this read-only analyzer on one APK, an explicit complete split set, or one
`.apks` container. It does not import, install, transform, or execute APK code.
Use Python 3.11 or newer, Android SDK `apksigner` (build-tools 35.0.0 was tested),
and GNU `readelf`. The baseline environment is the WSL2 setup in
`docs/development.md`.

```sh
python3 tools/apk_preflight.py \
  --apksigner "$ANDROID_SDK_ROOT/build-tools/35.0.0/apksigner" \
  /path/to/input.apk > report.json

python3 tools/apk_preflight.py \
  --apksigner "$ANDROID_SDK_ROOT/build-tools/35.0.0/apksigner" \
  /path/to/base.apk /path/to/feature.apk > report.json
```

The report includes each input's SHA-256, verified signing certificate SHA-256,
verified signature schemes, manifest identity/SDK/components/permissions/features,
split relationships, native ABIs, ELF class/machine/attributes/dependencies and
dynamic symbols, JNI/native entry hints, and native-library compression/alignment.
Findings have a `level` (`convertible`, `warning`, `unsupported`) and a `category`
(`conversion`, `install_signing`, `runtime`). An `analyzed` result means preflight
accepted the input structure; it does not mean the current runtime can run it.
Exit code 0 means analyzed, and 2 means unsupported or invalid. An invalid
archive/signature gives an `invalid` report with the reason.

Split validation requires one base, unique split names, matching package,
version code/name and signer, plus all declared `uses-split` and `configForSplit`
dependencies. An `.apks` container with alternate base variants is rejected as
ambiguous. This does not prove that an optional, undeclared feature split exists
or that a particular device configuration has been selected. AAB is outside this
step. NativeActivity and shared-UID input are reported as unsupported under the
current runtime/signing contract; other runtime warnings need later steps.

The limits are 1 GiB compressed input/set, 512 MiB total expanded bytes, 128 MiB
per ZIP entry, 10,000 entries, 128 APK splits, 4 MiB binary manifest, 64 MiB
native library, and 32 MiB `readelf` output. ZIP paths, duplicates, symlinks,
unsupported compression/encryption, CRC, expansion ratio, manifest structure,
ELF ABI, and APK signatures are checked before a result is accepted. Container
members are staged only in a private temporary directory and removed afterward.
