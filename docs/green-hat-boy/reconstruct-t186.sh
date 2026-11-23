#!/bin/bash
set -euo pipefail
bundle=$(cd -- "$(dirname -- "$0")" && pwd)
work=${1:?Pass a new target directory}
mkdir "$work"
cd "$work"
anchor=$(cat "$bundle/anchor")
shared=$(cat "$bundle/shared-revision")
git init -q exact
git -C exact fetch --depth 1 https://github.com/Vampinity/Eglantine-PS5-.git "$anchor"
git -C exact checkout --detach FETCH_HEAD
for pair in 120:134 134:180 180:181 181:182 182:183 183:184 184:185 185:186; do
    from=${pair%:*}
    to=${pair#*:}
    file="t$from-to-t$to.diff"
    curl -fsSL "https://raw.githubusercontent.com/Vampinity/GreenHatBoy/$shared/source/T$to/$file" -o "$file"
done
shasum -a 256 -c "$bundle/source-chain.sha256"
for pair in 120:134 134:180 180:181 181:182 182:183 183:184 184:185 185:186; do
    git -C exact apply --index --whitespace=nowarn "$PWD/t${pair%:*}-to-t${pair#*:}.diff"
done
[[ "$(git -C exact write-tree)" = b61e7cd93a7f6970a8ea7a59f57c7a1e25394df6 ]]
for patch in w39-mic-outer mic-start position-recorder minimap-steering; do
    git -C exact apply --index --whitespace=nowarn "$bundle/patches/$patch.patch"
done
cp "$bundle/patches/1211-up-flash-count.patch" exact/tools/eglantine-patches/
cp "$bundle/patches/depth-feedback-recheck.patch" exact/tools/eglantine-patches/1290-wave-depth-feedback-recheck.patch
cp "$bundle/test-flash-counter.cpp" exact/tools/
printf 'Prepared source at %s\n' "$PWD/exact"
