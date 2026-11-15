# ncrar-helper

A small, standalone RAR reader that provides optional RAR support to
[Nimble Commander-Fork](https://github.com/cfanmaoli/nimble-commander).

Nimble Commander is distributed under the GPLv3, and the UnRAR sources cannot be linked into it
(the UnRAR license forbids using them to build a RAR-compatible archiver and does not allow the
extra restriction under the GPL). This helper keeps the two apart: it is a **separate program**
that the file manager invokes through a pipe, so the GPL application never contains or links any
UnRAR code.

The helper only reads RAR archives - it lists entries and streams their data. It does not and
must not implement compression.

## Requirements

- macOS 12 or later (published builds are arm64; the sources build on any macOS with CMake)
- CMake 3.20+ and the Xcode command line tools
- `wget` for `scripts/bootstrap-unrar.sh` (`brew install wget`)

## Installing

Download the latest DMG from the [releases](https://github.com/cfanmaoli/ncrar-helper/releases),
open it and drag `ncrar-helper.app` into Applications. There is no installer and no script to run.

Nimble Commander-Fork looks for the helper inside the bundle at
`~/Applications/ncrar-helper.app/Contents/MacOS/ncrar-helper` or
`/Applications/ncrar-helper.app/Contents/MacOS/ncrar-helper` - the first executable one wins.
The bundle is ad-hoc signed and not notarised; if Gatekeeper quarantines it after a download, clear
the attribute once:

    xattr -dr com.apple.quarantine /Applications/ncrar-helper.app

In the file manager the component is controlled by **Preferences → Compression → Use the
ncrar-helper component for RAR archives** (中文界面：设置 → 压缩); the same pane shows whether it is
installed and which version it is. When it is disabled or missing, RAR archives are read by the
application's built-in libarchive reader.

## Building

    brew install wget                     # only needed for the UnRAR download
    scripts/bootstrap-unrar.sh            # downloads and unpacks UnRAR 7.3.2 into vendor/unrar
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

`scripts/package.sh` builds the distributable
`dist/ncrar-helper-<version>-macos-<arch>.dmg`: the `ncrar-helper.app` bundle (the binary, the
license and these docs inside) next to an `/Applications` shortcut, plus a SHA256 file. The version
defaults to 1.0 and can be overridden, e.g. `VERSION=1.1 scripts/package.sh`.

## Automation

`.github/workflows/check-unrar.yml` runs daily and compares the UnRAR version pinned in
`scripts/bootstrap-unrar.sh` with the one offered on rarlab.com. When a newer one appears it bumps
`VERSION` and `UNRAR_VERSION`, builds the DMG and prepares a **draft** release, then opens an issue
assigned to the maintainer. Publishing the draft release is still a manual step.

## Usage

    ncrar-helper version
    ncrar-helper list <archive> [--password-stdin]
    ncrar-helper read <archive> <entry-index> [--password-stdin]
    ncrar-helper link <archive> <entry-index> [--password-stdin]

`version` prints one JSON object, `{"protocol":1,"unrar":11}`, with the protocol version and the
UnRAR library version the helper was built against.

Each invocation performs one command. Passwords are taken from the `NCRAR_PASSWORD` environment
variable, or from the first line of stdin when `--password-stdin` is given (never from the command
line, so that they do not show up in `ps`).

## Protocol

See [docs/protocol.md](docs/protocol.md). The CLI is one-shot; `list` emits one JSON object per
entry (including the optional Unix mode field) followed by a `{"done":true,"entries":N}`
terminator, and `read` emits the raw entry data.

## License

The wrapper code in this repository (the CLI, the build and packaging scripts and the documentation)
is distributed under the MIT license, see LICENSE-MIT.

The bundled UnRAR sources are (c) Alexander Roshal and are used under the UnRAR license, see
LICENSE. That license still governs the RAR handling itself: the program may only be used to handle
RAR archives and must not be used to build a RAR-compatible archiver. The UnRAR license text is
shipped in the application bundle and quoted in the source.
