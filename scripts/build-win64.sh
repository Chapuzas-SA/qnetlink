#!/usr/bin/env bash
set -euo pipefail

# Package downloads use the MSYS2 geo redirector, primary server, then
# independent Tier-1 mirrors. MSYS2_BASE_URL can still pin one endpoint.
if [[ -n "${MSYS2_BASE_URL:-}" ]]; then
  msys2_base_urls=("${MSYS2_BASE_URL%/}")
else
  msys2_base_urls=(
    "https://mirror.msys2.org/mingw/clang64"
    "https://repo.msys2.org/mingw/clang64"
    "https://ftp.nluug.nl/pub/os/windows/msys2/builds/mingw/clang64"
    "https://mirrors.dotsrc.org/msys2/mingw/clang64"
  )
fi
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd -P)
LWS_VERSION=5.0.0
LWS_SHA256=f853c6582101cfcee3a5a9e28ae92ab19d9735c5f31f0bb2e9794b5106123962
LWS_URL=${LWS_URL:-https://github.com/warmcat/libwebsockets/archive/refs/tags/v${LWS_VERSION}.tar.gz}
PROJECT_VERSION=$(sed -n "s/^project('qnetlink', 'c', version: '\\([^']*\\)'.*/\\1/p" "$PROJECT_DIR/meson.build")
if [[ -z "$PROJECT_VERSION" ]]; then
  echo "error: cannot determine project version from meson.build" >&2
  exit 1
fi
SYSROOT_DIR=${SYSROOT_DIR:-"$PROJECT_DIR/.build/windows-x86_64/sysroot"}
PACKAGES_DIR="$SYSROOT_DIR/packages"
ROOT_DIR="$SYSROOT_DIR/root"
PREFIX_DIR="$ROOT_DIR/clang64"
BUILD_DIR=${BUILD_DIR:-"$PROJECT_DIR/.build/windows-x86_64/build"}
DIST_ROOT=${DIST_ROOT:-"$PROJECT_DIR/dist"}
DIST_NAME="qnetlink-${PROJECT_VERSION}-windows-x86_64"
PLATFORM_DIR="$DIST_ROOT/windows-x86_64"
PACKAGE_STAGE=$(mktemp -d "${TMPDIR:-/tmp}/qnetlink-windows-package.XXXXXX")
trap 'rm -rf "$PACKAGE_STAGE"' EXIT
DIST_DIR="$PACKAGE_STAGE/$DIST_NAME"
LWS_ARCHIVE="$PACKAGES_DIR/libwebsockets-v${LWS_VERSION}.tar.gz"
LWS_SOURCE_DIR="$SYSROOT_DIR/libwebsockets-src"
LWS_BUILD_DIR="$SYSROOT_DIR/libwebsockets-build"
LWS_PREFIX_DIR="$PREFIX_DIR"
LWS_PATCH_DIR="$PROJECT_DIR/patches/libwebsockets-${LWS_VERSION}"

packages=(
  mingw-w64-clang-x86_64-libslirp-4.9.3-1-any.pkg.tar.zst
  mingw-w64-clang-x86_64-glib2-2.90.0-1-any.pkg.tar.zst
  mingw-w64-clang-x86_64-pcre2-10.49-1-any.pkg.tar.zst
  mingw-w64-clang-x86_64-gettext-runtime-1.0-1-any.pkg.tar.zst
  mingw-w64-clang-x86_64-libiconv-1.19-1-any.pkg.tar.zst
)

runtime_dlls=(
  libslirp-0.dll
  libglib-2.0-0.dll
  libpcre2-8-0.dll
  libintl-8.dll
  libiconv-2.dll
)

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: required tool not found: $1" >&2
    exit 1
  }
}

for tool in curl bsdtar cmake meson ninja pkg-config sha256sum x86_64-w64-mingw32-gcc \
  x86_64-w64-mingw32-ar x86_64-w64-mingw32-strip \
  x86_64-w64-mingw32-windres x86_64-w64-mingw32-objdump patch; do
  need "$tool"
done

mkdir -p "$PACKAGES_DIR"

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
  curl --fail --location --retry 3 --retry-all-errors \
    --connect-timeout 10 --output "$LWS_ARCHIVE.tmp" "$LWS_URL"
  echo "$LWS_SHA256  $LWS_ARCHIVE.tmp" | sha256sum -c -
  tar -tzf "$LWS_ARCHIVE.tmp" >/dev/null
  mv "$LWS_ARCHIVE.tmp" "$LWS_ARCHIVE"
else
  echo "cached:   libwebsockets ${LWS_VERSION}"
fi

for package in "${packages[@]}"; do
  archive="$PACKAGES_DIR/$package"
  if [[ -f "$archive" ]] && ! bsdtar -tf "$archive" >/dev/null 2>&1; then
    echo "warning: invalid cached archive, removing: $package" >&2
    rm -f "$archive"
  fi
  if [[ ! -f "$archive" ]]; then
    echo "download: $package"
    rm -f "$archive.tmp"
    downloaded=false
    for base_url in "${msys2_base_urls[@]}"; do
      echo "  try: $base_url"
      rm -f "$archive.tmp"
      if curl --fail --location --retry 1 --retry-all-errors \
        --connect-timeout 10 --output "$archive.tmp" "$base_url/$package"; then
        if bsdtar -tf "$archive.tmp" >/dev/null 2>&1; then
          downloaded=true
          break
        fi
        echo "warning: invalid archive from $base_url" >&2
      else
        echo "warning: download failed from $base_url" >&2
      fi
    done
    if [[ "$downloaded" != true ]]; then
      rm -f "$archive.tmp"
      echo "error: unable to download $package from any configured MSYS2 mirror" >&2
      exit 1
    fi
    mv "$archive.tmp" "$archive"
  else
    echo "cached:   $package"
  fi
done

# Recreate the extracted sysroot every time so removed/renamed package files
# cannot survive from an earlier build.
rm -rf "$ROOT_DIR"
mkdir -p "$ROOT_DIR"
for package in "${packages[@]}"; do
  echo "extract:  $package"
  bsdtar -xf "$PACKAGES_DIR/$package" -C "$ROOT_DIR"
done

rm -rf "$LWS_SOURCE_DIR" "$LWS_BUILD_DIR"
mkdir -p "$LWS_SOURCE_DIR" "$LWS_BUILD_DIR"
tar -xzf "$LWS_ARCHIVE" -C "$LWS_SOURCE_DIR" --strip-components=1
if [[ -d "$LWS_PATCH_DIR" ]]; then
  for patch_file in "$LWS_PATCH_DIR"/*.patch; do
    [[ -e "$patch_file" ]] || continue
    echo "patch:    libwebsockets $(basename "$patch_file")"
    patch --directory="$LWS_SOURCE_DIR" --strip=1 --fuzz=0 --dry-run < "$patch_file"
    patch --directory="$LWS_SOURCE_DIR" --strip=1 --fuzz=0 < "$patch_file"
  done
fi

cmake -S "$LWS_SOURCE_DIR" -B "$LWS_BUILD_DIR" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DCMAKE_AR=x86_64-w64-mingw32-ar \
  -DCMAKE_INSTALL_PREFIX=/clang64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DDISABLE_WERROR=ON \
  -DLWS_HAVE_PTHREAD_H=0 \
  -DLWS_WITH_SHARED=OFF -DLWS_WITH_STATIC=ON \
  -DLWS_WITHOUT_SERVER=OFF -DLWS_WITHOUT_TESTAPPS=ON \
  -DLWS_ROLE_H1=ON -DLWS_ROLE_WS=ON \
  -DLWS_WITH_EXTERNAL_POLL=ON \
  -DLWS_WITH_HTTP2=OFF -DLWS_WITH_HTTP3=OFF -DLWS_ROLE_QUIC=OFF \
  -DLWS_WITHOUT_EXTENSIONS=ON -DLWS_WITH_ZLIB=OFF \
  -DLWS_WITH_PLUGINS=OFF -DLWS_WITH_EVLIB_PLUGINS=OFF \
  -DLWS_WITH_LIBUV=OFF -DLWS_WITH_LIBEVENT=OFF -DLWS_WITH_LIBEV=OFF -DLWS_WITH_GLIB=OFF \
  -DLWS_WITH_SECURE_STREAMS=OFF -DLWS_WITH_SECURE_STREAMS_PROXY_API=OFF \
  -DLWS_WITH_GENCRYPTO=ON -DLWS_WITH_JOSE=OFF \
  -DLWS_WITH_SCHANNEL=ON \
  -DLWS_WITH_SYS_ASYNC_DNS=OFF -DLWS_WITH_SYS_SMD=OFF \
  -DLWS_WITH_HTTP_BASIC_AUTH=OFF -DLWS_WITH_HTTP_DIGEST_AUTH=OFF

cache="$LWS_BUILD_DIR/CMakeCache.txt"
grep -q '^LWS_WITH_SCHANNEL:BOOL=ON$' "$cache" || {
  echo "error: libwebsockets CMake did not enable SChannel" >&2
  exit 1
}
grep -q '^LWS_WITH_SHARED:BOOL=OFF$' "$cache" || {
  echo "error: libwebsockets CMake unexpectedly enabled shared libraries" >&2
  exit 1
}
grep -q '^LWS_WITH_EXTERNAL_POLL:BOOL=ON$' "$cache" || {
  echo "error: libwebsockets CMake did not enable external poll integration" >&2
  exit 1
}
grep -q '^LWS_WITH_GENCRYPTO:BOOL=ON$' "$cache" || {
  echo "error: libwebsockets CMake did not enable GENCRYPTO required by SChannel TLS" >&2
  exit 1
}
grep -q '^LWS_HAVE_PTHREAD_H:UNINITIALIZED=0$' "$cache" || {
  echo "error: libwebsockets CMake unexpectedly enabled pthread use for MinGW" >&2
  exit 1
}
grep -q '^LWS_WITH_HTTP3:BOOL=OFF$' "$cache" || {
  echo "error: libwebsockets CMake unexpectedly enabled HTTP/3" >&2
  exit 1
}

cmake --build "$LWS_BUILD_DIR" -j2
DESTDIR="$ROOT_DIR" cmake --install "$LWS_BUILD_DIR"
if [[ -f "$PREFIX_DIR/lib/libwebsockets_static.a" && ! -f "$PREFIX_DIR/lib/libwebsockets.a" ]]; then
  cp -f "$PREFIX_DIR/lib/libwebsockets_static.a" "$PREFIX_DIR/lib/libwebsockets.a"
fi
for pc_file in "$PREFIX_DIR/lib/pkgconfig/libwebsockets.pc" "$PREFIX_DIR/lib/pkgconfig/libwebsockets_static.pc"; do
  [[ -f "$pc_file" ]] || continue
  if grep -q '^Libs.private:$' "$pc_file"; then
    sed -i 's/^Libs.private:$/Libs.private: -liphlpapi -lsecur32 -lcrypt32 -lncrypt -lbcrypt/' "$pc_file"
  elif ! grep -q '^Libs.private:' "$pc_file"; then
    printf '\nLibs.private: -liphlpapi -lsecur32 -lcrypt32 -lncrypt -lbcrypt\n' >>"$pc_file"
  fi
done

export PKG_CONFIG_LIBDIR="$PREFIX_DIR/lib/pkgconfig:$PREFIX_DIR/lib64/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$ROOT_DIR"
unset PKG_CONFIG_PATH

slirp_version=$(pkg-config --modversion slirp)
if [[ "$slirp_version" != 4.9.3 ]]; then
  echo "error: expected libslirp 4.9.3, found $slirp_version" >&2
  exit 1
fi
echo "libslirp: $slirp_version"

lws_version=$(pkg-config --modversion libwebsockets)
if [[ "$lws_version" != "$LWS_VERSION" && "$lws_version" != "$LWS_VERSION-"* ]]; then
  echo "error: expected libwebsockets $LWS_VERSION, found $lws_version" >&2
  exit 1
fi
echo "libwebsockets: $lws_version (static, SChannel)"

rm -rf "$BUILD_DIR"
meson setup "$BUILD_DIR" "$PROJECT_DIR" --cross-file "$PROJECT_DIR/cross/windows-x86_64.ini"
meson compile -C "$BUILD_DIR"

exe="$BUILD_DIR/qnetlink.exe"
[[ -f "$exe" ]] || { echo "error: missing build output: $exe" >&2; exit 1; }

imports=$(x86_64-w64-mingw32-objdump -p "$exe")
grep -q 'DLL Name: libslirp-0.dll' <<<"$imports" || {
  echo "error: qnetlink.exe does not import libslirp-0.dll" >&2
  exit 1
}
grep -q 'DLL Name: WS2_32.dll' <<<"$imports" || {
  echo "error: qnetlink.exe does not import WS2_32.dll" >&2
  exit 1
}
grep -Eqi 'DLL Name: (secur32|sspicli)\.dll' <<<"$imports" || {
  echo "error: qnetlink.exe does not import the Windows SChannel/SSPI runtime" >&2
  exit 1
}
grep -qi 'DLL Name: CRYPT32.dll' <<<"$imports" || {
  echo "error: qnetlink.exe does not import CRYPT32.dll" >&2
  exit 1
}
if grep -Eqi 'DLL Name: (libwebsockets|libssl|libcrypto)[^ ]*\.dll' <<<"$imports"; then
  echo "error: WSS introduced an unexpected dynamic libwebsockets/OpenSSL dependency" >&2
  grep -Ei 'DLL Name: (libwebsockets|libssl|libcrypto)[^ ]*\.dll' <<<"$imports" >&2
  exit 1
fi

mkdir -p "$DIST_DIR"
cp "$exe" "$DIST_DIR/"
cp "$PROJECT_DIR/qnetlink.example.ini" "$DIST_DIR/"
for dll in "${runtime_dlls[@]}"; do
  src="$PREFIX_DIR/bin/$dll"
  [[ -f "$src" ]] || { echo "error: missing runtime DLL: $src" >&2; exit 1; }
  cp "$src" "$DIST_DIR/"
done

x86_64-w64-mingw32-strip --strip-debug "$DIST_DIR/qnetlink.exe"
if x86_64-w64-mingw32-objdump -h "$DIST_DIR/qnetlink.exe" | grep -q '\.debug_'; then
  echo "error: debug sections remain in qnetlink.exe after stripping" >&2
  exit 1
fi
echo "stripped: Windows executable debug information"

expected=(qnetlink.exe qnetlink.example.ini "${runtime_dlls[@]}")
mapfile -t actual < <(find "$DIST_DIR" -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort)
mapfile -t wanted < <(printf '%s\n' "${expected[@]}" | LC_ALL=C sort)
if [[ "${actual[*]}" != "${wanted[*]}" ]]; then
  echo "error: unexpected portable package contents" >&2
  printf 'expected: %s\n' "${wanted[*]}" >&2
  printf 'actual:   %s\n' "${actual[*]}" >&2
  exit 1
fi

rm -rf "$PLATFORM_DIR"
mkdir -p "$PLATFORM_DIR"
cp -a "$DIST_DIR/." "$PLATFORM_DIR/"

rm -rf "$BUILD_DIR"
echo "created: $PLATFORM_DIR/qnetlink.exe"
echo "created: $PLATFORM_DIR/qnetlink.example.ini"
for dll in "${runtime_dlls[@]}"; do
  echo "created: $PLATFORM_DIR/$dll"
done
echo "removed build directory: $BUILD_DIR"
