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
import struct
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

# Version of the in-fetcher decoder output schema.  Bump this whenever the
# *decoded* JSON the harness consumes changes shape (e.g. a new per-case field),
# so an existing cache built by an older decoder is detected as stale and
# re-decoded even though the upstream archive (and its sha256) is unchanged.
#   v1: registers + prefetch + ram + length
#   v2: + per-case `addr_error` marker (ADR 0006 case-level cycle allowlist)
DECODER_VERSION = 2

# Stream downloads/hashing in chunks to keep memory flat on ~1 GB archives.
_CHUNK = 1024 * 1024

# Network timeout per mirror attempt, in seconds.  The corpus archives are
# large; a generous timeout avoids spurious failures on slow links while still
# bounding a hung connection.
_TIMEOUT = 600


class FetchError(Exception):
    """A user-facing fetch/verify failure (printed without a traceback)."""


# ---------------------------------------------------------------------------
# m68000 binary-fixture decoder
# ---------------------------------------------------------------------------
#
# The SingleStepTests/m68000 corpus ships its vectors as a custom little-endian
# binary container (``*.json.bin``) rather than plain JSON.  The format is
# documented by the upstream ``decode.py`` at the pinned ref; this is a faithful
# re-implementation that emits the same logical test objects the harness needs.
#
# We deliberately decode at *fetch* time (not in the C++ harness) so the harness
# stays JSON-uniform across all cores -- it discovers and parses ``*.json`` for
# every core identically.  The decoded JSON keeps the fields the oracle asserts
# on (per-register initial/final state, the prefetch queue, byte-addressed RAM,
# and the architectural cycle count ``length``); the verbose per-cycle
# ``transactions`` bus log is dropped to keep the cache from ballooning, since
# the harness measures cycles from the core itself, not from that log.

# 68000 register order in the binary state block (matches upstream decode.py).
_M68K_REG_ORDER = (
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "usp",
    "ssp", "sr", "pc",
)

# Magic constants from the upstream container format.
_M68K_MAGIC_FILE = 0x1A3F5D71
_M68K_MAGIC_TEST = 0xABC12367
_M68K_MAGIC_NAME = 0x89ABCDEF
_M68K_MAGIC_STATE = 0x01234567
_M68K_MAGIC_TRANS = 0x456789AB


def _m68k_read_name(content, ptr):
    _numbytes, magic = struct.unpack_from("<II", content, ptr)
    ptr += 8
    if magic != _M68K_MAGIC_NAME:
        raise FetchError("m68000 decode: bad name magic 0x{:08x}".format(magic))
    strlen = struct.unpack_from("<I", content, ptr)[0]
    ptr += 4
    name = struct.unpack_from("{}s".format(strlen), content, ptr)[0].decode("utf-8")
    ptr += strlen
    return ptr, name


def _m68k_read_state(content, ptr):
    state = {}
    _numbytes, magic = struct.unpack_from("<II", content, ptr)
    ptr += 8
    if magic != _M68K_MAGIC_STATE:
        raise FetchError("m68000 decode: bad state magic 0x{:08x}".format(magic))
    for reg in _M68K_REG_ORDER:
        state[reg] = struct.unpack_from("<I", content, ptr)[0]
        ptr += 4
    pf0, pf1 = struct.unpack_from("<II", content, ptr)
    ptr += 8
    state["prefetch"] = [pf0, pf1]
    num_rams = struct.unpack_from("<I", content, ptr)[0]
    ptr += 4
    ram = []
    for _ in range(num_rams):
        addr, data = struct.unpack_from("<IH", content, ptr)
        ptr += 6
        # each entry is a 16-bit big-endian word -> two byte cells
        ram.append([addr, data >> 8])
        ram.append([addr | 1, data & 0xFF])
    state["ram"] = ram
    return ptr, state


# Upstream transaction type bytes (SingleStepTests/m68000 decode.py): 0=idle,
# 1=write, 2=read, 3=TAS cycle, 4=read address error (AS not asserted),
# 5=write address error (AS not asserted).  The two address-error types mark a
# case whose bus activity diverges from a normally-committed access -- they are
# the case-level cycle-divergence allowlist key for the m68000 oracle gate
# (ADR 0006 Leg A): the corpus does not commit results when AS isn't asserted,
# so its cycle count for those cases is not comparable to the live core's.
_M68K_TW_READ_ADDR_ERROR = 4
_M68K_TW_WRITE_ADDR_ERROR = 5


def _m68k_skip_transactions(content, ptr):
    """Consume the transactions block, returning (ptr, num_cycles, addr_error).

    The verbose per-cycle bus log itself is discarded (the harness measures
    cycles from the core, not from the log -- decision #2, ADR 0006), but two
    summary facts are kept: ``num_cycles`` (the fixture's architectural cycle
    length) and ``addr_error`` -- True iff any transaction is a read/write
    address-error cycle (``re``/``we``).  ``addr_error`` is the single bit the
    case-level cycle allowlist needs; keeping just the bit (not the whole log)
    keeps the cache small while making the address-error cases self-describing."""
    _numbytes, magic = struct.unpack_from("<II", content, ptr)
    ptr += 8
    if magic != _M68K_MAGIC_TRANS:
        raise FetchError("m68000 decode: bad transactions magic 0x{:08x}".format(magic))
    num_cycles, num_transactions = struct.unpack_from("<II", content, ptr)
    ptr += 8
    addr_error = False
    for _ in range(num_transactions):
        tw = struct.unpack_from("<B", content, ptr)[0]
        ptr += 5  # type byte + 4-byte cycle count
        if tw in (_M68K_TW_READ_ADDR_ERROR, _M68K_TW_WRITE_ADDR_ERROR):
            addr_error = True
        if tw != 0:
            ptr += 20  # fc, addr, data, UDS, LDS (5 x u32)
    return ptr, num_cycles, addr_error


def _decode_m68000_bin(blob):
    """Decode one ``*.json.bin`` byte string into a list of test dicts."""
    ptr = 0
    magic, num_tests = struct.unpack_from("<II", blob, ptr)
    ptr += 8
    if magic != _M68K_MAGIC_FILE:
        raise FetchError("m68000 decode: bad file magic 0x{:08x}".format(magic))
    tests = []
    for _ in range(num_tests):
        _numbytes, tmagic = struct.unpack_from("<II", blob, ptr)
        ptr += 8
        if tmagic != _M68K_MAGIC_TEST:
            raise FetchError("m68000 decode: bad test magic 0x{:08x}".format(tmagic))
        test = {}
        ptr, test["name"] = _m68k_read_name(blob, ptr)
        ptr, test["initial"] = _m68k_read_state(blob, ptr)
        ptr, test["final"] = _m68k_read_state(blob, ptr)
        ptr, test["length"], addr_error = _m68k_skip_transactions(blob, ptr)
        # Only emit the marker when present, so the bit stays out of the vast
        # majority of (non-address-error) cases and the cache stays compact.
        if addr_error:
            test["addr_error"] = True
        tests.append(test)
    return tests


def _decode_bin_dir(dest_dir):
    """In-place convert every ``*.json.bin`` in ``dest_dir`` to ``*.json``.

    Returns the sorted list of resulting ``*.json`` basenames.  The ``.json.bin``
    inputs are removed so the cache holds only the harness-consumable JSON.
    """
    out_names = []
    for name in sorted(os.listdir(dest_dir)):
        if not name.endswith(".json.bin"):
            continue
        bin_path = os.path.join(dest_dir, name)
        with open(bin_path, "rb") as fh:
            blob = fh.read()
        tests = _decode_m68000_bin(blob)
        json_name = name[:-len(".bin")]   # strip trailing ".bin" -> "*.json"
        with open(os.path.join(dest_dir, json_name), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(tests, fh, separators=(",", ":"))
            fh.write("\n")
        os.remove(bin_path)
        out_names.append(json_name)
    return sorted(out_names)


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


def _verify_cached(dest_dir, expected_sha, decoder_version):
    """Verify an existing cache dir against its stamp + per-file index.

    Returns the number of verified vector files on success.  Raises FetchError
    with a clear hash-mismatch message if any cached byte has changed, or
    returns None if the cache is incomplete/absent (caller should (re)fetch).

    ``decoder_version`` is the schema version of the decoder that produces the
    cached files (None for cores with no in-fetcher decode step); a cache built
    by a different decoder version is treated as stale and re-decoded.
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
    if decoder_version is not None and stamp.get("decoder_version") != decoder_version:
        # The decoder output schema changed since this cache was built (e.g. a
        # new per-case field); the archive is unchanged but the decoded JSON is
        # stale -- refetch + re-decode.
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

    # Cores that decode a custom binary container in the fetcher carry a decoder
    # schema version so a cache built by an older decoder self-invalidates.
    decoder_version = DECODER_VERSION if entry.get("format") == "m68000_bin" else None

    # Idempotent fast path: a complete, untampered cache is a no-op.
    already = _verify_cached(dest_dir, expected_sha, decoder_version)
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

        # Some cores (m68000) ship a custom binary container; decode it to plain
        # JSON in place so the harness sees uniform *.json across every core.
        # The per-file SHA-256 index is written over the *decoded* files, so a
        # later integrity re-verify checks exactly what the harness will read.
        if entry.get("format") == "m68000_bin":
            names = _decode_bin_dir(dest_dir)

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
        if decoder_version is not None:
            stamp["decoder_version"] = decoder_version
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
