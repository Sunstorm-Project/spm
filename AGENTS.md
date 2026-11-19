# Sunstorm repository instructions

Before editing, open and follow `../sunstorm-engineering/AGENTS.md` (relative to this file's directory). If the sibling checkout is absent, read AGENTS.md on the default branch of the [shared engineering repo](https://github.com/Sunstorm-Project/sunstorm-engineering). If both are unavailable, report the missing rules before changing build, installer trust or release behavior.

The native CLI build is `CC=/opt/sst/bin/gcc make cli` on a configured Solaris target. See `README.md` for GUI, agent and packaging commands. Use the distro pipeline recipe when validating the shipped package. Exercise the affected install or upgrade path on Solaris 7 SPARC for package behavior changes.
