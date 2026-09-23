#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${ANDROID_SDK_ROOT:-$HOME/android-sdk}
ndk=${NDK:-$sdk/ndk/29.0.14206865}
build_tools="$sdk/build-tools/35.0.0"
work=$(mktemp -d -t zb-preflight-XXXXXX)
trap 'rm -rf -- "$work"' EXIT

cat > "$work/AndroidManifest.xml" <<'EOF'
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="org.zettabridge.synthetic" android:versionCode="7" android:versionName="1.0">
    <uses-sdk android:minSdkVersion="23" android:targetSdkVersion="35"/>
    <application android:label="Synthetic">
        <activity android:name=".Main" android:exported="true"/>
    </application>
</manifest>
EOF
cat > "$work/synthetic.c" <<'EOF'
extern void* malloc(unsigned);
void* Java_org_zettabridge_synthetic_Main_probe(void) { return malloc(4); }
int JNI_OnLoad(void* vm, void* reserved) {
    (void)vm;
    (void)reserved;
    return 0x10006;
}
EOF

"$build_tools/aapt2" link -o "$work/plain.apk" \
    --manifest "$work/AndroidManifest.xml" -I "$sdk/platforms/android-35/android.jar"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi23-clang" \
    -shared -fPIC "$work/synthetic.c" -o "$work/libsynthetic.so"
python3 - "$work" <<'PY'
from pathlib import Path
import sys
import zipfile
p = Path(sys.argv[1])
with zipfile.ZipFile(p / "plain.apk") as source, zipfile.ZipFile(p / "unsigned.apk", "w") as out:
    for info in source.infolist():
        out.writestr(info, source.read(info.filename))
    out.write(p / "libsynthetic.so", "lib/armeabi-v7a/libsynthetic.so")
PY
keytool -genkeypair -alias synthetic -keystore "$work/test.p12" \
    -storetype PKCS12 -storepass changeit -keypass changeit -dname 'CN=Synthetic' \
    -keyalg RSA -keysize 2048 -validity 1 -noprompt >/dev/null 2>&1
"$build_tools/apksigner" sign --ks "$work/test.p12" --ks-pass pass:changeit \
    --out "$work/signed.apk" "$work/unsigned.apk"
before=$(sha256sum "$work/signed.apk" | cut -d ' ' -f 1)
python3 "$repo/tools/apk_preflight.py" --apksigner "$build_tools/apksigner" \
    "$work/signed.apk" > "$work/report.json"
after=$(sha256sum "$work/signed.apk" | cut -d ' ' -f 1)
test "$before" = "$after"
python3 - "$work/report.json" <<'PY'
import json
import sys
r = json.load(open(sys.argv[1], encoding="utf-8"))
f = r["files"][0]
lib = f["libraries"][0]
assert r["status"] == "analyzed"
assert r["package"] == "org.zettabridge.synthetic"
assert f["manifest"]["version_code"] == "7"
assert f["manifest"]["target_sdk"] == "35"
assert len(f["signer"]["cert_sha256"]) == 1
assert lib["class"] == 32 and lib["machine"] == 40
assert "libc.so" in lib["needed"] and "malloc" in lib["imports"]
assert "JNI_OnLoad" in lib["jni_hints"]
print("signed binary manifest/ARM32 ELF/signature/source-unchanged: PASS")
PY
set +e
python3 "$repo/tools/apk_preflight.py" --apksigner "$build_tools/apksigner" \
    "$work/unsigned.apk" > "$work/unsigned-report.json"
code=$?
set -e
test "$code" -eq 2
python3 - "$work/unsigned-report.json" <<'PY'
import json
import sys
r = json.load(open(sys.argv[1], encoding="utf-8"))
assert r["status"] == "invalid" and "signature" in r["error"].lower()
print("unsigned APK rejection: PASS")
PY
