# CI Firmware Partition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make GitHub Actions compile the ESP32-C3 firmware within the target board's 4 MB flash layout.

**Architecture:** Keep the existing ESP32-C3 target and required libraries. Pin the Arduino core for reproducible builds, choose its supported 3 MiB `huge_app` application partition, and ensure a workflow-only change triggers the build.

**Tech Stack:** GitHub Actions, Arduino CLI, ESP32 Arduino Core 3.3.11.

## Global Constraints

- Use `esp32:esp32@3.3.11`.
- Use `esp32:esp32:esp32c3:PartitionScheme=huge_app`.
- Do not remove `pdulib` or `ReadyMail`.
- Do not modify firmware source or change the ESP32-C3 target.

---

### Task 1: Configure the reproducible ESP32-C3 build

**Files:**
- Modify: `.github/workflows/build.yml`
- Test: inline PowerShell workflow configuration assertion

**Interfaces:**
- Consumes: Arduino CLI FQBN menu option `PartitionScheme=huge_app`.
- Produces: a GitHub Actions build command with a 3 MiB ESP32-C3 application partition.

- [ ] **Step 1: Write and run the failing configuration assertion**

```powershell
$workflow = Get-Content -Raw '.github/workflows/build.yml'
if ($workflow -notmatch 'esp32:esp32@3\.3\.11') { throw 'ESP32 core is not pinned to 3.3.11' }
if ($workflow -notmatch 'esp32:esp32:esp32c3:PartitionScheme=huge_app') { throw 'ESP32-C3 huge_app partition is not selected' }
if ($workflow -notmatch "- '.github/workflows/build.yml'") { throw 'Workflow changes do not trigger the push build' }
```

Expected: the assertion fails because the workflow currently installs an unpinned core and compiles with the default partition scheme.

- [ ] **Step 2: Update the workflow**

```yaml
on:
  push:
    paths:
      - 'code/**'
      - '.github/workflows/build.yml'

# Install ESP32 platform
arduino-cli core install esp32:esp32@3.3.11

# Compile firmware
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=huge_app ./code
```

- [ ] **Step 3: Verify the configuration and regression baseline**

Run the assertion from Step 1 again. Then run:

```powershell
$testBinary = Join-Path $env:TEMP 'sms-forwarding-network-recovery-test.exe'
g++ -std=c++11 tests/network_recovery_test.cpp code/network_recovery.cpp -o $testBinary
& $testBinary
$exitCode = $LASTEXITCODE
Remove-Item -LiteralPath $testBinary
exit $exitCode
```

Expected: the assertion and standalone test both exit with code 0.

- [ ] **Step 4: Validate the full Arduino build in GitHub Actions**

Push the branch or invoke `workflow_dispatch` for the build workflow. Expected: the compile step reports program storage usage below the 3,145,728-byte application limit.

- [ ] **Step 5: Commit the workflow change**

```bash
git add .github/workflows/build.yml docs/superpowers/plans/2026-08-05-ci-firmware-partition.md
git commit -m "ci: expand ESP32-C3 firmware partition"
```
