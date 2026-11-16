"""The override report is off by default: every `[override]` line in src/app/guest/override.h except a
fatal one sits behind the `override` log category (src/platform/Core/LogCategories.h), and a fatal
one does not (a fault is always logged)."""
import pathlib

HEADER = pathlib.Path(__file__).resolve().parent.parent / 'src/app/guest/override.h'


def report_lines():
    lines = HEADER.read_text().splitlines()
    return [(n, lines) for n, text in enumerate(lines) if '"[override]' in text]


def test_there_are_override_lines():
    assert len(report_lines()) >= 6


def test_every_report_line_is_behind_the_category():
    for n, lines in report_lines():
        if 'fatal' in lines[n]:
            continue
        window = '\n'.join(lines[max(0, n - 4):n + 1])
        assert 'sfcop_log_enabled("override")' in window or 'SFCOP_GUEST_VERBOSE' in window, \
            'ungated [override] line %d: %s' % (n + 1, lines[n].strip())


def test_a_fatal_line_is_never_gated():
    for n, lines in report_lines():
        if 'fatal' in lines[n]:
            assert 'sfcop_log_enabled' not in lines[n - 1] + lines[n]
