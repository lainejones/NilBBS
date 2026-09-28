#!/bin/bash
# Transpile a Last Call BBS door (modern JS) to ES5 for Duktape.
#   transpile.sh <door.js> <out.js> [add-on.js ...]
# Add-on files (e.g. lord-addons.js) are appended after the door, so they
# share its globals and can wrap its functions.
# Top-level `let` becomes `var` first: Last Call tolerates re-declared
# top-level lets (the LORD remake has some), strict parsers don't.
export PATH=~/node/bin:$PATH
in="$(realpath "$1")"; out="$(realpath -m "$2")"; shift 2
addons=(); for a in "$@"; do addons+=("$(realpath "$a")"); done
cd ~/babelwork
sed -E '1s/^.use strict.;//; s/^let /var /; s/^const /var /; s/\$\{([0-9]+)\}/{\1}/g' "$in" > /tmp/lcb_pre.js
for a in "${addons[@]}"; do printf '\n' >> /tmp/lcb_pre.js; cat "$a" >> /tmp/lcb_pre.js; done
npx babel --presets @babel/preset-env --no-babelrc --source-type script /tmp/lcb_pre.js -o "$out" || exit 1
ls -la "$out"
echo "modern syntax left: $(grep -c -E '\blet |=>' "$out")"
grep -o -E '\.(includes|padStart|padEnd|repeat|startsWith|endsWith|find|findIndex|fill|entries)\(|Object\.(assign|entries|values)|new (Set|Map)\b|Symbol' "$out" | sort | uniq -c
