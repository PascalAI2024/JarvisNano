# Third-party notices

JarvisNano source is Apache-2.0. The compiled firmware also contains ESP-IDF and
registry components under their own licenses, including Apache-2.0, Espressif
modified/MIT terms, MIT, the FreeType license, and ISC-style terms.

A binary release must be accompanied by the **exact notice bundle generated from
the final resolved build**, not this summary alone:

```bash
./scripts/build-v5.sh
python3 scripts/generate-third-party-notices.py
```

The generated file is `.build_logs/THIRD_PARTY_NOTICES.txt`. The generator reads
`dependencies.lock`, requires a license/copying/notice file for every resolved
managed component, records versions and SHA-256 hashes, and fails closed when a
license is missing. Include the corresponding ESP-IDF release notices as well.

Current direct dependencies are pinned in `main/idf_component.yml` and
`tools/idf-pins.txt`. Indirect dependencies remain provider-controlled until
resolved by the pinned build, which is why releases must generate rather than
hand-maintain the complete bundle.

No firmware binary is release-ready without:

1. the generated third-party notice bundle;
2. the exact resolved dependency inventory or SBOM;
3. all model/asset redistribution terms reviewed;
4. the root Apache-2.0 license;
5. the positive and negative security gates in `docs/RELEASE_CHECKLIST.md`.
