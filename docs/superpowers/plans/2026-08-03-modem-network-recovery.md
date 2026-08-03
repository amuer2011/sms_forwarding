# ML307R 网络自动恢复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 ESP32-C3 + ML307R 固件增加按 ICCID 记忆 last-good PLMN、自动失败后有限扫描和手动兜底的非阻塞网络恢复能力，同时保持网页响应和短信 URC 完整性。

**Architecture:** 使用一个无 Arduino 依赖的纯 C++ 协议核心解析 `CEREG`、`COPS?` 和流式 `COPS=?`；固件侧通过固定大小缓存和 `Preferences` 保存四张 SIM 的成功运营商。`network_recovery` 在 `loop()` 中以非阻塞状态机推进，恢复事务独占 `Serial1`，网页模组操作在串口忙时返回 429，普通网页和 WiFi 服务继续运行。

**Tech Stack:** Arduino ESP32-C3、C++17 主机测试、ESP32 `Preferences`、Arduino `WebServer`、ML307R AT 命令、现有 `pdulib` 与 `ReadyMail`。

## Global Constraints

- 正常启动始终先使用 `AT+COPS=0`，兜底候选来自对应 SIM 的成功缓存或本轮扫描结果。
- SIM 身份优先使用 ICCID；ICCID 不可用时不得写持久化运营商缓存。
- 自动注册等待 120 秒；last-good 最长 180 秒；扫描最长 180 秒；扫描候选最多 4 个且每个最长 60 秒；全部失败退避 10 分钟。
- 手动兜底成功后，本次启动周期保持手动 PLMN；下一次冷启动重新从自动模式开始。
- `COPS=?` 只允许在无有效 last-good、last-good 已失败或 failCount 已达到 3 时执行，每轮最多一次。
- 不修改 `EF_FPLMN`，不自动反复切换 `CFUN`，不自动重启 ESP32。
- 不引入新第三方库，不增加大型网页页面，不完整缓存长 `COPS=?` 响应。
- 固定数组承载 4 条 SIM 缓存记录和最多 4 个待尝试扫描候选。
- 恢复事务运行时普通网页必须保持响应；需要模组串口的请求返回 HTTP 429。
- 每个实现任务完成后运行相关主机测试和固件编译，记录 Flash/RAM 用量变化。

---

### Task 1: 建立可主机测试的 AT 协议解析核心

**Files:**
- Create: `code/network_recovery_core.h`
- Create: `code/network_recovery_core.cpp`
- Create: `tests/network_recovery_core_test.cpp`

**Interfaces:**
- Produces: `RegState parseCeregState(const char* response)`
- Produces: `bool parseCurrentOperator(const char* response, OperatorCandidate* result)`
- Produces: `void resetCopsScanParser(CopsScanParser* parser)`
- Produces: `void feedCopsScanParser(CopsScanParser* parser, char value)`
- Produces: `uint8_t buildOrderedCandidates(const CopsScanParser&, const char* attemptedPlmn, OperatorCandidate* output, uint8_t capacity)`

- [ ] **Step 1: 写入 CEREG 与当前运营商解析的失败测试**

```cpp
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
}

static void testCurrentOperatorParsing() {
  OperatorCandidate result = {};
  assert(parseCurrentOperator("+COPS: 0,2,\"46001\",7\r\nOK\r\n", &result));
  assert(strcmp(result.plmn, "46001") == 0);
  assert(result.act == 7);
}
```

- [ ] **Step 2: 编译测试并确认因缺少接口而失败**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic code/network_recovery_core.cpp tests/network_recovery_core_test.cpp -o D:\tmp\network_recovery_core_test.exe
```

Expected: FAIL，提示 `network_recovery_core.h` 或声明的解析函数不存在。

- [ ] **Step 3: 定义纯 C++ 数据类型和解析接口**

```cpp
#ifndef NETWORK_RECOVERY_CORE_H
#define NETWORK_RECOVERY_CORE_H

#include <stddef.h>
#include <stdint.h>

enum RegState : int8_t {
  REG_PARSE_UNKNOWN = -1,
  REG_NOT_REGISTERED = 0,
  REG_HOME = 1,
  REG_SEARCHING = 2,
  REG_DENIED = 3,
  REG_NETWORK_UNKNOWN = 4,
  REG_ROAMING = 5
};

struct OperatorCandidate {
  int8_t status;
  char plmn[7];
  int8_t act;
};

constexpr uint8_t MAX_SCANNED_OPERATORS = 12;

struct CopsScanParser {
  bool inTuple;
  uint8_t tupleLength;
  char tuple[96];
  uint8_t count;
  OperatorCandidate operators[MAX_SCANNED_OPERATORS];
};

RegState parseCeregState(const char* response);
bool parseCurrentOperator(const char* response, OperatorCandidate* result);
void resetCopsScanParser(CopsScanParser* parser);
void feedCopsScanParser(CopsScanParser* parser, char value);
uint8_t buildOrderedCandidates(const CopsScanParser& parser,
                               const char* attemptedPlmn,
                               OperatorCandidate* output,
                               uint8_t capacity);

#endif
```

- [ ] **Step 4: 实现结构化 CEREG 与 COPS? 解析**

实现要求：定位响应前缀，逐字段读取整数和带引号 PLMN；没有第二个 `CEREG` 字段时返回 `REG_PARSE_UNKNOWN`；不得使用全响应 `indexOf(",5")` 式匹配。

- [ ] **Step 5: 运行测试确认基础解析通过**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic code/network_recovery_core.cpp tests/network_recovery_core_test.cpp -o D:\tmp\network_recovery_core_test.exe
& D:\tmp\network_recovery_core_test.exe
```

Expected: exit code `0`。

- [ ] **Step 6: 增加流式 COPS 扫描和排序失败测试**

使用本次真实响应：

```cpp
static void testCopsScanOrdering() {
  const char* response =
      "+COPS: (1,\"CHN-CT\",\"CT\",\"46011\",7),"
      "(3,\"CHN-UNICOM\",\"UNICOM\",\"46001\",7),"
      "(3,\"\",\"\",\"46015\",7),"
      "(1,\"CHINA MOBILE\",\"CMCC\",\"46000\",7),,"
      "(0,1,2,3,4),(0,1,2)\r\nOK\r\n";
  CopsScanParser parser;
  resetCopsScanParser(&parser);
  for (const char* p = response; *p; ++p) feedCopsScanParser(&parser, *p);

  OperatorCandidate ordered[4] = {};
  uint8_t count = buildOrderedCandidates(parser, "", ordered, 4);
  assert(count == 4);
  assert(strcmp(ordered[0].plmn, "46011") == 0);
  assert(strcmp(ordered[1].plmn, "46000") == 0);
  assert(strcmp(ordered[2].plmn, "46001") == 0);
  assert(strcmp(ordered[3].plmn, "46015") == 0);
}
```

- [ ] **Step 7: 实现流式元组解析、去重和排序**

只接受包含三个引号字符串和数字 PLMN 的运营商元组，忽略 `(0,1,2,3,4)` 等能力元组。排序为 `stat=1`、`stat=2`、`stat=3`、`stat=0`，并排除本轮已尝试 PLMN。

- [ ] **Step 8: 运行全部主机测试并提交**

Run: 上述 `g++` 编译和测试命令。

Expected: exit code `0`，无编译警告。

Commit:

```powershell
git add code/network_recovery_core.h code/network_recovery_core.cpp tests/network_recovery_core_test.cpp
git commit -m "test: add modem registration protocol core"
```

---

### Task 2: 增加按 ICCID 保存的运营商缓存

**Files:**
- Create: `code/operator_cache.h`
- Create: `code/operator_cache.cpp`
- Modify: `tests/network_recovery_core_test.cpp`

**Interfaces:**
- Produces: `bool loadOperatorCache(const String& iccid, OperatorCacheRecord* result)`
- Produces: `void recordOperatorSuccess(const String& iccid, const char* plmn, int8_t act, uint32_t timestamp)`
- Produces: `void recordOperatorFailure(const String& iccid)`
- Produces: `bool shouldTryLastGood(const OperatorCacheRecord& record)`

- [ ] **Step 1: 增加 failCount 判定的失败测试**

在纯核心头中增加：

```cpp
bool shouldTryLastGood(uint8_t failCount);
uint8_t incrementFailureCount(uint8_t failCount);
```

测试：

```cpp
assert(shouldTryLastGood(0));
assert(shouldTryLastGood(2));
assert(!shouldTryLastGood(3));
assert(incrementFailureCount(2) == 3);
assert(incrementFailureCount(255) == 255);
```

- [ ] **Step 2: 运行测试确认新接口缺失**

Run: Task 1 的主机测试命令。

Expected: FAIL，提示 `shouldTryLastGood` 或 `incrementFailureCount` 未定义。

- [ ] **Step 3: 实现失败计数纯逻辑并使测试通过**

```cpp
bool shouldTryLastGood(uint8_t failCount) { return failCount < 3; }
uint8_t incrementFailureCount(uint8_t failCount) {
  return failCount == 255 ? 255 : static_cast<uint8_t>(failCount + 1);
}
```

- [ ] **Step 4: 实现四槽 Preferences 缓存**

```cpp
struct OperatorCacheRecord {
  bool valid;
  String iccid;
  String plmn;
  int8_t act;
  uint32_t lastSuccessTime;
  uint8_t failCount;
};
```

使用独立命名空间 `plmn_cache`，键名为 `i0..i3`、`p0..p3`、`a0..a3`、`t0..t3`、`f0..f3` 和 `next`。更新已有 ICCID 时保留槽位；新 ICCID 使用 `next` 并按模 4 轮转。ICCID 为空时所有写操作立即返回。

- [ ] **Step 5: 运行主机测试和 Arduino 编译**

Run: 主机测试命令。

Run firmware build when Arduino CLI is available:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32c3 .\code
```

Expected: 主机测试通过；固件编译通过，记录 Flash/RAM 数值。若本机 CLI 不可用，记录该限制并在最终 CI 等价编译阶段执行。

- [ ] **Step 6: 提交缓存实现**

```powershell
git add code/network_recovery_core.cpp code/operator_cache.h code/operator_cache.cpp tests/network_recovery_core_test.cpp
git commit -m "feat: cache last successful operator per sim"
```

---

### Task 3: 提取短信 URC 行处理并增加串口所有权

**Files:**
- Modify: `code/sms_process.h`
- Modify: `code/sms_process.cpp:229-249,404-510`
- Modify: `code/modem.h`
- Modify: `code/modem.cpp:4-28,132-147,170-240`
- Modify: `code/web_handlers.cpp:297-326,328-560,675-850,978-1085`

**Interfaces:**
- Produces: `bool processModemUrcLine(const String& line)`
- Produces: `bool acquireModemIo(ModemIoOwner owner)`
- Produces: `void releaseModemIo(ModemIoOwner owner)`
- Produces: `bool modemIoBusy()`

- [ ] **Step 1: 将短信 URC 状态从读取函数中分离**

`checkSerial1URC()` 只负责读取一行；原有 `IDLE/WAIT_PDU` 状态和 PDU 处理移动到：

```cpp
bool processModemUrcLine(const String& line);
```

返回 `true` 表示该行是 `+CMT:`、等待中的 PDU 或已消费的短信相关行；普通 `OK`、`ERROR` 和 AT 响应返回 `false`。

- [ ] **Step 2: 增加固定串口所有权枚举**

```cpp
enum ModemIoOwner : uint8_t {
  MODEM_IO_NONE = 0,
  MODEM_IO_SYNC = 1,
  MODEM_IO_RECOVERY = 2,
  MODEM_IO_SMS = 3
};
```

`acquireModemIo()` 只允许从 `NONE` 获取；释放时必须匹配 owner。`sendATCommand()`、`sendATandWaitOK()` 和 `sendSMS()` 获取相应所有权，失败时记录忙碌并立即返回失败。

- [ ] **Step 3: 防止通用读取器与恢复事务争用串口**

`checkSerial1URC()` 在 `modemIoBusy()` 时立即返回。同步命令读取到完整行时先调用 `processModemUrcLine()`；已消费 URC 不得拼入命令响应。

- [ ] **Step 4: 网页模组操作增加 429 忙碌响应**

在 `handleATCommand()`、`handleQuery()`、`handlePing()`、`handleFlightMode()` 和 `handleModem()` 开始处检查：

```cpp
if (modemIoBusy()) {
  server.send(429, "application/json",
              "{\"success\":false,\"message\":\"模组正在恢复网络，请稍后重试\"}");
  return;
}
```

- [ ] **Step 5: 编译固件并提交**

Run: 主机测试；随后运行 Arduino CLI 编译。

Expected: 测试和编译通过；网页非模组路由没有行为变化。

Commit:

```powershell
git add code/sms_process.h code/sms_process.cpp code/modem.h code/modem.cpp code/web_handlers.cpp
git commit -m "refactor: serialize modem io and route sms urcs"
```

---

### Task 4: 实现非阻塞恢复事务和状态机

**Files:**
- Create: `code/network_recovery.h`
- Create: `code/network_recovery.cpp`
- Modify: `code/modem.h`
- Modify: `code/modem.cpp:50-123,149-168`

**Interfaces:**
- Consumes: Task 1 解析接口、Task 2 缓存接口、Task 3 串口所有权和 URC 行入口。
- Produces: `void beginNetworkRecovery()`
- Produces: `void serviceNetworkRecovery()`
- Produces: `void resetNetworkRecovery()`
- Produces: `bool networkRecoveryBusy()`

- [ ] **Step 1: 定义状态、事务和时间常量**

```cpp
enum RecoveryState : uint8_t {
  WAIT_SIM,
  READ_ICCID,
  READ_IMSI,
  START_AUTO,
  WAIT_AUTO,
  TRY_LAST_GOOD,
  SCAN_START,
  SCAN_WAIT,
  TRY_CANDIDATE,
  VERIFY_OPERATOR,
  REGISTERED_AUTO,
  REGISTERED_MANUAL,
  BACKOFF
};

constexpr uint32_t SIM_READY_TIMEOUT_MS = 30000;
constexpr uint32_t AUTO_REGISTER_TIMEOUT_MS = 120000;
constexpr uint32_t LAST_GOOD_TIMEOUT_MS = 180000;
constexpr uint32_t SCAN_TIMEOUT_MS = 180000;
constexpr uint32_t CANDIDATE_TIMEOUT_MS = 60000;
constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 30000;
constexpr uint32_t BACKOFF_MS = 600000;
constexpr uint8_t MAX_CANDIDATE_ATTEMPTS = 4;
```

- [ ] **Step 2: 实现一次只处理有限字节的异步 AT 事务**

事务结构至少保存 owner、命令用途、开始时间、截止时间、固定行缓冲、短响应缓冲和最终结果。`serviceNetworkRecovery()` 每轮最多读取 64 字节后返回。完整行等于 `OK` 或 `ERROR` 时结束事务；`+CMT:` 和等待中的 PDU 转发给 `processModemUrcLine()`；扫描字符直接喂给 `CopsScanParser`，不构造完整扫描 `String`。

- [ ] **Step 3: 实现启动自动注册路径**

`beginNetworkRecovery()` 从 `WAIT_SIM` 开始：等待 `CPIN READY`，读取 ICCID/IMSI，加载缓存，执行 `AT+COPS=0`，在总计 120 秒窗口中查询 `CEREG`。成功后查询数字 `COPS?`、保存缓存并进入 `REGISTERED_AUTO`。

- [ ] **Step 4: 实现 last-good 路径**

自动失败后，如果缓存有效且 `failCount < 3`，发送：

```cpp
AT+COPS=1,2,"<plmn>",<act>
```

等待最长 180 秒并查询 `CEREG`。成功则保存并进入 `REGISTERED_MANUAL`；确认失败才调用 `recordOperatorFailure()`。

- [ ] **Step 5: 实现扫描和候选路径**

无 last-good 或 last-good 失败后，每轮只发送一次 `AT+COPS=?`。使用 Task 1 流式解析器生成最多四个有序候选，每个候选手动尝试 60 秒。成功进入 `REGISTERED_MANUAL`；全部失败执行 `AT+COPS=0` 后进入 `BACKOFF`。

- [ ] **Step 6: 实现运行时健康检查**

自动注册状态每 30 秒查询 `CEREG`；连续三次 `DENIED` 或 120 秒无法恢复时重新走 last-good。手动注册状态掉线时先重试当前 PLMN 一次，失败后进入扫描或退避。`modemReady` 只在确认 `HOME/ROAMING` 时为 `true`。

- [ ] **Step 7: 将 modemInit 缩短为基础初始化**

保留 AT 握手、型号判断、`CGACT=0,1`、`CNMI` 和 `CMGF`。删除原有 30 次阻塞 `waitCEREG()` 循环，结尾调用 `beginNetworkRecovery()` 并立即返回。

- [ ] **Step 8: 编译、检查资源并提交**

Run: 主机测试和 Arduino CLI 编译。

Expected: 编译通过；记录与 1,261,557 字节历史基线相比的 Flash 增量。若当前分区编译失败，先压缩日志和重复代码，不在本任务中修改分区表。

Commit:

```powershell
git add code/network_recovery.h code/network_recovery.cpp code/modem.h code/modem.cpp
git commit -m "feat: add nonblocking modem network recovery"
```

---

### Task 5: 接入主循环和网页状态

**Files:**
- Modify: `code/code.ino:107-124`
- Modify: `code/web_handlers.cpp:100-110,488-528`
- Modify: `dev_doc/api_reference.md`
- Modify: `dev_doc/module_details.md`
- Modify: `dev_doc/architecture.md`

**Interfaces:**
- Consumes: `serviceNetworkRecovery()`、`networkRecoveryBusy()`、结构化 `parseCeregState()`。

- [ ] **Step 1: 在主循环推进恢复状态机**

```cpp
void loop() {
  server.handleClient();
  serviceWifiHealth();
  serviceNetworkRecovery();
  // 保留现有配置、长短信、离线队列和串口桥接逻辑。
  checkSerial1URC();
}
```

`serviceNetworkRecovery()` 必须位于常规 URC 读取之前，使串口所有权决定由谁读取数据。

- [ ] **Step 2: 网页网络状态复用结构化解析器**

将 `handleQuery(type=network)` 的单字符截取替换为 `parseCeregState(resp.c_str())`，显示解析失败、本地注册、搜索、拒绝、网络未知和漫游注册的不同文本。

- [ ] **Step 3: 更新开发文档**

记录：启动不再阻塞等待注册、缓存命名空间 `plmn_cache`、恢复时间参数、手动兜底本次启动保持、网页模组请求的 429 行为和硬件验证日志关键字。

- [ ] **Step 4: 编译并提交集成**

Run: 主机测试和 Arduino CLI 编译。

Expected: 所有测试通过；固件编译通过；普通网页路由不依赖模组串口。

Commit:

```powershell
git add code/code.ino code/web_handlers.cpp dev_doc/api_reference.md dev_doc/module_details.md dev_doc/architecture.md
git commit -m "feat: integrate modem recovery with runtime and web"
```

---

### Task 6: 完整验证与资源门禁

**Files:**
- Modify if needed: `.github/workflows/build.yml`
- Modify: `.github/agent/编译烧录实测.md`

**Interfaces:**
- Verifies: 所有前置任务交付物。

- [ ] **Step 1: 运行完整主机测试**

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic code/network_recovery_core.cpp tests/network_recovery_core_test.cpp -o D:\tmp\network_recovery_core_test.exe
& D:\tmp\network_recovery_core_test.exe
```

Expected: exit code `0`，无警告。

- [ ] **Step 2: 运行两个 FQBN 的固件编译**

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32c3 .\code
arduino-cli compile --fqbn esp32:esp32:makergo_c3_supermini .\code
```

Expected: 两次编译成功。记录程序存储和全局 RAM 用量；若剩余 Flash 少于 20 KB，先减少重复日志和未使用代码，再重新编译。

- [ ] **Step 3: 执行静态检查**

```powershell
git diff --check
rg -n "delay\((60000|120000|180000)|while.*COPS" code
```

Expected: 无空白错误；不存在分钟级阻塞 delay 或循环扫描。

- [ ] **Step 4: 进行硬件验证**

依次验证：正常自动注册、已知 ICCID last-good、无缓存扫描、forbidden 候选成功、全部失败退避、恢复期间网页响应、恢复成功后短信到达、冷启动重新自动选网、换卡不复用缓存。每个场景保存设备日志和 `COPS?/CEREG?` 结果。

- [ ] **Step 5: 更新实测文档并提交**

将实际编译用量、验证过的板型、关键日志和仍需硬件验证的项目写入 `.github/agent/编译烧录实测.md`。

```powershell
git add .github/workflows/build.yml .github/agent/编译烧录实测.md
git commit -m "docs: record modem recovery verification"
```

- [ ] **Step 6: 最终回归检查**

Run: 主机测试、两个 FQBN 编译、`git diff --check`、`git status --short`。

Expected: 测试和编译通过，工作区只包含预期提交，硬件未执行的项目被明确列出而不是声称通过。
