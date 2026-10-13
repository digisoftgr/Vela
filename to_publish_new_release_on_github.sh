#!/usr/bin/env bash
#
# Publishes a new Vela version on GitHub.
#
# The version is whatever CMakeLists.txt says. You set it yourself, commit it,
# and run this. The script never changes CMakeLists.txt. It checks that the
# version is higher than every version already tagged (locally and on GitHub),
# tags the current commit, and, once the tag is public, syncs PKGBUILD to it.
#
# The order matters: the PKGBUILD checksum can only be computed from the tarball
# GitHub builds for the tag, so the tag has to be public before it exists.
#
#   you commit the new version  ->  tag  ->  push main + tag  ->  PKGBUILD commit  ->  push
#
# Usage:
#   ./to_publish_new_release_on_github.sh --check     Run every check, change nothing.
#   ./to_publish_new_release_on_github.sh             Tag the current commit locally. Nothing is pushed.
#   ./to_publish_new_release_on_github.sh --push      The above, then push, update PKGBUILD, push again.
#   ./to_publish_new_release_on_github.sh --finish    Resume after pushing the tag by hand.
#
# Add --test-build to build the package with makepkg before the PKGBUILD commit
# is pushed. It needs the build dependencies installed on this machine, and it
# is not a clean-chroot build.

set -euo pipefail

cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

SELF=./$(basename "$0")
ORIGIN=origin

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

usage() { sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; }

MODE=release
PUSH=0
TEST_BUILD=0

while [ $# -gt 0 ]; do
    case "$1" in
        --check)      MODE=check ;;
        --finish)     MODE=finish ;;
        --push)       PUSH=1 ;;
        --test-build) TEST_BUILD=1 ;;
        -h|--help)    usage; exit 0 ;;
        -*)           die "unknown option: $1" ;;
        *)            die "no version argument: the version is read from CMakeLists.txt (got '$1')" ;;
    esac
    shift
done

if [ "$MODE" = finish ] && [ "$PUSH" = 1 ]; then
    die "--finish already pushes; drop --push"
fi

for f in CMakeLists.txt PKGBUILD CHANGELOG.md README.md; do
    [ -f "$f" ] || die "$f not found (run this from the Vela repository)"
done

cmake_version() { sed -n 's/^project(Vela VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt; }
pkg_url()       { sed -n 's/^url="\(.*\)"$/\1/p' PKGBUILD; }

VERSION=$(cmake_version)
[ -n "$VERSION" ] || die "could not read the version from CMakeLists.txt (expected 'project(Vela VERSION x.y.z')"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version in CMakeLists.txt must look like 1.2.3, got '$VERSION'"
TAG="v$VERSION"

changelog_section() {
    awk -v h="## [$VERSION]" 'index($0, h) == 1 { f = 1; next } /^## \[/ { f = 0 } f' CHANGELOG.md
}

require_tools() {
    local t
    for t in git curl sha256sum sort awk "$@"; do
        command -v "$t" >/dev/null 2>&1 || die "required tool not found: $t"
    done
}

# Highest version tag that exists locally or on origin, empty if there is none.
highest_tag() {
    local remote
    remote=$(git ls-remote --tags --refs "$ORIGIN" 'v*' 2>/dev/null) \
        || die "could not read tags from $ORIGIN"
    { git tag --list 'v*'; printf '%s\n' "$remote" | sed -n 's|.*refs/tags/||p'; } \
        | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sed 's/^v//' | sort -V | tail -n 1 || true
}

check_clean_main() {
    [ "$(git rev-parse --abbrev-ref HEAD)" = main ] || die "not on branch main"
    [ -z "$(git status --porcelain)" ] || die "working tree is not clean. Commit your changes first:
$(git status --short)"
}

check_release() {
    check_clean_main

    git fetch --quiet "$ORIGIN" || die "could not fetch from $ORIGIN"
    git merge-base --is-ancestor "$ORIGIN/main" HEAD \
        || die "local main is behind $ORIGIN/main. Pull first."

    local prev
    prev=$(highest_tag)
    if [ -n "$prev" ]; then
        [ "$prev" != "$VERSION" ] || die "version $VERSION is already tagged. Raise the version in CMakeLists.txt."
        [ "$(printf '%s\n%s\n' "$prev" "$VERSION" | sort -V | tail -n 1)" = "$VERSION" ] \
            || die "version $VERSION is not higher than the highest tagged version $prev"
    fi
    PREVIOUS=${prev:-none}

    git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "tag $TAG already exists locally"
    if git ls-remote --exit-code --tags "$ORIGIN" "refs/tags/$TAG" >/dev/null 2>&1; then
        die "tag $TAG already exists on $ORIGIN. Published tags are never moved."
    fi

    local re=${VERSION//./\\.}
    grep -Eq "^## \[$re\] - [0-9]{4}-[0-9]{2}-[0-9]{2}\$" CHANGELOG.md \
        || die "CHANGELOG.md has no heading like '## [$VERSION] - YYYY-MM-DD'"
    changelog_section | grep -q '[^[:space:]]' \
        || die "the CHANGELOG.md entry for $VERSION is empty"

    local hits
    hits=$(grep -nE '[0-9]+\.[0-9]+\.[0-9]+' README.md || true)
    [ -z "$hits" ] || die "README.md must not mention version numbers (history belongs in CHANGELOG.md):
$hits"

    [ -n "$(pkg_url)" ] || die "could not read url= from PKGBUILD"
}

check_finish() {
    check_clean_main

    git rev-parse -q --verify "refs/tags/$TAG" >/dev/null \
        || die "tag $TAG does not exist locally. Run $SELF first."
    git merge-base --is-ancestor "$TAG" HEAD || die "tag $TAG is not an ancestor of HEAD"
    [ "$(git show "$TAG:CMakeLists.txt" | sed -n 's/^project(Vela VERSION \([0-9.]*\).*/\1/p')" = "$VERSION" ] \
        || die "tag $TAG was made at a different version than CMakeLists.txt says now"
    git ls-remote --exit-code --tags "$ORIGIN" "refs/tags/$TAG" >/dev/null 2>&1 \
        || die "tag $TAG is not on $ORIGIN yet. Push it first: git push $ORIGIN main && git push $ORIGIN $TAG"

    if [ "$(sed -n 's/^pkgver=//p' PKGBUILD)" = "$VERSION" ] \
        && grep -Eq "^sha256sums=\('[0-9a-f]{64}'\)" PKGBUILD; then
        die "PKGBUILD is already set up for $VERSION; nothing to finish"
    fi
    [ -n "$(pkg_url)" ] || die "could not read url= from PKGBUILD"
}

make_tag() {
    git tag -a "$TAG" -m "Vela $VERSION"
    info "Tagged $TAG at $(git rev-parse --short HEAD)"
}

push_release() {
    info "Pushing main and $TAG to $ORIGIN"
    git push "$ORIGIN" main || die "push failed (no credentials?). Push by hand, then run: $SELF --finish"
    git push "$ORIGIN" "$TAG" || die "tag push failed. Push by hand, then run: $SELF --finish"
}

update_pkgbuild() {
    require_tools updpkgsums
    local url tarball tmp i
    url=$(pkg_url)
    tarball="$url/archive/refs/tags/$TAG.tar.gz"

    info "Waiting for $tarball"
    for i in $(seq 1 12); do
        if curl -sfIL "$tarball" >/dev/null 2>&1; then break; fi
        [ "$i" -lt 12 ] || die "tarball is not available after 60 seconds: $tarball"
        sleep 5
    done

    info "Syncing PKGBUILD to $VERSION and computing the checksum"
    sed -i -e "s/^pkgver=.*/pkgver=$VERSION/" \
           -e "s/^pkgrel=.*/pkgrel=1/" \
           -e "s/^sha256sums=.*/sha256sums=('SKIP')/" PKGBUILD

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    SRCDEST="$tmp" updpkgsums >/dev/null 2>&1 \
        || die "updpkgsums failed. PKGBUILD is modified but not committed: git checkout PKGBUILD"
    grep -Eq "^sha256sums=\('[0-9a-f]{64}'\)" PKGBUILD \
        || die "PKGBUILD has no valid checksum after updpkgsums. Undo with: git checkout PKGBUILD"

    if [ "$TEST_BUILD" = 1 ]; then
        require_tools makepkg
        info "Test build with makepkg (not a clean chroot; slow)"
        mkdir -p "$tmp/build"
        cp PKGBUILD "$tmp/build/"
        ( cd "$tmp/build" && makepkg -f --nocolor >"$tmp/makepkg.log" 2>&1 ) \
            || { tail -n 30 "$tmp/makepkg.log" >&2
                 die "test build failed. Do NOT publish to the AUR. PKGBUILD is modified but not committed: git checkout PKGBUILD"; }
        info "Test build succeeded"
    fi

    git add PKGBUILD
    git commit --quiet -m "Update PKGBUILD for $VERSION"
    info "Committed the PKGBUILD update"

    git push "$ORIGIN" main || die "push failed. Run: git push"
}

print_aur_reminder() {
    cat <<EOF

Done. Not automated, and still to do by hand:
  1. In your AUR clone: copy the new PKGBUILD, run
       makepkg --printsrcinfo > .SRCINFO
  2. git add PKGBUILD .SRCINFO && git commit -m "Update to $VERSION" && git push
  3. Optional: create a GitHub Release for $TAG with the CHANGELOG entry.
EOF
}

case "$MODE" in
    check)
        require_tools
        check_release
        echo "All checks passed. Version $VERSION is higher than the previous version ($PREVIOUS). Nothing was changed."
        echo "A real run would tag the current commit $TAG."
        ;;
    release)
        require_tools
        [ "$PUSH" = 0 ] || require_tools updpkgsums
        check_release
        make_tag
        if [ "$PUSH" = 1 ]; then
            push_release
            update_pkgbuild
            print_aur_reminder
        else
            cat <<EOF

Local only. Nothing was pushed. Next:
  git push origin main && git push origin $TAG
  $SELF --finish
EOF
        fi
        ;;
    finish)
        require_tools updpkgsums
        check_finish
        update_pkgbuild
        print_aur_reminder
        ;;
esac
