#include "network_recovery.h"

#include <ctype.h>
#include <string.h>

namespace {

void skipSpaces(const char*& cursor) {
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
}

bool readNumber(const char*& cursor, uint16_t* value) {
  skipSpaces(cursor);
  if (!isdigit(static_cast<unsigned char>(*cursor))) return false;

  uint16_t result = 0;
  while (isdigit(static_cast<unsigned char>(*cursor))) {
    result = static_cast<uint16_t>(result * 10 + (*cursor - '0'));
    ++cursor;
  }
  *value = result;
  return true;
}

bool isNumericPlmn(const char* value, size_t length) {
  if (length != 5 && length != 6) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool hasCandidate(const char candidates[][SIM_NETWORK_PLMN_LENGTH],
                  uint8_t count,
                  const char* plmn,
                  size_t length) {
  for (uint8_t i = 0; i < count; ++i) {
    if (strlen(candidates[i]) == length && strncmp(candidates[i], plmn, length) == 0) {
      return true;
    }
  }
  return false;
}

const char* nextQuotedValue(const char* cursor, const char** value, size_t* length) {
  const char* begin = strchr(cursor, '"');
  if (begin == nullptr) return nullptr;
  ++begin;
  const char* end = strchr(begin, '"');
  if (end == nullptr) return nullptr;
  *value = begin;
  *length = static_cast<size_t>(end - begin);
  return end + 1;
}

bool isEmptyRecord(const SimNetworkRecord& record) {
  return record.iccid[0] == '\0' && record.imsi[0] == '\0';
}

}  // namespace

bool isCeregRegistered(const char* response) {
  if (response == nullptr) return false;

  const char* cursor = strstr(response, "+CEREG:");
  if (cursor == nullptr) return false;
  cursor += strlen("+CEREG:");

  uint16_t firstValue = 0;
  if (!readNumber(cursor, &firstValue)) return false;
  uint16_t status = firstValue;
  if (*cursor == ',') {
    ++cursor;
    if (!readNumber(cursor, &status)) return false;
  }
  return status == 1 || status == 5;
}

uint8_t extractCopsCandidates(const char* response,
                              char candidates[][SIM_NETWORK_PLMN_LENGTH],
                              uint8_t capacity) {
  if (response == nullptr || candidates == nullptr || capacity == 0) return 0;

  const char* cursor = strstr(response, "+COPS:");
  if (cursor == nullptr) return 0;

  uint8_t count = 0;
  while (count < capacity) {
    const char* value = nullptr;
    size_t length = 0;
    cursor = nextQuotedValue(cursor, &value, &length);
    if (cursor == nullptr) break;
    if (!isNumericPlmn(value, length) || hasCandidate(candidates, count, value, length)) {
      continue;
    }
    memcpy(candidates[count], value, length);
    candidates[count][length] = '\0';
    ++count;
  }
  return count;
}

bool extractCurrentCops(const char* response,
                        char plmn[SIM_NETWORK_PLMN_LENGTH],
                        uint8_t* act) {
  if (response == nullptr || plmn == nullptr || act == nullptr) return false;

  const char* cursor = strstr(response, "+COPS:");
  if (cursor == nullptr) return false;

  while (true) {
    const char* value = nullptr;
    size_t length = 0;
    cursor = nextQuotedValue(cursor, &value, &length);
    if (cursor == nullptr) return false;
    if (!isNumericPlmn(value, length)) continue;

    const char* actCursor = cursor;
    skipSpaces(actCursor);
    if (*actCursor != ',') return false;
    ++actCursor;
    uint16_t parsedAct = 0;
    if (!readNumber(actCursor, &parsedAct) || parsedAct > 255) return false;

    memcpy(plmn, value, length);
    plmn[length] = '\0';
    *act = static_cast<uint8_t>(parsedAct);
    return true;
  }
}

int findSimNetworkRecord(const SimNetworkRecord records[],
                         const char* iccid,
                         const char* imsi) {
  if (records == nullptr) return -1;

  const bool matchIccid = iccid != nullptr && iccid[0] != '\0';
  const char* identity = matchIccid ? iccid : imsi;
  if (identity == nullptr || identity[0] == '\0') return -1;

  for (uint8_t i = 0; i < MAX_SIM_NETWORK_RECORDS; ++i) {
    const char* storedIdentity = matchIccid ? records[i].iccid : records[i].imsi;
    if (strcmp(storedIdentity, identity) == 0) return i;
  }
  return -1;
}

uint8_t selectSimNetworkRecordSlot(const SimNetworkRecord records[]) {
  if (records == nullptr) return 0;

  uint8_t oldestSlot = 0;
  uint32_t oldestTime = records[0].lastSuccessTime;
  for (uint8_t i = 0; i < MAX_SIM_NETWORK_RECORDS; ++i) {
    if (isEmptyRecord(records[i])) return i;
    if (records[i].lastSuccessTime < oldestTime) {
      oldestTime = records[i].lastSuccessTime;
      oldestSlot = i;
    }
  }
  return oldestSlot;
}
