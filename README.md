# spm — Sunstorm Package Manager

A package manager for Solaris 7 SPARC. Aggregates packages from the [Sunstorm](https://github.com/Sunstorm-Project/sunstorm) distribution and configured GitHub releases, and installs them via the native SVR4 `pkgadd`/`pkgrm` tools.

Public repo: github.com/firefly128/spm — v1.0.0 released.

Part of the Sunstorm distribution (`SSTspm`).

## Features

- **Multi-source repositories** — Indexes TGCware packages and GitHub Releases
- **Dependency resolution** — Recursive dependency tracking for TGCware packages
- **SVR4 integration** — Wraps native `pkgadd`/`pkgrm` for install/remove
- **Rollback** — Roll back to previous package versions from cache
- **Search** — Case-insensitive search across name, description, and package code
- **Source correlation** — Links TGCware binaries to their GitHub source trees
- **Native HTTPS** — Direct TLS via OpenSSL loaded at runtime via `dlopen` — no proxy needed
- **Motif/CDE GUI** — Graphical package manager (`spm-gui`) with search, install, remove, and upgrade
- **Background agent** — `spm-agent` daemon that checks for updates on a configurable interval

## Installation

### Bootstrap package (recommended)

Download `SSTspm-1.0.0-bootstrap-sparc.pkg.Z` from the [Releases](https://github.com/firefly128/spm/releases) page, transfer it to the Solaris system, then:

```sh
uncompress SSTspm-1.0.0-bootstrap-sparc.pkg.Z
pkgadd -d SSTspm-1.0.0-bootstrap-sparc.pkg
```

The bootstrap package bundles all dependencies (OpenSSL, zlib, libsolcompat, prngd, wget). No prerequisites are needed beyond a base Solaris 7 SPARC install. The postinstall script starts `prngd`, sets up init scripts, and adds `/opt/sst/bin` to the system PATH via `/etc/default/login`.

Log out and back in, or in an existing shell:

```sh
PATH=/opt/sst/bin:$PATH; export PATH
spm update
spm install bash gcc curl
```

### From the Sunstorm distribution

If Sunstorm packages are already installed, spm is available as `SSTspm` along with its dependencies.

## Requirements

- Solaris 7 (SunOS 5.7) on SPARC
- CDE/Motif (for GUI — standard on Solaris 7 with CDE installed)
- Network access (DNS + HTTPS)

The bootstrap package bundles all other dependencies. The regular `SSTspm` package additionally requires: OpenSSL (`SSTossl`), libsolcompat (`SSTlsolc`), libgcc_s, zlib (`SSTzlib`), and prngd (`SSTprngd`).

## Building from source

```sh
# On the SPARCstation (requires GCC and libsolcompat)
CC=/opt/sst/bin/gcc make    # builds spm, spm-gui, spm-agent

# Individual targets
make cli      # CLI only
make gui      # GUI only
make agent    # Agent only
```

Cross-compilation is not currently supported for spm — it must be built natively on Solaris 7 because it links against Motif (`-lXm`) for the GUI.

## Usage

### CLI

```sh
spm update                    # Refresh package indices from all repos
spm search curl               # Search by name, description, or package code
spm info curl                 # Show package details including dependencies
spm deps curl                 # Show dependency tree
spm install curl              # Install package (resolves dependencies automatically)
spm install bash vim make     # Install multiple packages
spm remove curl               # Remove a package
spm rollback curl             # Roll back to previous version
spm upgrade                   # Upgrade all packages
spm upgrade curl              # Upgrade specific package
spm list                      # List all available packages
spm list --installed          # List installed packages
spm repo list                 # Show configured repositories
spm cache clean               # Clear download cache
```

### GUI (Motif/CDE)

```sh
DISPLAY=:0 spm-gui
```

Two-pane layout: package list with search/filter on the left, package info and log on the right. Action buttons for Install, Remove, Upgrade, and Deps. The status bar shows agent update notifications.

### Background agent

```sh
spm-agent                    # Start agent (daemonizes by default)
spm-agent -f                 # Run in foreground (for debugging)
spm-agent -i 3600            # Set check interval in seconds
spm-agent -s                 # Check agent status
spm-agent -k                 # Stop the agent

/etc/init.d/spm-agent start
/etc/init.d/spm-agent stop
/etc/init.d/spm-agent status
/etc/init.d/spm-agent reload  # SIGHUP: reload config + re-check
```

The agent checks for updates at a configurable interval (default: 6 hours) and writes status to `/opt/sst/var/update.status`. Send `SIGUSR1` to force an immediate check:

```sh
kill -USR1 $(cat /opt/sst/var/agent.pid)
```

## Configuration

`/opt/sst/etc/repos.conf`:

```ini
[ssl]
ca_bundle = /opt/sst/etc/ssl/cert.pem

[agent]
interval = 21600
notify = yes

[repo:sunstorm]
type = github
name = Sunstorm Distribution
owner = Sunstorm-Project
repos = sunstorm
enabled = yes
```

### Repository types

**TGCware** (`type = tgcware`): Indexes the Apache directory listing at jupiterrise.com. Parses filenames like `curl-8.18.0-1.tgc-sunos5.7-sparc-tgcware.gz` and descriptions containing package codes, dependencies, and MD5 checksums.

**GitHub** (`type = github`): Fetches GitHub Releases via the API for each configured repo. Indexes `.pkg` and `.gz` assets as installable packages.

## How Dependencies Work

TGCware packages embed their dependency list in the HTML directory listing as comma-separated package codes (e.g., `TGCgtext,TGClgcc1,TGCzlib`).

When you run `spm install curl`, spm:
1. Looks up `curl` in the available package index
2. Reads its dependency list
3. Checks each dependency against `pkginfo` (the system SVR4 database)
4. Recursively resolves sub-dependencies
5. Installs all missing dependencies in order, then installs the requested package

## SSL / HTTPS

spm loads the package-owned OpenSSL3 libraries from `/opt/sst/lib` via `dlopen`. CA certificates come from `/opt/sst/etc/ssl/cert.pem`; an explicitly configured alternate bundle must load successfully. There is no fallback to legacy installation paths or an unrelated trust store.

## Directory Layout

```
/opt/sst/
├── bin/
│   ├── spm              # CLI package manager
│   ├── spm-gui          # Motif/CDE graphical UI
│   └── spm-agent        # Background update daemon
├── etc/
│   └── repos.conf       # Repository + agent configuration
└── var/
    ├── cache/           # Downloaded package files
    ├── index/
    │   └── available.idx
    ├── installed.db     # Installed package database
    ├── rollback/        # Backed-up packages for rollback
    ├── agent.pid
    ├── agent.log
    └── update.status

/etc/init.d/spm-agent
/etc/rc2.d/S99spm-agent
/etc/rc0.d/K01spm-agent
```

## SVR4 Package

Two package variants are distributed:

- **Bootstrap** (`SSTspm-1.0.0-bootstrap-sparc.pkg.Z`) — Self-contained, bundles all dependencies. For fresh Solaris 7 installs.
- **Regular** (`SSTspm-1.0.0-sparc.pkg.Z`) — Depends on separately installed SSTossl, SSTzlib, SSTlsolc, SSTprngd. Included in the Sunstorm distribution.

Build with:

```sh
CC=/opt/sst/bin/gcc sh build-pkg.sh
```

## License

MIT
