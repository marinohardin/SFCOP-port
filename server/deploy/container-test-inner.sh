#!/bin/sh
# Runs inside the container (server/deploy/test-container.sh starts it): installs the lobby with
# deploy.sh on a fresh Debian, then checks what the deploy promised. The repository is mounted
# read-only at /src. Prints "container test: ok" last, or the first failure and exit status 1.
set -eu

fail() { echo "container test: FAIL: $*" >&2; exit 1; }
src=/src

echo "== deploy (first run)"
sh "$src/server/deploy/deploy.sh" --no-systemd > /tmp/deploy1.log 2>&1 || { tail -n 30 /tmp/deploy1.log; fail "deploy.sh"; }
tail -n 3 /tmp/deploy1.log

echo "== what was installed"
[ -x /opt/sfcop/bin/sfcop-lobby ] || fail "no binary"
[ -x /opt/sfcop/bin/sfcop-relay ] || fail "no relay binary"
for s in directory auth; do
    [ -x /opt/sfcop/bin/sfcop-$s ] || fail "no $s binary"
    [ "$(stat -c '%a %U:%G' /etc/sfcop/$s.env)" = "640 root:sfcop" ] || fail "$s env file mode/owner"
    [ -f /etc/systemd/system/sfcop-$s.service ] || fail "no $s unit"
done
[ "$(stat -c '%a %U:%G' /etc/sfcop/campaigns.txt)" = "640 root:sfcop" ] || fail "campaign file mode/owner"
[ "$(stat -c '%a %U:%G' /etc/sfcop/relay.env)" = "640 root:sfcop" ] || fail "relay env file mode/owner"
[ "$(stat -c '%a' /var/log/sfcop)" = 750 ] || fail "log directory mode"
[ -x /opt/sfcop/bin/sfcop-health-check ] || fail "no health check"
[ "$(stat -c '%a %U:%G' /etc/sfcop/lobby.env)" = "640 root:sfcop" ] || fail "env file mode/owner: $(stat -c '%a %U:%G' /etc/sfcop/lobby.env)"
[ "$(stat -c '%a %U' /etc/sfcop/sfcop.env)" = "600 root" ] || fail "shared env file mode/owner"
grep -q '^SFCOP_TICKET_KEY=' /etc/sfcop/lobby.env && fail "the key is in lobby.env"
key=$(sed -n 's/^SFCOP_TICKET_KEY=//p' /etc/sfcop/sfcop.env)
[ "${#key}" = 64 ] || fail "ticket key is not 64 digits"
for f in /etc/systemd/system/sfcop-lobby.service /etc/systemd/system/sfcop-relay.service /etc/systemd/system/sfcop-health@.service \
         /etc/systemd/system/sfcop-health@.timer /etc/logrotate.d/sfcop; do
    [ -f "$f" ] || fail "missing $f"
done
[ "$(stat -c '%U' /var/log/sfcop)" = sfcop ] || fail "log directory owner"
grep -q 'rotate 7' /etc/logrotate.d/sfcop || fail "logrotate does not keep 7"
logrotate -d /etc/logrotate.d/sfcop > /tmp/logrotate.out 2>&1 || { cat /tmp/logrotate.out; fail "logrotate rejects the rule"; }
grep -q '/var/log/sfcop' /tmp/logrotate.out || fail "logrotate dry run does not consider the log directory"
echo "ok"

echo "== unit files verify"
if apt-get install -y -qq --no-install-recommends systemd > /dev/null 2>&1 && command -v systemd-analyze > /dev/null; then
    systemd-analyze verify /etc/systemd/system/sfcop-lobby.service /etc/systemd/system/sfcop-relay.service /etc/systemd/system/sfcop-directory.service /etc/systemd/system/sfcop-auth.service 'sfcop-health@relay.service' 'sfcop-health@directory.service' 'sfcop-health@auth.service' 'sfcop-health@lobby.service' 'sfcop-health@lobby.timer' 2>&1 | tee /tmp/verify.out
    echo "systemd-analyze verify: no problems reported"
    grep -qi 'sfcop' /tmp/verify.out && grep -qiE 'error|invalid|bad|unknown' /tmp/verify.out && fail "systemd-analyze reported a problem"
else
    echo "skip: systemd-analyze not installable here"
fi

echo "== deploy again changes nothing it should keep"
before=$(sha256sum /etc/sfcop/lobby.env /etc/sfcop/relay.env /etc/sfcop/sfcop.env /etc/sfcop/directory.env /etc/sfcop/auth.env /etc/sfcop/campaigns.txt)
sh "$src/server/deploy/deploy.sh" --no-systemd > /tmp/deploy2.log 2>&1 || { tail -n 30 /tmp/deploy2.log; fail "second deploy.sh"; }
[ "$before" = "$(sha256sum /etc/sfcop/lobby.env /etc/sfcop/relay.env /etc/sfcop/sfcop.env /etc/sfcop/directory.env /etc/sfcop/auth.env /etc/sfcop/campaigns.txt)" ] || fail "second deploy rewrote an env file"
grep -q 'kept /etc/sfcop/lobby.env' /tmp/deploy2.log && grep -q 'kept the ticket key' /tmp/deploy2.log || fail "second deploy did not say it kept the env files"
echo "ok"

echo "== the placeholder host is refused at start-up"
if runas_probe=$(SFCOP_TICKET_KEY=$key su -p -s /bin/sh sfcop -c "set -a; . /etc/sfcop/lobby.env; set +a; /opt/sfcop/bin/sfcop-lobby" 2>&1); then fail "started with the placeholder host"; fi
echo "$runas_probe" | grep -q 'SFCOP_RENDEZVOUS_HOST' || fail "no clear message: $runas_probe"
sed -i 's/^SFCOP_RENDEZVOUS_HOST=.*/SFCOP_RENDEZVOUS_HOST=relay.test.invalid/' /etc/sfcop/lobby.env
echo "ok"

echo "== run the service as sfcop and talk to it"
touch /tmp/alerts; chmod 666 /tmp/alerts
# the shared key file is root-only, as under systemd: read it as root, then drop to sfcop
runas() { SFCOP_TICKET_KEY=$key su -p -s /bin/sh sfcop -c "set -a; . /etc/sfcop/lobby.env; set +a; $*"; }
runas '/opt/sfcop/bin/sfcop-lobby >> /var/log/sfcop/lobby.log 2>&1 &'
sleep 1
health=$(curl -fsS http://127.0.0.1:47712/v1/health) || fail "health did not answer"
echo "$health" | grep -q '"status":"ok"' || fail "health body: $health"
reg=$(curl -fsS -X POST http://127.0.0.1:8080/v1/sessions \
    -d '{"protocol":1,"application":"{0AB1C531-4745-11D1-A7A1-0000F803ABFC}","name":"TestCaptain","flags":0,"max_players":4,"current_players":1,"user":[0,0,0,0],"password_required":false}') \
    || fail "register"
echo "$reg" | grep -q '"session_id":"' || fail "register body"
list=$(curl -fsS 'http://127.0.0.1:8080/v1/sessions?protocol=1&application=%7B0AB1C531-4745-11D1-A7A1-0000F803ABFC%7D') || fail "list"
echo "$list" | grep -q '"name":"TestCaptain"' || fail "list body: $list"
echo "ok (health, register, list)"

echo "== the log carries no address, name or secret"
sleep 0.5
grep -q 'registered, 1 players' /var/log/sfcop/lobby.log || fail "no registration line"
if grep -E '127\.0\.0\.1|TestCaptain' /var/log/sfcop/lobby.log > /dev/null; then fail "an address or a name is in the log"; fi
secret=$(echo "$reg" | sed -n 's/.*"host_secret":"\([^"]*\)".*/\1/p')
if grep -F "$secret" /var/log/sfcop/lobby.log > /dev/null; then fail "the host secret is in the log"; fi
echo "ok"

echo "== the monitor: one message per failure, quiet while it lasts, armed again after recovery"
check() {
    runas "SFCOP_ALERT_COMMAND='echo \"\$SFCOP_ALERT_MESSAGE\" >> /tmp/alerts' /opt/sfcop/bin/sfcop-health-check lobby" > /dev/null 2>&1
}
check || fail "health check failed while the service is up"
[ ! -s /tmp/alerts ] || fail "an alert was sent while healthy"
pkill -TERM sfcop-lobby
sleep 1
check && fail "health check passed with the service down"
check && fail "health check passed with the service down (second)"
[ "$(wc -l < /tmp/alerts)" = 1 ] || fail "expected exactly one alert, got $(wc -l < /tmp/alerts)"
grep -q 'lobby failed its health check' /tmp/alerts || fail "alert text"
runas '/opt/sfcop/bin/sfcop-lobby >> /var/log/sfcop/lobby.log 2>&1 &'
sleep 1
check || fail "health check failed after recovery"
pkill -TERM sfcop-lobby
sleep 1
check && fail "health check passed with the service down again"
[ "$(wc -l < /tmp/alerts)" = 2 ] || fail "expected a second alert after recovery, got $(wc -l < /tmp/alerts)"
echo "ok"

echo "== the relay: run as sfcop, two peers and a stranger over loopback UDP, the lobby's removal call"
runasr() { SFCOP_TICKET_KEY=$key su -p -s /bin/sh sfcop -c "set -a; . /etc/sfcop/relay.env; set +a; $*"; }
runasr '/opt/sfcop/bin/sfcop-relay --blocklist /etc/sfcop/relay-blocklist.txt >> /var/log/sfcop/relay.log 2>&1 &'
sleep 1
python3 "$src/server/deploy/relay-peers.py" || { cat /var/log/sfcop/relay.log; fail "relay peers"; }
echo "== the monitor sees the relay too"
checkr() { runasr "/opt/sfcop/bin/sfcop-health-check relay" > /dev/null 2>&1; }
checkr || fail "relay health check failed while up"
pkill -TERM sfcop-relay
sleep 1
grep -q 'relay: stopped' /var/log/sfcop/relay.log || fail "relay did not stop cleanly"
if grep -E '127\.0\.0\.1|5346434f50000001|SFCOP_TICKET_KEY' /var/log/sfcop/relay.log > /dev/null; then fail "the relay log names an address, a session or the key"; fi
echo "ok"

echo "== the Dynaverse stand-ins: run as sfcop, a directory query, a detection, a version check, a new account and a logon"
# The directory refuses to start without the host's public address.
if su -p -s /bin/sh sfcop -c "set -a; . /etc/sfcop/directory.env; set +a; /opt/sfcop/bin/sfcop-directory" > /tmp/dir-probe.out 2>&1; then
    fail "the directory started without SFCOP_PUBLIC_ADDRESS"
fi
grep -q 'SFCOP_PUBLIC_ADDRESS' /tmp/dir-probe.out || fail "no clear message: $(cat /tmp/dir-probe.out)"
sed -i 's/^#SFCOP_PUBLIC_ADDRESS=.*/SFCOP_PUBLIC_ADDRESS=192.0.2.20/' /etc/sfcop/directory.env
runasd() { su -p -s /bin/sh sfcop -c "set -a; . /etc/sfcop/$1.env; set +a; $2"; }
runasd directory '/opt/sfcop/bin/sfcop-directory >> /var/log/sfcop/directory.log 2>&1 &'
# Port 80 needs the capability the unit grants; here (no systemd) the probe runs as root instead.
set -a; . /etc/sfcop/auth.env; set +a
/opt/sfcop/bin/sfcop-auth >> /var/log/sfcop/auth.log 2>&1 &
sleep 1
python3 "$src/server/deploy/dynaverse-probe.py" || { cat /var/log/sfcop/directory.log /var/log/sfcop/auth.log; fail "dynaverse probe"; }
for s in directory auth; do
    runasd $s "/opt/sfcop/bin/sfcop-health-check $s" > /dev/null 2>&1 || fail "$s health check failed while up"
done
curl -fsS http://127.0.0.1:47714/v1/health | grep -q '"accounts":1' || fail "auth health does not count the account"
pkill -TERM sfcop-directory
pkill -TERM sfcop-auth
sleep 1
grep -q 'directory: stopped' /var/log/sfcop/directory.log || fail "directory did not stop cleanly"
if grep -E 'throwaway|8957a7e865c41c02e58f18bcd7c9f2b9|testcaptain|127\.0\.0\.1' /var/log/sfcop/auth.log /var/log/sfcop/directory.log > /dev/null; then
    fail "a password, hash, email or address is in a stand-in's log"
fi
echo "ok"

echo "container test: ok"
