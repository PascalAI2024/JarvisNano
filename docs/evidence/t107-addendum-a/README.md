# Addendum A evidence

The [report](../t107-addendum-a.md) interprets these receipts. The manifest
records each exported file's size and SHA-256. Raw owner-bearing STATUS and
ACTIVITY captures, cockpit readbacks, LAN addresses and pairing credentials
are excluded. Panel images are composed frame mirrors, not physical photos.

`before-long.jsonl` and `after-long.jsonl` retain every measured voltage point.
The after gate failed timer wake (exit 1): its sleep-end and DREAM window
are missing. `after-measurement-status.json` and `final-health.json` record
the blocker; no missing values are filled in. Run `summarize_long.py` beside those files
to reproduce block fits; `plot_long.py` also needs matplotlib. Negative drain
means voltage rose. Scatter and rounding bounds do not bound relaxation,
state of charge, temperature, calibration or image-order bias.

To reproduce the software-only rim comparison at the source commit named
in `deployment.json`, copy the four files in `perf/` into the checkout's
`.evidence/t107a/perf/`. From the checkout root, run
`bash .evidence/t107a/perf/rim-paired-run.sh` inside the host-test Docker image
used by `scripts/host-tests.sh`. This compares the actual overlay against
the frozen renderer and repeats alternating timings with fresh frame pixels.
It measures process CPU time, excluding panel/DMA and vendor decoding.

For the scratch work/pixel mutations, run
`python .evidence/t107a/perf/check-rim-work.py`, then run its generated
`.evidence/t107a/rim-mutants/run.sh` in that container. The generator refuses
to overwrite an existing scratch destination. The restored full arc remains
pixel-identical but fails the conservative work bound. Omitting only the
duplicated endpoint is equivalent and is not asserted to fail.

`measure_long.py` and `device_lib.py` document the live protocol. Copy them to
the checkout's `.evidence/t107a/` before use. They require the paired repository
CLI, the private evidence destination, and an explicitly
authorized device. They never unmute privacy. Screenshots are taken at
boundaries. The baseline DREAM first-sample boundary also received three
extra GETs to verify privacy following the owner report; the report discloses
this disturbance and retains every sample. `owner-voice.json` records the
owner-observed audible answer and the limits of its image attribution.
Executor audit reports are checkpoints from before the parent gates; the
main report supplies the final accepted and blocked states. A future retry
must preserve these original receipts and use a separate evidence destination.
