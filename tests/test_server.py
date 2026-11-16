"""The hosted services under server/ (N3, N4, V1): builds and runs each service's doctest runner, and tests the
deploy kit's shell scripts that do not need a container.

scripts/check.sh runs this file with the rest of tests/, so the service's C++ tests are run by
check.sh through pytest, with no change to the root CMake files. The container test of
deploy.sh (server/deploy/test-container.sh) downloads packages and is run by hand.

The runner is built standalone from server/CMakeLists.txt into a persistent directory under the
work area, one per checkout (or $SFCOP_SERVER_BUILD), so the second run only rebuilds what
changed and two worktrees never share a CMake cache. This is how
check.sh runs every service's tests: a new service adds its directory to server/CMakeLists.txt and
its tests are found by that build.

No network: doctest is taken from $SFCOP_DOCTEST_DIR or $FETCHCONTENT_SOURCE_DIR_DOCTEST when set,
else from a doctest an existing build tree under the work area has already fetched (the same
pinned release as third_party), else the test is skipped with a reason. It never downloads.
"""
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / "server"
DEPLOY = SERVER / "deploy"


def work_dir() -> Path:
    env = os.environ.get("SFCOP_WORK")
    if env:
        return Path(env)
    common = subprocess.run(
        ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
        cwd=ROOT, capture_output=True, text=True,
    )
    if common.returncode == 0:
        return Path(common.stdout.strip()).parent.parent / "work"
    return ROOT / "build"


def checkout_slug(root: Path = ROOT) -> str:
    """A short name for this checkout, from its absolute path, so each worktree gets its own build
    tree: a CMake cache remembers its source directory and refuses another checkout's."""
    digest = hashlib.sha256(str(root).encode("utf-8")).hexdigest()[:12]
    return root.name + "-" + digest


def build_dir() -> Path:
    env = os.environ.get("SFCOP_SERVER_BUILD")
    d = Path(env) if env else work_dir() / "server-tests" / checkout_slug()
    d.mkdir(parents=True, exist_ok=True)
    return d


def find_doctest():
    """The directory holding doctest/doctest.h, or None; looks at the environment, then at the
    first existing build tree's _deps under the work area. Nothing is fetched."""
    for name in ("SFCOP_DOCTEST_DIR", "FETCHCONTENT_SOURCE_DIR_DOCTEST"):
        d = os.environ.get(name)
        if d and (Path(d) / "doctest" / "doctest.h").is_file():
            return Path(d)
    for found in sorted(work_dir().glob("*/build*/_deps/doctest-src")):
        if (found / "doctest" / "doctest.h").is_file():
            return found
    return None


_built = {}


def build_server():
    """Configures and builds every service with its tests, once per run; returns the build dir."""
    if "dir" in _built:
        return _built["dir"]
    doctest_dir = find_doctest()
    if doctest_dir is None:
        pytest.skip("no doctest source: set SFCOP_DOCTEST_DIR or FETCHCONTENT_SOURCE_DIR_DOCTEST "
                    "(nothing is downloaded during the tests)")
    build = build_dir() / "lobby"
    args = ["cmake", "-S", str(SERVER), "-B", str(build), "-G", "Ninja",
            "-DSFCOP_DOCTEST_DIR=" + str(doctest_dir)]
    configure = subprocess.run(args, capture_output=True, text=True)
    assert configure.returncode == 0, configure.stdout[-2000:] + configure.stderr[-2000:]
    built = subprocess.run(["nice", "-n", "19", "cmake", "--build", str(build), "-j4"],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stdout[-4000:] + built.stderr[-2000:]
    _built["dir"] = build
    return build


@pytest.mark.skipif(shutil.which("cmake") is None or shutil.which("ninja") is None,
                    reason="cmake and ninja are needed to build the services")
@pytest.mark.parametrize("service", ["lobby", "relay", "directory", "auth"])
def test_service_doctests_pass(service):
    build = build_server()
    ran = subprocess.run([str(build / service / ("sfcop_%s_tests" % service)), "--no-version"],
                         capture_output=True, text=True)
    assert ran.returncode == 0, ran.stdout[-4000:]
    assert "Status: SUCCESS!" in ran.stdout


@pytest.mark.parametrize("script", [
    "deploy.sh", "sfcop-health-check.sh", "test-container.sh", "container-test-inner.sh",
])
def test_deploy_scripts_parse(script):
    r = subprocess.run(["sh", "-n", str(DEPLOY / script)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_relay_unit_is_not_root_reads_its_own_environment_and_keeps_its_log_private():
    unit = (DEPLOY / "sfcop-relay.service").read_text()
    assert "User=sfcop" in unit
    assert "EnvironmentFile=/etc/sfcop/relay.env" in unit
    assert "EnvironmentFile=/etc/sfcop/sfcop.env" in unit          # the shared key, root-only file
    assert "/var/log/sfcop/relay.log" in unit and "UMask=0027" in unit
    env = (DEPLOY / "relay.env.example").read_text()
    assert "SFCOP_HEALTH_LISTEN=127.0.0.1:47711" in env          # the monitor reads the same name
    assert not any(line.startswith("SFCOP_TICKET_KEY=") for line in env.splitlines())


@pytest.mark.parametrize("service,health", [("directory", "47713"), ("auth", "47714")])
def test_dynaverse_unit_is_not_root_reads_its_own_environment_and_keeps_its_log_private(service, health):
    unit = (DEPLOY / ("sfcop-%s.service" % service)).read_text()
    assert "User=sfcop" in unit
    assert "EnvironmentFile=/etc/sfcop/%s.env" % service in unit
    assert "EnvironmentFile=/etc/sfcop/sfcop.env" not in unit     # no use for the ticket key
    assert "/var/log/sfcop/%s.log" % service in unit and "UMask=0027" in unit
    env = (DEPLOY / ("%s.env.example" % service)).read_text()
    assert "SFCOP_HEALTH_LISTEN=127.0.0.1:%s" % health in env       # the monitor reads the same name
    deploy = (DEPLOY / "deploy.sh").read_text()
    assert 'SERVICES="lobby relay directory auth"' in deploy


def test_auth_unit_holds_only_the_port_80_capability():
    unit = (DEPLOY / "sfcop-auth.service").read_text()
    assert "CapabilityBoundingSet=CAP_NET_BIND_SERVICE\n" in unit
    assert "AmbientCapabilities=CAP_NET_BIND_SERVICE\n" in unit
    assert "NoNewPrivileges=true" in unit
    directory = (DEPLOY / "sfcop-directory.service").read_text()
    assert "CapabilityBoundingSet=\n" in directory


def test_deploy_kit_files_say_what_the_brief_requires():
    logrotate = (DEPLOY / "sfcop.logrotate").read_text()
    assert "daily" in logrotate and "rotate 7" in logrotate and "maxage 7" in logrotate
    unit = (DEPLOY / "sfcop-lobby.service").read_text()
    assert "EnvironmentFile=/etc/sfcop/lobby.env" in unit      # configuration outside the repository
    assert "User=sfcop" in unit                                 # not root
    assert "/var/log/sfcop/lobby.log" in unit
    timer = (DEPLOY / "sfcop-health@.timer").read_text()
    assert "OnUnitActiveSec=1min" in timer                      # the monitor polls every minute
    env = (DEPLOY / "lobby.env.example").read_text()
    assert "SFCOP_TICKET_KEY=" not in env.replace("(SFCOP_TICKET_KEY, 64", "")  # the key is in the shared file, never here


@pytest.fixture
def fake_curl(tmp_path):
    """A curl that succeeds while <tmp>/up exists and fails otherwise; no network is touched."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    curl = bin_dir / "curl"
    curl.write_text(textwrap.dedent(f"""\
        #!/bin/sh
        [ -e "{tmp_path}/up" ]
        """))
    curl.chmod(curl.stat().st_mode | stat.S_IXUSR)
    return tmp_path


def run_check(tmp, alert_log, **extra):
    env = {
        "PATH": str(tmp / "bin") + os.pathsep + os.environ["PATH"],
        "SFCOP_STATE_DIR": str(tmp),
        "SFCOP_ALERT_COMMAND": f'echo "$SFCOP_ALERT_MESSAGE" >> "{alert_log}"',
    }
    env.update(extra)
    return subprocess.run(["sh", str(DEPLOY / "sfcop-health-check.sh"), "lobby"],
                          env=env, capture_output=True, text=True)


def test_health_check_sends_one_message_per_failure_and_rearms(fake_curl):
    tmp = fake_curl
    alerts = tmp / "alerts"
    (tmp / "up").touch()
    assert run_check(tmp, alerts).returncode == 0
    assert not alerts.exists()

    (tmp / "up").unlink()
    assert run_check(tmp, alerts).returncode == 1
    assert run_check(tmp, alerts).returncode == 1          # still failing: no second message
    assert alerts.read_text() == "sfcop: lobby failed its health check\n"

    (tmp / "up").touch()
    assert run_check(tmp, alerts).returncode == 0          # recovered: marker cleared, no message
    assert alerts.read_text() == "sfcop: lobby failed its health check\n"

    (tmp / "up").unlink()
    assert run_check(tmp, alerts).returncode == 1          # a new failure: a new message
    assert alerts.read_text().count("failed its health check") == 2


def test_health_check_without_an_alert_command_only_reports_the_exit_status(fake_curl):
    tmp = fake_curl
    r = run_check(tmp, tmp / "alerts", SFCOP_ALERT_COMMAND="")
    assert r.returncode == 1
    assert "lobby did not answer its health request" in r.stdout
    assert not (tmp / "alerts").exists()


def test_health_check_asks_the_url_the_environment_names(tmp_path):
    """The URL comes from SFCOP_HEALTH_URL, else the loopback listener of SFCOP_HEALTH_LISTEN."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    curl = bin_dir / "curl"
    curl.write_text('#!/bin/sh\nfor a; do last=$a; done\necho "$last" > "%s/asked"\n' % tmp_path)
    curl.chmod(curl.stat().st_mode | stat.S_IXUSR)
    base = {"PATH": str(bin_dir) + os.pathsep + os.environ["PATH"], "SFCOP_STATE_DIR": str(tmp_path)}
    script = str(DEPLOY / "sfcop-health-check.sh")

    subprocess.run(["sh", script, "lobby"], env=base, check=True)
    assert (tmp_path / "asked").read_text().strip() == "http://127.0.0.1:47712/v1/health"
    subprocess.run(["sh", script, "lobby"], env={**base, "SFCOP_HEALTH_LISTEN": "127.0.0.1:9000"}, check=True)
    assert (tmp_path / "asked").read_text().strip() == "http://127.0.0.1:9000/v1/health"
    subprocess.run(["sh", script, "relay"], env={**base, "SFCOP_HEALTH_URL": "http://127.0.0.1:1234/x"}, check=True)
    assert (tmp_path / "asked").read_text().strip() == "http://127.0.0.1:1234/x"


def test_each_checkout_gets_its_own_build_directory(tmp_path, monkeypatch):
    # A CMake cache belongs to one source directory, so two worktrees must not share one. The
    # slug is the checkout's name and the first 12 hex digits of sha256 over its absolute path;
    # the literal below was computed once by hand for "/x/ExampleCheckout".
    assert checkout_slug(Path("/x/ExampleCheckout")) == "ExampleCheckout-57484229bbfb"
    assert checkout_slug(Path("/x/.claude/worktrees/wp-X/ExampleCheckout")) != "ExampleCheckout-57484229bbfb"
    monkeypatch.delenv("SFCOP_SERVER_BUILD", raising=False)
    monkeypatch.setenv("SFCOP_WORK", str(tmp_path / "work"))
    assert build_dir().parent == tmp_path / "work" / "server-tests"
    assert build_dir().name.startswith(ROOT.name + "-") and len(build_dir().name) == len(ROOT.name) + 13
    monkeypatch.setenv("SFCOP_SERVER_BUILD", str(tmp_path / "explicit"))
    assert build_dir() == tmp_path / "explicit"
