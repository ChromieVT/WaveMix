# Maintainer: ChromieVT <chromiewabbit@gmail.com>
#
# WaveMix, built from the latest commit.

pkgname=wavemix-git
pkgver=0.0.7a.r0.g0000000
pkgrel=1
pkgdesc="Wave Link-style audio mixer for PipeWire: separate personal and stream volumes per app channel (development version)"
arch=('x86_64' 'aarch64')
url="https://github.com/ChromieVT/wavemix"
license=('GPL-3.0-or-later')
# The libraries it links against, then what it needs at run time without linking
# (namcap can't see those): the sound server, and the mic's filters, plugins the
# service loads, as the Flatpak brings its own: RNNoise for noise removal, the LADSPA
# SDK's amplifier for gain, and LSP for the rest.
depends=('glibc' 'libgcc' 'libstdc++' 'glib2' 'glibmm-2.68' 'libsigc++-3.0' 'gdk-pixbuf2' 'gtkmm-4.0'
         'hicolor-icon-theme' 'libpipewire' 'libpulse'
         'pipewire-pulse' 'wireplumber'
         'noise-suppression-for-voice' 'ladspa' 'lsp-plugins-ladspa')
makedepends=('git' 'meson' 'nlohmann-json')
checkdepends=('gtest' 'desktop-file-utils' 'appstream')
optdepends=('xapp: tray icon on Cinnamon (XApp Status applet)')
provides=('wavemix')
conflicts=('wavemix')
source=("wavemix::git+$url.git")
sha256sums=('SKIP')

pkgver() {
    cd wavemix
    # VERSION file + commits since + short hash, e.g. 0.0.7a.r42.g1a2b3c4
    printf "%s.r%s.g%s" "$(cat VERSION)" "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
}

build() {
    arch-meson wavemix build
    meson compile -C build
}

check() {
    # The unit tests, and the packaging checks: the menu entry, the autostart entry and
    # the metainfo.
    meson test -C build --print-errorlogs
}

package() {
    meson install -C build --destdir "$pkgdir"
}
