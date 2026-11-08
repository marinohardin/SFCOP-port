#!/bin/sh
# Installs the SFCOP hosted services on a fresh Debian or Ubuntu host (DESIGN §10). Run as root
# from a checkout of the repository:
#
#   sudo sh server/deploy/deploy.sh                 install, build and start the lobby
#   sudo SFCOP_LOBBY_DOMAIN=lobby.example.test sh server/deploy/deploy.sh
#                                                   also install Caddy as the TLS front
#   sh server/deploy/deploy.sh --no-systemd         install only (a container, or a host without
#                                                   systemd); nothing is enabled or started
#
# What it does, in order, and all of it safe to run again:
#   1. installs the build tools and curl and logrotate with apt;
#   2. builds sfcop-lobby from server/ into /opt/sfcop/bin (no tests, so no download);
#   3. creates the sfcop system user, /etc/sfcop and /var/log/sfcop;
#   4. creates /etc/sfcop/lobby.env from lobby.env.example the first time (mode 640) and, only if
#      absent, the ticket key in /etc/sfcop/sfcop.env (mode 600), the file the relay shares;
#      existing files are never touched;
#   5. installs the service unit, the health-check script with its unit and timer, and the
#      logrotate rule (daily, 7 days);
#   6. with SFCOP_LOBBY_DOMAIN set, installs Caddy and a Caddyfile for that name;
#   7. enables and starts the service and its health timer.
# Services other than the lobby (the relay) add their own unit, env example and a line in the
# SERVICES list below; the health check, the user, the log directory and the rotation are shared.
#
# Secrets: the ticket key is written to sfcop.env and never printed. The owner's alert
# command goes in the same file by hand.
set -eu

NO_SYSTEMD=0
for arg in "$@"; do
    case $arg in
    --no-systemd) NO_SYSTEMD=1 ;;
    *) echo "deploy: unknown argument $arg" >&2; exit 2 ;;
    esac
done

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
prefix=${SFCOP_PREFIX:-/opt/sfcop}
etc=${SFCOP_ETC:-/etc/sfcop}
logs=${SFCOP_LOGS:-/var/log/sfcop}
unitdir=${SFCOP_UNIT_DIR:-/etc/systemd/system}
builddir=${SFCOP_BUILD_DIR:-/var/lib/sfcop/build}

# The services this script installs. N4 appends "relay" here with its unit and env example.
SERVICES="lobby"

if [ "$(id -u)" != 0 ]; then
    echo "deploy: run as root" >&2
    exit 1
fi
if ! command -v apt-get >/dev/null 2>&1; then
    echo "deploy: this script supports Debian and Ubuntu (apt-get not found)" >&2
    exit 1
fi

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build curl logrotate openssl ca-certificates >/dev/null

echo "== build"
mkdir -p "$builddir" "$prefix/bin"
cmake -S "$root/server" -B "$builddir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSFCOP_SERVER_TESTS=OFF >/dev/null
cmake --build "$builddir" -j2
install -m 755 "$builddir/lobby/sfcop-lobby" "$prefix/bin/sfcop-lobby"
install -m 755 "$here/sfcop-health-check.sh" "$prefix/bin/sfcop-health-check"

echo "== user and directories"
if ! id sfcop >/dev/null 2>&1; then
    useradd --system --home-dir /var/lib/sfcop --shell /usr/sbin/nologin sfcop
fi
install -d -m 750 -o root -g sfcop "$etc"
install -d -m 750 -o sfcop -g sfcop "$logs"
install -d -m 750 -o sfcop -g sfcop /var/lib/sfcop

echo "== configuration"
# The ticket key is shared with the relay, whose install.sh uses the same file and variable and
# adds the key only if absent: so it is written here only if absent, 64 hex digits, mode 600.
shared=$etc/sfcop.env
if grep -qs '^SFCOP_TICKET_KEY=[0-9a-fA-F]\{64\}$' "$shared"; then
    echo "kept the ticket key in $shared"
elif grep -qs '^SFCOP_TICKET_KEY' "$shared"; then
    echo "deploy: $shared has a SFCOP_TICKET_KEY line that is not 64 hex digits; fix or remove it" >&2
    exit 1
else
    umask 077
    # a file that does not end in a newline must not have the key glued to its last line
    if [ -s "$shared" ] && [ -n "$(tail -c1 "$shared")" ]; then echo >> "$shared"; fi
    printf 'SFCOP_TICKET_KEY=%s\n' "$(openssl rand -hex 32)" >> "$shared"
    chmod 600 "$shared"
    echo "wrote a new ticket key to $shared"
fi
for s in $SERVICES; do
    envfile=$etc/$s.env
    if [ -e "$envfile" ]; then
        echo "kept $envfile"
        continue
    fi
    umask 027
    cp "$here/$s.env.example" "$envfile"
    chown root:sfcop "$envfile"
    chmod 640 "$envfile"
    echo "created $envfile (edit SFCOP_RENDEZVOUS_HOST and SFCOP_ALERT_COMMAND)"
done

echo "== units and log rotation"
install -d "$unitdir"
for s in $SERVICES; do
    install -m 644 "$here/sfcop-$s.service" "$unitdir/sfcop-$s.service"
done
install -m 644 "$here/sfcop-health@.service" "$unitdir/sfcop-health@.service"
install -m 644 "$here/sfcop-health@.timer" "$unitdir/sfcop-health@.timer"
install -d /etc/logrotate.d
install -m 644 "$here/sfcop.logrotate" /etc/logrotate.d/sfcop

if [ -n "${SFCOP_LOBBY_DOMAIN:-}" ]; then
    echo "== caddy (TLS front) for the configured domain"
    apt-get install -y -qq --no-install-recommends caddy >/dev/null
    install -d /etc/caddy
    install -m 644 "$here/Caddyfile.example" /etc/caddy/Caddyfile
    install -d /etc/systemd/system/caddy.service.d
    printf '[Service]\nEnvironment=SFCOP_LOBBY_DOMAIN=%s\n' "$SFCOP_LOBBY_DOMAIN" \
        > /etc/systemd/system/caddy.service.d/sfcop.conf
fi

if [ "$NO_SYSTEMD" = 1 ]; then
    echo "== done (--no-systemd: nothing enabled or started)"
    exit 0
fi

echo "== start"
systemctl daemon-reload
if grep -qs '^SFCOP_RENDEZVOUS_HOST=lobby.example.test' "$etc/lobby.env"; then
    echo "deploy: edit SFCOP_RENDEZVOUS_HOST in $etc/lobby.env (the relay's public name), then:"
    echo "        systemctl enable --now sfcop-lobby.service sfcop-health@lobby.timer"
    exit 0
fi
for s in $SERVICES; do
    systemctl enable sfcop-"$s".service "sfcop-health@$s.timer" >/dev/null
    systemctl restart sfcop-"$s".service
    systemctl restart "sfcop-health@$s.timer"
done
if [ -n "${SFCOP_LOBBY_DOMAIN:-}" ]; then
    systemctl enable caddy >/dev/null
    systemctl restart caddy
fi
echo "== done"
