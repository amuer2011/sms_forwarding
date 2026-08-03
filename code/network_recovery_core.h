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

constexpr uint8_t MAX_SCANNED_OPERATORS = 12;

struct CopsScanParser {
  bool inTuple;
  uint8_t tupleLength;
  char tuple[96];
  uint8_t count;
  OperatorCandidate operators[MAX_SCANNED_OPERATORS];
};

RegState parseCeregState(const char* response);
bool parseCurrentOperator(const char* response, OperatorCandidate* result);
void resetCopsScanParser(CopsScanParser* parser);
void feedCopsScanParser(CopsScanParser* parser, char value);
uint8_t buildOrderedCandidates(const CopsScanParser& parser,
                               const char* attemptedPlmn,
                               OperatorCandidate* output,
                               uint8_t capacity);

#endif
