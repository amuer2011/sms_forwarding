# Per-SIM Network Recovery Design

## Goal

Improve modem network recovery without adding dependencies or changing the web UI. The firmware records the last PLMN that successfully registered for each SIM and uses that history before scanning networks.

## Scope

- Modify the modem initialization and registration helpers only.
- Store at most 20 SIM network records in a dedicated NVS namespace.
- Identify a SIM by ICCID when available; use IMSI only when ICCID cannot be read.
- Treat only `CEREG` states 1 and 5 as successful registration.
- Do not use `AT+COPS=?` result status values to decide whether a PLMN may be attempted.

## Cache

Each fixed-size record contains:

- ICCID
- IMSI
- Last successful numeric PLMN
- Last successful access technology
- Last successful timestamp

Records are persisted as one fixed-size NVS blob. A new record replaces the entry with the oldest successful timestamp when all 20 slots are occupied. This bounds RAM, NVS use, and code size.

## Registration Flow

1. Read `AT+CCID` and `AT+CIMI` once after the modem AT handshake.
2. Leave operator selection automatic and poll `AT+CEREG?` for up to 120 seconds.
3. When registration succeeds (`CEREG=1` or `CEREG=5`), query `AT+COPS?`, save the actual registered PLMN and access technology for the current SIM, then complete initialization.
4. If automatic registration does not succeed, locate the current SIM's last-good record. Use ICCID for lookup when it is available; otherwise use IMSI.
5. When a record exists, issue `AT+COPS=1,2,"<last-good>",7`, then poll for a successful `CEREG` for up to 45 seconds. On success, update the record and complete initialization.
6. If no record exists or the last-good attempt fails, run `AT+COPS=?`, extract at most eight distinct numeric PLMNs, and attempt each with `AT+COPS=1,2,"<PLMN>",7`. Poll for successful `CEREG` for up to 45 seconds after each accepted command. Stop at the first successful registration and persist it.
7. If all candidates fail, issue `AT+COPS=0` to remove the manual selection, leave `modemReady` false, and log the failure.

`AT+COPS=?` only discovers candidates. Its availability/forbidden field is ignored because the observed ML307R output marked a PLMN forbidden even though the modem subsequently registered with it.

## Error Handling

- A failed or empty ICCID is not cached as an ICCID identity; IMSI may be used for lookup only in that case.
- Empty or malformed PLMN values are skipped.
- An `OK` from a manual `COPS` command is not success by itself; only `CEREG=1/5` is success.
- The existing UI remains available while all waits run because the AT wait loops continue serving `server.handleClient()`.

## Verification

- Add host-testable parsing and cache-selection checks where the project toolchain allows them.
- Compile the ESP32-C3 sketch with its configured Arduino toolchain.
- Review the resulting binary size against the current build output and ensure the project remains flashable.
