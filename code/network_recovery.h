#ifndef NETWORK_RECOVERY_H
#define NETWORK_RECOVERY_H

#include <Arduino.h>

enum RecoveryState : uint8_t {
  WAIT_SIM,
  READ_ICCID,
  READ_IMSI,
  START_AUTO,
  WAIT_AUTO,
  TRY_LAST_GOOD,
  SCAN_START,
  SCAN_WAIT,
  TRY_CANDIDATE,
  VERIFY_OPERATOR,
  REGISTERED_AUTO,
  REGISTERED_MANUAL,
  BACKOFF
};

void beginNetworkRecovery();
void serviceNetworkRecovery();
void resetNetworkRecovery();
bool networkRecoveryBusy();

#endif
