#!/bin/sh
# build-pkg.sh - Build spm SVR4 package on Solaris
# Run this on the SPARCstation (or any Solaris 7 SPARC system)
#
# Usage: sh build-pkg.sh
#
# Produces: spm-VERSION-sparc.pkg

PKGNAME="SSTspm"
VERSION="0.1.0"
BASEDIR="/opt/sst"
SRCDIR=${SRCDIR:-`pwd`}
BUILDDIR="/tmp/spm-build"
STAGEDIR="/tmp/spm-stage"
PKGDIR="/tmp/spm-pkg"

echo "=== spm SVR4 Package Builder ==="
echo "Source: ${SRCDIR}"
echo ""

# Clean previous builds
rm -rf ${BUILDDIR} ${STAGEDIR} ${PKGDIR} /tmp/spm-spool
mkdir -p ${BUILDDIR}
mkdir -p ${STAGEDIR}${BASEDIR}/bin
mkdir -p ${STAGEDIR}${BASEDIR}/etc
mkdir -p ${STAGEDIR}${BASEDIR}/var/cache
mkdir -p ${STAGEDIR}${BASEDIR}/var/index
mkdir -p ${STAGEDIR}${BASEDIR}/var/rollback
mkdir -p ${STAGEDIR}${BASEDIR}/man/man1m
mkdir -p ${PKGDIR}
mkdir -p /tmp/spm-spool

# Copy source and build
echo "--- Compiling ---"
cp ${SRCDIR}/*.c ${SRCDIR}/*.h ${SRCDIR}/Makefile ${BUILDDIR}/
cd ${BUILDDIR}
make clean 2>/dev/null
make ${CC:+CC=$CC}
if [ $? -ne 0 ]; then
    echo "BUILD FAILED"
    exit 1
fi
echo "Build OK"

# Stage files
echo "--- Staging ---"
cp ${BUILDDIR}/spm ${STAGEDIR}${BASEDIR}/bin/
cp ${BUILDDIR}/spm-gui ${STAGEDIR}${BASEDIR}/bin/
cp ${BUILDDIR}/spm-agent ${STAGEDIR}${BASEDIR}/bin/
cp ${SRCDIR}/README.md ${STAGEDIR}${BASEDIR}/ 2>/dev/null
cp ${SRCDIR}/spm.1m ${STAGEDIR}${BASEDIR}/man/man1m/ 2>/dev/null

# Stage init.d script
mkdir -p ${STAGEDIR}/etc/init.d
cp ${SRCDIR}/spm-agent.init ${STAGEDIR}/etc/init.d/spm-agent
chmod 755 ${STAGEDIR}/etc/init.d/spm-agent 2>/dev/null

# Stage CDE desktop integration files
mkdir -p ${STAGEDIR}/usr/dt/appconfig/types/C
mkdir -p ${STAGEDIR}/usr/dt/appconfig/icons/C
cp ${SRCDIR}/dt/types/spm.dt ${STAGEDIR}/usr/dt/appconfig/types/C/
cp ${SRCDIR}/dt/icons/Spm.l.pm ${STAGEDIR}/usr/dt/appconfig/icons/C/
cp ${SRCDIR}/dt/icons/Spm.m.pm ${STAGEDIR}/usr/dt/appconfig/icons/C/
cp ${SRCDIR}/dt/icons/Spm.l.bm ${STAGEDIR}/usr/dt/appconfig/icons/C/

# Also stage to share/dt/ so postinstall can copy them (bootstrap fallback)
mkdir -p ${STAGEDIR}${BASEDIR}/share/dt
cp ${SRCDIR}/dt/types/spm.dt ${STAGEDIR}${BASEDIR}/share/dt/
cp ${SRCDIR}/dt/icons/Spm.l.pm ${STAGEDIR}${BASEDIR}/share/dt/
cp ${SRCDIR}/dt/icons/Spm.m.pm ${STAGEDIR}${BASEDIR}/share/dt/
cp ${SRCDIR}/dt/icons/Spm.l.bm ${STAGEDIR}${BASEDIR}/share/dt/

# Generate default config if not present
if [ ! -f ${STAGEDIR}${BASEDIR}/etc/repos.conf ]; then
    cp "${SRCDIR}/repos.conf.example" "${STAGEDIR}${BASEDIR}/etc/repos.conf"
fi

echo "Staged to ${STAGEDIR}"

# Generate prototype file
echo "--- Generating prototype ---"
PSTAMP=`date '+%Y%m%d%H%M%S'`
cat > ${PKGDIR}/pkginfo <<EOF
PKG=${PKGNAME}
NAME=spm - Sunstorm Package Manager
ARCH=sparc
VERSION=${VERSION}
CATEGORY=system
VENDOR=Julian Wolfe
EMAIL=
PSTAMP=${PSTAMP}
BASEDIR=/
CLASSES=none
EOF

cp ${SRCDIR}/pkg/depend ${PKGDIR}/ 2>/dev/null
cp ${SRCDIR}/pkg/postinstall ${PKGDIR}/
cp ${SRCDIR}/pkg/preremove ${PKGDIR}/

# Build prototype — all absolute paths, root = STAGEDIR
cat > ${PKGDIR}/prototype <<PROTO
i pkginfo
i postinstall
i preremove
d none ${BASEDIR}/bin 0755 root bin
f none ${BASEDIR}/bin/spm 0755 root bin
f none ${BASEDIR}/bin/spm-gui 0755 root bin
f none ${BASEDIR}/bin/spm-agent 0755 root bin
d none ${BASEDIR}/etc 0755 root bin
f none ${BASEDIR}/etc/repos.conf 0644 root bin
d none ${BASEDIR}/var 0755 root bin
d none ${BASEDIR}/var/cache 0755 root bin
d none ${BASEDIR}/var/index 0755 root bin
d none ${BASEDIR}/var/rollback 0755 root bin
d none /etc/init.d 0755 root sys
f none /etc/init.d/spm-agent 0755 root sys
d none /usr/dt/appconfig/types/C 0755 root bin
f none /usr/dt/appconfig/types/C/spm.dt 0644 root bin
d none /usr/dt/appconfig/icons/C 0755 root bin
f none /usr/dt/appconfig/icons/C/Spm.l.pm 0644 root bin
f none /usr/dt/appconfig/icons/C/Spm.m.pm 0644 root bin
f none /usr/dt/appconfig/icons/C/Spm.l.bm 0644 root bin
d none ${BASEDIR}/share 0755 root bin
d none ${BASEDIR}/share/dt 0755 root bin
f none ${BASEDIR}/share/dt/spm.dt 0644 root bin
f none ${BASEDIR}/share/dt/Spm.l.pm 0644 root bin
f none ${BASEDIR}/share/dt/Spm.m.pm 0644 root bin
f none ${BASEDIR}/share/dt/Spm.l.bm 0644 root bin
PROTO

# Add depend if present
if [ -f ${PKGDIR}/depend ]; then
    echo "i depend" >> ${PKGDIR}/prototype
fi

# Add README if staged
if [ -f ${STAGEDIR}${BASEDIR}/README.md ]; then
    echo "f none ${BASEDIR}/README.md 0644 root bin" >> ${PKGDIR}/prototype
fi

# Add man page if staged
if [ -f ${STAGEDIR}${BASEDIR}/man/man1m/spm.1m ]; then
    echo "d none ${BASEDIR}/man 0755 root bin" >> ${PKGDIR}/prototype
    echo "d none ${BASEDIR}/man/man1m 0755 root bin" >> ${PKGDIR}/prototype
    echo "f none ${BASEDIR}/man/man1m/spm.1m 0644 root bin" >> ${PKGDIR}/prototype
fi

echo "Prototype:"
cat ${PKGDIR}/prototype
echo ""

# Build package
echo "--- Building package ---"
pkgmk -o -r ${STAGEDIR} -d /tmp/spm-spool -f ${PKGDIR}/prototype
if [ $? -ne 0 ]; then
    echo "PACKAGING FAILED"
    exit 1
fi

# Convert to datastream
echo "--- Creating datastream package ---"
OUTPKG="/tmp/${PKGNAME}-${VERSION}-sparc.pkg"
pkgtrans -s /tmp/spm-spool ${OUTPKG} ${PKGNAME}

# Compress with Solaris compress
echo "--- Compressing ---"
compress -f ${OUTPKG}
OUTPKG="${OUTPKG}.Z"

echo ""
echo "=== Package built successfully ==="
echo "Output: ${OUTPKG}"
ls -la ${OUTPKG}
echo ""
echo "Install with: zcat ${OUTPKG} | pkgadd -d /dev/stdin"
