#!/bin/sh
# Builds the package repository and publishes it as its own GitHub repository, so that
#
#     ppm update && ppm install cowsay
#
# works from any Petrichor machine (ppm downloads from raw.githubusercontent.com over HTTPS).
#
#   tools/publish-packages.sh                  assemble build/packages-repo (nothing is uploaded)
#   tools/publish-packages.sh OWNER/NAME       ...and push it to github.com/OWNER/NAME (create it first, as an
#                                              empty public repository, or let `gh` create it)
set -e
cd "$(dirname "$0")/.."

make repo >/dev/null
out=build/packages-repo
rm -rf "$out"
mkdir -p "$out"
cp build/repo/index build/repo/*.ppk "$out/"
mkdir -p "$out/packages"
cp -R packages/. "$out/packages/"
cp LICENSE "$out/LICENSE"
cat >"$out/README.md" <<'EOT'
# Petrichor packages

Packages for [Petrichor OS](https://github.com/krspybacon/PetrichorOS). On a Petrichor machine:

    ppm update
    ppm install cowsay

`index` lists every package (name, version, size, SHA-256, dependencies); the `.ppk` files are the
packages. `ppm` verifies the size and SHA-256 before it installs anything. `packages/` holds the
sources, so you can rebuild with `make repo` in the main repository.
EOT

echo "assembled $out:"
ls "$out"

if [ -z "$1" ]; then
    echo
    echo "To publish: create an empty public repository (https://github.com/new, name: petrichor-packages), then run"
    echo "    tools/publish-packages.sh OWNER/petrichor-packages"
    exit 0
fi

repo=$1
if command -v gh >/dev/null 2>&1 && ! git ls-remote "https://github.com/$repo.git" >/dev/null 2>&1; then
    gh repo create "$repo" --public --description "Packages for Petrichor OS" >/dev/null
fi
cd "$out"
git init -q -b main
git add -A
git commit -q -m "Petrichor packages"
git remote add origin "https://github.com/$repo.git"
git push -u -f origin main
echo "published: ppm will use https://raw.githubusercontent.com/$repo/main"
