#include <assert.h>
#include <string.h>

#include "../code/network_recovery_core.h"

static void testCeregParsing() {
  assert(parseCeregState("+CEREG: 0,1\r\nOK\r\n") == REG_HOME);
  assert(parseCeregState("+CEREG: 0,5\r\nOK\r\n") == REG_ROAMING);
  assert(parseCeregState("+CEREG: 0,3\r\nOK\r\n") == REG_DENIED);
  assert(parseCeregState("+CEREG: 0\r\nOK\r\n") == REG_PARSE_UNKNOWN);
  assert(parseCeregState("+CEREG:\r\nOK\r\n") == REG_PARSE_UNKNOWN);
  assert(parseCeregState("") == REG_PARSE_UNKNOWN);
  assert(isRegisteredState(REG_HOME));
  assert(isRegisteredState(REG_ROAMING));
  assert(!isRegisteredState(REG_DENIED));
}

static void testCurrentOperatorParsing() {
  OperatorCandidate result = {};
  assert(parseCurrentOperator("+COPS: 0,2,\"46001\",7\r\nOK\r\n", &result));
  assert(strcmp(result.plmn, "46001") == 0);
  assert(result.act == 7);
}

static void testCopsScanOrdering() {
  const char* response =
      "+COPS: (1,\"CHN-CT\",\"CT\",\"46011\",7),"
      "(3,\"CHN-UNICOM\",\"UNICOM\",\"46001\",7),"
      "(3,\"\",\"\",\"46015\",7),"
      "(1,\"CHINA MOBILE\",\"CMCC\",\"46000\",7),,"
      "(0,1,2,3,4),(0,1,2)\r\nOK\r\n";
  CopsScanParser parser;
  resetCopsScanParser(&parser);
  for (const char* value = response; *value != '\0'; ++value) {
    feedCopsScanParser(&parser, *value);
  }

  OperatorCandidate ordered[4] = {};
  const uint8_t count = buildOrderedCandidates(parser, "", ordered, 4);
  assert(count == 4);
  assert(strcmp(ordered[0].plmn, "46011") == 0);
  assert(strcmp(ordered[1].plmn, "46000") == 0);
  assert(strcmp(ordered[2].plmn, "46001") == 0);
  assert(strcmp(ordered[3].plmn, "46015") == 0);
}

static void testLastGoodFailureThreshold() {
  assert(shouldTryLastGood(0));
  assert(shouldTryLastGood(2));
  assert(!shouldTryLastGood(3));
  assert(incrementFailureCount(2) == 3);
  assert(incrementFailureCount(255) == 255);
}

static void testModemIoOwnership() {
  ModemIoOwner owner = MODEM_IO_NONE;
  assert(acquireModemIoOwner(&owner, MODEM_IO_SYNC));
  assert(owner == MODEM_IO_SYNC);
  assert(!acquireModemIoOwner(&owner, MODEM_IO_RECOVERY));
  assert(!releaseModemIoOwner(&owner, MODEM_IO_RECOVERY));
  assert(owner == MODEM_IO_SYNC);
  assert(releaseModemIoOwner(&owner, MODEM_IO_SYNC));
  assert(owner == MODEM_IO_NONE);
}

static void testModemLineAssemblyAcrossServiceCalls() {
  ModemLineAssembler assembler = {};
  assert(!feedModemLineAssembler(&assembler, '+'));
  assert(!feedModemLineAssembler(&assembler, 'O'));
  assert(!feedModemLineAssembler(&assembler, 'K'));
  assert(!feedModemLineAssembler(&assembler, '\r'));
  assert(feedModemLineAssembler(&assembler, '\n'));
  assert(strcmp(assembler.buffer, "+OK") == 0);
  resetModemLineAssembler(&assembler);
  assert(assembler.length == 0);
}

int main() {
  testCeregParsing();
  testCurrentOperatorParsing();
  testCopsScanOrdering();
  testLastGoodFailureThreshold();
  testModemIoOwnership();
  testModemLineAssemblyAcrossServiceCalls();
  return 0;
}
