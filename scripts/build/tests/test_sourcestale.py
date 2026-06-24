#!/usr/bin/python
##
## license:BSD-3-Clause
## copyright-holders:MAMEdev Team
##
## Unit tests for scripts/build/sourcestale.py (ADR 0005 / Pick 5, Task 19).
##
## These run against FIXTURE source trees built in a temp dir -- no MAME build,
## no GENie, no network.  Run with:
##   python -m pytest scripts/build/tests/test_sourcestale.py -v

import os
import os.path
import sys

import pytest

# Make the sibling sourcestale.py importable regardless of CWD.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import sourcestale  # noqa: E402


LICENSE = '// license:BSD-3-Clause\n'


def _write(path, text=''):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


def make_fixture_tree(root, files):
    """Create src/mame/<...> fixture files under `root`.

    `files` is an iterable of root-relative forward-slash paths, e.g.
    "src/mame/atari/asteroid.cpp".
    """
    for rel in files:
        _write(os.path.join(root, *rel.split('/')), LICENSE)


# A small but representative baseline source set.
BASELINE = frozenset({
    'src/mame/atari/asteroid.cpp',
    'src/mame/atari/asteroid.h',
    'src/mame/atari/avalnche.cpp',
    'src/mame/sega/segas16a.cpp',
    'src/mame/nintendo/nes.cpp',
})

BASELINE_CPP = frozenset(f for f in BASELINE if f.endswith('.cpp'))


@pytest.fixture
def tree(tmp_path):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE)
    return root


def test_walk_target_sources_collects_globbed_extensions(tree):
    files = sourcestale.walk_target_sources(tree, 'mame')
    # All baseline files (cpp + h) are discovered.
    assert files == set(BASELINE)
    # Paths are forward-slash, root-relative.
    assert all('\\' not in f for f in files)
    assert all(f.startswith('src/mame/') for f in files)


def test_unchanged_tree_is_up_to_date(tree):
    # Baked-in set == current .cpp set -> no false positive.
    result = sourcestale.check_staleness(
        tree, projectdir=None, target='mame', baked=set(BASELINE_CPP))
    assert result.generated is True
    assert result.stale is False
    assert result.status == 'up-to-date'
    assert result.added == []
    assert result.removed == []


def test_added_cpp_is_detected_stale(tree):
    # A new .cpp exists on disk but is NOT in the baked-in project set.
    make_fixture_tree(tree, {'src/mame/atari/newgame.cpp'})
    result = sourcestale.check_staleness(
        tree, projectdir=None, target='mame', baked=set(BASELINE_CPP))
    assert result.stale is True
    assert result.status == 'stale'
    assert result.added == ['src/mame/atari/newgame.cpp']
    assert result.removed == []


def test_added_cpp_in_brand_new_directory_is_detected_stale(tree):
    # GENie globs whole directories; a brand-new manufacturer dir must be caught.
    make_fixture_tree(tree, {'src/mame/newvendor/firstdriver.cpp'})
    result = sourcestale.check_staleness(
        tree, projectdir=None, target='mame', baked=set(BASELINE_CPP))
    assert result.stale is True
    assert 'src/mame/newvendor/firstdriver.cpp' in result.added


def test_removed_cpp_is_detected_stale(tree):
    # A .cpp baked into the project set no longer exists on disk.
    os.remove(os.path.join(tree, 'src', 'mame', 'sega', 'segas16a.cpp'))
    result = sourcestale.check_staleness(
        tree, projectdir=None, target='mame', baked=set(BASELINE_CPP))
    assert result.stale is True
    assert result.status == 'stale'
    assert result.removed == ['src/mame/sega/segas16a.cpp']
    assert result.added == []


def test_added_header_only_does_not_change_cpp_comparison(tree):
    # Headers are not emitted as objects in project files; adding a lone .h must
    # not produce a false "stale" against the .cpp-only baked set.
    make_fixture_tree(tree, {'src/mame/atari/extra.h'})
    result = sourcestale.check_staleness(
        tree, projectdir=None, target='mame', baked=set(BASELINE_CPP))
    assert result.stale is False
    assert result.status == 'up-to-date'


def test_baked_source_set_round_trips_through_generated_make(tmp_path):
    # Simulate a generated gmake project file and confirm we recover the .cpp set.
    root = str(tmp_path)
    projectdir = os.path.join(root, 'build', 'projects', 'windows', 'mame', 'gmake')
    os.makedirs(projectdir)
    makefile = os.path.join(projectdir, 'mame_mame.make')
    with open(makefile, 'w', encoding='utf-8') as f:
        f.write('  OBJECTS := \\\n')
        f.write('\t$(OBJDIR)/src/mame/atari/asteroid.o \\\n')
        f.write('\t$(OBJDIR)/src/mame/sega/segas16a.o \\\n')
        f.write('\t$(OBJDIR)/src/emu/machine.o \\\n')  # wrong target -> ignored
    baked = sourcestale.baked_source_set(
        os.path.join(root, 'build', 'projects'), 'mame')
    assert baked == {
        'src/mame/atari/asteroid.cpp',
        'src/mame/sega/segas16a.cpp',
    }


def test_baked_set_excludes_top_level_hand_listed_sources(tmp_path):
    # src/mame/mame.cpp is listed directly under src/mame (the entry point); it
    # is added explicitly by the build scripts, NOT by the directory glob, so it
    # must not appear in the baked set (else a spurious "removed" is reported).
    root = str(tmp_path)
    projectdir = os.path.join(root, 'build', 'projects', 'gmake')
    os.makedirs(projectdir)
    with open(os.path.join(projectdir, 'mame.make'), 'w', encoding='utf-8') as f:
        f.write('\t$(OBJDIR)/src/mame/mame.o \\\n')          # top-level -> excluded
        f.write('\t$(OBJDIR)/src/mame/atari/asteroid.o \\\n')  # globbed -> included
    baked = sourcestale.baked_source_set(
        os.path.join(root, 'build', 'projects'), 'mame')
    assert baked == {'src/mame/atari/asteroid.cpp'}
    assert 'src/mame/mame.cpp' not in baked


def test_top_level_source_does_not_cause_false_stale(tmp_path):
    # Full path: a tree that has src/mame/mame.cpp on disk and baked in must be
    # reported up-to-date, not stale, despite mame.cpp being hand-listed.
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE)
    _write(os.path.join(root, 'src', 'mame', 'mame.cpp'), LICENSE)
    projectdir = os.path.join(root, 'build', 'projects', 'gmake')
    os.makedirs(projectdir)
    with open(os.path.join(projectdir, 'mame.make'), 'w', encoding='utf-8') as f:
        f.write('\t$(OBJDIR)/src/mame/mame.o \\\n')
        for cpp in sorted(BASELINE_CPP):
            f.write('\t$(OBJDIR)/%s \\\n' % (cpp[:-len('.cpp')] + '.o'))
    result = sourcestale.check_staleness(
        root, projectdir=os.path.join(root, 'build', 'projects'), target='mame')
    assert result.stale is False, (result.added, result.removed)
    assert result.status == 'up-to-date'


def test_is_globbed_source_helper():
    assert sourcestale.is_globbed_source('src/mame/atari/asteroid.o', 'src/mame/')
    assert not sourcestale.is_globbed_source('src/mame/mame.o', 'src/mame/')
    assert not sourcestale.is_globbed_source('src/emu/machine.o', 'src/mame/')
    # Deeply nested globbed source still counts.
    assert sourcestale.is_globbed_source('src/mame/atari/sub/x.o', 'src/mame/')


def test_no_generated_project_files_reports_not_generated(tmp_path):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE)
    # projectdir does not exist -> baked set is None -> not-generated.
    result = sourcestale.check_staleness(
        root,
        projectdir=os.path.join(root, 'build', 'projects'),
        target='mame')
    assert result.not_generated is True
    assert result.status == 'not-generated'


def test_end_to_end_against_generated_makefile_detects_added(tmp_path):
    # Full path: real fixture tree + real generated make file, no `baked` kwarg.
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE | {'src/mame/atari/newgame.cpp'})
    projectdir = os.path.join(root, 'build', 'projects', 'gmake')
    os.makedirs(projectdir)
    with open(os.path.join(projectdir, 'mame_mame.make'), 'w', encoding='utf-8') as f:
        for cpp in sorted(BASELINE_CPP):
            obj = cpp[:-len('.cpp')] + '.o'
            f.write('\t$(OBJDIR)/%s \\\n' % obj)
    result = sourcestale.check_staleness(
        root,
        projectdir=os.path.join(root, 'build', 'projects'),
        target='mame')
    assert result.generated is True
    assert result.stale is True
    assert result.added == ['src/mame/atari/newgame.cpp']
    assert result.removed == []


def test_selector_signatures_reads_lua_directives(tmp_path):
    root = str(tmp_path)
    luadir = os.path.join(root, 'scripts', 'src')
    os.makedirs(luadir)
    with open(os.path.join(luadir, 'cpu.lua'), 'w', encoding='utf-8') as f:
        f.write('--@src/devices/cpu/z80/z80.h,CPUS["Z80"] = true\n')
        f.write('local foo = 1\n')
        f.write('--@src/devices/cpu/m6502/m6502.h,CPUS["M6502"] = true\n')
    sigs = sourcestale.selector_signatures(root)
    assert any('Z80' in s for s in sigs)
    assert any('M6502' in s for s in sigs)
    assert len(sigs) == 2


def _make_lua(root, name, lines):
    luadir = os.path.join(root, 'scripts', 'src')
    os.makedirs(luadir, exist_ok=True)
    with open(os.path.join(luadir, name + '.lua'), 'w', encoding='utf-8') as f:
        f.write(''.join(lines))


def test_combined_fingerprint_is_stable_and_sensitive(tmp_path):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE)
    _make_lua(root, 'cpu', ['--@src/devices/cpu/z80/z80.h,CPUS["Z80"] = true\n'])
    fp1 = sourcestale.combined_fingerprint(root, 'mame')
    # Deterministic: same tree -> same digest.
    assert fp1 == sourcestale.combined_fingerprint(root, 'mame')
    # Adding a source changes the digest.
    make_fixture_tree(root, {'src/mame/atari/newgame.cpp'})
    assert sourcestale.combined_fingerprint(root, 'mame') != fp1
    # Adding a selector also changes the digest (selectors are in the fingerprint).
    fp2 = sourcestale.combined_fingerprint(root, 'mame')
    _make_lua(root, 'cpu', [
        '--@src/devices/cpu/z80/z80.h,CPUS["Z80"] = true\n',
        '--@src/devices/cpu/m6502/m6502.h,CPUS["M6502"] = true\n',
    ])
    assert sourcestale.combined_fingerprint(root, 'mame') != fp2


def _generated_make(root, cpp_set):
    projectdir = os.path.join(root, 'build', 'projects', 'gmake')
    os.makedirs(projectdir, exist_ok=True)
    with open(os.path.join(projectdir, 'mame_mame.make'), 'w', encoding='utf-8') as f:
        for cpp in sorted(cpp_set):
            f.write('\t$(OBJDIR)/%s \\\n' % (cpp[:-len('.cpp')] + '.o'))
    return os.path.join(root, 'build', 'projects')


def test_main_warn_mode_exits_zero_even_when_stale(tmp_path, capsys):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE | {'src/mame/atari/newgame.cpp'})
    projectdir = _generated_make(root, BASELINE_CPP)
    rc = sourcestale.main(['--root', root, '--projectdir', projectdir])
    out = capsys.readouterr().out
    assert rc == 0                        # warn-only default never fails
    assert 'STALE' in out
    assert 'newgame.cpp' in out


def test_main_fail_on_stale_exits_nonzero_when_stale(tmp_path, capsys):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE | {'src/mame/atari/newgame.cpp'})
    projectdir = _generated_make(root, BASELINE_CPP)
    rc = sourcestale.main(
        ['--root', root, '--projectdir', projectdir, '--fail-on-stale'])
    assert rc == 1


def test_main_fail_on_stale_exits_zero_when_clean(tmp_path, capsys):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE)
    projectdir = _generated_make(root, BASELINE_CPP)
    rc = sourcestale.main(
        ['--root', root, '--projectdir', projectdir, '--fail-on-stale'])
    out = capsys.readouterr().out
    assert rc == 0
    assert 'up-to-date' in out


def test_main_fail_on_stale_exits_nonzero_when_not_generated(tmp_path, capsys):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE)
    # No generated project files at all.
    projectdir = os.path.join(root, 'build', 'projects')
    rc = sourcestale.main(
        ['--root', root, '--projectdir', projectdir, '--fail-on-stale'])
    out = capsys.readouterr().out
    assert rc == 1
    assert 'no generated project files' in out


def test_main_warn_mode_exits_zero_when_not_generated(tmp_path, capsys):
    root = str(tmp_path)
    make_fixture_tree(root, BASELINE)
    projectdir = os.path.join(root, 'build', 'projects')
    rc = sourcestale.main(['--root', root, '--projectdir', projectdir])
    assert rc == 0
