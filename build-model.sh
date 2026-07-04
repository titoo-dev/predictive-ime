#!/usr/bin/env bash
# Build the predictive-ime model from pinned third-party corpora.
#
# Standalone, distro-agnostic port of the Nix `ime-model` derivation: same
# inputs, same pinned SHA-256, same pipeline. Runs anywhere with bash,
# coreutils, curl, python3, tar and gzip.
#
# The AUTHORITATIVE distribution is the prebuilt release artifact (built once
# in CI, checksummed). This script is the "regenerate it / audit the inputs"
# path. Per-corpus licenses: see NOTICE-DATASETS.md.
#
# Usage:  ./build-model.sh [OUTPUT_DIR]      (default: ./model-out)
# Output: words.tsv  morph.tsv  bigrams.tsv  trigrams.tsv  emoji.tsv  NOTICE
set -euo pipefail

OUT="${1:-$PWD/model-out}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

for tool in curl python3 awk sort sha256sum base64 od tar gzip zcat; do
  command -v "$tool" >/dev/null 2>&1 || { echo "missing required tool: $tool" >&2; exit 1; }
done

# url | sha256 (nix SRI base64 form, verbatim from flake.nix) | local name
SOURCES="\
https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/fr/fr_50k.txt|sha256-+B98VwtkM3ZNqZqjD037CNgcWSYwE3e3Ca8joTsflZY=|fr_50k.txt
https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/en/en_50k.txt|sha256-U1H/QFsRJu9VV5HdTZeYpI4+mlAan8SBqdqVd1LPtFg=|en_50k.txt
https://downloads.wortschatz-leipzig.de/corpora/fra_news_2024_1M.tar.gz|sha256-kH7tKXq3tfvg6ocQUISJkVj2YIkOh/EhsqJOuYU6ZGc=|fra_news.tar.gz
https://downloads.wortschatz-leipzig.de/corpora/eng_news_2024_1M.tar.gz|sha256-jx1NB7l3H4p/whmtWH1Tguq/UAnvVj8bxMEqRnqOOpc=|eng_news.tar.gz
https://object.pouta.csc.fi/OPUS-Tatoeba/v2023-04-12/mono/fr.txt.gz|sha256-eaRS30u3OCaYItrYR+pdtEuOtdNi2Nnve2YwXzgJi10=|tatoeba-fr.txt.gz
https://object.pouta.csc.fi/OPUS-Tatoeba/v2023-04-12/mono/en.txt.gz|sha256-oyxVAM12uUeYWXZPt4U3pLm1P6uPo73A/ATdcPKL8ps=|tatoeba-en.txt.gz
https://raw.githubusercontent.com/unicode-org/cldr-json/48.2.0/cldr-json/cldr-annotations-full/annotations/fr/annotations.json|sha256-M9oFL3R4J5GYnmIm1ghxyEZ5ngd6Wn+06H7Pp8o4VzM=|cldr-fr.json
https://raw.githubusercontent.com/unicode-org/cldr-json/48.2.0/cldr-json/cldr-annotations-full/annotations/en/annotations.json|sha256-8iCDy4bf+2OmQ9W6y12YmfgtL6XTiK1POu1yGErO9QU=|cldr-en.json
https://unicode.org/Public/emoji/16.0/emoji-test.txt|sha256-JPDFNOhs8ULiSWlT6PDkaj5wI5KRHt3NKcbM7YUTlpc=|emoji-test.txt
https://huggingface.co/datasets/sagot/lefff_morpho/resolve/main/lefff_morpho-3.5.json|sha256-WNXsQuu8tIuX8CIILOF5qemwx/ep/85cY8UMsexgNDc=|lefff.json"

# nix "sha256-<base64>" -> hex digest
sri_to_hex() { printf '%s' "${1#sha256-}" | base64 -d | od -An -tx1 | tr -d ' \n'; }

echo "==> fetching + verifying corpora" >&2
while IFS='|' read -r url sri name; do
  [ -n "$url" ] || continue
  echo "    $name" >&2
  curl -fsSL --retry 3 -o "$WORK/$name" "$url"
  want="$(sri_to_hex "$sri")"; got="$(sha256sum "$WORK/$name" | cut -d' ' -f1)"
  if [ "$want" != "$got" ]; then
    echo "!! checksum mismatch for $name" >&2
    echo "   expected $want" >&2
    echo "   got      $got" >&2
    exit 1
  fi
done <<EOF
$SOURCES
EOF

echo "==> 1/3 words.tsv (frequency-ranked, col 3 = language by dominance)" >&2
awk 'NF==2 && length($1)>=2 && $1 !~ /^[0-9]+$/ {
       f[$1]+=$2
       if (FILENAME ~ /fr_50k/) { frf[$1]=$2; frtot+=$2 }
       else                     { enf[$1]=$2; entot+=$2 }
     }
     END {
       for (w in f) {
         hf = (w in frf); he = (w in enf)
         if (hf && he) {
           rf = frf[w]/frtot; re = enf[w]/entot
           lang = (rf >= 3*re ? "fr" : re >= 3*rf ? "en" : "both")
         } else lang = (hf ? "fr" : "en")
         print w, f[w], lang
       }
     }' "$WORK/fr_50k.txt" "$WORK/en_50k.txt" \
  | sort -k2,2nr > "$OUT/words.tsv"

# Chat abbreviation lexicon: their PRESENCE in the vocabulary (literalIsWord)
# protects them from autocorrection ("pcq" never becomes "pc"). Modest freq.
for w in pcq bcp tkt mdr ptdr jsp jpp dsl slt stp auj rdv qd qq qqn \
         qqch nrml askip osef oklm vrmt grv bjr bsr dak ftg wsh frr \
         btw imo imho idk tbh brb omg lol wtf asap fyi rn ty np thx \
         pls dm irl afaik ikr smh tbd eta atm fr ong icl; do
  echo "$w 3000 both"
done >> "$OUT/words.tsv"
echo "    words.tsv: $(wc -l < "$OUT/words.tsv") words" >&2

echo "==> 1b/3 morph.tsv (Lefff gender/number, restricted to vocab)" >&2
python3 "$HERE/scripts/build_morph.py" "$WORK/lefff.json" "$OUT/words.tsv" > "$OUT/morph.tsv"
echo "    morph.tsv: $(wc -l < "$OUT/morph.tsv") forms" >&2

echo "==> 2/3 n-grams (Kneser-Ney: Leipzig news + Tatoeba conversational)" >&2
mkdir -p "$WORK/corpus"
tar xzf "$WORK/fra_news.tar.gz" -C "$WORK/corpus"
tar xzf "$WORK/eng_news.tar.gz" -C "$WORK/corpus"
zcat "$WORK/tatoeba-fr.txt.gz" | head -n 600000 > "$WORK/corpus/tatoeba-fr.txt" || true
zcat "$WORK/tatoeba-en.txt.gz" | head -n 600000 > "$WORK/corpus/tatoeba-en.txt" || true

echo "==> 1c/3 élisions combinées (j'ai, c'est, qu'il…) → words.tsv" >&2
python3 "$HERE/scripts/extract_elisions.py" "$OUT/words.tsv" \
  "$WORK"/corpus/*/*-sentences.txt \
  "$WORK/corpus/tatoeba-fr.txt" "$WORK/corpus/tatoeba-en.txt" >> "$OUT/words.tsv"
echo "    words.tsv (+élisions): $(wc -l < "$OUT/words.tsv") words" >&2

python3 "$HERE/daemon/build_ngrams.py" "$OUT/words.tsv" "$OUT" \
  "$WORK"/corpus/*/*-sentences.txt \
  "$WORK/corpus/tatoeba-fr.txt" "$WORK/corpus/tatoeba-en.txt"
echo "    bigrams.tsv:  $(wc -l < "$OUT/bigrams.tsv") | trigrams.tsv: $(wc -l < "$OUT/trigrams.tsv")" >&2

echo "==> 3/3 emoji.tsv (CLDR fr+en keyword index)" >&2
python3 "$HERE/daemon/build_emoji.py" "$WORK/emoji-test.txt" "$OUT/emoji.tsv" \
  "$WORK/cldr-fr.json" "$WORK/cldr-en.json"

cp "$HERE/NOTICE-DATASETS.md" "$OUT/NOTICE"
echo "==> model built in $OUT" >&2
