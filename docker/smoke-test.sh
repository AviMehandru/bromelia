#!/bin/sh
# Checks a built image without a drive: every program the daemon calls is there and loads its libraries, MakeMKV's
# library has its LGPL FFmpeg built in rather than Debian's GPL one, the first
# start writes the configuration, the web page answers with its token (and refuses without it), makemkvcon runs, and
# `docker stop` ends the daemon cleanly. CI runs it after building; run it after your own builds the same way:
#
#   docker build -f docker/Dockerfile --build-arg ACCEPT_MAKEMKV_EULA=yes -t bromelia .
#   docker/smoke-test.sh bromelia
set -eu

image="${1:-bromelia}"
name="bromelia-smoke-$$"
port="${SMOKE_PORT:-51280}"
token="smoke-$$"
config="$(mktemp -d)"
output="$(mktemp -d)"
chmod 777 "$config" "$output"

fail() { echo "FAIL: $*"; docker logs "$name" 2>&1 | sed 's/^/  log: /' || true; exit 1; }
cleanup() { docker rm -f "$name" >/dev/null 2>&1 || true; rm -rf "$config" "$output" 2>/dev/null || true; }
trap cleanup EXIT

echo "== Programs and libraries"
docker run --rm --entrypoint sh "$image" -ec '
  for p in makemkvcon bromelia-daemon mkvmerge ffmpeg tesseract eject udevadm curl abcde cdparanoia; do
    command -v "$p" >/dev/null || { echo "missing: $p"; exit 1; }
  done
  missing=$(for p in /usr/bin/makemkvcon /usr/bin/bromelia-daemon; do ldd "$p"; done | grep "not found" || true)
  [ -z "$missing" ] || { echo "libraries not found:"; echo "$missing"; exit 1; }
  # The MakeMKV library has its own LGPL FFmpeg built in (docs/docker.md, "Licences"). Debian ships a GPL build of
  # libavcodec, and linking that to the closed makemkvcon would make an image that cannot be published.
  lib=/usr/lib/libmakemkv.so.1
  if ldd "$lib" | grep -q "libav"; then echo "$lib links a shared FFmpeg library"; exit 1; fi
  grep -aq "LGPL version 2.1 or later" "$lib" || { echo "$lib has no LGPL FFmpeg in it"; exit 1; }
  if grep -aq -- "--enable-gpl" "$lib"; then echo "$lib has a GPL FFmpeg in it"; exit 1; fi
  [ -f /usr/share/doc/makemkv/ffmpeg/COPYING.LGPLv2.1 ] || { echo "the FFmpeg licence is not in the image"; exit 1; }
  echo "libmakemkv: FFmpeg built in, LGPL"
  if command -v cyanrip >/dev/null; then echo "cyanrip: yes"; else echo "cyanrip: not in this distribution (abcde rips audio CDs)"; fi
  bromelia-daemon --version
  # With no drive, makemkvcon still starts, prints its version and scans; that proves its own libraries load.
  makemkvcon -r --cache=1 info disc:9999 | grep -m1 "^MSG:1005"
' || fail "the image is missing a program or a library"

echo "== First start"
docker run -d --name "$name" -e BROMELIA_WEB_TOKEN="$token" -p "127.0.0.1:$port:51280" \
  -v "$config:/config" -v "$output:/output" "$image" >/dev/null
for _ in $(seq 1 60); do
  curl -fsS -m 2 -H "Authorization: Bearer $token" "http://127.0.0.1:$port/api/status" -o "$config/status.json" 2>/dev/null && break
  sleep 1
done
[ -s "$config/status.json" ] || fail "the web page did not answer within 60 seconds"
[ -f "$config/bromelia/config.json" ] || fail "the first start wrote no configuration"
grep -q '"outputRoot": "/output"' "$config/bromelia/config.json" || fail "the configuration doesn't save to /output"
echo "status: $(cat "$config/status.json")"

echo "== Web page"
code() { curl -s -o /dev/null -w '%{http_code}' -m 5 "$@"; }
[ "$(code "http://127.0.0.1:$port/api/status")" = 401 ] || fail "no token should give 401"
[ "$(code "http://127.0.0.1:$port/?token=$token")" = 200 ] || fail "the page with ?token= should give 200"
curl -fsS -m 5 "http://127.0.0.1:$port/?token=$token" | grep -q "<title>Bromelia</title>" || fail "the page is not the web page"
[ "$(code -X POST "http://127.0.0.1:$port/api/verify?token=$token")" = 403 ] || fail "POST without X-Bromelia should give 403"
[ "$(code -X POST -H 'X-Bromelia: 1' "http://127.0.0.1:$port/api/verify?token=$token")" = 204 ] || fail "the archive check should start"

echo "== MakeMKV from the daemon"
for _ in $(seq 1 30); do
  curl -fsS -m 2 -H "Authorization: Bearer $token" "http://127.0.0.1:$port/api/status" | grep -q '"makemkv" *: *"MakeMKV v' && break
  sleep 1
done
curl -fsS -m 2 -H "Authorization: Bearer $token" "http://127.0.0.1:$port/api/status" | grep -q '"makemkv" *: *"MakeMKV v' \
  || fail "the daemon never read makemkvcon's version (drive scans don't work)"

echo "== docker stop"
docker stop -t 20 "$name" >/dev/null
[ "$(docker inspect -f '{{.State.ExitCode}}' "$name")" = 0 ] || fail "the daemon did not exit cleanly on docker stop"

docker logs "$name" 2>&1 | sed 's/^/  log: /'
echo "Smoke test passed."
