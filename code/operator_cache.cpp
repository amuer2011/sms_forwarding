#include "operator_cache.h"

#include <Preferences.h>

#include "network_recovery_core.h"

namespace {

constexpr char CACHE_NAMESPACE[] = "plmn_cache";
constexpr uint8_t CACHE_SLOT_COUNT = 4;

void makeSlotKey(char prefix, uint8_t slot, char key[3]) {
  key[0] = prefix;
  key[1] = static_cast<char>('0' + slot);
  key[2] = '\0';
}

int8_t findSlot(Preferences& storage, const String& iccid) {
  char key[3] = {};
  for (uint8_t slot = 0; slot < CACHE_SLOT_COUNT; ++slot) {
    makeSlotKey('i', slot, key);
    if (storage.getString(key, "") == iccid) {
      return static_cast<int8_t>(slot);
    }
  }
  return -1;
}

}  // namespace

bool loadOperatorCache(const String& iccid, OperatorCacheRecord* result) {
  if (result == nullptr) {
    return false;
  }
  *result = {};
  if (iccid.isEmpty()) {
    return false;
  }

  Preferences storage;
  if (!storage.begin(CACHE_NAMESPACE, true)) {
    return false;
  }

  const int8_t slot = findSlot(storage, iccid);
  if (slot < 0) {
    storage.end();
    return false;
  }

  char key[3] = {};
  makeSlotKey('p', static_cast<uint8_t>(slot), key);
  const String plmn = storage.getString(key, "");
  if (plmn.isEmpty()) {
    storage.end();
    return false;
  }

  result->valid = true;
  result->iccid = iccid;
  result->plmn = plmn;
  makeSlotKey('a', static_cast<uint8_t>(slot), key);
  result->act = static_cast<int8_t>(storage.getUChar(key, 7));
  makeSlotKey('t', static_cast<uint8_t>(slot), key);
  result->lastSuccessTime = storage.getUInt(key, 0);
  makeSlotKey('f', static_cast<uint8_t>(slot), key);
  result->failCount = storage.getUChar(key, 0);
  storage.end();
  return true;
}

void recordOperatorSuccess(const String& iccid,
                           const char* plmn,
                           int8_t act,
                           uint32_t timestamp) {
  if (iccid.isEmpty() || plmn == nullptr || plmn[0] == '\0') {
    return;
  }

  Preferences storage;
  if (!storage.begin(CACHE_NAMESPACE, false)) {
    return;
  }

  int8_t slot = findSlot(storage, iccid);
  if (slot < 0) {
    slot = static_cast<int8_t>(storage.getUChar("next", 0) %
                               CACHE_SLOT_COUNT);
    storage.putUChar("next", static_cast<uint8_t>((slot + 1) %
                                                   CACHE_SLOT_COUNT));
  }

  char key[3] = {};
  makeSlotKey('i', static_cast<uint8_t>(slot), key);
  storage.putString(key, iccid);
  makeSlotKey('p', static_cast<uint8_t>(slot), key);
  storage.putString(key, plmn);
  makeSlotKey('a', static_cast<uint8_t>(slot), key);
  storage.putUChar(key, static_cast<uint8_t>(act));
  makeSlotKey('t', static_cast<uint8_t>(slot), key);
  storage.putUInt(key, timestamp);
  makeSlotKey('f', static_cast<uint8_t>(slot), key);
  storage.putUChar(key, 0);
  storage.end();
}

void recordOperatorFailure(const String& iccid) {
  if (iccid.isEmpty()) {
    return;
  }

  Preferences storage;
  if (!storage.begin(CACHE_NAMESPACE, false)) {
    return;
  }

  const int8_t slot = findSlot(storage, iccid);
  if (slot < 0) {
    storage.end();
    return;
  }

  char key[3] = {};
  makeSlotKey('f', static_cast<uint8_t>(slot), key);
  const uint8_t failCount = storage.getUChar(key, 0);
  storage.putUChar(key, incrementFailureCount(failCount));
  storage.end();
}

bool shouldTryLastGood(const OperatorCacheRecord& record) {
  return record.valid && !record.plmn.isEmpty() &&
         shouldTryLastGood(record.failCount);
}
