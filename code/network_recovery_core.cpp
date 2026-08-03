#include "network_recovery_core.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char* skipSpaces(const char* value) {
  while (value != nullptr && (*value == ' ' || *value == '\t')) {
    ++value;
  }
  return value;
}

bool parseInteger(const char** cursor, long* result) {
  if (cursor == nullptr || *cursor == nullptr || result == nullptr) {
    return false;
  }

  const char* start = skipSpaces(*cursor);
  char* end = nullptr;
  const long value = strtol(start, &end, 10);
  if (end == start) {
    return false;
  }

  *cursor = end;
  *result = value;
  return true;
}

bool consume(const char** cursor, char expected) {
  if (cursor == nullptr || *cursor == nullptr) {
    return false;
  }

  const char* value = skipSpaces(*cursor);
  if (*value != expected) {
    return false;
  }

  *cursor = value + 1;
  return true;
}

bool parseQuotedField(const char** cursor, char* output, size_t capacity) {
  if (cursor == nullptr || *cursor == nullptr) {
    return false;
  }

  const char* value = skipSpaces(*cursor);
  if (*value != '"') {
    return false;
  }
  ++value;

  size_t length = 0;
  while (*value != '\0' && *value != '"') {
    if (output != nullptr && length + 1 < capacity) {
      output[length] = *value;
    }
    ++length;
    ++value;
  }
  if (*value != '"' || (output != nullptr && length >= capacity)) {
    return false;
  }

  if (output != nullptr) {
    output[length] = '\0';
  }
  *cursor = value + 1;
  return true;
}

bool isNumericPlmn(const char* plmn) {
  if (plmn == nullptr) {
    return false;
  }

  const size_t length = strlen(plmn);
  if (length < 5 || length > 6) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (!isdigit(static_cast<unsigned char>(plmn[index]))) {
      return false;
    }
  }
  return true;
}

bool parseOperatorTuple(const char* tuple, OperatorCandidate* result) {
  if (tuple == nullptr || result == nullptr) {
    return false;
  }

  const char* cursor = tuple;
  long status = -1;
  char plmn[sizeof(result->plmn)] = {};
  long act = -1;
  if (!parseInteger(&cursor, &status) || status < 0 || status > 3 ||
      !consume(&cursor, ',') || !parseQuotedField(&cursor, nullptr, 0) ||
      !consume(&cursor, ',') || !parseQuotedField(&cursor, nullptr, 0) ||
      !consume(&cursor, ',') ||
      !parseQuotedField(&cursor, plmn, sizeof(plmn)) ||
      !consume(&cursor, ',') || !parseInteger(&cursor, &act) || act < 0 ||
      act > 127 || !isNumericPlmn(plmn)) {
    return false;
  }

  result->status = static_cast<int8_t>(status);
  memcpy(result->plmn, plmn, sizeof(result->plmn));
  result->act = static_cast<int8_t>(act);
  return true;
}

int candidateRank(int8_t status) {
  switch (status) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 3:
      return 2;
    case 0:
      return 3;
    default:
      return 4;
  }
}

}  // namespace

RegState parseCeregState(const char* response) {
  if (response == nullptr) {
    return REG_PARSE_UNKNOWN;
  }

  const char* cursor = strstr(response, "+CEREG:");
  if (cursor == nullptr) {
    return REG_PARSE_UNKNOWN;
  }
  cursor += strlen("+CEREG:");

  long reportMode = 0;
  long state = 0;
  if (!parseInteger(&cursor, &reportMode) || !consume(&cursor, ',') ||
      !parseInteger(&cursor, &state)) {
    return REG_PARSE_UNKNOWN;
  }

  if (state < REG_NOT_REGISTERED || state > REG_ROAMING) {
    return REG_PARSE_UNKNOWN;
  }
  return static_cast<RegState>(state);
}

bool parseCurrentOperator(const char* response, OperatorCandidate* result) {
  if (response == nullptr || result == nullptr) {
    return false;
  }

  const char* cursor = strstr(response, "+COPS:");
  if (cursor == nullptr) {
    return false;
  }
  cursor += strlen("+COPS:");

  long mode = 0;
  long format = 0;
  if (!parseInteger(&cursor, &mode) || !consume(&cursor, ',') ||
      !parseInteger(&cursor, &format) || !consume(&cursor, ',')) {
    return false;
  }

  cursor = skipSpaces(cursor);
  if (*cursor != '"') {
    return false;
  }
  ++cursor;

  char plmn[sizeof(result->plmn)] = {};
  size_t length = 0;
  while (*cursor != '\0' && *cursor != '"') {
    if (!isdigit(static_cast<unsigned char>(*cursor)) ||
        length >= sizeof(plmn) - 1) {
      return false;
    }
    plmn[length++] = *cursor++;
  }
  if (*cursor != '"' || length < 5) {
    return false;
  }
  ++cursor;

  long act = -1;
  if (!consume(&cursor, ',') || !parseInteger(&cursor, &act) || act < 0 ||
      act > 127) {
    return false;
  }

  result->status = 2;
  memcpy(result->plmn, plmn, sizeof(result->plmn));
  result->act = static_cast<int8_t>(act);
  return true;
}

void resetCopsScanParser(CopsScanParser* parser) {
  if (parser == nullptr) {
    return;
  }
  memset(parser, 0, sizeof(*parser));
}

void feedCopsScanParser(CopsScanParser* parser, char value) {
  if (parser == nullptr) {
    return;
  }

  if (value == '(') {
    parser->inTuple = true;
    parser->tupleLength = 0;
    return;
  }
  if (!parser->inTuple) {
    return;
  }

  if (value != ')') {
    if (parser->tupleLength < sizeof(parser->tuple) - 1) {
      parser->tuple[parser->tupleLength++] = value;
    } else {
      parser->tupleLength = sizeof(parser->tuple);
    }
    return;
  }

  parser->inTuple = false;
  if (parser->tupleLength >= sizeof(parser->tuple) ||
      parser->count >= MAX_SCANNED_OPERATORS) {
    parser->tupleLength = 0;
    return;
  }

  parser->tuple[parser->tupleLength] = '\0';
  OperatorCandidate candidate = {};
  if (!parseOperatorTuple(parser->tuple, &candidate)) {
    parser->tupleLength = 0;
    return;
  }

  for (uint8_t index = 0; index < parser->count; ++index) {
    if (strcmp(parser->operators[index].plmn, candidate.plmn) == 0) {
      if (candidateRank(candidate.status) <
          candidateRank(parser->operators[index].status)) {
        parser->operators[index] = candidate;
      }
      parser->tupleLength = 0;
      return;
    }
  }

  parser->operators[parser->count++] = candidate;
  parser->tupleLength = 0;
}

uint8_t buildOrderedCandidates(const CopsScanParser& parser,
                               const char* attemptedPlmn,
                               OperatorCandidate* output,
                               uint8_t capacity) {
  if (output == nullptr || capacity == 0) {
    return 0;
  }

  uint8_t count = 0;
  for (int rank = 0; rank <= 4 && count < capacity; ++rank) {
    for (uint8_t index = 0; index < parser.count && count < capacity; ++index) {
      const OperatorCandidate& candidate = parser.operators[index];
      if (candidateRank(candidate.status) != rank) {
        continue;
      }
      if (attemptedPlmn != nullptr && attemptedPlmn[0] != '\0' &&
          strcmp(candidate.plmn, attemptedPlmn) == 0) {
        continue;
      }
      output[count++] = candidate;
    }
  }
  return count;
}

bool shouldTryLastGood(uint8_t failCount) {
  return failCount < 3;
}

uint8_t incrementFailureCount(uint8_t failCount) {
  return failCount == UINT8_MAX ? UINT8_MAX
                                : static_cast<uint8_t>(failCount + 1);
}
