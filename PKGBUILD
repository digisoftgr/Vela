# Maintainer: Digisoftgr <info@digisoftgr.gr>
# Contributor: Kapetas Dimitris <kapetasdim98@gmail.com>
pkgname=vela
_name=Vela
pkgver=0.7.5
pkgrel=1
pkgdesc="Tabbed KDE Plasma file manager with built-in file and folder previews"
arch=('x86_64')
url="https://github.com/digisoftgr/Vela"
license=('GPL-3.0-or-later')
depends=(
    'qt6-base'
    'kbookmarks'
    'kcolorscheme'
    'kconfig'
    'kconfigwidgets'
    'kcoreaddons'
    'kglobalaccel'
    'kguiaddons'
    'ki18n'
    'kio'
    'kjobwidgets'
    'kparts'
    'kservice'
    'kwidgetsaddons'
    'kxmlgui'
    'poppler-qt6'
    'solid'
    'syntax-highlighting'
)
makedepends=(
    'cmake'
    'extra-cmake-modules'
    'qt6-tools'
)
optdepends=(
    'ark: compress and extract archives from the context menu'
    'konsole: embedded terminal panel (F4) and default terminal for "Open Terminal Here"'
    'kio-extras: thumbnails and network protocols such as sftp and smb'
)
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('6c0c2e0eda7559461e077dc9ca110ba8f3df247852ccc02383d8f5af22dc1a6f')

build() {
    cmake -B build -S "$_name-$pkgver" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DBUILD_TESTING=OFF
    cmake --build build -j"$(nproc)"
}

package() {
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 "$_name-$pkgver/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
