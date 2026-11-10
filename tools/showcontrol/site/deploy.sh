#!/bin/sh
# Puts the phone page at showcontrol.matiascevallos.com: Vercel passes every request on to this
# Mac's Tailscale Funnel address, where Show Control answers (port 8770). Needs the Tailscale app
# signed in (Funnel allowed for this Mac) and the Vercel CLI signed in (`vercel login`).
# The Funnel address stays out of the repo: the Vercel files are written to site/out.
set -e
cd "$(dirname "$0")"
TS=/Applications/Tailscale.app/Contents/MacOS/Tailscale
DOMAIN=${DOMAIN:-showcontrol.matiascevallos.com}
HOST=$("$TS" status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["Self"]["DNSName"].rstrip("."))')
"$TS" funnel --bg 8770
mkdir -p out
printf 'User-agent: *\nDisallow: /\n' > out/robots.txt
cat > out/vercel.json <<EOF
{
  "rewrites": [{ "source": "/(.*)", "destination": "https://$HOST/\$1" }],
  "headers": [{ "source": "/(.*)", "headers": [{ "key": "X-Robots-Tag", "value": "noindex" }] }]
}
EOF
cd out
vercel link --yes --project showcontrol >/dev/null
vercel deploy --prod --yes
vercel domains add "$DOMAIN" showcontrol 2>/dev/null || true
echo "https://$DOMAIN"
