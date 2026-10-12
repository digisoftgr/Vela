# Maintainer: Digisoftgr <info@digisoftgr.gr>
pkgname=vela
_name=Vela
pkgver=0.7.2
pkgrel=1
pkgdesc="Tabbed KDE Plasma file manager with built-in file and folder previews"
arch=('x86_64')
url="https://github.com/digisoftgr/Vela"
license=('GPL-3.0-or-later')
depends=(
    'qt6-base'
    'kf6-kcolorscheme'
    'kf6-kconfig'
    'kf6-kconfigwidgets'
    'kf6-kcoreaddons'
    'kf6-kglobalaccel'
    'kf6-kguiaddons'
    'kf6-ki18n'
    'kf6-kio'
    'kf6-kjobwidgets'
    'kf6-kservice'
    'kf6-kbookmarks'
    'kf6-kwidgetsaddons'
    'kf6-kxmlgui'
    'kf6-solid'
    'kf6-syntax-highlighting'
    'poppler-qt6'
)
makedepends=(
    'cmake'
    'extra-cmake-modules'
    'qt6-tools'
)
optdepends=(
    'ark: compress and extract archives from the context menu'
    'konsole: default terminal for "Open Terminal Here"'
    'kio-extras: thumbnails and network protocols such as sftp and smb'
)
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')

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
