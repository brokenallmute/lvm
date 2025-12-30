# Maintainer: Artem Fedulaev <artem.fedulaev11@email.com>
pkgname=lwm-wm
pkgver=1.0
pkgrel=1
pkgdesc="Legacy X11 window manager"
arch=('x86_64')
url="https://github.com/brokenallmute/lwm"
license=('MIT')
depends=('libx11')
makedepends=('gcc' 'make') 
source=("${pkgname}-${pkgver}.tar.gz::https://github.com/brokenallmute/lwm/archive/refs/tags/v${pkgver}.tar.gz")

sha256sums=('b3fb9357eb72961d1be29b12ca8db9c9c14e7e15419a0142507784a9ffb5827b')

build() {
    cd "lwm-${pkgver}"
    make
}

package() {
    cd "lwm-${pkgver}"
    make DESTDIR="$pkgdir" install
    
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}