# Maintainer: ChromieVT <chromie@chromie.uk>

pkgname=wavemix
# The release's tag on GitHub, and its version as pacman reads it: pkgver can't have a
# "-", and "0.0.8alpha" sorts before "0.0.8", so the final release counts as an upgrade.
_tag=v0.0.8-alpha
pkgver=0.0.8alpha
pkgrel=1
pkgdesc="An audio mixer for streamers."
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
makedepends=('meson' 'nlohmann-json')
checkdepends=('gtest' 'desktop-file-utils' 'appstream')
optdepends=('xapp: tray icon on Cinnamon (XApp Status applet)')
conflicts=('wavemix-git')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/$_tag.tar.gz")
sha256sums=('7613d422cd7d41870a1e72243a0431e5ec94613880c125f6bbb3da7c448fcc68')

# GitHub's tarball unpacks to <repository>-<tag without the v>: wavemix-0.0.8-alpha.
build() {
    arch-meson "$pkgname-${_tag#v}" build
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
