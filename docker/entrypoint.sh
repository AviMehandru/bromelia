#!/bin/sh
# Starts bromelia-daemon. On the first run it writes a configuration that saves to /output and serves the web page on
# port 51280 (which needs BROMELIA_WEB_TOKEN, because the container's network is not only this computer).
set -e

config="${XDG_CONFIG_HOME:-/config}/bromelia/config.json"
if [ ! -f "$config" ]; then
  mkdir -p "$(dirname "$config")"
  cat > "$config" <<'EOF'
{
  "outputRoot": "/output",
  "webUI": { "enabled": true, "address": "0.0.0.0", "port": 51280 }
}
EOF
  echo "Wrote $config; edit it (or use the Bromelia app's configuration) to set up drives and automatic rips."
fi

if [ -z "$BROMELIA_WEB_TOKEN" ]; then
  echo "BROMELIA_WEB_TOKEN is not set: the web page stays off unless the configuration has a token."
fi

exec bromelia-daemon --config "$config" "$@"
