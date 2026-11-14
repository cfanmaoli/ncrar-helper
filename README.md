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

    ncrar-helper version
    ncrar-helper list <archive>                  # JSON lines, one per entry
    ncrar-helper read <archive> <entry-index>    # raw entry data on stdout
    ncrar-helper link <archive> <entry-index>    # link target as JSON

Each invocation performs one command. Passwords are taken from the NCRAR_PASSWORD environment
variable, or from the first line of stdin when --password-stdin is given (never from the command
line, so that they do not show up in ps).

## Protocol

See docs/protocol.md. The CLI is one-shot; list emits one JSON object per entry (including the
optional Unix mode field) and read emits the raw entry data.

## Packaging

    scripts/package.sh

Builds dist/ncrar-helper-<version>-macos-<arch>.dmg: a ncrar-helper.app bundle (the binary, the
license and these docs inside) next to an /Applications shortcut, plus a SHA256 file. The user
drags the bundle into Applications - there is no installer and no script to run. The component is
distributed separately from the GPL application.
