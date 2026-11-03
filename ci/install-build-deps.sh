#!/bin/bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
[[ "$ID" == debian && "$VERSION_ID" == 10 ]] || { echo 'Run only inside the Debian 10 CI builder.' >&2; exit 1; }

# Debian 10 shares UOS 20's glibc 2.28 / Qt 5.11 baseline. No UOS intranet
# repository, host desktop, system proxy or application configuration is used.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 755 /usr/sbin/policy-rc.d
printf 'deb http://archive.debian.org/debian buster main\ndeb http://archive.debian.org/debian-security buster/updates main\n' > /etc/apt/sources.list
printf 'Acquire::Check-Valid-Until "false";\nAcquire::Retries "3";\n' > /etc/apt/apt.conf.d/99archive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 git curl ca-certificates patch \
    dpkg-dev debhelper fakeroot file openssl gzip xz-utils dbus-x11 fonts-dejavu-core \
    qtbase5-dev qtbase5-private-dev qtdeclarative5-dev qttools5-dev qttools5-dev-tools \
    libqt5svg5-dev libqt5x11extras5-dev libgsettings-qt-dev libxcb-util0-dev \
    libstartup-notification0-dev libxext-dev libxi-dev libxrender-dev \
    libfontconfig1-dev libfreetype6-dev libglib2.0-dev libudev-dev libmtdev-dev \
    libegl1-mesa-dev libcups2-dev libuchardet-dev libicu-dev librsvg2-dev
rm -rf /var/lib/apt/lists/*
