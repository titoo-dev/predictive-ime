# Dataset licenses & attribution

The predictive-ime **source code** is MIT (see `LICENSE`). The prediction
**model** shipped as a release artifact is *derived* from the third-party
corpora listed below, each under its own license. These licenses — and the
attribution they require — travel with the model: redistributing the model
(or a package that bundles it) means reproducing this notice.

Each source is pinned by URL **and SHA-256** in `build-model.sh`, so the model
build is reproducible and every input is auditable.

| Dataset | Used for | License | Attribution |
|---|---|---|---|
| **OpenSubtitles 2018 frequency lists** (hermitdave/FrequencyWords, `fr_50k`, `en_50k`) | `words.tsv` — word list ranked by frequency | CC BY-SA 4.0 | Hermit Dave, *FrequencyWords* (OpenSubtitles 2018). https://github.com/hermitdave/FrequencyWords |
| **Leipzig Corpora Collection** — `fra_news_2024_300K`, `eng_news_2024_300K` | n-grams (`bigrams.tsv`, `trigrams.tsv`) | CC BY 4.0 | D. Goldhahn, T. Eckart, U. Quasthoff, *Building Large Monolingual Dictionaries at the Leipzig Corpora Collection* (LREC 2012). https://wortschatz.uni-leipzig.de |
| **Tatoeba via OPUS** (release v2023-04-12, `fr`, `en`) | n-grams (conversational register) | CC BY 2.0 FR | Tatoeba.org contributors; OPUS (J. Tiedemann, LREC 2012). https://opus.nlpl.eu/Tatoeba |
| **Unicode CLDR annotations** (v48.2.0, `fr`, `en`) | emoji keyword index (`emoji.tsv`) | Unicode License v3 | Unicode, Inc. — CLDR. https://github.com/unicode-org/cldr-json |
| **Unicode emoji-test.txt** (Emoji 16.0) | authoritative emoji list / fully-qualified forms | Unicode License v3 | Unicode, Inc. https://unicode.org/Public/emoji/16.0/ |

## On the generated model

The generated n-gram tables are statistical aggregates (counts/probabilities)
over the corpora above. Because the largest contributors (Leipzig, Tatoeba,
OpenSubtitles) are **CC BY / CC BY-SA**, the model artifact is distributed
under **CC BY-SA 4.0** with the attribution above — the most restrictive of
the share-alike inputs. The model artifact ships a copy of this file as its
`NOTICE`.

> Note for distro packagers: the model is a *separate* artifact with a
> *separate* (CC BY-SA) license from the MIT code. Package it from the pinned
> release tarball, not by re-downloading corpora at build time.
