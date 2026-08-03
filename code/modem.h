#ifndef MODEM_H
#define MODEM_H

#include "globals.h"
#include "network_recovery_core.h"

String sendATCommand(const char* cmd, unsigned long timeout);
bool acquireModemIo(ModemIoOwner owner);
void releaseModemIo(ModemIoOwner owner);
bool modemIoBusy();
void modemPowerCycle();
void resetModule();
void modemInit();
bool sendATandWaitOK(const char* cmd, unsigned long timeout);
bool waitCEREG();
void blink_short(unsigned long gap_time = 500);
bool sendSMS(const char* phoneNumber, const char* message);

#endif
