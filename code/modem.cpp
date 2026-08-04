#include "modem.h"
#include "web_handlers.h"
#include "network_recovery.h"

#include <string.h>
#include <time.h>

namespace {

constexpr uint32_t SIM_NETWORK_CACHE_MAGIC = 0x534E4331UL;
constexpr const char* SIM_NETWORK_NAMESPACE = "sim_network";
constexpr const char* SIM_NETWORK_CACHE_KEY = "records";
constexpr unsigned long AUTO_REGISTER_TIMEOUT_MS = 120000;
constexpr unsigned long MANUAL_REGISTER_TIMEOUT_MS = 45000;
constexpr unsigned long COPS_SCAN_TIMEOUT_MS = 180000;

struct SimNetworkCache {
  uint32_t magic;
  SimNetworkRecord records[MAX_SIM_NETWORK_RECORDS];
};

void resetSimNetworkCache(SimNetworkCache& cache) {
  memset(&cache, 0, sizeof(cache));
  cache.magic = SIM_NETWORK_CACHE_MAGIC;
}

bool loadSimNetworkCache(SimNetworkCache& cache) {
  resetSimNetworkCache(cache);
  Preferences storage;
  if (!storage.begin(SIM_NETWORK_NAMESPACE, true)) return false;

  const bool valid = storage.getBytesLength(SIM_NETWORK_CACHE_KEY) == sizeof(cache) &&
                     storage.getBytes(SIM_NETWORK_CACHE_KEY, &cache, sizeof(cache)) == sizeof(cache) &&
                     cache.magic == SIM_NETWORK_CACHE_MAGIC;
  storage.end();
  if (!valid) resetSimNetworkCache(cache);
  return valid;
}

bool saveSimNetworkCache(const SimNetworkCache& cache) {
  Preferences storage;
  if (!storage.begin(SIM_NETWORK_NAMESPACE, false)) {
    logCaptureLn(String("⚠️ last-good缓存写入失败"));
    return false;
  }
  const bool saved = storage.putBytes(SIM_NETWORK_CACHE_KEY, &cache, sizeof(cache)) == sizeof(cache);
  storage.end();
  if (!saved) logCaptureLn(String("⚠️ last-good缓存写入不完整"));
  return saved;
}

void copyIdentity(char* target, size_t capacity, const String& source) {
  if (capacity == 0) return;
  size_t length = source.length();
  if (length >= capacity) length = capacity - 1;
  memcpy(target, source.c_str(), length);
  target[length] = '\0';
}

String extractLongestDigits(const String& response, uint8_t minimumLength, uint8_t maximumLength) {
  int bestStart = -1;
  int bestLength = 0;
  int runStart = -1;
  for (int i = 0; i <= response.length(); ++i) {
    const bool digit = i < response.length() &&
                       response.charAt(i) >= '0' && response.charAt(i) <= '9';
    if (digit && runStart < 0) runStart = i;
    if (!digit && runStart >= 0) {
      const int runLength = i - runStart;
      if (runLength >= minimumLength && runLength <= maximumLength && runLength > bestLength) {
        bestStart = runStart;
        bestLength = runLength;
      }
      runStart = -1;
    }
  }
  return bestStart >= 0 ? response.substring(bestStart, bestStart + bestLength) : "";
}

String queryIccid() {
  return extractLongestDigits(sendATCommand("AT+ICCID", 3000), 15, 22);
}

String queryImsi() {
  return extractLongestDigits(sendATCommand("AT+CIMI", 3000), 15, 15);
}

void waitWithWebServer(unsigned long duration) {
  const unsigned long start = millis();
  while (millis() - start < duration) {
    server.handleClient();
    delay(10);
  }
}

bool waitForCeregSuccess(unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (waitCEREG()) return true;
    waitWithWebServer(1000);
  }
  return false;
}

bool saveCurrentNetwork(const String& iccid, const String& imsi) {
  if (iccid.length() == 0 && imsi.length() == 0) return false;

  if (!sendATandWaitOK("AT+COPS=3,2", 3000)) {
    logCaptureLn(String("⚠️ 无法切换数字运营商格式，未保存last-good"));
    return false;
  }

  const String response = sendATCommand("AT+COPS?", 3000);
  char plmn[SIM_NETWORK_PLMN_LENGTH] = {};
  uint8_t act = 0;
  if (!extractCurrentCops(response.c_str(), plmn, &act)) {
    logCaptureLn(String("⚠️ 未获取到当前数字运营商，未保存last-good"));
    return false;
  }

  SimNetworkCache cache;
  loadSimNetworkCache(cache);
  int slot = findSimNetworkRecord(cache.records, iccid.c_str(), imsi.c_str());
  if (slot < 0) slot = selectSimNetworkRecordSlot(cache.records);

  SimNetworkRecord& record = cache.records[slot];
  memset(&record, 0, sizeof(record));
  copyIdentity(record.iccid, sizeof(record.iccid), iccid);
  copyIdentity(record.imsi, sizeof(record.imsi), imsi);
  memcpy(record.plmn, plmn, sizeof(record.plmn));
  record.act = act;
  const time_t now = time(nullptr);
  record.lastSuccessTime = now > 0 ? static_cast<uint32_t>(now) : millis() / 1000;
  if (!saveSimNetworkCache(cache)) return false;
  logCaptureLn(String("已保存SIM last-good运营商: ") + String(plmn));
  return true;
}

bool tryManualNetwork(const char* plmn, const String& iccid, const String& imsi) {
  String command = "AT+COPS=1,2,\"";
  command += plmn;
  command += "\",7";
  logCaptureLn(String("尝试运营商: ") + String(plmn));
  if (!sendATandWaitOK(command.c_str(), 10000)) {
    logCaptureLn(String("运营商选择命令未确认，继续等待注册状态: ") + String(plmn));
  }
  if (!waitForCeregSuccess(MANUAL_REGISTER_TIMEOUT_MS)) {
    logCaptureLn(String("运营商注册失败: ") + String(plmn));
    return false;
  }
  saveCurrentNetwork(iccid, imsi);
  return true;
}

bool recoverNetwork(const String& iccid, const String& imsi) {
  SimNetworkCache cache;
  loadSimNetworkCache(cache);
  const int cachedSlot = findSimNetworkRecord(cache.records, iccid.c_str(), imsi.c_str());
  const char* lastGood = cachedSlot >= 0 ? cache.records[cachedSlot].plmn : "";

  if (lastGood[0] != '\0') {
    logCaptureLn(String("自动注册失败，尝试SIM对应的last-good运营商: ") + String(lastGood));
    if (tryManualNetwork(lastGood, iccid, imsi)) return true;
  } else {
    logCaptureLn(String("自动注册失败，当前SIM没有last-good记录"));
  }

  logCaptureLn(String("开始扫描运营商候选..."));
  const String scanResponse = sendATCommand("AT+COPS=?", COPS_SCAN_TIMEOUT_MS);
  char candidates[MAX_COPS_CANDIDATES][SIM_NETWORK_PLMN_LENGTH] = {};
  const uint8_t count = extractCopsCandidates(scanResponse.c_str(), candidates, MAX_COPS_CANDIDATES);
  for (uint8_t i = 0; i < count; ++i) {
    if (lastGood[0] != '\0' && strcmp(candidates[i], lastGood) == 0) continue;
    if (tryManualNetwork(candidates[i], iccid, imsi)) return true;
  }

  logCaptureLn(String("⚠️ 所有候选运营商注册失败，恢复自动选网"));
  sendATandWaitOK("AT+COPS=0", 10000);
  return false;
}

}  // namespace

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        // 读取剩余数据（最多 50ms）
        unsigned long t = millis();
        while (millis() - t < 50) {
          if (Serial1.available()) resp += (char)Serial1.read();
          server.handleClient();
        }
        return resp;
      }
    }
    server.handleClient();
  }
  return resp;
}

// 新增"模组断电重启"函数
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);

  logCaptureLn(String("EN 拉低：关闭模组"));
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1200);  // 关机时间给够

  logCaptureLn(String("EN 拉高：开启模组"));
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(6000);  // 等模组完全启动再发AT（关键）
}

// 重启模组（EN引脚断电重启 + 重新初始化）
void resetModule() {
  logCaptureLn(String("正在硬重启模组（EN 断电重启）..."));
  modemPowerCycle();
  modemInit();
}

// 模组 AT 初始化流程（setup 中调用，resetModule 后也调用）
void modemInit() {
  // 清掉上电噪声/残留
  while (Serial1.available()) Serial1.read();

  while (!sendATandWaitOK("AT", 1000)) {
    logCaptureLn(String("AT未响应，重试..."));
    blink_short();
  }
  logCaptureLn(String("模组AT响应正常"));

  //判断型号，做一些特定操作
  bool need_set_CGACT = true;
  String resp = sendATCommand("ATI", 2000);
  logCaptureLn(String("ATI响应: " + resp));
  if (resp.indexOf("OK") >= 0) {
    // 解析ATI响应
    String manufacturer = "未知";
    String model = "未知";
    String version = "未知";
    
    // 按行解析
    int lineStart = 0;
    int lineNum = 0;
    for (int i = 0; i < resp.length(); i++) {
      if (resp.charAt(i) == '\n' || i == resp.length() - 1) {
        String line = resp.substring(lineStart, i);
        line.trim();
        if (line.length() > 0 && line != "ATI" && line != "OK") {
          lineNum++;
          if (lineNum == 1) manufacturer = line;
          else if (lineNum == 2) model = line;
          else if (lineNum == 3) version = line;
        }
        lineStart = i + 1;
      }
    }
    //这个模组这条命令有bug
    if(model == "ML307Y") need_set_CGACT = false;
  }

  if(need_set_CGACT) {
    while (!sendATandWaitOK("AT+CGACT=0,1", 5000)) {
      logCaptureLn(String("设置CGACT失败，重试..."));
      blink_short();
    }
    logCaptureLn(String("已禁用数据连接(AT+CGACT=0,1)，防止流量消耗"));
  } else {
    logCaptureLn(String("该型号无法配置(AT+CGACT=0,1)，跳过该命令，会不会消耗流量？自求多福"));
  }
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) {
    logCaptureLn(String("设置CNMI失败，重试..."));
    blink_short();
  }
  logCaptureLn(String("CNMI参数设置完成"));
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) {
    logCaptureLn(String("设置PDU模式失败，重试..."));
    blink_short();
  }
  logCaptureLn(String("PDU模式设置完成"));

  const String iccid = queryIccid();
  const String imsi = queryImsi();
  if (iccid.length() > 0) logCaptureLn(String("已读取SIM ICCID"));
  else logCaptureLn(String("⚠️ 未读取到SIM ICCID，将使用IMSI兜底"));
  if (imsi.length() == 0) logCaptureLn(String("⚠️ 未读取到SIM IMSI"));

  logCaptureLn(String("开始自动选网，最长等待120秒..."));
  if (!sendATandWaitOK("AT+COPS=0", 10000)) {
    logCaptureLn(String("⚠️ 自动选网命令未确认，继续检查注册状态"));
  }
  bool registered = waitForCeregSuccess(AUTO_REGISTER_TIMEOUT_MS);
  if (registered) {
    saveCurrentNetwork(iccid, imsi);
  } else {
    registered = recoverNetwork(iccid, imsi);
  }

  if (registered) {
    logCaptureLn(String("网络已注册"));
    modemReady = true;
  } else {
    logCaptureLn(String("⚠️ 网络注册失败，模组功能不可用"));
    modemReady = false;
  }
}

void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
    server.handleClient();
  }
  return false;
}

// 检测网络注册状态（LTE/4G）
// CEREG状态: 1=已注册本地, 5=已注册漫游
bool waitCEREG() {
  const String response = sendATCommand("AT+CEREG?", 2000);
  return isCeregRegistered(response.c_str());
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  logCaptureLn(String("准备发送短信..."));
  logCapture(String("目标号码: ")); logCaptureLn(String(phoneNumber));
  logCapture(String("短信内容: ")); logCaptureLn(String(message));

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    logCapture(String("PDU编码失败，错误码: "));
    logCaptureLn(String(pduLen));
    return false;
  }
  
  logCapture(String("PDU数据: ")); logCaptureLn(String(pdu.getSMS()));
  logCapture(String("PDU长度: ")); logCaptureLn(String(pduLen));
  
  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;
  
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmgsCmd);
  
  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      logCapture(String(c));
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    server.handleClient();
  }
  
  if (!gotPrompt) {
    logCaptureLn(String("未收到>提示符"));
    return false;
  }
  
  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束
  
  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      logCapture(String(c));
      if (resp.indexOf("OK") >= 0) {
        logCaptureLn(String("\n短信发送成功"));
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        logCaptureLn(String("\n短信发送失败"));
        return false;
      }
    }
    server.handleClient();
  }
  logCaptureLn(String("短信发送超时"));
  return false;
}
