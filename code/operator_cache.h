#ifndef OPERATOR_CACHE_H
#define OPERATOR_CACHE_H

#include <Arduino.h>

struct OperatorCacheRecord {
  bool valid;
  String iccid;
  String plmn;
  int8_t act;
  uint32_t lastSuccessTime;
  uint8_t failCount;
};

bool loadOperatorCache(const String& iccid, OperatorCacheRecord* result);
void recordOperatorSuccess(const String& iccid,
                           const char* plmn,
                           int8_t act,
                           uint32_t timestamp);
void recordOperatorFailure(const String& iccid);
bool shouldTryLastGood(const OperatorCacheRecord& record);

#endif
