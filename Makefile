# spm Makefile for Solaris 7 SPARC
#
# Sunstorm Package Manager for Solaris 7 SPARC.
# Uses OpenSSL loaded at runtime via dlopen for HTTPS.
# Search order: Sunstorm (/opt/sst/lib), TGCware, then system.
#
# Builds three targets:
#   spm       - CLI package manager
#   spm-gui   - Motif/CDE graphical package manager
#   spm-agent - Background update check daemon
#
# Requires: OpenSSL (Sunstorm SSTossl or TGCware, loaded at runtime)
#           Solaris socket/dl libs (-lsocket -lnsl -ldl)
#           Motif/CDE (for GUI: -lXm -lXt -lX11)

# Use tgcware GCC if available, fall back to system cc
CC = /usr/tgcware/gcc47/bin/gcc
#CC = cc

# TGCware paths for OpenSSL headers and libs
TGCWARE = /usr/tgcware

# Sunstorm install prefix
SSTDIR = /opt/sst

# Motif / X11 / CDE paths
MOTIFDIR = /usr/dt
OPENWINDIR = /usr/openwin

CFLAGS = -O2 -Wall -D_REENTRANT -I$(TGCWARE)/include -I$(SSTDIR)/include
LDFLAGS = -s -L$(SSTDIR)/lib -Wl,-R,$(SSTDIR)/lib -L$(TGCWARE)/lib -Wl,-R,$(TGCWARE)/lib
LIBS = -lsolcompat -lsocket -lnsl -ldl

# Motif/X11 link flags (GUI only)
MOTIF_CFLAGS = -I$(MOTIFDIR)/include -I$(OPENWINDIR)/include
MOTIF_LDFLAGS = -L$(MOTIFDIR)/lib -Wl,-R,$(MOTIFDIR)/lib \
                -L$(OPENWINDIR)/lib -Wl,-R,$(OPENWINDIR)/lib
MOTIF_LIBS = -lXm -lXt -lX11 -lm

# Shared source objects (used by all three targets)
SHARED_SRCS = http.c json.c html.c config.c pkgdb.c integrity.c process.c
SHARED_OBJS = $(SHARED_SRCS:.c=.o)

# CLI
CLI_TARGET = spm
CLI_OBJS = spm.o $(SHARED_OBJS)

# GUI
GUI_TARGET = spm-gui
GUI_OBJS = spm-gui.o $(SHARED_OBJS)

# Agent
AGENT_TARGET = spm-agent
AGENT_OBJS = spm-agent.o $(SHARED_OBJS)

PREFIX = /opt/sst

all: $(CLI_TARGET) $(GUI_TARGET) $(AGENT_TARGET)

cli: $(CLI_TARGET)

gui: $(GUI_TARGET)

agent: $(AGENT_TARGET)

$(CLI_TARGET): $(CLI_OBJS)
	$(CC) $(LDFLAGS) -o $(CLI_TARGET) $(CLI_OBJS) $(LIBS)

$(GUI_TARGET): $(GUI_OBJS)
	$(CC) $(LDFLAGS) $(MOTIF_LDFLAGS) -o $(GUI_TARGET) $(GUI_OBJS) $(LIBS) $(MOTIF_LIBS)

$(AGENT_TARGET): $(AGENT_OBJS)
	$(CC) $(LDFLAGS) -o $(AGENT_TARGET) $(AGENT_OBJS) $(LIBS)

# Default .c.o rule for shared sources
.c.o:
	$(CC) $(CFLAGS) -c $<

# GUI source needs Motif includes
spm-gui.o: spm-gui.c config.h http.h html.h json.h pkgdb.h
	$(CC) $(CFLAGS) $(MOTIF_CFLAGS) -c spm-gui.c

# Dependencies
spm.o: spm.c config.h http.h html.h json.h pkgdb.h
spm-agent.o: spm-agent.c config.h http.h json.h html.h pkgdb.h
http.o: http.c http.h config.h
json.o: json.c json.h
html.o: html.c html.h
config.o: config.c config.h
process.o: process.c process.h
integrity.o: integrity.c integrity.h
pkgdb.o: process.h integrity.h pkgdb.c pkgdb.h http.h json.h html.h config.h

clean:
	rm -f $(CLI_TARGET) $(GUI_TARGET) $(AGENT_TARGET) \
	      spm.o spm-gui.o spm-agent.o $(SHARED_OBJS)

install: all
	@echo "Installing spm..."
	mkdir -p $(PREFIX)/bin 2>/dev/null || true
	mkdir -p $(PREFIX)/etc 2>/dev/null || true
	mkdir -p $(PREFIX)/var/cache 2>/dev/null || true
	mkdir -p $(PREFIX)/var/index 2>/dev/null || true
	mkdir -p $(PREFIX)/var/rollback 2>/dev/null || true
	cp $(CLI_TARGET) $(PREFIX)/bin/spm
	cp $(GUI_TARGET) $(PREFIX)/bin/spm-gui
	cp $(AGENT_TARGET) $(PREFIX)/bin/spm-agent
	chmod 755 $(PREFIX)/bin/spm
	chmod 755 $(PREFIX)/bin/spm-gui
	chmod 755 $(PREFIX)/bin/spm-agent
	@if [ ! -f $(PREFIX)/etc/repos.conf ]; then \
		echo "Generating default config..."; \
		$(PREFIX)/bin/spm --version >/dev/null 2>&1; \
	fi
	@echo ""
	@echo "Installing init.d script..."
	cp spm-agent.init /etc/init.d/spm-agent 2>/dev/null || true
	chmod 755 /etc/init.d/spm-agent 2>/dev/null || true
	@echo ""
	@echo "Done! Add /opt/sst/bin to your PATH:"
	@echo "  PATH=/opt/sst/bin:\$$PATH; export PATH"
	@echo ""
	@echo "Start the update agent:"
	@echo "  /etc/init.d/spm-agent start"
	@echo ""
	@echo "Launch the GUI:"
	@echo "  spm-gui"
	@echo ""
	@echo "Or use the CLI: spm update"

.PHONY: all cli gui agent clean install
