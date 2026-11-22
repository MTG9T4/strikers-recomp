#!/bin/sh
# Install or update the Strikers relay on a Linux server with systemd and python3 (Ubuntu 22.04+, Debian 12+).
#   sh deploy.sh user@server
# Copies strikers_relay.py and the systemd unit, (re)starts the service and opens UDP 24890 in ufw if ufw is active.
set -eu
target=${1:?usage: sh deploy.sh user@server}
here=$(cd "$(dirname "$0")" && pwd)
scp "$here/strikers_relay.py" "$here/strikers-relay.service" "$target:/tmp/"
ssh "$target" 'set -eu
  sudo install -d -m 755 /opt/strikers-relay
  sudo install -m 644 /tmp/strikers_relay.py /opt/strikers-relay/strikers_relay.py
  sudo install -m 644 /tmp/strikers-relay.service /etc/systemd/system/strikers-relay.service
  rm -f /tmp/strikers_relay.py /tmp/strikers-relay.service
  python3 -c "import sys; assert sys.version_info >= (3, 8), sys.version"
  if command -v ufw >/dev/null && sudo ufw status | grep -q "Status: active"; then sudo ufw allow 24890/udp; fi
  sudo systemctl daemon-reload
  sudo systemctl enable strikers-relay
  sudo systemctl restart strikers-relay
  sleep 1
  systemctl --no-pager --lines=5 status strikers-relay'
