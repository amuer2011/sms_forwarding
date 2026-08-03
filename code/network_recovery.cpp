#include "network_recovery.h"

#include <string.h>
#include <time.h>

#include "globals.h"
#include "modem.h"
#include "network_recovery_core.h"
#include "operator_cache.h"
#include "sms_process.h"
#include "web_handlers.h"

namespace {

constexpr uint32_t SIM_READY_TIMEOUT_MS = 30000;
constexpr uint32_t AUTO_REGISTER_TIMEOUT_MS = 120000;
constexpr uint32_t LAST_GOOD_TIMEOUT_MS = 180000;
constexpr uint32_t SCAN_TIMEOUT_MS = 180000;
constexpr uint32_t CANDIDATE_TIMEOUT_MS = 60000;
constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 30000;
constexpr uint32_t REGISTER_LOSS_TIMEOUT_MS = 120000;
constexpr uint32_t BACKOFF_MS = 600000;
constexpr uint32_t REGISTRATION_POLL_MS = 5000;
constexpr uint8_t MAX_CANDIDATE_ATTEMPTS = 4;
constexpr uint8_t MAX_DENIED_HEALTH_CHECKS = 3;
constexpr uint16_t RECOVERY_READ_BUDGET = 64;
constexpr size_t SHORT_RESPONSE_CAPACITY = 256;

enum TransactionPurpose : uint8_t {
  TX_NONE,
  TX_CPIN,
  TX_ICCID,
  TX_IMSI,
  TX_START_AUTO,
  TX_CEREG,
  TX_MANUAL_OPERATOR,
  TX_SCAN,
  TX_SET_NUMERIC_FORMAT,
  TX_CURRENT_OPERATOR,
  TX_BACKOFF_AUTO
};

enum ManualAttemptPhase : uint8_t {
  MANUAL_SEND_COMMAND,
  MANUAL_CHECK_REGISTRATION
};

struct AsyncAtTransaction {
  bool active;
  bool completed;
  bool success;
  bool timedOut;
  TransactionPurpose purpose;
  uint32_t deadline;
  uint16_t responseLength;
  char response[SHORT_RESPONSE_CAPACITY];
};

RecoveryState recoveryState = WAIT_SIM;
AsyncAtTransaction transaction = {};
CopsScanParser scanParser = {};
OperatorCandidate candidates[MAX_CANDIDATE_ATTEMPTS] = {};
OperatorCandidate attemptedOperator = {};
OperatorCandidate registeredOperator = {};
OperatorCacheRecord cachedOperator = {};
String currentIccid;
String currentImsi;
uint32_t stateStarted = 0;
uint32_t nextActionAt = 0;
uint32_t lastHealthCheck = 0;
uint32_t registrationLostAt = 0;
uint32_t backoffUntil = 0;
uint8_t candidateCount = 0;
uint8_t candidateIndex = 0;
uint8_t deniedHealthChecks = 0;
uint8_t verifyStep = 0;
ManualAttemptPhase manualPhase = MANUAL_SEND_COMMAND;
bool recoveryStarted = false;
bool recoveryOwnsIo = false;
bool scanPerformed = false;
bool verifyingManualRegistration = false;
bool backoffInitialized = false;
char attemptedPlmn[7] = {};

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool elapsed(uint32_t now, uint32_t start, uint32_t duration) {
  return static_cast<uint32_t>(now - start) >= duration;
}

void setRecoveryState(RecoveryState state) {
  recoveryState = state;
  stateStarted = millis();
  nextActionAt = 0;
}

bool ensureRecoveryOwnership() {
  if (recoveryOwnsIo) return true;
  if (!acquireModemIo(MODEM_IO_RECOVERY)) return false;
  recoveryOwnsIo = true;
  return true;
}

void releaseRecoveryOwnership() {
  if (!recoveryOwnsIo) return;
  releaseModemIo(MODEM_IO_RECOVERY);
  recoveryOwnsIo = false;
}

void resetTransaction() {
  transaction = {};
}

bool isFinalLine(const String& line) {
  return line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR") ||
         line.startsWith("+CMS ERROR");
}

void appendResponseLine(const String& line) {
  const size_t length = line.length();
  if (length == 0 || transaction.responseLength >= SHORT_RESPONSE_CAPACITY - 1) {
    return;
  }

  size_t available = SHORT_RESPONSE_CAPACITY - 1 - transaction.responseLength;
  size_t copyLength = length < available ? length : available;
  memcpy(transaction.response + transaction.responseLength, line.c_str(),
         copyLength);
  transaction.responseLength += static_cast<uint16_t>(copyLength);
  available = SHORT_RESPONSE_CAPACITY - 1 - transaction.responseLength;
  if (available > 0) {
    transaction.response[transaction.responseLength++] = '\n';
  }
  transaction.response[transaction.responseLength] = '\0';
}

bool startTransaction(TransactionPurpose purpose,
                      const String& command,
                      uint32_t timeout,
                      bool resetScan = false) {
  if (!ensureRecoveryOwnership() || transaction.active || transaction.completed) {
    return false;
  }

  resetTransaction();
  transaction.active = true;
  transaction.purpose = purpose;
  transaction.deadline = millis() + timeout;
  if (resetScan) resetCopsScanParser(&scanParser);
  Serial1.println(command);
  return true;
}

void serviceTransaction() {
  if (!transaction.active) return;

  const uint32_t now = millis();
  if (timeReached(now, transaction.deadline)) {
    transaction.active = false;
    transaction.completed = true;
    transaction.success = false;
    transaction.timedOut = true;
    return;
  }

  String line;
  CopsScanParser* parser = transaction.purpose == TX_SCAN ? &scanParser : nullptr;
  if (!readSerialLineLimited(Serial1, &line, RECOVERY_READ_BUDGET, parser)) return;
  if (processModemUrcLine(line)) return;

  line.trim();
  if (line.length() == 0) return;
  if (transaction.purpose != TX_SCAN || isFinalLine(line)) {
    appendResponseLine(line);
  }
  if (!isFinalLine(line)) return;

  transaction.active = false;
  transaction.completed = true;
  transaction.success = line == "OK";
}

void consumeTransaction() {
  resetTransaction();
}

String extractLongestDigitRun(const char* response, uint8_t minimumLength) {
  if (response == nullptr) return "";

  const char* bestStart = nullptr;
  size_t bestLength = 0;
  const char* cursor = response;
  while (*cursor != '\0') {
    if (*cursor < '0' || *cursor > '9') {
      ++cursor;
      continue;
    }

    const char* start = cursor;
    while (*cursor >= '0' && *cursor <= '9') ++cursor;
    const size_t length = static_cast<size_t>(cursor - start);
    if (length > bestLength) {
      bestStart = start;
      bestLength = length;
    }
  }

  if (bestStart == nullptr || bestLength < minimumLength) return "";
  String result;
  result.reserve(bestLength);
  for (size_t index = 0; index < bestLength; ++index) result += bestStart[index];
  return result;
}

uint32_t successTimestamp() {
  const time_t now = time(nullptr);
  return now >= 100000 ? static_cast<uint32_t>(now) : millis() / 1000;
}

void copyAttemptedPlmn(const char* plmn) {
  memset(attemptedPlmn, 0, sizeof(attemptedPlmn));
  if (plmn == nullptr) return;
  strncpy(attemptedPlmn, plmn, sizeof(attemptedPlmn) - 1);
}

void enterRegistered(bool manual) {
  modemReady = true;
  deniedHealthChecks = 0;
  registrationLostAt = 0;
  lastHealthCheck = millis();
  setRecoveryState(manual ? REGISTERED_MANUAL : REGISTERED_AUTO);
  logCaptureLn(manual ? String("网络已通过手动兜底注册")
                      : String("网络已自动注册"));
  releaseRecoveryOwnership();
}

void enterBackoff() {
  modemReady = false;
  backoffInitialized = false;
  setRecoveryState(BACKOFF);
  logCaptureLn(String("网络恢复失败，切回自动选网并退避10分钟"));
}

void beginScan() {
  candidateCount = 0;
  candidateIndex = 0;
  setRecoveryState(SCAN_START);
}

void beginFallback() {
  modemReady = false;
  if (shouldTryLastGood(cachedOperator)) {
    attemptedOperator = {};
    attemptedOperator.status = 2;
    strncpy(attemptedOperator.plmn, cachedOperator.plmn.c_str(),
            sizeof(attemptedOperator.plmn) - 1);
    attemptedOperator.act = cachedOperator.act;
    copyAttemptedPlmn(attemptedOperator.plmn);
    manualPhase = MANUAL_SEND_COMMAND;
    setRecoveryState(TRY_LAST_GOOD);
    logCaptureLn(String("自动注册失败，尝试SIM对应的last-good运营商: ") +
                 cachedOperator.plmn);
    return;
  }

  beginScan();
}

void beginVerification(bool manual) {
  verifyingManualRegistration = manual;
  verifyStep = 0;
  setRecoveryState(VERIFY_OPERATOR);
}

void finishManualFailure() {
  if (recoveryState == TRY_LAST_GOOD) {
    if (!currentIccid.isEmpty()) recordOperatorFailure(currentIccid);
    beginScan();
    return;
  }

  if (candidateIndex + 1 >= candidateCount) {
    enterBackoff();
    return;
  }
  candidateIndex++;
  attemptedOperator = candidates[candidateIndex];
  copyAttemptedPlmn(attemptedOperator.plmn);
  manualPhase = MANUAL_SEND_COMMAND;
  setRecoveryState(TRY_CANDIDATE);
}

void serviceManualAttempt(uint32_t timeout) {
  const uint32_t now = millis();
  if (transaction.completed) {
    if (transaction.purpose == TX_MANUAL_OPERATOR) {
      const bool success = transaction.success;
      consumeTransaction();
      if (!success) {
        finishManualFailure();
        return;
      }
      manualPhase = MANUAL_CHECK_REGISTRATION;
      nextActionAt = 0;
    } else if (transaction.purpose == TX_CEREG) {
      const RegState state = parseCeregState(transaction.response);
      consumeTransaction();
      if (isRegisteredState(state)) {
        beginVerification(true);
        return;
      }
      nextActionAt = now + REGISTRATION_POLL_MS;
    }
  }

  if (elapsed(now, stateStarted, timeout)) {
    if (!transaction.active && !transaction.completed) finishManualFailure();
    return;
  }

  if (transaction.active || transaction.completed) return;
  if (manualPhase == MANUAL_SEND_COMMAND) {
    String command = "AT+COPS=1,2,\"";
    command += attemptedOperator.plmn;
    command += "\",";
    command += String(attemptedOperator.act);
    startTransaction(TX_MANUAL_OPERATOR, command,
                     timeout - static_cast<uint32_t>(now - stateStarted));
    return;
  }

  if (nextActionAt == 0 || timeReached(now, nextActionAt)) {
    startTransaction(TX_CEREG, "AT+CEREG?", 3000);
  }
}

void serviceRegisteredState(bool manual) {
  const uint32_t now = millis();
  if (transaction.completed) {
    const RegState state = transaction.success
                               ? parseCeregState(transaction.response)
                               : REG_PARSE_UNKNOWN;
    consumeTransaction();
    if (isRegisteredState(state)) {
      modemReady = true;
      deniedHealthChecks = 0;
      registrationLostAt = 0;
      lastHealthCheck = now;
      releaseRecoveryOwnership();
      return;
    }

    modemReady = false;
    if (registrationLostAt == 0) registrationLostAt = now;
    if (state == REG_DENIED && deniedHealthChecks < UINT8_MAX) {
      deniedHealthChecks++;
    }

    if (manual) {
      if (registeredOperator.plmn[0] != '\0') {
        attemptedOperator = registeredOperator;
        copyAttemptedPlmn(attemptedOperator.plmn);
        manualPhase = MANUAL_SEND_COMMAND;
        setRecoveryState(TRY_LAST_GOOD);
      } else {
        beginScan();
      }
      return;
    }

    if (deniedHealthChecks >= MAX_DENIED_HEALTH_CHECKS ||
        elapsed(now, registrationLostAt, REGISTER_LOSS_TIMEOUT_MS)) {
      beginFallback();
      return;
    }

    lastHealthCheck = now;
    releaseRecoveryOwnership();
    return;
  }

  if (transaction.active || !elapsed(now, lastHealthCheck,
                                     HEALTH_CHECK_INTERVAL_MS)) {
    return;
  }
  if (!ensureRecoveryOwnership()) return;
  startTransaction(TX_CEREG, "AT+CEREG?", 3000);
}

}  // namespace

void beginNetworkRecovery() {
  resetNetworkRecovery();
  recoveryStarted = true;
  modemReady = false;
  cachedOperator = {};
  currentIccid = "";
  currentImsi = "";
  registeredOperator = {};
  scanPerformed = false;
  copyAttemptedPlmn("");
  setRecoveryState(WAIT_SIM);
  ensureRecoveryOwnership();
  logCaptureLn(String("已启动非阻塞网络注册流程"));
}

void serviceNetworkRecovery() {
  if (!recoveryStarted) return;
  serviceTransaction();
  const uint32_t now = millis();

  switch (recoveryState) {
    case WAIT_SIM:
      if (!ensureRecoveryOwnership()) return;
      if (transaction.completed) {
        const bool ready = transaction.success &&
                           strstr(transaction.response, "+CPIN: READY") != nullptr;
        consumeTransaction();
        if (ready) {
          setRecoveryState(READ_ICCID);
        } else {
          nextActionAt = now + 1000;
        }
      }
      if (elapsed(now, stateStarted, SIM_READY_TIMEOUT_MS)) {
        if (!transaction.active && !transaction.completed) enterBackoff();
        return;
      }
      if (!transaction.active && !transaction.completed &&
          (nextActionAt == 0 || timeReached(now, nextActionAt))) {
        startTransaction(TX_CPIN, "AT+CPIN?", 2000);
      }
      break;

    case READ_ICCID:
      if (transaction.completed) {
        if (transaction.success) {
          currentIccid = extractLongestDigitRun(transaction.response, 15);
          loadOperatorCache(currentIccid, &cachedOperator);
        }
        consumeTransaction();
        setRecoveryState(READ_IMSI);
      } else if (!transaction.active) {
        startTransaction(TX_ICCID, "AT+ICCID", 3000);
      }
      break;

    case READ_IMSI:
      if (transaction.completed) {
        if (transaction.success) {
          currentImsi = extractLongestDigitRun(transaction.response, 14);
        }
        consumeTransaction();
        setRecoveryState(START_AUTO);
      } else if (!transaction.active) {
        startTransaction(TX_IMSI, "AT+CIMI", 3000);
      }
      break;

    case START_AUTO:
      if (transaction.completed) {
        consumeTransaction();
        setRecoveryState(WAIT_AUTO);
        logCaptureLn(String("已切换自动选网，等待网络注册"));
      } else if (!transaction.active) {
        startTransaction(TX_START_AUTO, "AT+COPS=0", 15000);
      }
      break;

    case WAIT_AUTO:
      if (transaction.completed) {
        const RegState state = transaction.success
                                   ? parseCeregState(transaction.response)
                                   : REG_PARSE_UNKNOWN;
        consumeTransaction();
        if (isRegisteredState(state)) {
          beginVerification(false);
          return;
        }
        if (state == REG_DENIED) {
          logCaptureLn(String("自动选网被拒绝，继续等待窗口后执行兜底"));
        }
        nextActionAt = now + REGISTRATION_POLL_MS;
      }
      if (elapsed(now, stateStarted, AUTO_REGISTER_TIMEOUT_MS)) {
        if (!transaction.active && !transaction.completed) beginFallback();
        return;
      }
      if (!transaction.active && !transaction.completed &&
          (nextActionAt == 0 || timeReached(now, nextActionAt))) {
        startTransaction(TX_CEREG, "AT+CEREG?", 3000);
      }
      break;

    case TRY_LAST_GOOD:
      serviceManualAttempt(LAST_GOOD_TIMEOUT_MS);
      break;

    case SCAN_START:
      if (scanPerformed) {
        enterBackoff();
        return;
      }
      if (!transaction.active && !transaction.completed) {
        scanPerformed = true;
        if (startTransaction(TX_SCAN, "AT+COPS=?", SCAN_TIMEOUT_MS, true)) {
          setRecoveryState(SCAN_WAIT);
          logCaptureLn(String("last-good不可用，开始一次运营商扫描"));
        }
      }
      break;

    case SCAN_WAIT:
      if (transaction.completed) {
        const bool scanSucceeded = transaction.success;
        consumeTransaction();
        if (!scanSucceeded) {
          enterBackoff();
          return;
        }
        candidateCount = buildOrderedCandidates(
            scanParser, attemptedPlmn, candidates, MAX_CANDIDATE_ATTEMPTS);
        if (candidateCount == 0) {
          enterBackoff();
          return;
        }
        candidateIndex = 0;
        attemptedOperator = candidates[0];
        copyAttemptedPlmn(attemptedOperator.plmn);
        manualPhase = MANUAL_SEND_COMMAND;
        setRecoveryState(TRY_CANDIDATE);
      }
      break;

    case TRY_CANDIDATE:
      serviceManualAttempt(CANDIDATE_TIMEOUT_MS);
      break;

    case VERIFY_OPERATOR:
      if (transaction.completed) {
        if (transaction.purpose == TX_SET_NUMERIC_FORMAT) {
          consumeTransaction();
          verifyStep = 1;
        } else if (transaction.purpose == TX_CURRENT_OPERATOR) {
          OperatorCandidate parsed = {};
          const bool parsedCurrent =
              transaction.success &&
              parseCurrentOperator(transaction.response, &parsed);
          consumeTransaction();
          if (parsedCurrent) {
            registeredOperator = parsed;
          } else if (verifyingManualRegistration) {
            registeredOperator = attemptedOperator;
          } else {
            registeredOperator = {};
          }

          if (!currentIccid.isEmpty() && registeredOperator.plmn[0] != '\0') {
            recordOperatorSuccess(currentIccid, registeredOperator.plmn,
                                  registeredOperator.act, successTimestamp());
            cachedOperator = {};
            loadOperatorCache(currentIccid, &cachedOperator);
          }
          enterRegistered(verifyingManualRegistration);
          return;
        }
      }
      if (!transaction.active && !transaction.completed) {
        if (verifyStep == 0) {
          startTransaction(TX_SET_NUMERIC_FORMAT, "AT+COPS=3,2", 3000);
        } else {
          startTransaction(TX_CURRENT_OPERATOR, "AT+COPS?", 3000);
        }
      }
      break;

    case REGISTERED_AUTO:
      serviceRegisteredState(false);
      break;

    case REGISTERED_MANUAL:
      serviceRegisteredState(true);
      break;

    case BACKOFF:
      if (!backoffInitialized) {
        if (!ensureRecoveryOwnership()) return;
        if (transaction.completed) {
          if (transaction.purpose == TX_BACKOFF_AUTO) {
            consumeTransaction();
            backoffInitialized = true;
            backoffUntil = now + BACKOFF_MS;
            releaseRecoveryOwnership();
          } else {
            consumeTransaction();
          }
        } else if (!transaction.active) {
          startTransaction(TX_BACKOFF_AUTO, "AT+COPS=0", 15000);
        }
        return;
      }
      if (timeReached(now, backoffUntil)) {
        beginNetworkRecovery();
      }
      break;
  }
}

void resetNetworkRecovery() {
  if (recoveryOwnsIo) releaseRecoveryOwnership();
  resetTransaction();
  recoveryStarted = false;
  backoffInitialized = false;
}

bool networkRecoveryBusy() {
  return recoveryOwnsIo;
}
