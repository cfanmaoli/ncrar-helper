# ncrar-helper protocol (v1)

The helper is a plain executable. Nimble Commander launches it and talks to it over pipes; nothing
is linked into the GPL application. Each invocation performs one command and exits.

## Commands

    ncrar-helper version
    ncrar-helper list <archive> [--password-stdin]
    ncrar-helper read <archive> <entry-index> [--password-stdin]
    ncrar-helper link <archive> <entry-index> [--password-stdin]

--password-stdin reads one line from stdin (never pass passwords as arguments). Without it the
password is taken from the NCRAR_PASSWORD environment variable. An empty password means "no
password".

## version

One JSON object on stdout:

    {"protocol":1,"unrar":11}

`protocol` is the version of this document. `unrar` is the UnRAR **DLL API** version reported by
`RARGetDllVersion()` (11 for UnRAR 7.x), not the library's release number - the library release the
binary was built against is stated in the README and the release notes.

## list

One JSON object per entry, in archive order, then a terminator:

    {"index":0,"name":"dir/file.txt","dir":false,"encrypted":false,"solid":false,
     "size":123,"packed":100,"mtime":1537937022,"crc":123456,"link_type":0,"mode":33188}
    ...
    {"done":true,"entries":N}

Fields:

| field | meaning |
|---|---|
| index | 0-based entry index, the same value that read and link expect |
| name | entry path inside the archive, UTF-8, '/'-separated, no leading slash |
| dir | the entry is a directory |
| encrypted | the entry payload is encrypted |
| solid | the entry belongs to a solid block |
| size, packed | unpacked and packed sizes in bytes |
| mtime | modification time, Unix seconds (0 when unknown) |
| crc | CRC32 from the archive header |
| link_type | 0 = regular, 1 = unix symlink, 2 = windows symlink, 3 = junction, 4 = hardlink, 5 = file copy |
| link_target | target of a link when the archive header already carries it (normally RAR5); absent otherwise |
| mode | Unix file mode from the archive header (st_mode, e.g. 33188 = 0100644); absent when the entry has no Unix attributes |

RAR4 unix symlinks store their target in the entry payload, so list does not extract them and
leaves link_target out. Use the link command for those entries.

Entries are written to stdout as soon as they are read, so a consumer can start processing while the
archive is still being walked. The helper refuses a listing beyond 5,000,000 entries or 512 MB of
output (exit code 4) instead of buffering it.

Because of that streaming the terminator is not guaranteed: if the archive becomes unreadable
half-way through (a damaged block, a missing volume), the entries read so far have already been
printed and the command then exits with code 4. A consumer must treat a non-zero exit status as a
failure and not rely on the terminator alone.

mode is optional on purpose: Windows-written archives carry DOS attributes instead of a Unix mode
and entries without the 0x8000 Unix-attributes marker have no mode to report. Clients must treat
the field as absent, not as zero, when it isn't there.

## read

Writes the raw payload of the entry to stdout, byte for byte, and nothing else. Directories fail
with exit code 4; links produce no data (use link for their target). The caller may kill the
process (SIGTERM) to cancel.

## link

Writes one JSON object with the target of a symlink, junction or hardlink:

    {"target":"test.txt"}

Targets that UnRAR stores in the header (normally RAR5 links) are returned directly. RAR4 unix
symlinks store the target in the entry payload and fail RAR_TEST; for those entries the helper
extracts the current entry into a fresh mkdtemp directory, using a fixed output name (`link`) so
that the entry name from the archive cannot influence where it is written, reads the created
symlink with readlink(2), and removes the directory again. The target string is returned exactly as
stored in the archive.

The command fails with code 4 when the entry is not a link or when its target cannot be
determined. The temporary directory is removed on every normal exit path, including errors; a
process killed with SIGKILL/SIGTERM can leave it behind in `$TMPDIR`.

## Exit codes

| code | meaning |
|---|---|
| 0 | success |
| 2 | usage error / invalid entry index |
| 3 | the archive cannot be opened |
| 4 | the archive/entry cannot be read, a volume is missing, or the entry is not a link |
| 5 | a password is required or wrong |

Diagnostics go to stderr as plain text.

## Multi-volume archives

The helper follows the volumes that sit next to the first one; the library derives their names.
Before continuing into the next volume the helper checks that it exists - a missing, unreadable or
renamed volume ends the operation promptly with exit code 4 instead of being retried. A volume that
exists but can't be a RAR volume (a FIFO, a device, a directory) is refused the same way: the
helper reports 3 or 4 depending on where the library notices, but never waits for it.

## Limitations

* Link targets are returned as stored by the archive. RAR4 absolute symlink targets and targets
  containing ".." that UnRAR considers unsafe may be refused during the temporary extraction; the
  link command then answers with code 4.
* Hardlinks whose target is not stored in the header cannot be resolved by the extraction trick:
  UnRAR needs the target file to exist. RAR5 hardlinks normally carry the target in the header and
  work through link; otherwise the command fails with code 4.
* A file-copy entry (`link_type` 5) is a reference to another entry, not a payload. `link` returns
  the referenced name when the header carries it; `read` produces no data.
* An encrypted RAR4 link can be resolved only if its password was supplied to link via
  NCRAR_PASSWORD or --password-stdin. Without a password, link fails with code 5.
* RAR4 has no password verifier. A wrong password on an encrypted RAR4 payload may therefore be
  reported as code 4 ("the archive data is damaged") rather than code 5.
* A truncated archive can be accepted by UnRAR as an empty archive (0 entries) instead of raising
  an error. This is UnRAR's header-search behaviour, not a crash; malformed headers that UnRAR
  rejects are answered with an error.
