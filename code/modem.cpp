#include "modem.h"
#include "network_recovery.h"
#include "sms_process.h"
#include "web_handlers.h"

namespace {

ModemIoOwner currentModemIoOwner = MODEM_IO_NONE;

bool isFinalResponseLine(const String& line) {
  return line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR") ||
         line.startsWith("+CMS ERROR");
}

bool isSuccessfulResponse(const String& response) {
  return response.endsWith("OK\r\n");
}

}  // namespace

bool acquireModemIo(ModemIoOwner owner) {
  return acquireModemIoOwner(&currentModemIoOwner, owner);
}

void releaseModemIo(ModemIoOwner owner) {
  if (!releaseModemIoOwner(&currentModemIoOwner, owner)) {
    logCaptureLn(String("模组串口所有权释放不匹配"));
  }
}

bool modemIoBusy() {
  return currentModemIoOwner != MODEM_IO_NONE;
}

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  if (!acquireModemIo(MODEM_IO_SYNC)) {
    logCaptureLn(String("模组串口忙，无法发送: ") + cmd);
    return "";
  }

  Serial1.println(cmd);

  unsigned long start = millis();
  String resp = "";
  resp.reserve(256);
  while (millis() - start < timeout) {
    String line = readSerialLine(Serial1);
    if (line.length() == 0) {
      delay(1);
      continue;
    }

    if (processModemUrcLine(line)) {
      continue;
    }

    line.trim();
    if (line.length() == 0) continue;
    resp += line;
    resp += "\r\n";
    if (isFinalResponseLine(line)) break;
  }

  releaseModemIo(MODEM_IO_SYNC);
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
  resetNetworkRecovery();
  modemReady = false;

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
    unsigned int lineStart = 0;
    int lineNum = 0;
    for (unsigned int i = 0; i < resp.length(); i++) {
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
  beginNetworkRecovery();
}

void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  return isSuccessfulResponse(sendATCommand(cmd, timeout));
}

// 检测网络注册状态（LTE/4G）
// CEREG状态: 1=已注册本地, 5=已注册漫游
bool waitCEREG() {
  const RegState state = parseCeregState(sendATCommand("AT+CEREG?", 2000).c_str());
  return state == REG_HOME || state == REG_ROAMING;
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  if (!acquireModemIo(MODEM_IO_SMS)) {
    logCaptureLn(String("模组串口忙，暂时无法发送短信"));
    return false;
  }

  logCaptureLn(String("准备发送短信..."));
  logCapture(String("目标号码: ")); logCaptureLn(String(phoneNumber));
  logCapture(String("短信内容: ")); logCaptureLn(String(message));

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    logCapture(String("PDU编码失败，错误码: "));
    logCaptureLn(String(pduLen));
    releaseModemIo(MODEM_IO_SMS);
    return false;
  }
  
  logCapture(String("PDU数据: ")); logCaptureLn(String(pdu.getSMS()));
  logCapture(String("PDU长度: ")); logCaptureLn(String(pduLen));
  
  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;
  
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
      String line;
      if (feedModemInputChar(c, &line) && line.length() > 0) {
        processModemUrcLine(line);
      }
    }
    delay(1);
  }
  
  if (!gotPrompt) {
    logCaptureLn(String("未收到>提示符"));
    releaseModemIo(MODEM_IO_SMS);
    return false;
  }
  
  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束
  
  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    String line = readSerialLine(Serial1);
    if (line.length() == 0) {
      delay(1);
      continue;
    }

    if (processModemUrcLine(line)) continue;
    line.trim();
    if (line.length() == 0) continue;
    resp += line;
    resp += "\r\n";
    logCaptureLn(line);
    if (line == "OK") {
      logCaptureLn(String("短信发送成功"));
      releaseModemIo(MODEM_IO_SMS);
      return true;
    }
    if (line == "ERROR" || line.startsWith("+CMS ERROR")) {
      logCaptureLn(String("短信发送失败"));
      releaseModemIo(MODEM_IO_SMS);
      return false;
    }
  }
  logCaptureLn(String("短信发送超时"));
  releaseModemIo(MODEM_IO_SMS);
  return false;
}
