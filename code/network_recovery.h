#ifndef NETWORK_RECOVERY_H
#define NETWORK_RECOVERY_H

#include <stdint.h>

constexpr uint8_t MAX_SIM_NETWORK_RECORDS = 20;
constexpr uint8_t MAX_COPS_CANDIDATES = 8;
constexpr uint8_t SIM_NETWORK_ICCID_LENGTH = 24;
constexpr uint8_t SIM_NETWORK_IMSI_LENGTH = 17;
constexpr uint8_t SIM_NETWORK_PLMN_LENGTH = 7;

struct SimNetworkRecord {
  char iccid[SIM_NETWORK_ICCID_LENGTH];
  char imsi[SIM_NETWORK_IMSI_LENGTH];
  char plmn[SIM_NETWORK_PLMN_LENGTH];
  uint8_t act;
  uint32_t lastSuccessTime;
};

bool isCeregRegistered(const char* response);
uint8_t extractCopsCandidates(const char* response,
                              char candidates[][SIM_NETWORK_PLMN_LENGTH],
                              uint8_t capacity);
bool extractCurrentCops(const char* response,
                        char plmn[SIM_NETWORK_PLMN_LENGTH],
                        uint8_t* act);
int findSimNetworkRecord(const SimNetworkRecord records[],
                         const char* iccid,
                         const char* imsi);
uint8_t selectSimNetworkRecordSlot(const SimNetworkRecord records[]);

#endif
