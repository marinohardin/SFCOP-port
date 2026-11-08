#!/bin/sh
# The host's monitor for one service (DESIGN §10): the timer runs this every minute. A failed
# health request sends the owner ONE message, when the service goes from healthy to failing; it
# stays quiet while the failure lasts and arms itself again when the service answers.
#
#   sfcop-health-check <service>
#
# Environment (from /etc/sfcop/<service>.env through the unit):
#   SFCOP_HEALTH_URL       the URL to GET; default http://$SFCOP_HEALTH_LISTEN/v1/health
#   SFCOP_HEALTH_LISTEN    host:port of the loopback health listener, default 127.0.0.1:47712
#   SFCOP_ALERT_COMMAND    run with sh -c when the failure begins; it may use $SFCOP_ALERT_MESSAGE.
#                          Empty: the failure is only logged. It holds the owner's channel and
#                          is never printed here.
#   SFCOP_STATE_DIR        where the failure marker lives, default /var/lib/sfcop
# Exit status: 0 healthy, 1 failing (so that the unit's state shows it too).
set -u

service=${1:?usage: sfcop-health-check <service>}
url=${SFCOP_HEALTH_URL:-http://${SFCOP_HEALTH_LISTEN:-127.0.0.1:47712}/v1/health}
state=${SFCOP_STATE_DIR:-/var/lib/sfcop}
marker=$state/$service.failing

if curl -fsS --max-time 5 --output /dev/null "$url" 2>/dev/null; then
    rm -f "$marker"
    exit 0
fi

echo "sfcop-health: $service did not answer its health request"
if [ ! -e "$marker" ]; then
    : > "$marker"
    if [ -n "${SFCOP_ALERT_COMMAND:-}" ]; then
        SFCOP_ALERT_MESSAGE="sfcop: $service failed its health check" sh -c "$SFCOP_ALERT_COMMAND" \
            || echo "sfcop-health: the alert command failed"
    fi
fi
exit 1
