#include "../code/network_recovery.h"

#include <assert.h>
#include <string.h>

int main() {
  assert(isCeregRegistered("+CEREG: 0,1\r\nOK\r\n"));
  assert(isCeregRegistered("+CEREG: 0,5\r\nOK\r\n"));
  assert(!isCeregRegistered("+CEREG: 0,3\r\nOK\r\n"));

  const char* scan =
      "+COPS: (1,\"CHN-CT\",\"CT\",\"46011\",7),"
      "(3,\"CHN-UNICOM\",\"UNICOM\",\"46001\",7),"
      "(3,\"\",\"\",\"46015\",7),"
      "(1,\"CHINA MOBILE\",\"CMCC\",\"46000\",7),,(0,1,2,3,4),(0,1,2)";
  char candidates[8][SIM_NETWORK_PLMN_LENGTH] = {};
  assert(extractCopsCandidates(scan, candidates, 8) == 4);
  assert(strcmp(candidates[0], "46011") == 0);
  assert(strcmp(candidates[1], "46001") == 0);
  assert(strcmp(candidates[2], "46015") == 0);
  assert(strcmp(candidates[3], "46000") == 0);

  char plmn[SIM_NETWORK_PLMN_LENGTH] = {};
  uint8_t act = 0;
  assert(extractCurrentCops("+COPS: 1,2,\"46001\",7\r\nOK\r\n", plmn, &act));
  assert(strcmp(plmn, "46001") == 0);
  assert(act == 7);

  SimNetworkRecord records[MAX_SIM_NETWORK_RECORDS] = {};
  strcpy(records[0].iccid, "ICCID_A");
  strcpy(records[0].imsi, "248020000000001");
  records[0].lastSuccessTime = 100;
  strcpy(records[1].iccid, "ICCID_B");
  strcpy(records[1].imsi, "460000000000002");
  records[1].lastSuccessTime = 200;
  assert(findSimNetworkRecord(records, "ICCID_A", "ignored") == 0);
  assert(findSimNetworkRecord(records, "", "460000000000002") == 1);
  assert(findSimNetworkRecord(records, "ICCID_C", "248020000000001") == -1);
  assert(selectSimNetworkRecordSlot(records) == 2);
  for (uint8_t i = 2; i < MAX_SIM_NETWORK_RECORDS; ++i) {
    strcpy(records[i].iccid, "FULL");
    records[i].lastSuccessTime = 300 + i;
  }
  assert(selectSimNetworkRecordSlot(records) == 0);

  SimNetworkRecord imsiOnly[MAX_SIM_NETWORK_RECORDS] = {};
  strcpy(imsiOnly[0].imsi, "901700000000001");
  assert(findSimNetworkRecord(imsiOnly, "", "901700000000001") == 0);
  assert(selectSimNetworkRecordSlot(imsiOnly) == 1);
}
