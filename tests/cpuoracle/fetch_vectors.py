# license:BSD-3-Clause
# copyright-holders:Mark Mackelprang
"""Fetch SingleStepTests/ProcessorTests CPU vectors (fetched, not vendored).

This script populates a gitignored cache (``build/cpuoracle/<core>/``) with the
per-instruction test vectors from the SingleStepTests (formerly "TomHarte")
ProcessorTests corpus, for use by the differential CPU oracle (ADR 0001).

Policy (resolved): the corpus is *fetched, not vendored*, with a **pinned
upstream ref + a SHA-256 per fetched archive + a mirror fallback**.  The corpus
is hundreds of megabytes per core and separately licensed, so only the small
``manifest.json`` is committed to the MAME tree -- never the vectors.

For each requested core the fetcher:

  1. Downloads the pinned upstream source archive (a single ``.tar.gz`` at a
     pinned tag or commit) into the cache, trying each mirror URL in order
     until one succeeds.
  2. Verifies the archive's SHA-256 against ``manifest.json`` and **fails
     (non-zero exit) on mismatch** with a clear message.
  3. Extracts the vector files (the corpus ``v1/`` directory, flattened) into
     ``build/cpuoracle/<core>/`` and writes a per-file SHA-256 index
     (``checksums.sha256``) plus a ``.fetch_stamp.json`` recording the verified
     archive ref/hash.

The operation is **idempotent**: a subsequent run re-verifies every cached file
against the stored per-file index and the recorded archive hash, and is a no-op
("already verified") when everything matches.  If any cached byte is corrupted,
re-verification fails with a hash-mismatch message and a non-zero exit.

No network access is required at test-run time -- only at fetch time.

Usage::

    python tests/cpuoracle/fetch_vectors.py --cores z80
    python tests/cpuoracle/fetch_vectors.py --cores z80,m6502,m68000

Exit codes: 0 = all requested cores verified; non-zero = a download, hash, or
extraction error (see the message).
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request


# Project-root-relative paths.  This file lives at tests/cpuoracle/, so the repo
# root is two levels up; the cache lives under build/cpuoracle/ (gitignored).
HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(HERE, os.pardir, os.pardir))
MANIFEST_PATH = os.path.join(HERE, "manifest.json")
CACHE_ROOT = os.path.join(REPO_ROOT, "build", "cpuoracle")

# Names of the bookkeeping files the fetcher writes into each core's cache dir.
# These are excluded from the "vector file" accounting.
CHECKSUM_INDEX = "checksums.sha256"
FETCH_STAMP = ".fetch_stamp.json"
_BOOKKEEPING = {CHECKSUM_INDEX, FETCH_STAMP}

# Stream downloads/hashing in chunks to keep memory flat on ~1 GB archives.
_CHUNK = 1024 * 1024

# Network timeout per mirror attempt, in seconds.  The corpus archives are
# large; a generous timeout avoids spurious failures on slow links while still
# bounding a hung connection.
_TIMEOUT = 600


class FetchError(Exception):
    """A user-facing fetch/verify failure (printed without a traceback)."""


def _sha256_file(path):
    """Return the hex SHA-256 of a file, read in streaming chunks."""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def _load_manifest():
    """Load and lightly validate ``manifest.json``."""
    try:
        with open(MANIFEST_PATH, "r", encoding="utf-8") as fh:
            manifest = json.load(fh)
    except FileNotFoundError:
        raise FetchError("manifest not found: {}".format(MANIFEST_PATH))
    except json.JSONDecodeError as exc:
        raise FetchError("manifest is not valid JSON: {}".format(exc))

    cores = manifest.get("cores")
    if not isinstance(cores, dict) or not cores:
        raise FetchError("manifest has no 'cores' map")
    return manifest


def _core_entry(manifest, core):
    """Return the manifest entry for ``core`` or raise a helpful error."""
    entry = manifest["cores"].get(core)
    if entry is None:
        known = ", ".join(sorted(manifest["cores"]))
        raise FetchError(
            "unknown core '{}' (manifest knows: {})".format(core, known))
    for field in ("ref", "sha256", "mirrors", "archive_member_prefix"):
        if field not in entry:
            raise FetchError(
                "manifest core '{}' is missing required field '{}'"
                .format(core, field))
    if not entry["mirrors"]:
        raise FetchError("manifest core '{}' has an empty 'mirrors' list".format(core))
    return entry


def _download(url, dest, timeout=_TIMEOUT):
    """Stream ``url`` to ``dest``.  Raises urllib errors on failure."""
    req = urllib.request.Request(url, headers={"User-Agent": "mame-cpuoracle-fetch/1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        with open(dest, "wb") as out:
            shutil.copyfileobj(resp, out, length=_CHUNK)


def _download_with_mirrors(mirrors, dest):
    """Try each mirror URL in order; return the URL that succeeded.

    On a primary-URL failure (network/HTTP error) we fall through to the next
    mirror.  If every mirror fails we raise a FetchError summarising each
    attempt.
    """
    errors = []
    for idx, url in enumerate(mirrors):
        label = "primary" if idx == 0 else "mirror {}".format(idx)
        try:
            print("  fetching ({}): {}".format(label, url))
            _download(url, dest)
            return url
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
            print("  ! {} failed: {}".format(label, exc))
            errors.append("{} ({}): {}".format(label, url, exc))
            # Remove any partial file before trying the next mirror.
            if os.path.exists(dest):
                os.remove(dest)
    raise FetchError(
        "all {} mirror(s) failed:\n    {}".format(len(mirrors), "\n    ".join(errors)))


def _extract_vectors(archive_path, member_prefix, dest_dir):
    """Extract the corpus vector files from ``archive_path`` into ``dest_dir``.

    The corpus archives are GitHub source tarballs whose contents live under a
    top-level ``<repo>-<ref>/`` directory; the vectors themselves are under that
    directory's ``v1/`` subdirectory.  ``member_prefix`` is the in-archive path
    prefix up to and including ``v1/`` (e.g. ``z80-1.0-beta.2/v1/``).  Files are
    extracted flat into ``dest_dir`` (their basename), which is what the harness
    discovers.

    Returns the sorted list of extracted basenames.
    """
    extracted = []
    with tarfile.open(archive_path, "r:gz") as tf:
        for member in tf:
            if not member.isfile():
                continue
            name = member.name
            if not name.startswith(member_prefix):
                continue
            rel = name[len(member_prefix):]
            # Vectors are flat files directly under v1/; ignore anything nested,
            # empty, or that could escape dest_dir.  The archive SHA-256 is the
            # primary integrity anchor, but guard extraction independently so a
            # path-traversal entry (forward slash, OS separator, or an absolute
            # path -- e.g. a backslash entry on Windows) can never write outside
            # the cache directory regardless of the hash gate.
            if (not rel or "/" in rel or os.sep in rel
                    or (os.altsep and os.altsep in rel) or os.path.isabs(rel)):
                continue
            out_path = os.path.join(dest_dir, rel)
            src = tf.extractfile(member)
            if src is None:
                continue
            with src, open(out_path, "wb") as out:
                shutil.copyfileobj(src, out, length=_CHUNK)
            extracted.append(rel)
    if not extracted:
        raise FetchError(
            "no vector files found under '{}' in archive -- is the manifest "
            "'archive_member_prefix' correct?".format(member_prefix))
    return sorted(extracted)


def _write_checksum_index(dest_dir, names):
    """Compute and write a per-file SHA-256 index for tamper detection."""
    lines = []
    for name in sorted(names):
        digest = _sha256_file(os.path.join(dest_dir, name))
        lines.append("{}  {}".format(digest, name))
    with open(os.path.join(dest_dir, CHECKSUM_INDEX), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def _read_checksum_index(dest_dir):
    """Parse the per-file SHA-256 index into a {name: digest} dict."""
    path = os.path.join(dest_dir, CHECKSUM_INDEX)
    index = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            digest, _, name = line.partition("  ")
            if not name:
                raise FetchError("malformed checksum index line: {!r}".format(line))
            index[name] = digest
    return index


def _verify_cached(dest_dir, expected_sha):
    """Verify an existing cache dir against its stamp + per-file index.

    Returns the number of verified vector files on success.  Raises FetchError
    with a clear hash-mismatch message if any cached byte has changed, or
    returns None if the cache is incomplete/absent (caller should (re)fetch).
    """
    stamp_path = os.path.join(dest_dir, FETCH_STAMP)
    index_path = os.path.join(dest_dir, CHECKSUM_INDEX)
    if not (os.path.isfile(stamp_path) and os.path.isfile(index_path)):
        return None

    with open(stamp_path, "r", encoding="utf-8") as fh:
        stamp = json.load(fh)
    if stamp.get("archive_sha256") != expected_sha:
        # The manifest was re-pinned since this cache was built -- refetch.
        return None

    index = _read_checksum_index(dest_dir)
    present = {
        n for n in os.listdir(dest_dir)
        if os.path.isfile(os.path.join(dest_dir, n)) and n not in _BOOKKEEPING
    }
    expected_names = set(index)
    missing = expected_names - present
    if missing:
        return None  # incomplete cache -> refetch
    extra = present - expected_names
    if extra:
        raise FetchError(
            "cache for '{}' has {} unexpected file(s) (e.g. {}); "
            "delete the directory and refetch".format(
                os.path.basename(dest_dir), len(extra), sorted(extra)[0]))

    for name, expected_digest in index.items():
        actual = _sha256_file(os.path.join(dest_dir, name))
        if actual != expected_digest:
            raise FetchError(
                "hash mismatch for cached vector '{}/{}':\n"
                "    expected {}\n    found    {}\n"
                "  the cached file has been corrupted or modified -- delete "
                "build/cpuoracle/{} and refetch".format(
                    os.path.basename(dest_dir), name, expected_digest, actual,
                    os.path.basename(dest_dir)))
    return len(index)


def fetch_core(manifest, core):
    """Fetch + verify one core.  Returns the number of verified vector files."""
    entry = _core_entry(manifest, core)
    expected_sha = entry["sha256"].lower()
    dest_dir = os.path.join(CACHE_ROOT, core)

    # Idempotent fast path: a complete, untampered cache is a no-op.
    already = _verify_cached(dest_dir, expected_sha)
    if already is not None:
        print("[{}] already verified {} files (no-op)".format(core, already))
        return already

    print("[{}] ref={} -- fetching".format(core, entry["ref"]))
    # Fresh extraction: start from a clean directory.  Surface a clean message
    # (not a raw traceback) if a stale cache dir cannot be removed -- e.g. a
    # locked file on Windows left behind by a previously-interrupted run.
    if os.path.isdir(dest_dir):
        try:
            shutil.rmtree(dest_dir)
        except OSError as exc:
            raise FetchError(
                "cannot clear stale cache directory '{}': {}\n"
                "  delete it manually and re-run".format(dest_dir, exc))
    os.makedirs(dest_dir, exist_ok=True)

    with tempfile.NamedTemporaryFile(
            prefix="cpuoracle-{}-".format(core), suffix=".tar.gz", delete=False) as tmp:
        archive_path = tmp.name
    try:
        _download_with_mirrors(entry["mirrors"], archive_path)

        actual_sha = _sha256_file(archive_path)
        if actual_sha != expected_sha:
            raise FetchError(
                "archive SHA-256 mismatch for core '{}':\n"
                "    expected {}\n    found    {}\n"
                "  the downloaded archive does not match the pinned manifest "
                "hash -- aborting (no files extracted)".format(
                    core, expected_sha, actual_sha))
        print("  archive sha256 verified: {}".format(actual_sha))

        names = _extract_vectors(archive_path, entry["archive_member_prefix"], dest_dir)
        _write_checksum_index(dest_dir, names)

        expected_count = entry.get("expected_file_count")
        if expected_count is not None and len(names) != expected_count:
            raise FetchError(
                "extracted {} vector file(s) for core '{}' but manifest "
                "expects {} -- aborting".format(len(names), core, expected_count))

        stamp = {
            "core": core,
            "ref": entry["ref"],
            "archive_sha256": actual_sha,
            "file_count": len(names),
        }
        with open(os.path.join(dest_dir, FETCH_STAMP), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(stamp, fh, indent=2, sort_keys=True)
            fh.write("\n")
    except BaseException:
        # Never leave a half-populated cache that would look "verified".
        if os.path.isdir(dest_dir):
            shutil.rmtree(dest_dir, ignore_errors=True)
        raise
    finally:
        if os.path.exists(archive_path):
            os.remove(archive_path)

    print("[{}] verified {} files".format(core, len(names)))
    return len(names)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Fetch + verify SingleStepTests CPU vectors into the "
                    "gitignored build/cpuoracle/ cache (ADR 0001).")
    parser.add_argument(
        "--cores", required=True,
        help="comma-separated cores to fetch, e.g. 'z80' or 'z80,m6502,m68000'")
    args = parser.parse_args(argv)

    cores = [c.strip() for c in args.cores.split(",") if c.strip()]
    if not cores:
        print("error: --cores listed no cores", file=sys.stderr)
        return 2

    try:
        manifest = _load_manifest()
        total = 0
        for core in cores:
            total += fetch_core(manifest, core)
    except FetchError as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130

    print("done: {} core(s), {} vector file(s) verified".format(len(cores), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
