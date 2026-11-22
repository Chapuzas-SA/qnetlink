#!/usr/bin/env bash
set -euo pipefail

CALLER_USER=${SUDO_USER:-$(id -un)}
CALLER_GROUP=$(id -gn "$CALLER_USER")

SLIRP_VERSION=4.9.3
LWS_VERSION=5.0.0
LWS_SHA256=f853c6582101cfcee3a5a9e28ae92ab19d9735c5f31f0bb2e9794b5106123962
DEBIAN_SUITE=bullseye
DEBIAN_MIRROR=${DEBIAN_MIRROR:-https://deb.debian.org/debian}
SLIRP_URL=${SLIRP_URL:-https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v${SLIRP_VERSION}/libslirp-v${SLIRP_VERSION}.tar.gz}
LWS_URL=${LWS_URL:-https://github.com/warmcat/libwebsockets/archive/refs/tags/v${LWS_VERSION}.tar.gz}

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
PROJECT_VERSION=$(sed -n "s/^project('qnetlink', 'c', version: '\\([^']*\\)'.*/\\1/p" "$PROJECT_DIR/meson.build")
if [[ -z "$PROJECT_VERSION" ]]; then
  echo "error: cannot determine project version from meson.build" >&2
  exit 1
fi
CHROOT_DIR=${CHROOT_DIR:-"$PROJECT_DIR/.build/linux-x86_64/chroot"}
CACHE_DIR=${CACHE_DIR:-"$PROJECT_DIR/.build/linux-x86_64/cache"}
DIST_ROOT=${DIST_ROOT:-"$PROJECT_DIR/dist"}
DIST_NAME="qnetlink-${PROJECT_VERSION}-linux-x86_64"
PLATFORM_DIR="$DIST_ROOT/linux-x86_64"
PACKAGE_STAGE=$(mktemp -d "${TMPDIR:-/tmp}/qnetlink-linux-package.XXXXXX")
trap 'rm -rf "$PACKAGE_STAGE"' EXIT
DIST_DIR="$PACKAGE_STAGE/$DIST_NAME"
SLIRP_ARCHIVE="$CACHE_DIR/libslirp-v${SLIRP_VERSION}.tar.gz"
LWS_ARCHIVE="$CACHE_DIR/libwebsockets-v${LWS_VERSION}.tar.gz"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: required tool not found: $1" >&2
    exit 1
  }
}

for tool in sudo debootstrap curl tar readelf sha256sum; do
  need "$tool"
done

mkdir -p "$CACHE_DIR" "$PLATFORM_DIR"

if [[ -f "$SLIRP_ARCHIVE" ]] && ! tar -tzf "$SLIRP_ARCHIVE" >/dev/null 2>&1; then
  echo "warning: invalid cached libslirp archive, removing" >&2
  rm -f "$SLIRP_ARCHIVE"
fi
if [[ ! -f "$SLIRP_ARCHIVE" ]]; then
  echo "download: libslirp ${SLIRP_VERSION}"
  rm -f "$SLIRP_ARCHIVE.tmp"
  curl --fail --location --retry 3 --retry-all-errors \
    --output "$SLIRP_ARCHIVE.tmp" "$SLIRP_URL"
  tar -tzf "$SLIRP_ARCHIVE.tmp" >/dev/null
  mv "$SLIRP_ARCHIVE.tmp" "$SLIRP_ARCHIVE"
else
  echo "cached:   libslirp ${SLIRP_VERSION}"
fi

if [[ -f "$LWS_ARCHIVE" ]]; then
  actual_lws_sha=$(sha256sum "$LWS_ARCHIVE" | awk '{print $1}')
  if [[ "$actual_lws_sha" != "$LWS_SHA256" ]] || ! tar -tzf "$LWS_ARCHIVE" >/dev/null 2>&1; then
    echo "warning: invalid cached libwebsockets archive, removing" >&2
    rm -f "$LWS_ARCHIVE"
  fi
fi
if [[ ! -f "$LWS_ARCHIVE" ]]; then
  echo "download: libwebsockets ${LWS_VERSION}"
  rm -f "$LWS_ARCHIVE.tmp"
  curl --fail --location --retry 3 --retry-all-errors --output "$LWS_ARCHIVE.tmp" "$LWS_URL"
  echo "$LWS_SHA256  $LWS_ARCHIVE.tmp" | sha256sum -c -
  tar -tzf "$LWS_ARCHIVE.tmp" >/dev/null
  mv "$LWS_ARCHIVE.tmp" "$LWS_ARCHIVE"
else
  echo "cached:   libwebsockets ${LWS_VERSION}"
fi

if [[ ! -x "$CHROOT_DIR/bin/sh" ]]; then
  echo "bootstrap: Debian ${DEBIAN_SUITE} amd64"
  sudo rm -rf "$CHROOT_DIR"
  sudo debootstrap --arch=amd64 --variant=minbase \
    "$DEBIAN_SUITE" "$CHROOT_DIR" "$DEBIAN_MIRROR"
fi

# Keep package setup idempotent. A modern Meson is a build tool only; all C
# objects and runtime libraries are still built/selected inside Bullseye.
echo "prepare:   Bullseye build environment"
sudo chroot "$CHROOT_DIR" /bin/bash -ec '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends \
    build-essential ninja-build pkg-config python3 python3-pip patchelf cmake \
    libglib2.0-dev libssl-dev ca-certificates
  python3 -m pip install --disable-pip-version-check "meson>=0.60,<2"
'

# Refresh sources without ever recursively copying the chroot/cache/build dirs.
echo "source:    libslirp ${SLIRP_VERSION}"
sudo rm -rf "$CHROOT_DIR/src/libslirp" "$CHROOT_DIR/src/libwebsockets" "$CHROOT_DIR/src/qnetlink"
sudo mkdir -p "$CHROOT_DIR/src/libslirp" "$CHROOT_DIR/src/libwebsockets" "$CHROOT_DIR/src/qnetlink"
sudo tar -xzf "$SLIRP_ARCHIVE" -C "$CHROOT_DIR/src/libslirp" --strip-components=1
sudo tar -xzf "$LWS_ARCHIVE" -C "$CHROOT_DIR/src/libwebsockets" --strip-components=1
LWS_PATCH="$PROJECT_DIR/patches/libwebsockets-${LWS_VERSION}/0003-skip-reuse-options-for-inherited-listener.patch"
if [[ -f "$LWS_PATCH" ]]; then
  patch --directory="$CHROOT_DIR/src/libwebsockets" --strip=1 --fuzz=0 --dry-run < "$LWS_PATCH"
  sudo patch --directory="$CHROOT_DIR/src/libwebsockets" --strip=1 --fuzz=0 < "$LWS_PATCH"
fi
(
  cd "$PROJECT_DIR"
  tar -cf - \
    --exclude='./.build' \
    --exclude='./.git' \
    --exclude='./build' \
    --exclude='./build-*' \
    --exclude='./dist' \
    .
) | sudo tar -xf - -C "$CHROOT_DIR/src/qnetlink"

sudo chroot "$CHROOT_DIR" /bin/bash -ec "
  cd /src/libslirp
  meson setup build --prefix=/opt/libslirp-${SLIRP_VERSION} --buildtype=release
  meson compile -C build
  meson install -C build

  cd /src/libwebsockets
  cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/libwebsockets-${LWS_VERSION} \
    -DDISABLE_WERROR=ON \
    -DLWS_WITH_SHARED=OFF -DLWS_WITH_STATIC=ON \
    -DLWS_WITHOUT_SERVER=OFF -DLWS_WITHOUT_TESTAPPS=ON \
    -DLWS_ROLE_H1=ON -DLWS_ROLE_WS=ON -DLWS_WITH_CLIENT=ON \
    -DLWS_WITH_EXTERNAL_POLL=ON \
    -DLWS_WITH_HTTP2=OFF -DLWS_WITH_HTTP3=OFF -DLWS_ROLE_QUIC=OFF \
    -DLWS_WITHOUT_EXTENSIONS=ON -DLWS_WITH_ZLIB=OFF \
    -DLWS_WITH_PLUGINS=OFF -DLWS_WITH_LIBUV=OFF -DLWS_WITH_LIBEVENT=OFF -DLWS_WITH_LIBEV=OFF \
    -DLWS_WITH_GENCRYPTO=ON -DLWS_WITH_LIBCAP=OFF \
    -DLWS_WITH_SSL=ON -DLWS_WITH_SCHANNEL=OFF
  grep -q '^LWS_WITH_EXTERNAL_POLL:BOOL=ON$' build/CMakeCache.txt || exit 1
  cmake --build build -j2
  cmake --install build

  cd /src/qnetlink
  export PKG_CONFIG_PATH=/opt/libslirp-${SLIRP_VERSION}/lib/x86_64-linux-gnu/pkgconfig:/opt/libwebsockets-${LWS_VERSION}/lib/pkgconfig:/opt/libwebsockets-${LWS_VERSION}/lib/x86_64-linux-gnu/pkgconfig
  meson setup build --buildtype=release
  meson compile -C build
  rm -rf /src/qnetlink/stage
  DESTDIR=/src/qnetlink/stage meson install -C build

  mkdir -p /src/qnetlink/stage/usr/local/bin/lib
  cp -L /opt/libslirp-${SLIRP_VERSION}/lib/x86_64-linux-gnu/libslirp.so.0 \
    /src/qnetlink/stage/usr/local/bin/lib/libslirp.so.0
  cp -L /lib/x86_64-linux-gnu/libglib-2.0.so.0 \
    /src/qnetlink/stage/usr/local/bin/lib/libglib-2.0.so.0
  cp -L /lib/x86_64-linux-gnu/libpcre.so.3 \
    /src/qnetlink/stage/usr/local/bin/lib/libpcre.so.3
  cp -L /usr/lib/x86_64-linux-gnu/libssl.so.1.1 \
    /src/qnetlink/stage/usr/local/bin/lib/libssl.so.1.1
  cp -L /usr/lib/x86_64-linux-gnu/libcrypto.so.1.1 \
    /src/qnetlink/stage/usr/local/bin/lib/libcrypto.so.1.1
  patchelf --set-rpath '\$ORIGIN' /src/qnetlink/stage/usr/local/bin/lib/libslirp.so.0
  patchelf --set-rpath '\$ORIGIN' /src/qnetlink/stage/usr/local/bin/lib/libglib-2.0.so.0
  strip --strip-debug /src/qnetlink/stage/usr/local/bin/qnetlink \
    /src/qnetlink/stage/usr/local/bin/lib/*
"

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/lib"
sudo cp "$CHROOT_DIR/src/qnetlink/stage/usr/local/bin/qnetlink" "$DIST_DIR/qnetlink"
sudo cp "$CHROOT_DIR/src/qnetlink/stage/usr/local/bin/lib/libslirp.so.0" "$DIST_DIR/lib/libslirp.so.0"
sudo cp "$CHROOT_DIR/src/qnetlink/stage/usr/local/bin/lib/libglib-2.0.so.0" "$DIST_DIR/lib/libglib-2.0.so.0"
sudo cp "$CHROOT_DIR/src/qnetlink/stage/usr/local/bin/lib/libpcre.so.3" "$DIST_DIR/lib/libpcre.so.3"
sudo cp "$CHROOT_DIR/src/qnetlink/stage/usr/local/bin/lib/libssl.so.1.1" "$DIST_DIR/lib/libssl.so.1.1"
sudo cp "$CHROOT_DIR/src/qnetlink/stage/usr/local/bin/lib/libcrypto.so.1.1" "$DIST_DIR/lib/libcrypto.so.1.1"
cp "$PROJECT_DIR/qnetlink.example.ini" "$DIST_DIR/"
sudo chown -R "$(id -u):$(id -g)" "$DIST_DIR"

for artifact in "$DIST_DIR/qnetlink" "$DIST_DIR"/lib/*; do
  if readelf -SW "$artifact" | grep -q '\.debug_'; then
    echo "error: debug sections remain in $artifact after stripping" >&2
    exit 1
  fi
done
echo "stripped: Linux executable and bundled libraries (debug info removed)"

runpath=$(
  readelf -dW "$DIST_DIR/qnetlink" |
  sed -nE 's/^.*\((RPATH|RUNPATH)\).*\[([^]]*)\].*$/\2/p'
)
if [[ "$runpath" != '$ORIGIN/lib' ]]; then
  echo 'error: Linux executable RUNPATH must be exactly $ORIGIN/lib' >&2
  printf 'actual RUNPATH: %s\n' "${runpath:-<none>}" >&2
  exit 1
fi

for lib in libslirp.so.0 libglib-2.0.so.0; do
  lib_runpath=$(
    readelf -dW "$DIST_DIR/lib/$lib" |
    sed -nE 's/^.*\((RPATH|RUNPATH)\).*\[([^]]*)\].*$/\2/p'
  )
  if [[ "$lib_runpath" != '$ORIGIN' ]]; then
    echo "error: $lib RUNPATH must be exactly \$ORIGIN" >&2
    printf 'actual RUNPATH: %s\n' "${lib_runpath:-<none>}" >&2
    exit 1
  fi
done

needed=$(readelf -dW "$DIST_DIR/qnetlink")
grep -q 'Shared library: \[libslirp.so.0\]' <<<"$needed" || {
  echo "error: Linux executable does not require libslirp.so.0" >&2
  exit 1
}
if grep -q 'Shared library: \[libwebsockets' <<<"$needed"; then
  echo "error: libwebsockets must be linked statically" >&2
  exit 1
fi
grep -q 'Shared library: \[libssl.so.1.1\]' <<<"$needed" || { echo "error: missing OpenSSL runtime dependency" >&2; exit 1; }
grep -q 'Shared library: \[libcrypto.so.1.1\]' <<<"$needed" || { echo "error: missing libcrypto runtime dependency" >&2; exit 1; }

expected=(qnetlink qnetlink.example.ini lib/libslirp.so.0 lib/libglib-2.0.so.0 lib/libpcre.so.3 lib/libssl.so.1.1 lib/libcrypto.so.1.1)
mapfile -t actual < <(cd "$DIST_DIR" && find . -type f -printf '%P\n' | LC_ALL=C sort)
mapfile -t wanted < <(printf '%s\n' "${expected[@]}" | LC_ALL=C sort)
if [[ "${actual[*]}" != "${wanted[*]}" ]]; then
  echo "error: unexpected Linux portable package contents" >&2
  printf 'expected: %s\n' "${wanted[*]}" >&2
  printf 'actual:   %s\n' "${actual[*]}" >&2
  exit 1
fi

ldd_out=$(ldd "$DIST_DIR/qnetlink")
for lib in libslirp.so.0 libglib-2.0.so.0 libpcre.so.3 libssl.so.1.1 libcrypto.so.1.1; do
  expected_path="$DIST_DIR/lib/$lib"
  grep -Fq "$lib => $expected_path " <<<"$ldd_out" || {
    echo "error: $lib is not resolved from the portable package" >&2
    echo "$ldd_out" >&2
    exit 1
  }
done

max_glibc=$(
  for f in "$DIST_DIR/qnetlink" "$DIST_DIR"/lib/*; do
    readelf --version-info "$f" 2>/dev/null || true
  done | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -n 1
)
echo "maximum GLIBC symbol requirement: ${max_glibc:-unknown}"
if [[ "$max_glibc" != "GLIBC_2.28" ]]; then
  echo "error: expected maximum GLIBC requirement GLIBC_2.28, found ${max_glibc:-none}" >&2
  exit 1
fi

version_out=$("$DIST_DIR/qnetlink" --version)
if [[ "${version_out%% build=*}" != "qnetlink $PROJECT_VERSION" || "$version_out" != *" build=$PROJECT_VERSION+"* ]]; then
  echo "error: packaged executable reports unexpected version: $version_out" >&2
  exit 1
fi
example_out=$("$DIST_DIR/qnetlink" --config "$PROJECT_DIR/qnetlink.example.ini" --version)
if [[ "${example_out%% build=*}" != "qnetlink $PROJECT_VERSION" ]]; then
  echo "error: example INI is rejected by the executable: $example_out" >&2
  exit 1
fi

rm -rf "$PLATFORM_DIR"
mkdir -p "$PLATFORM_DIR"
cp -a "$DIST_DIR/." "$PLATFORM_DIR/"
chown -R "$CALLER_USER:$CALLER_GROUP" "$DIST_ROOT"
echo "created: $PLATFORM_DIR/qnetlink"
echo "created: $PLATFORM_DIR/qnetlink.example.ini"
echo "created: $PLATFORM_DIR/lib/"
