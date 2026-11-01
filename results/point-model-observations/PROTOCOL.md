# Point-model observation experiment

Written before measuring the alternative observation stream.

The current controller learns its thermal model from a filtered hot-spot
sensor, while `forecast_c` is scored against a future raw sensor reading.
The first forecast study exercised the learning module directly with raw
sensors, so it does not establish daemon-path accuracy.

Evaluate an independent point-forecast thermal model trained on raw hot-spot
and package sensors, with the same observed Energy Model power estimates.
Keep the existing filtered control model, every safety limit, controller
choice and actuation unchanged. The delayed-feedback forecaster and its
parameters stay frozen. Compare both predictors on the exact same closed-loop
trajectory; neither sees future loads, simulator temperatures or fitted truth.

Use separate development and reserved profile groups. Score saved five-second
predictions only when raw future sensor labels arrive. Report persistence,
current and candidate MAE/RMSE, physical-model readiness and all-window counts,
including fallback. Warm-up is 300 s; total trajectory 1200 s. Aggregate equally
across profiles, retaining regressions. Confirm any accepted change through
real Linux daemon telemetry and preserve the distinction between module,
controller and file-ABI runtime evidence. Do not change the default policy.
