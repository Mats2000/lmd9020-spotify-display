#!/bin/sh
# Puts the phone page on Vercel (showcontrol.matiascevallos.com): the page, and the relay that
# carries taps to Show Control on the Mac and its state back (api/, Upstash Redis on Vercel).
# Needs the Vercel CLI signed in and this folder linked (`vercel link --project showcontrol`).
set -e
cd "$(dirname "$0")"
rm -rf public && mkdir -p public/fonts
cp ../web/index.html public/
sips -z 180 180 ../AppIcon.png --out public/icon.png >/dev/null
for f in Exo2.0-SemiBold.otf Sora-Regular.ttf Sora-Light.ttf; do cp "$HOME/Library/Fonts/$f" public/fonts/; done
printf 'User-agent: *\nDisallow: /\n' > public/robots.txt
vercel deploy --prod --yes
