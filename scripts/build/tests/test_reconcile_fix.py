#!/usr/bin/python
##
## license:BSD-3-Clause
## copyright-holders:MAMEdev Team
##
## Golden tests for makedep.py `reconcilelist --fix` (ADR 0005 / Task 22).
##
## These build SMALL fixture .lst files and SMALL fixture -listxml strings in a
## temp dir and assert the autofix output is byte-exact -- no MAME build, no
## GENie, no network.  Run with:
##   python -m pytest scripts/build/tests/test_reconcile_fix.py

import io
import os
import os.path
import sys

import pytest

# Make the sibling makedep.py importable regardless of CWD.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import makedep  # noqa: E402


CRLF = '\r\n'

# Verbatim header block shared by the fixtures.  Mirrors the real lists: two
# license lines then a /* ... */ comment block, then a blank line.  CRLF
# throughout, ending with a blank line before the first @source:.
HEADER = CRLF.join([
    '// license:BSD-3-Clause',
    '// copyright-holders:MAMEdev Team',
    '/******************************',
    '',
    '    test fixture list',
    '',
    '******************************/',
    '',
    '',
]) + ''  # HEADER ends right before the first @source: line


def _lst(*groups):
    """Build a CRLF .lst body string from (source, [drivers]) groups.

    Produces: HEADER + '@source:<s>\r\n' + drivers... with a blank CRLF line
    between groups and a trailing CRLF after the last driver -- exactly the
    real-file convention.
    """
    blocks = []
    for source, drivers in groups:
        lines = ['@source:' + source] + list(drivers)
        blocks.append(CRLF.join(lines))
    return HEADER + (CRLF + CRLF).join(blocks) + CRLF


def _xml(*groups):
    """Build a tiny -listxml document from (source, [drivers]) groups.

    Drivers appear in document order so the fixer can reproduce registration
    order.  Uses LF internally (irrelevant -- it is parsed by SAX, not compared
    byte-for-byte).
    """
    out = ['<mame>']
    for source, drivers in groups:
        for d in drivers:
            out.append('  <machine name="%s" sourcefile="%s"/>' % (d, source))
    out.append('</mame>')
    return '\n'.join(out) + '\n'


def _write_lst(tmp_path, text):
    path = os.path.join(str(tmp_path), 'test.lst')
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    return path


def _write_xml(tmp_path, text):
    path = os.path.join(str(tmp_path), 'info.xml')
    with io.open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    return path


def _read_bytes(path):
    with io.open(path, 'rb') as f:
        return f.read()


def _run_fix(lstpath, xmlpath):
    """Invoke the fixer exactly as the dispatch does and write the result."""
    with io.open(xmlpath, 'rb') as f:
        collected = makedep.DriverReconciler.collect_xml(f)
    newtext, summary = makedep.fix_driver_list(lstpath, collected)
    with io.open(lstpath, 'w', encoding='utf-8', newline='') as f:
        f.write(newtext)
    return summary


class _Options:
    def __init__(self, listpath):
        self.list = listpath
        self.root = '.'
        self.filter = None


def _check_clean(lstpath, xmlpath):
    """Run CHECK mode (no --fix) on the fixed list and return reconciler.bad."""
    reconciler = makedep.DriverReconciler(_Options(lstpath))
    with io.open(xmlpath, 'rb') as f:
        reconciler.reconcile_xml(f)
    return reconciler.bad


# --------------------------------------------------------------------------- #
#  Golden cases                                                               #
# --------------------------------------------------------------------------- #

def test_missing_driver_existing_group(tmp_path):
    # Binary has a driver the list lacks, under an existing @source: group ->
    # appended to that group.
    lst = _lst(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    xml = _xml(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux', 'astdelux2']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    summary = _run_fix(lstpath, xmlpath)

    expected = _lst(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux', 'astdelux2']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    assert _read_bytes(lstpath) == expected.encode('utf-8')
    assert summary['added'] == 1
    assert _check_clean(lstpath, xmlpath) is False


def test_stale_driver_removed(tmp_path):
    # List has a driver the binary doesn't -> line removed; group keeps the rest.
    lst = _lst(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux', 'bogusdrv']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    xml = _xml(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    summary = _run_fix(lstpath, xmlpath)

    expected = _lst(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    assert _read_bytes(lstpath) == expected.encode('utf-8')
    assert summary['removed'] == 1
    assert _check_clean(lstpath, xmlpath) is False


def test_stale_source_group_fully_removed(tmp_path):
    # A whole @source: group's drivers are gone from the binary -> the entire
    # group (header + trailing blank) is removed.
    lst = _lst(
        ('atari/asteroid.cpp', ['asteroid']),
        ('dead/gone.cpp', ['ghost1', 'ghost2']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    xml = _xml(
        ('atari/asteroid.cpp', ['asteroid']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    summary = _run_fix(lstpath, xmlpath)

    expected = _lst(
        ('atari/asteroid.cpp', ['asteroid']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    assert _read_bytes(lstpath) == expected.encode('utf-8')
    assert summary['groups_removed'] == 1
    assert summary['removed'] == 2
    assert _check_clean(lstpath, xmlpath) is False


def test_new_source_group_inserted_natural_sorted(tmp_path):
    # Binary introduces a brand-new source that natural-sorts BETWEEN two
    # existing akai groups: mpc60 < mpc1000 < mpc2000 (numeric-aware).  The new
    # group's drivers follow document order.
    lst = _lst(
        ('akai/mpc60.cpp', ['mpc60']),
        ('akai/mpc2000.cpp', ['mpc2000']),
    )
    xml = _xml(
        ('akai/mpc60.cpp', ['mpc60']),
        ('akai/mpc2000.cpp', ['mpc2000']),
        ('akai/mpc1000.cpp', ['mpc1000', 'mpc2500']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    summary = _run_fix(lstpath, xmlpath)

    # mpc1000 sorts after mpc60 (60 < 1000) and before mpc2000 (1000 < 2000).
    expected = _lst(
        ('akai/mpc60.cpp', ['mpc60']),
        ('akai/mpc1000.cpp', ['mpc1000', 'mpc2500']),
        ('akai/mpc2000.cpp', ['mpc2000']),
    )
    assert _read_bytes(lstpath) == expected.encode('utf-8')
    assert summary['groups_added'] == 1
    assert summary['added'] == 2
    assert _check_clean(lstpath, xmlpath) is False


def test_new_group_appended_when_sorts_last(tmp_path):
    # A brand-new source that natural-sorts after all existing groups is
    # appended at the end (no trailing blank line after it).
    lst = _lst(
        ('akai/mpc60.cpp', ['mpc60']),
    )
    xml = _xml(
        ('akai/mpc60.cpp', ['mpc60']),
        ('zaccaria/zac.cpp', ['zacm1']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    _run_fix(lstpath, xmlpath)

    expected = _lst(
        ('akai/mpc60.cpp', ['mpc60']),
        ('zaccaria/zac.cpp', ['zacm1']),
    )
    assert _read_bytes(lstpath) == expected.encode('utf-8')
    assert _check_clean(lstpath, xmlpath) is False


def test_driver_moved_between_sources(tmp_path):
    # Binary reports a driver under a different source than the list has it ->
    # removed from the wrong group, added under the correct group.
    lst = _lst(
        ('atari/asteroid.cpp', ['asteroid', 'wrongplace']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    xml = _xml(
        ('atari/asteroid.cpp', ['asteroid']),
        ('sega/segas16a.cpp', ['shinobi', 'wrongplace']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    summary = _run_fix(lstpath, xmlpath)

    expected = _lst(
        ('atari/asteroid.cpp', ['asteroid']),
        ('sega/segas16a.cpp', ['shinobi', 'wrongplace']),
    )
    assert _read_bytes(lstpath) == expected.encode('utf-8')
    assert summary['moved'] == 1
    # A move is neither an add nor a remove.
    assert summary['added'] == 0
    assert summary['removed'] == 0
    assert _check_clean(lstpath, xmlpath) is False


def test_noop_on_already_correct_list(tmp_path):
    # A list that already matches the xml -> --fix leaves it byte-identical.
    lst = _lst(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    xml = _xml(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux']),
        ('sega/segas16a.cpp', ['shinobi']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    before = _read_bytes(lstpath)
    summary = _run_fix(lstpath, xmlpath)

    assert _read_bytes(lstpath) == before
    assert summary == {'added': 0, 'removed': 0, 'moved': 0,
                       'groups_added': 0, 'groups_removed': 0}
    assert _check_clean(lstpath, xmlpath) is False


def test_crlf_preserved_and_header_verbatim(tmp_path):
    # The output must keep CRLF line endings and the header block byte-for-byte.
    lst = _lst(
        ('atari/asteroid.cpp', ['asteroid']),
    )
    xml = _xml(
        ('atari/asteroid.cpp', ['asteroid', 'astdelux']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)
    _run_fix(lstpath, xmlpath)

    data = _read_bytes(lstpath)
    assert b'\r\n' in data
    # Every LF must be part of a CRLF (no bare LF).
    assert b'\n' not in data.replace(b'\r\n', b'')
    # Header preserved verbatim.
    assert data.startswith(HEADER.encode('utf-8'))


def test_fix_is_idempotent(tmp_path):
    # Running the fixer twice produces no further change.
    lst = _lst(
        ('atari/asteroid.cpp', ['asteroid', 'stale']),
        ('dead/gone.cpp', ['ghost']),
    )
    xml = _xml(
        ('atari/asteroid.cpp', ['asteroid']),
        ('akai/mpc1000.cpp', ['mpc1000']),
    )
    lstpath = _write_lst(tmp_path, lst)
    xmlpath = _write_xml(tmp_path, xml)

    _run_fix(lstpath, xmlpath)
    after_first = _read_bytes(lstpath)
    summary2 = _run_fix(lstpath, xmlpath)
    after_second = _read_bytes(lstpath)

    assert after_first == after_second
    assert summary2 == {'added': 0, 'removed': 0, 'moved': 0,
                        'groups_added': 0, 'groups_removed': 0}
    assert _check_clean(lstpath, xmlpath) is False


def test_natural_sort_key_numeric_aware():
    # The load-bearing ordering invariant: mpc60 < mpc2000 (60 < 2000), which
    # plain lexicographic order would get wrong.
    nsk = makedep.natural_sort_key
    assert nsk('akai/mpc60.cpp') < nsk('akai/mpc2000.cpp')
    assert nsk('akai/mpc1000.cpp') < nsk('akai/mpc2000.cpp')
    assert nsk('akai/mpc60.cpp') < nsk('akai/mpc1000.cpp')
    # Pure-alpha ordering still behaves.
    assert nsk('atari/asteroid.cpp') < nsk('sega/segas16a.cpp')
