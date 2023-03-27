#!/bin/bash
set -x
set -o errexit
set -o nounset

error() {
    echo $1
    exit 1
}

CHECKINSTALL=`which checkinstall`
[ -n "$CHECKINSTALL" ] || error "Error: Please install checkinstall."

src=$(pwd)
pkgversion="22.0.4"
[ -n "$src" -a -d "$src" ] || error "Error: qemu source code repo doesn't exist."


[ ! -d ${src}/build ] && mkdir -p ${src}/build
pushd $src/build

distribution=$(lsb_release -i | cut -d: -f2 | sed 's|^\s||g' | tr '[:upper:]' '[:lower:]') # -> ubuntu
os_dist_name=$(lsb_release -c | cut -d: -f2 | sed 's|^\s||g') # -> bionic

pkgversion=${pkgversion}-${distribution}-${os_dist_name}

ARCH=$(dpkg --print-architecture)

../configure --prefix=/usr/local --target-list=x86_64-linux-user,x86_64-softmmu --disable-smartcard --disable-seccomp --disable-glusterfs '--with-pkgversion=test-qemu ubuntu-bionic 99 build-1' --disable-tpm --disable-gtk --disable-cocoa --disable-sdl --disable-xen --disable-libssh --enable-numa --enable-plugins

$CHECKINSTALL  -t debian -D -y -d2 --pkgarch=amd64 --type=debian --install=no --fstrans=no --deldoc=yes --gzman=yes --reset-uids=yes --pkgname=test-qemu --pkgversion=$pkgversion --pkgrelease=01 --exclude=/home --maintainer=test@test.com --backup=no

make -j32

popd
