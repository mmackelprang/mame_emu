#!/usr/bin/python
##
## license:BSD-3-Clause
## copyright-holders:MAMEdev Team
##
## sourcestale.py -- source-set fingerprint + staleness detector (ADR 0005 / Pick 5).
##
## GENie globs the MAME source tree at *project-generation* time
## (scripts/target/mame/mame.lua: linkProjects_mame_mame ~:46 and
## createProjects_mame_mame ~:97 walk src/<target>/* and pull in every
## **.cpp / **.h / **.ipp under each directory that has at least one).  A file
## or directory added after the last `make REGENIE=1` is therefore invisible to
## the build until the project files are regenerated -- a silent, confusing
## failure mode.
##
## This helper computes a fingerprint of the source-file set GENie *would* glob
## right now and compares it to the set baked into the last generated project
## files, reporting `up-to-date` / `stale` (with the added/removed files).  It
## reuses the directory-walking style of makedep.py's collect_sources() so the
## path-walking is shared and unit-testable in isolation.

import argparse
import hashlib
import os
import os.path
import re
import sys


# Source extensions GENie globs into a driver library, mirroring the
# `**.cpp` / `**.h` / `**.ipp` os.matchfiles tests in mame.lua.
SOURCE_EXTENSIONS = ('.cpp', '.h', '.ipp')

# Selector .lua files whose `--@` device directives feed the build
# (mame.lua: selectors_get over scripts/src/*.lua).  A change here can change
# which optional devices are compiled in, so they are part of the fingerprint.
SELECTOR_LUA = ('bus', 'cpu', 'machine', 'sound', 'video', 'formats')


def _norm(path):
    """Normalise a path to forward-slash form for stable comparison/output."""
    return path.replace(os.sep, '/')


def walk_target_sources(root, target):
    """Return the set of source files GENie would glob for `src/<target>`.

    Mirrors mame.lua's createProjects_mame_mame / linkProjects_mame_mame: for
    every immediate subdirectory of src/<target>, if it contains at least one
    **.cpp / **.h / **.ipp *anywhere in its subtree* (the GENie glob recurses)
    it contributes *all* of those files.  Walking style follows makedep.py's
    collect_sources(); we extend it to the three globbed extensions rather than
    just `.cpp`.  Empty directories contribute nothing, matching GENie's
    "only create a project if it has sources" gate.

    Paths are returned root-relative with forward slashes, e.g.
    "src/mame/atari/asteroid.cpp".
    """
    result = set()
    targetroot = os.path.join(root, 'src', target)
    if not os.path.isdir(targetroot):
        return result
    for name in sorted(os.listdir(targetroot)):
        dirpath = os.path.join(targetroot, name)
        if not os.path.isdir(dirpath):
            continue
        # Collect every globbed-extension file under this directory.  GENie
        # only creates a project for the directory if at least one such file
        # exists, but in that case it pulls them all in -- so a single set walk
        # captures both the "does the dir contribute" test and its contents.
        for subdir, dirs, files in os.walk(dirpath):
            # Deterministic traversal so output ordering is stable.
            dirs.sort()
            for candidate in sorted(files):
                ext = os.path.splitext(candidate)[1].lower()
                if ext in SOURCE_EXTENSIONS:
                    abspath = os.path.join(subdir, candidate)
                    rel = os.path.relpath(abspath, root)
                    result.add(_norm(rel))
    return result


def selector_signatures(root):
    """Return the set of `--@name,action` device selector directives.

    These live in scripts/src/{bus,cpu,machine,sound,video,formats}.lua and are
    parsed by mame.lua's selectors_get() / makedep.py's collect_lua_directives().
    A selector added/removed here changes the device set baked into the build,
    so it belongs in the staleness fingerprint alongside the file globs.
    """
    result = set()
    base = os.path.join(root, 'scripts', 'src')
    for name in SELECTOR_LUA:
        path = os.path.join(base, name + '.lua')
        try:
            with open(path, 'r', encoding='utf-8') as f:
                for line in f:
                    stripped = line.strip()
                    if stripped.startswith('--@'):
                        # Store the directive verbatim (after the `--@`) so an
                        # added/removed/changed selector shows up as a delta.
                        result.add('%s:%s' % (name, stripped[3:]))
        except IOError:
            # Missing selector file is itself a meaningful change; skip rather
            # than abort so a partial/fixture tree can still be fingerprinted.
            continue
    return result


def current_source_set(root, target):
    """Full current fingerprint: globbed source files + selector directives."""
    files = walk_target_sources(root, target)
    selectors = selector_signatures(root)
    return files, selectors


def combined_fingerprint(root, target):
    """Return a stable SHA-256 digest of the full source fingerprint.

    The fingerprint is the union of the globbed source files (walk_target_sources)
    and the device selector directives (selector_signatures), exactly the set the
    plan calls out (ADR 0005 §1).  This digest is what a future REGENIE hook can
    persist next to the generated project files so that *selector* drift -- not
    just file drift -- becomes detectable on a later run.

    The project files themselves only record compiled object paths, so the
    project-file comparison in check_staleness() can recover the .cpp set but not
    the selector set; persisting this digest is the forward-compatible way to
    close that gap without altering the build path now.
    """
    files, selectors = current_source_set(root, target)
    digest = hashlib.sha256()
    for item in sorted(files):
        digest.update(('F:' + item + '\n').encode('utf-8'))
    for item in sorted(selectors):
        digest.update(('S:' + item + '\n').encode('utf-8'))
    return digest.hexdigest()


# Object lines in a generated gmake project look like:
#   $(OBJDIR)/src/mame/atari/asteroid.o \
# and headers/ipp are not emitted as objects, so the baked-in *file* set we can
# reliably recover from project files is the .cpp set.  We therefore compare the
# .cpp projection of the current glob against the baked-in .cpp set.
_OBJ_RE = re.compile(r'src/[A-Za-z0-9_]+/[A-Za-z0-9_./-]+\.(?:o|obj)\b')


def is_globbed_source(objpath, prefix):
    """True if `objpath` is a glob-discovered manufacturer-library source.

    The staleness we care about (ADR 0005 §1) is GENie's directory glob over
    src/<target>/* (linkProjects_mame_mame / createProjects_mame_mame), i.e.
    files of the form src/<target>/<dir>/.../<file>.o.  Files listed *directly*
    under src/<target>/ (e.g. src/mame/mame.cpp, the hand-listed entry point)
    are added explicitly by the build scripts, not by the directory glob, so
    they must be excluded from the comparison to avoid a spurious "removed"
    report.  This mirrors walk_target_sources(), which only walks subdirectories
    of src/<target>.
    """
    if not objpath.startswith(prefix):
        return False
    rest = objpath[len(prefix):]
    # Require at least one subdirectory component before the filename:
    # "atari/asteroid.o" -> globbed; "mame.o" -> hand-listed, excluded.
    return '/' in rest


def baked_source_set(projectdir, target):
    """Extract the source-file set baked into the last generated project files.

    Scans the generated gmake `*.make` files for `$(OBJDIR)/src/<target>/....o`
    object entries and maps each back to its `.cpp` source path.  Returns the
    set of root-relative `.cpp` paths, e.g. "src/mame/atari/asteroid.cpp".

    Returns None if no generated project files are found (never generated /
    cleaned tree) so callers can distinguish "stale" from "not generated".
    """
    if not os.path.isdir(projectdir):
        return None
    prefix = 'src/%s/' % target
    found_any_makefile = False
    result = set()
    for subdir, dirs, files in os.walk(projectdir):
        for candidate in files:
            if not candidate.endswith('.make'):
                continue
            found_any_makefile = True
            path = os.path.join(subdir, candidate)
            try:
                with open(path, 'r', encoding='utf-8', errors='replace') as f:
                    text = f.read()
            except IOError:
                continue
            # We scan every *.make file under projectdir (emu.make, optional.make,
            # the per-target driver project, etc.); object lines from non-target
            # projects (e.g. src/emu/...o) are excluded by the src/<target>/
            # prefix guard below rather than by selecting specific make files.
            for match in _OBJ_RE.findall(text):
                objpath = _norm(match)
                if is_globbed_source(objpath, prefix):
                    result.add(objpath[:-2] + '.cpp')
    if not found_any_makefile:
        return None
    return result


class StalenessResult:
    """Outcome of a staleness comparison."""

    def __init__(self, generated, added, removed):
        self.generated = generated      # were project files found at all
        self.added = sorted(added)      # source files present now, not baked in
        self.removed = sorted(removed)  # baked in, no longer present

    @property
    def not_generated(self):
        return not self.generated

    @property
    def stale(self):
        return bool(self.added) or bool(self.removed)

    @property
    def status(self):
        if self.not_generated:
            return 'not-generated'
        return 'stale' if self.stale else 'up-to-date'


def check_staleness(root, projectdir, target, baked=None):
    """Compare the current source glob to the baked-in project-file set.

    `baked` may be supplied directly (set of root-relative `.cpp` paths) to make
    the comparison unit-testable without a real build tree; otherwise it is read
    from the generated project files under `projectdir`.

    Scope note: generated project files record only compiled object paths, so the
    comparison is the .cpp projection of the current glob against the baked .cpp
    set -- this catches the dominant "added/removed a driver .cpp, forgot
    REGENIE" case.  Header-only and selector-only changes are *not* flagged here
    because the project files carry no comparable baseline for them; use
    combined_fingerprint() with a persisted digest if/when selector-drift
    detection is wired in.
    """
    if baked is None:
        baked = baked_source_set(projectdir, target)
    if baked is None:
        return StalenessResult(generated=False, added=set(), removed=set())
    files, _selectors = current_source_set(root, target)
    # Project files only carry the .cpp object set, so compare like-for-like:
    # the .cpp projection of the current glob against the baked .cpp set.
    current_cpp = set(f for f in files if f.lower().endswith('.cpp'))
    added = current_cpp - baked
    removed = baked - current_cpp
    return StalenessResult(generated=True, added=added, removed=removed)


def _emit(result, target):
    if result.not_generated:
        print('check-sources: no generated project files found for target '
              '"%s" -- run `make REGENIE=1`.' % target)
        return
    if not result.stale:
        print('check-sources: source set is up-to-date for target "%s".' % target)
        return
    print('check-sources: source set is STALE for target "%s" -- '
          'new sources detected, run `make REGENIE=1`.' % target)
    for path in result.added:
        print('  + %s' % path)
    for path in result.removed:
        print('  - %s' % path)


def parse_command_line(argv):
    parser = argparse.ArgumentParser(
        description='Detect whether the MAME source set has drifted from the '
                    'last generated GENie project files.')
    parser.add_argument(
        '--root', default='.',
        help='repository root (default: current directory)')
    parser.add_argument(
        '--target', default='mame',
        help='source target under src/ to fingerprint (default: mame)')
    parser.add_argument(
        '--projectdir', default=None,
        help='directory containing generated project files '
             '(default: <root>/build/projects)')
    parser.add_argument(
        '--fail-on-stale', action='store_true',
        help='exit non-zero when stale (CI fail mode); default warns + exit 0')
    return parser.parse_args(argv)


def main(argv=None):
    options = parse_command_line(sys.argv[1:] if argv is None else argv)
    root = options.root
    projectdir = options.projectdir
    if projectdir is None:
        projectdir = os.path.join(root, 'build', 'projects')
    result = check_staleness(root, projectdir, options.target)
    _emit(result, options.target)
    if (result.stale or result.not_generated) and options.fail_on_stale:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
