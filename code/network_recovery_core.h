#ifndef NETWORK_RECOVERY_CORE_H
#define NETWORK_RECOVERY_CORE_H

#include <stddef.h>
#include <stdint.h>

enum RegState : int8_t {
  REG_PARSE_UNKNOWN = -1,
  REG_NOT_REGISTERED = 0,
  REG_HOME = 1,
  REG_SEARCHING = 2,
  REG_DENIED = 3,
  REG_NETWORK_UNKNOWN = 4,
  REG_ROAMING = 5
};

struct OperatorCandidate {
  int8_t status;
  char plmn[7];
  int8_t act;
};

enum ModemIoOwner : uint8_t {
  MODEM_IO_NONE = 0,
  MODEM_IO_SYNC = 1,
  MODEM_IO_RECOVERY = 2,
  MODEM_IO_SMS = 3
};

constexpr uint16_t MODEM_LINE_CAPACITY = 500;

struct ModemLineAssembler {
  uint16_t length;
  bool overflow;
  char buffer[MODEM_LINE_CAPACITY];
};

constexpr uint8_t MAX_SCANNED_OPERATORS = 12;

struct CopsScanParser {
  bool inTuple;
  uint8_t tupleLength;
  char tuple[96];
  uint8_t count;
  OperatorCandidate operators[MAX_SCANNED_OPERATORS];
};

RegState parseCeregState(const char* response);
bool isRegisteredState(RegState state);
bool parseCurrentOperator(const char* response, OperatorCandidate* result);
void resetCopsScanParser(CopsScanParser* parser);
void feedCopsScanParser(CopsScanParser* parser, char value);
uint8_t buildOrderedCandidates(const CopsScanParser& parser,
                               const char* attemptedPlmn,
                               OperatorCandidate* output,
                               uint8_t capacity);
bool shouldTryLastGood(uint8_t failCount);
uint8_t incrementFailureCount(uint8_t failCount);
bool acquireModemIoOwner(ModemIoOwner* current, ModemIoOwner requested);
bool releaseModemIoOwner(ModemIoOwner* current, ModemIoOwner requested);
void resetModemLineAssembler(ModemLineAssembler* assembler);
bool feedModemLineAssembler(ModemLineAssembler* assembler, char value);

#endif
