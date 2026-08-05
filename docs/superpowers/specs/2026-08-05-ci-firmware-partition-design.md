# CI Firmware Partition Design

## Context

The ESP32-C3 firmware image is 1,324,173 bytes, exceeding the default
1,310,720-byte application partition. The target hardware documented by this
repository is a 4 MB ESP32-C3 SuperMini.

## Decision

The build workflow will use ESP32 Arduino Core 3.3.11 and explicitly select
the `huge_app` partition scheme for `esp32c3`. This provides a 3 MiB
application partition and leaves 1 MiB for SPIFFS. The repository does not
use OTA update APIs or a filesystem, so losing OTA slots does not remove an
existing feature.

The workflow file will also be included in the push path filter so a change
to the build configuration triggers its own validation.

## Scope

- Modify only `.github/workflows/build.yml`.
- Keep `pdulib` and `ReadyMail`, because production source uses both.
- Do not change firmware source or hardware targets.

## Verification

- Confirm the workflow pins `esp32:esp32@3.3.11`.
- Confirm the FQBN uses `PartitionScheme=huge_app`.
- Confirm the workflow path is included in the push filter.
- Run the existing standalone network recovery test as a regression baseline.
- Trigger or observe the GitHub Actions workflow to validate the complete
  Arduino compilation on Ubuntu.
