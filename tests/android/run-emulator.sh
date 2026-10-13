#!/usr/bin/env bash
set -euo pipefail
build_dir=${1:?Pass the Android build directory}
mapfile -t apks < <(find "$build_dir/android-build/build/outputs/apk" -name '*.apk' -type f)
if [[ ${#apks[@]} != 1 ]]; then
    echo "Expected exactly one test APK" >&2
    exit 1
fi
adb install -t "${apks[0]}"
adb shell run-as org.yolopdf.app rm -f files/stage1.pdf files/stage1-complete
adb logcat -c
adb shell am start -n org.yolopdf.app/org.qtproject.qt.android.bindings.QtActivity \
    --es applicationArguments '--fixture-spike --smoke-test --save-as content://org.yolopdf.app.stage1/document/stage1.pdf'
found=false
for attempt in {1..40}; do
    if adb shell run-as org.yolopdf.app test -s files/stage1-complete; then
        found=true
        break
    fi
    sleep 1
done
if [[ "$found" != true ]]; then
    adb logcat -d -s Qt:D yolopdf:D AndroidRuntime:E
    exit 1
fi
adb shell run-as org.yolopdf.app cat files/stage1.pdf > "$build_dir/stage1-saf.pdf"
qpdf --check "$build_dir/stage1-saf.pdf"
pdftotext "$build_dir/stage1-saf.pdf" "$build_dir/stage1-saf.txt"
python3 - "$build_dir/stage1-saf.txt" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
assert 'éè' in text and 'Hello éééé' in text, text
print('Android engine replacement and document-provider save verified')
PY
stage="$build_dir/native-stage"
mkdir -p "$stage"
unzip -q -o "${apks[0]}" 'lib/x86_64/*' -d "$stage/apk"
cp "$build_dir/yolopdf-cli" "$stage/"
cp "$stage"/apk/lib/x86_64/*.so "$stage/"
python3 tests/corpus/generate.py "$stage/corpus"
adb shell mkdir -p /data/local/tmp/yolopdf-stage1
adb push "$stage/yolopdf-cli" "$stage/corpus" /data/local/tmp/yolopdf-stage1/
for library in "$stage"/*.so; do adb push "$library" /data/local/tmp/yolopdf-stage1/; done
adb shell chmod 755 /data/local/tmp/yolopdf-stage1/yolopdf-cli
for fixture in subset-latin subset-cjk; do
    if [[ "$fixture" == subset-latin ]]; then replacement='éè'; else replacement='日文'; fi
    adb shell "LD_LIBRARY_PATH=/data/local/tmp/yolopdf-stage1 /data/local/tmp/yolopdf-stage1/yolopdf-cli replace-text /data/local/tmp/yolopdf-stage1/corpus/$fixture.pdf /data/local/tmp/yolopdf-stage1/$fixture-edited.pdf --text '$replacement'"
    adb pull "/data/local/tmp/yolopdf-stage1/$fixture-edited.pdf" "$build_dir/"
    qpdf --check "$build_dir/$fixture-edited.pdf"
    pdftotext "$build_dir/$fixture-edited.pdf" "$build_dir/$fixture-edited.txt"
    python3 - "$build_dir/$fixture-edited.txt" "$replacement" <<'PY'
from pathlib import Path
import sys
assert sys.argv[2] in Path(sys.argv[1]).read_text()
PY
done
