# ncrar-helper

A small, standalone RAR reader that provides optional RAR support to Nimble Commander-Fork.

Nimble Commander is distributed under the GPLv3, and the UnRAR sources cannot be linked into it
(the UnRAR license forbids using them to build a RAR-compatible archiver and does not allow the
extra restriction under the GPL). This helper keeps the two apart: it is a **separate program**
that the file manager invokes through a pipe, so the GPL application never contains or links any
UnRAR code.

The helper only reads RAR archives - it lists entries and streams their data. It does not and
must not implement compression.

## License

The UnRAR sources are (c) Alexander Roshal and are used under the UnRAR license, see LICENSE.
This project's own wrapper code is released under the same terms as the rest of this package.

## Building

    scripts/bootstrap-unrar.sh     # downloads and unpacks the UnRAR sources into vendor/unrar
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

## Usage

One-shot commands:

    ncrar-helper version
    ncrar-helper list <archive>                  # JSON lines, one per entry
    ncrar-helper read <archive> <entry-index>    # raw entry data on stdout
    ncrar-helper link <archive> <entry-index>    # link target as JSON

Persistent mode:

    ncrar-helper serve                           # JSON commands on stdin, responses on stdout

Passwords are taken from the NCRAR_PASSWORD environment variable, or from the first line of stdin
when --password-stdin is given (never from the command line, so that they do not show up in ps).
In serve mode the password travels in the open command instead.

## Protocol

See docs/protocol.md. Both the one-shot commands and the persistent serve mode are documented
there. serve keeps the entry listing in memory between commands and supports open, list, read,
link, close and quit. It reopens the archive for read and for the RAR4 symlink fallback in link.

## Packaging

    scripts/package.sh

Builds dist/ncrar-helper-<version>-macos-<arch>.zip with the binary, the license and these
docs, plus a SHA256 next to it. The component is distributed separately from the GPL application.
