# ncrar-helper protocol (v1)

The helper is a plain executable. Nimble Commander launches it and talks to it over pipes; nothing
is linked into the GPL application. Two modes are available:

* one-shot commands, for simple file-manager operations;
* serve, a persistent JSON-line session that keeps one archive's listing cached.

## Commands

    ncrar-helper version
    ncrar-helper list <archive> [--password-stdin]
    ncrar-helper read <archive> <entry-index> [--password-stdin]
    ncrar-helper link <archive> <entry-index> [--password-stdin]
    ncrar-helper serve

The first four are one-shot commands; serve is the persistent mode documented below.

--password-stdin reads one line from stdin (never pass passwords as arguments). Without it the
password is taken from the NCRAR_PASSWORD environment variable. An empty password means "no
password". serve does not use either mechanism: it takes the password in each open command.

## version

One JSON object on stdout:

    {"protocol":1,"unrar":11}

## list

One JSON object per entry, in archive order, then a terminator:

    {"index":0,"name":"dir/file.txt","dir":false,"encrypted":false,"solid":false,
     "size":123,"packed":100,"mtime":1537937022,"crc":123456,"link_type":0}
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

mode is optional on purpose: Windows-written archives carry DOS attributes instead of a Unix mode and
entries without the 0x8000 Unix-attributes marker have no mode to report. Clients must treat the
field as absent, not as zero, when it isn't there.

## read

Writes the raw payload of the entry to stdout, byte for byte, and nothing else. Directories fail
with exit code 4; links produce no data (use link for their target). The caller may kill the
process (SIGTERM) to cancel.

## link

Writes one JSON object with the target of a symlink, junction or hardlink:

    {"target":"test.txt"}

Exit codes are the same as for the other one-shot commands. The command fails with code 4 when the
entry is not a link or its target cannot be determined.

## Exit codes

| code | meaning |
|---|---|
| 0 | success |
| 2 | usage error / malformed request |
| 3 | the archive cannot be opened |
| 4 | the archive/entry cannot be read, or the entry is not a link |
| 5 | a password is required or wrong |

Diagnostics go to stderr as plain text in one-shot mode.

## Multi-volume archives

The helper follows the volumes that sit next to the first one; the library derives their names.
A missing volume ends the operation with exit code 4.

# serve mode

ncrar-helper serve reads commands from stdin and writes responses to stdout. The mode is stateful:
open selects one archive and caches its entry listing, and subsequent commands refer to that
archive until close, another open, or quit.

## Framing

* Every command and every JSON response is exactly one line, terminated by a newline, and is a
  JSON object. The helper does not require a particular key order.
* Binary data is written only after a {"event":"data","size":S} header and consists of exactly S
  bytes. There is no terminating newline after the payload; the next byte on stdout starts the next
  JSON response.
* A malformed line is answered with an error object. The session stays alive and keeps reading the
  next line.
* quit exits with status 0 without writing a response.

## Commands

### open

    {"cmd":"open","archive":"/path/to/file.rar","password":"secret"}

Opens the archive, reads its entry listing, and caches it. password is optional; when it is
missing or empty the helper uses no password. Response:

    {"ok":true,"entries":N}

If the archive contains encrypted entries and no password was supplied, the command fails with
code 5:

    {"ok":false,"code":5,"error":"a password is required"}

The password is only checked for presence at this point. A wrong password may still let RAR4
headers be listed; read then fails when the payload cannot be decoded. Headers that UnRAR itself
cannot decrypt fail during open with code 5. An unreadable or malformed archive is answered with
code 3 or 4 instead of terminating the process. A failed open leaves the session closed.

open is also the only way to set or change the archive and password. A second open replaces the
current archive.

### list

    {"cmd":"list"}

Replays the cached entry listing as the same JSON lines that the one-shot list command prints,
followed by the terminator:

    {...entry...}
    ...
    {"done":true,"entries":N}

It does not re-read the archive. With no open archive it answers with an error object.

### read

    {"cmd":"read","index":N}

On success the helper writes a data header and then the raw payload:

    {"event":"data","size":S}
    <exactly S bytes>

size is the number of bytes that follow. Directories answer with an error object. Link entries
(answer 0 bytes) produce:

    {"event":"data","size":0}

The helper extracts the entry into an unlinked temporary file before announcing the size, so an
extraction or decryption error is reported as an error object and never truncates a binary payload.
On error no data header is written; the response is:

    {"ok":false,"code":C,"error":"..."}

A missing or negative index, or one beyond the cached listing, answers with an error. The index is
checked against the cached listing from open.

### link

    {"cmd":"link","index":N}

Returns the target of a symlink, junction or hardlink:

    {"target":"relative/or/absolute/path"}

Targets that UnRAR stores in the header (normally RAR5 links) are returned directly from the cached
metadata. RAR4 unix symlinks store the target in the entry payload and fail RAR_TEST; for those
entries the helper extracts the current entry into a fresh mkdtemp directory with
RARProcessFile(RAR_EXTRACT, tempdir, nullptr), reads the created symlink with readlink(2), and
removes the directory again. The target string is returned exactly as stored in the archive.

The command answers with {"ok":false,"code":4,"error":"..."} when the entry is not a link or when
the target cannot be recovered. No temporary files or directories are left behind on any path,
including errors.

### close

    {"cmd":"close"}

Forgets the cached archive and password. Response:

    {"ok":true}

It is valid to call close when no archive is open.

### quit

    {"cmd":"quit"}

Exits the process with status 0. Any buffered command input is discarded.

### Unknown commands

Any other or missing cmd answers with an error object and leaves the session alive.

## Error objects in serve mode

    {"ok":false,"code":C,"error":"human readable message"}

code uses the same values as the one-shot exit codes: 2 = usage/malformed request, 3 = cannot
open the archive, 4 = cannot read the entry or not a link, 5 = password required/wrong. Error text
is stable enough to display to a user but clients should rely on code first.

## Example session

    -> {"cmd":"open","archive":"/tmp/test_read_format_rar5_stored.rar"}
    <- {"ok":true,"entries":1}
    -> {"cmd":"list"}
    <- {"index":0,"name":"helloworld.txt","dir":false,"encrypted":false,"solid":false,"size":29,"packed":29,"mtime":1537937022,"crc":2510308276,"link_type":0}
    <- {"done":true,"entries":1}
    -> {"cmd":"read","index":0}
    <- {"event":"data","size":29}
    <- hello libarchive test suite!
    -> {"cmd":"close"}
    <- {"ok":true}
    -> {"cmd":"quit"}

## Limitations

* Link targets are returned as stored by the archive. RAR4 absolute symlink targets and targets
  containing ".." that UnRAR considers unsafe may be refused during the temporary extraction; the
  link command then answers with code 4.
* Hardlinks whose target is not stored in the header cannot be resolved by the extraction trick:
  UnRAR needs the target file to exist. RAR5 hardlinks normally carry the target in the header and
  work through link; otherwise the command fails with code 4.
* An encrypted RAR4 link can be resolved only if its password was supplied to open. Without a
  password, open already refuses encrypted data with code 5.
* RAR4 has no password verifier. A wrong password on an encrypted RAR4 payload may therefore be
  reported as code 4 ("the archive data is damaged") rather than code 5.
* The cached listing is memory-resident; the helper reopens the archive for read and for the
  extraction fallback of link. The process itself stays alive between commands.
* A truncated archive can be accepted by UnRAR as an empty archive (0 entries) instead of raising
  an error. This is UnRAR's header-search behaviour, not a crash; malformed headers that UnRAR
  rejects are answered with an error object.
