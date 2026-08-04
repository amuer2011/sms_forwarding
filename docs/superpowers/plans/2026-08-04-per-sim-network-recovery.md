# 按 SIM 卡恢复网络 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在自动注册失败后，优先恢复当前 SIM 的 last-good PLMN；无记录或失败时才扫描并逐个尝试网络。

**Architecture:** 将响应解析和缓存淘汰规则放入不依赖 Arduino 的小型 C++ 模块，以 `g++` 运行断言测试。`modem.cpp` 保留 NVS、AT 命令和恢复编排，使用固定长度的 20 项缓存，避免引入 JSON、动态持久化格式或第三方库。

**Tech Stack:** Arduino ESP32 Core 3.3.10、ESP32 Preferences NVS、C++17 `g++`、Arduino CLI。

## Global Constraints

- 不修改网页、短信、推送或既有 `sms_config` 配置格式。
- 使用 NVS namespace `sim_network` 和 key `records` 保存固定长度缓存。
- ICCID 可读取时只按 ICCID 匹配；ICCID 缺失时才按 IMSI 匹配。
- 仅 `CEREG=1/5` 是成功；不使用 `COPS=?` 的可用/禁止状态筛除候选。
- 自动注册限时 120 秒，last-good 与每个扫描候选各限时 45 秒。
- 扫描最多尝试 8 个不重复的 5/6 位数字 PLMN；所有候选失败后执行 `AT+COPS=0`。

---

### Task 1: 响应解析的主机测试与实现

**Files:**
- Create: `code/network_recovery.h`
- Create: `code/network_recovery.cpp`
- Create: `tests/network_recovery_test.cpp`

**Interfaces:**
- Produces: `bool isCeregRegistered(const char* response)`。
- Produces: `uint8_t extractCopsCandidates(const char* response, char candidates[][SIM_NETWORK_PLMN_LENGTH], uint8_t capacity)`。
- Produces: `bool extractCurrentCops(const char* response, char plmn[SIM_NETWORK_PLMN_LENGTH], uint8_t* act)`。

- [x] **Step 1: 写入失败的解析测试**

创建 `tests/network_recovery_test.cpp`：

```cpp
#include "../code/network_recovery.h"
#include <assert.h>
#include <string.h>

int main() {
  assert(isCeregRegistered("+CEREG: 0,1\r\nOK\r\n"));
  assert(isCeregRegistered("+CEREG: 0,5\r\nOK\r\n"));
  assert(!isCeregRegistered("+CEREG: 0,3\r\nOK\r\n"));
  const char* scan = "+COPS: (1,\"CHN-CT\",\"CT\",\"46011\",7),(3,\"CHN-UNICOM\",\"UNICOM\",\"46001\",7),(3,\"\",\"\",\"46015\",7),(1,\"CHINA MOBILE\",\"CMCC\",\"46000\",7),,(0,1,2,3,4),(0,1,2)";
  char candidates[8][SIM_NETWORK_PLMN_LENGTH] = {};
  assert(extractCopsCandidates(scan, candidates, 8) == 4);
  assert(strcmp(candidates[0], "46011") == 0);
  assert(strcmp(candidates[1], "46001") == 0);
  assert(strcmp(candidates[2], "46015") == 0);
  assert(strcmp(candidates[3], "46000") == 0);
  char plmn[SIM_NETWORK_PLMN_LENGTH] = {};
  uint8_t act = 0;
  assert(extractCurrentCops("+COPS: 1,2,\"46001\",7\r\nOK\r\n", plmn, &act));
  assert(strcmp(plmn, "46001") == 0 && act == 7);
}
```

- [x] **Step 2: 确认测试在接口未实现时失败**

运行 `New-Item -ItemType Directory -Force -Path build | Out-Null`，随后运行：

```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests\network_recovery_test.cpp code\network_recovery.cpp -o build\network_recovery_test.exe
```

预期：因头文件或函数缺失而编译失败。

- [x] **Step 3: 写入最小解析实现**

在 `code/network_recovery.h` 定义：

```cpp
#include <stdint.h>

constexpr uint8_t MAX_SIM_NETWORK_RECORDS = 20;
constexpr uint8_t MAX_COPS_CANDIDATES = 8;
constexpr uint8_t SIM_NETWORK_ICCID_LENGTH = 24;
constexpr uint8_t SIM_NETWORK_IMSI_LENGTH = 17;
constexpr uint8_t SIM_NETWORK_PLMN_LENGTH = 7;

bool isCeregRegistered(const char* response);
uint8_t extractCopsCandidates(const char* response, char candidates[][SIM_NETWORK_PLMN_LENGTH], uint8_t capacity);
bool extractCurrentCops(const char* response, char plmn[SIM_NETWORK_PLMN_LENGTH], uint8_t* act);
```

在 `code/network_recovery.cpp` 只使用标准 C 字符串函数：`isCeregRegistered` 找到 `+CEREG:` 后只接受状态 1/5；扫描解析按引号内的纯数字 5/6 位 token 去重，不读取 tuple 的第一个状态数；当前运营商解析读取数字 PLMN 和紧随其后的接入技术。

- [x] **Step 4: 确认解析测试通过**

```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests\network_recovery_test.cpp code\network_recovery.cpp -o build\network_recovery_test.exe
.\build\network_recovery_test.exe
```

预期：退出码为 0，且 `46001` 即使在扫描结果中标记为禁止，也会被列为候选。

- [x] **Step 5: 提交任务**

```powershell
git add code/network_recovery.h code/network_recovery.cpp tests/network_recovery_test.cpp
git commit -m "feat: parse modem network recovery responses"
```

### Task 2: 每卡缓存匹配与淘汰规则

**Files:**
- Modify: `code/network_recovery.h`
- Modify: `code/network_recovery.cpp`
- Modify: `tests/network_recovery_test.cpp`

**Interfaces:**
- Produces: `SimNetworkRecord`，字段为固定长度 `iccid`、`imsi`、`plmn`、`act`、`lastSuccessTime`。
- Produces: `int findSimNetworkRecord(const SimNetworkRecord records[], const char* iccid, const char* imsi)`。
- Produces: `uint8_t selectSimNetworkRecordSlot(const SimNetworkRecord records[])`。

- [x] **Step 1: 增加失败的缓存规则测试**

在现有 `main()` 末尾加入：

```cpp
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
```

- [x] **Step 2: 确认缓存测试在接口未实现时失败**

```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests\network_recovery_test.cpp code\network_recovery.cpp -o build\network_recovery_test.exe
```

预期：因 `SimNetworkRecord` 或缓存函数未定义而失败。

- [x] **Step 3: 实现固定大小缓存接口**

在头文件添加：

```cpp
struct SimNetworkRecord {
  char iccid[SIM_NETWORK_ICCID_LENGTH];
  char imsi[SIM_NETWORK_IMSI_LENGTH];
  char plmn[SIM_NETWORK_PLMN_LENGTH];
  uint8_t act;
  uint32_t lastSuccessTime;
};

int findSimNetworkRecord(const SimNetworkRecord records[], const char* iccid, const char* imsi);
uint8_t selectSimNetworkRecordSlot(const SimNetworkRecord records[]);
```

实现要求：仅 ICCID 与 IMSI 均为空时才识别为空槽；ICCID 非空时只比较 ICCID，ICCID 为空时才比较 IMSI；选槽先返回空槽，满额时返回最小 `lastSuccessTime` 的索引。

- [x] **Step 4: 确认完整主机测试通过**

```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests\network_recovery_test.cpp code\network_recovery.cpp -o build\network_recovery_test.exe
.\build\network_recovery_test.exe
```

预期：退出码为 0，验证 ICCID 优先、IMSI 兜底、空槽优先与第 21 张卡淘汰最早记录。

- [x] **Step 5: 提交任务**

```powershell
git add code/network_recovery.h code/network_recovery.cpp tests/network_recovery_test.cpp
git commit -m "feat: add per-sim network cache selection"
```

### Task 3: NVS 持久化与模组恢复编排

**Files:**
- Modify: `code/modem.cpp`
- Modify: `code/modem.h`
- Modify: `dev_doc/module_details.md`

**Interfaces:**
- Consumes: Task 1 的响应解析函数和 Task 2 的缓存类型/选择函数。
- Produces: `bool waitForCeregSuccess(unsigned long timeoutMs)`，只在时限内得到 `CEREG=1/5` 时返回真。
- Produces: `bool recoverNetwork(const String& iccid, const String& imsi)`，按 last-good 后扫描候选的顺序恢复。

- [x] **Step 1: 先写入手工验收表**

在 `dev_doc/module_details.md` 的网络注册说明中添加：

```markdown
| 初始条件 | 期望 AT 顺序 | 成功判定 |
|---|---|---|
| 自动注册成功 | `CEREG?` | `CEREG=1/5` 后 `COPS?` 保存 |
| 自动失败且命中 last-good | `COPS=1,2,"last-good",7` → `CEREG?` | `CEREG=1/5` |
| 无记录或 last-good 失败 | `COPS=?` → 逐个 `COPS=1,2,"PLMN",7` → `CEREG?` | 首个 `CEREG=1/5` |
| 所有候选失败 | `COPS=0` | `modemReady=false` |
```

- [ ] **Step 2: 在设备上确认当前固件不具备恢复流程**

烧录基线固件并重启模组。预期日志只轮询 `AT+CEREG?`，不会读取 `AT+CCID`、`AT+CIMI`，也不会在注册超时后尝试 `AT+COPS`。保留此日志作为集成前对照。

- [x] **Step 3: 实现 NVS、身份读取和恢复流程**

在 `modem.cpp` 包含 `network_recovery.h`，私有实现 NVS 结构与函数：

```cpp
namespace {
constexpr uint32_t SIM_NETWORK_CACHE_MAGIC = 0x534E4331UL;
constexpr const char* SIM_NETWORK_NAMESPACE = "sim_network";
constexpr const char* SIM_NETWORK_CACHE_KEY = "records";

struct SimNetworkCache {
  uint32_t magic;
  SimNetworkRecord records[MAX_SIM_NETWORK_RECORDS];
};
}

bool loadSimNetworkCache(SimNetworkCache& cache);
void saveSimNetworkCache(const SimNetworkCache& cache);
String queryIccid();
String queryImsi();
bool saveCurrentNetwork(const String& iccid, const String& imsi);
bool waitForCeregSuccess(unsigned long timeoutMs);
bool recoverNetwork(const String& iccid, const String& imsi);
```

NVS 每次读写使用局部 `Preferences` 并立刻 `end()`；读取长度、magic 或数据不正确时清空缓存。`saveCurrentNetwork` 先发送 `AT+COPS=3,2`，再查询 `AT+COPS?`，只在成功解析数字 PLMN 且身份存在时更新记录；时间戳使用 `time(nullptr)`。`recoverNetwork` 仅尝试一次命中的 last-good，之后执行一次 `AT+COPS=?`（180 秒超时），对最多 8 个候选逐一发送 `AT+COPS=1,2,"PLMN",7` 并等待 45 秒，跳过已尝试过的 last-good；全部失败后发送 `AT+COPS=0`。

将 `modemInit()` 中现有 30 次 `waitCEREG()` 循环替换为：读取 ICCID/IMSI，`waitForCeregSuccess(120000)` 自动注册；成功则保存当前网络，失败则 `recoverNetwork`。仅这两条路径之一成功时设置 `modemReady=true`。

- [ ] **Step 4: 编译 ESP32-C3 草图**

```powershell
$env:Path = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources;$env:Path"
$env:ARDUINO_DIRECTORIES_DATA = "D:\dev\arduino_pack"
$env:ARDUINO_DIRECTORIES_USER = "D:\dev\arduino_pack\user"
arduino-cli compile --fqbn esp32:esp32:makergo_c3_supermini --build-path "D:\dev\arduino_pack\build" "D:\CHANG\project\sms_forwarding\code"
```

预期：退出码为 0，输出包含 `Sketch uses`；记录 flash 使用量并确认未超出分区上限。

- [ ] **Step 5: 在真实模组验收四条路径**

验证自动成功、last-good 成功、扫描候选成功和全部失败四种场景。每次成功先用 `AT+CEREG?` 确认 1 或 5，再用 `AT+COPS?` 确认实际 PLMN；全部失败时确认日志包含 `AT+COPS=0` 且模组保持未就绪。

- [ ] **Step 6: 提交任务**

```powershell
git add code/modem.cpp code/modem.h dev_doc/module_details.md
git commit -m "feat: recover network by per-sim last-good plmn"
```

### Task 4: 最终回归与边界检查

**Files:**
- Verify: `code/network_recovery.cpp`
- Verify: `code/modem.cpp`
- Verify: `tests/network_recovery_test.cpp`

**Interfaces:**
- Consumes: Tasks 1-3 的解析、缓存、NVS 和恢复逻辑。
- Produces: 主机测试、目标板编译和真实模组验收的证据。

- [x] **Step 1: 执行主机回归测试**

```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests\network_recovery_test.cpp code\network_recovery.cpp -o build\network_recovery_test.exe
.\build\network_recovery_test.exe
```

预期：退出码为 0。

- [ ] **Step 2: 重新执行目标板编译**

运行 Task 3 Step 4 的 `arduino-cli compile` 命令。预期退出码为 0，没有新的警告或链接错误。

- [x] **Step 3: 检查改动范围与空白问题**

运行 `git diff --check` 和 `git status --short`。

预期：源码改动仅限计划中的解析模块、测试、`modem` 集成与文档；不修改未跟踪的 `.worktrees/`，不提交 `build/` 产物。
