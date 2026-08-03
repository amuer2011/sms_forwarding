# ML307R Network Recovery Design

## Context

The firmware currently waits for LTE registration during `modemInit()` and reduces
all non-successful `AT+CEREG?` states to a boolean failure. It does not configure
`AT+COPS`, remember a successful operator, or recover when automatic selection
skips an operator that was temporarily marked forbidden.

The observed failure was:

1. The active 9eSIM profile was readable and returned IMSI `24802...`.
2. Automatic selection returned `CEREG=3`.
3. `AT+COPS=?` reported China Unicom `46001` with status `3` (forbidden).
4. Manual selection of `46001` registered successfully with `CEREG=5`.
5. Returning to `COPS=0` and cold-starting later registered automatically.

This shows that normal automatic selection should remain the default, but the
firmware needs a bounded recovery path that can override a stale forbidden state.

## Goals

- Keep automatic operator selection as the normal path.
- Recover without user interaction when automatic registration times out or is
  repeatedly rejected.
- Support different physical SIMs and eSIM profiles without hard-coding `46001`.
- Remember the last operator that actually worked for each SIM identity.
- Scan and try candidate operators for a new SIM with no saved history.
- Try a scanned forbidden operator as a final candidate because the observed
  failure proved that manual selection can still succeed.
- Keep the HTTP server responsive during long scans and registration attempts.
- Prevent long AT commands from mixing responses with web diagnostics or SMS URCs.
- Bound retries and add backoff so a network outage cannot create a reset loop.

## Non-Goals

- Directly edit the SIM/USIM `EF_FPLMN` file.
- Hard-code operator mappings for every IMSI prefix.
- Rework every modem feature into a general-purpose AT command framework.
- Activate mobile data; the existing SMS-only behavior remains unchanged.

## Selected Approach

Add a non-blocking network recovery state machine. The state machine owns the
modem serial port while a registration command is active, but the ESP32 main loop
continues to service WiFi and HTTP.

The existing synchronous modem helpers remain available for short initialization
and diagnostic commands. Recovery commands use a separate long-running
transaction path with explicit ownership and final-result parsing.

## Registration State Model

Replace the boolean-only registration result with a parsed status:

| Status | Meaning | Recovery behavior |
|---|---|---|
| `0` | Not registered, not searching | Continue until automatic deadline |
| `1` | Registered, home | Success |
| `2` | Searching | Continue waiting |
| `3` | Registration denied | Count consecutive denials, then recover |
| `4` | Unknown | Continue until deadline |
| `5` | Registered, roaming | Success |

Unknown future status values are logged and treated as not yet registered. The
parser must extract the `stat` field structurally rather than searching for
`,1` or `,5` anywhere in the complete response.

## State Machine

The recovery service runs from `loop()` and advances through these states:

| State | Purpose |
|---|---|
| `WAIT_SIM` | Wait for `AT+CPIN?` to report `READY` |
| `READ_SIM_ID` | Read ICCID and IMSI and load the matching cache entry |
| `START_AUTO` | Request `AT+COPS=0` |
| `WAIT_AUTO` | Poll registration while automatic selection runs |
| `TRY_LAST_GOOD` | Manually select the cached PLMN for this SIM |
| `SCAN_START` | Start `AT+COPS=?` with a long deadline |
| `SCAN_WAIT` | Collect and parse the complete scan response |
| `TRY_CANDIDATE` | Manually try one scanned PLMN |
| `VERIFY_OPERATOR` | Confirm `CEREG=1/5` and query the selected PLMN |
| `RETURN_AUTO` | Return to `COPS=0` after recovery and verify stability |
| `REGISTERED` | Periodically monitor registration health |
| `BACKOFF` | Stop active recovery until the next bounded retry window |

Only one state may own an active AT transaction. State deadlines use `millis()`
and must remain safe across wraparound.

## Default Timing

| Operation | Default |
|---|---:|
| SIM ready wait | 30 seconds |
| Initial automatic registration | 120 seconds |
| Registration polling interval | 5 seconds |
| Consecutive `CEREG=3` threshold | 3 observations |
| Operator scan deadline | 180 seconds |
| Manual operator attempt | 180 seconds per candidate |
| Post-recovery automatic verification | 30 seconds |
| Registered health check | 30 seconds |
| Full recovery backoff | 10 minutes |

Timing constants remain compile-time values for the first implementation. They
are logged when a recovery cycle starts.

## SIM Identity And Persistent Cache

The cache is keyed by ICCID. If ICCID is unavailable, IMSI is used only for the
current boot and no persistent record is updated.

ESP32 Preferences keys have a short maximum length, so the cache uses a small
fixed set of records rather than using the full ICCID as a key. Each record stores:

- Full ICCID.
- Last successful numeric PLMN.
- Last successful access technology when available.

Four records are sufficient for the expected card-swapping workflow. When full,
new SIM identities replace records in round-robin order. Updating an existing
ICCID keeps its current slot. No record from one ICCID may be used for another
ICCID.

The cache is updated only after `CEREG=1/5` and a valid numeric operator has been
confirmed. A failed manual attempt never overwrites a working record.

## Determining The Successful PLMN

After registration succeeds:

1. Send `AT+COPS=3,2` to request numeric operator formatting.
2. Query `AT+COPS?`.
3. Parse the numeric PLMN and access technology.
4. Save the PLMN for the current ICCID.

Changing the output format must not change the selected operator.

## Candidate Scan And Ordering

`AT+COPS=?` may take several minutes. The complete response is collected until a
final `OK`, `ERROR`, or the 180-second deadline.

Each parsed tuple contributes:

- Status (`0` unknown, `1` available, `2` current, `3` forbidden).
- Numeric PLMN.
- Access technology.

Candidates must be unique by numeric PLMN and access technology. Empty or invalid
PLMN values are ignored.

Attempt order is:

1. Cached last-good PLMN, even if the scan marks it forbidden.
2. Current operator entries (`stat=2`) if registration was not confirmed.
3. Available operators (`stat=1`) in scan order.
4. Forbidden operators (`stat=3`) in scan order.
5. Unknown operators (`stat=0`) only if all other candidates fail.

The state machine tries at most eight unique candidates in one recovery cycle.
Any PLMN already attempted as the cached last-good operator is not tried twice.
LTE access technology reported by the scan is preserved in the manual `COPS`
command. If access technology is missing, selection omits the optional field.

## Manual Attempt And Return To Automatic Mode

A manual attempt sends a numeric selection command equivalent to:

```text
AT+COPS=1,2,"<plmn>",<act>
```

The transaction waits for its final result and then verifies registration with
`AT+CEREG?`. A command timeout is not treated as success merely because bytes were
received.

After a successful manual recovery:

1. Save the confirmed operator for the current ICCID.
2. Wait 5 seconds for the registration to stabilize.
3. Send `AT+COPS=0`.
4. Verify `CEREG=1/5` for 30 seconds.

If returning to automatic mode loses registration, manually reconnect the saved
PLMN and keep manual mode for the remainder of the current boot. The next cold
start still begins with automatic selection.

## Serial Ownership And URC Handling

Long recovery commands must not share `Serial1` with another command reader.

- A global modem-I/O ownership flag identifies an active recovery transaction.
- Web AT commands and diagnostic queries return a busy response while recovery
  owns the port.
- The normal loop does not call the generic serial-line reader while recovery
  owns the port.
- Recovery parsing recognizes unsolicited lines separately from command response
  lines.
- `+CMT:` and its following PDU are forwarded to the existing SMS pipeline rather
  than appended to a `COPS` response or discarded.
- A transaction consumes its complete final response so trailing `OK` bytes cannot
  contaminate the next command.

The existing SMS line state is moved behind a function that can accept a complete
line from either the normal URC reader or the recovery transaction reader.

## Runtime Monitoring

While registered, the service queries `CEREG` every 30 seconds. One failed query
does not start recovery. A non-registered result moves the state machine into a
new 120-second automatic reacquisition window. Candidate recovery begins when:

- `CEREG=3` is observed three consecutive times, or
- registration remains unsuccessful for that automatic registration deadline.

`modemReady` is updated from the latest confirmed state instead of remaining an
initialization-only value.

## Failure And Backoff

If the cached PLMN and all scanned candidates fail:

1. Request `AT+COPS=0`.
2. Mark `modemReady=false`.
3. Log a summary of attempted PLMNs and results.
4. Enter a 10-minute backoff.
5. Start a new automatic recovery cycle after backoff.

The recovery path does not automatically hard-reset the ESP32 or repeatedly toggle
`CFUN`. Radio or module reset remains a later diagnostic action, not the first
response to a policy rejection.

## Logging And Web Behavior

Logs include:

- Parsed SIM identity with ICCID partially masked.
- Registration state transitions.
- Automatic-registration deadline and denial count.
- Cache hit or miss.
- Scan start, completion, tuple count, and parse errors.
- Each attempted PLMN, status, access technology, and outcome.
- Whether automatic mode remained stable after recovery.
- Backoff start and next retry time.

Sensitive identifiers are not printed in full in routine logs. The web overview
continues to show `modemReady`, now backed by periodic registration checks. Web AT
and network diagnostic actions report a clear busy error during recovery instead
of competing for the serial port.

## Testing Strategy

Host-side tests cover pure parsing and ordering logic:

- `CEREG` responses for states `0` through `5`.
- Extended `CEREG` responses containing additional commas and numeric fields.
- The observed `COPS=?` response containing available and forbidden operators.
- Duplicate PLMN removal and candidate ordering.
- Cache lookup, update, and round-robin replacement.
- Malformed and partial modem responses.

Firmware compilation uses the existing Arduino CLI workflow and both project FQBNs
already documented in the repository.

Hardware verification covers:

1. Normal automatic registration without invoking recovery.
2. Known ICCID: automatic failure followed by last-good recovery.
3. New ICCID: scan followed by candidate attempts.
4. A forbidden candidate that succeeds manually.
5. All candidates failing and entering backoff.
6. Returning to automatic mode after manual recovery.
7. A `+CMT` arriving immediately after operator registration.
8. Web access and busy responses during a long scan.
9. Cold restart after successful recovery.
10. Swapping to a different SIM without reusing the previous SIM's PLMN.

## Rollout

The first release keeps all recovery timings as constants and exposes detailed
logs. Raw `EF_FPLMN` modification is deliberately excluded. If field logs show
that operator-specific configuration is still needed, a later change can expose
per-ICCID preferred PLMNs in the web UI without changing the recovery state
machine.
