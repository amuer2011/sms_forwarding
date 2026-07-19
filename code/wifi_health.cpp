#include "wifi_health.h"
#include "globals.h"
#include "web_handlers.h"
#include "wifi_config.h"

namespace {
constexpr unsigned long WIFI_RECONNECT_DELAY_MS = 5000;
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr unsigned long WIFI_REINIT_DELAY_MS = 45000;
constexpr unsigned long WIFI_REINIT_INTERVAL_MS = 60000;
constexpr unsigned long WIFI_RESTART_DELAY_MS = 300000;
constexpr uint8_t WIFI_RECONNECT_ATTEMPTS = 3;

volatile bool disconnectEventPending = false;
volatile bool gotIpEventPending = false;
volatile uint8_t lastDisconnectReason = 0;

unsigned long disconnectedSince = 0;
unsigned long lastRecoveryAction = 0;
uint8_t reconnectAttempts = 0;
bool wifiReinitialized = false;

void handleWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    disconnectEventPending = true;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    gotIpEventPending = true;
  }
}

void startWifiConnection() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}
}  // namespace

void initWifiHealth() {
  WiFi.onEvent(handleWifiEvent);
}

void serviceWifiHealth() {
  unsigned long now = millis();

  if (disconnectEventPending) {
    uint8_t reason = lastDisconnectReason;
    disconnectEventPending = false;
    logCaptureF("WiFi断开，原因码: %u\n", reason);
  }

  if (gotIpEventPending) {
    gotIpEventPending = false;
    logCaptureLn(String("WiFi获得IP: ") + WiFi.localIP().toString());
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (disconnectedSince != 0) {
      logCaptureF("WiFi已恢复，离线时长: %lu秒\n", (now - disconnectedSince) / 1000UL);
    }
    disconnectedSince = 0;
    lastRecoveryAction = 0;
    reconnectAttempts = 0;
    wifiReinitialized = false;
    return;
  }

  if (disconnectedSince == 0) {
    disconnectedSince = now == 0 ? 1 : now;
    lastRecoveryAction = now;
    reconnectAttempts = 0;
    wifiReinitialized = false;
    return;
  }

  unsigned long offlineTime = now - disconnectedSince;
  if (offlineTime >= WIFI_RESTART_DELAY_MS) {
    logCaptureLn(String("WiFi持续离线5分钟，重启ESP32恢复"));
    delay(100);
    ESP.restart();
  }

  if (offlineTime >= WIFI_REINIT_DELAY_MS &&
      (!wifiReinitialized || now - lastRecoveryAction >= WIFI_REINIT_INTERVAL_MS)) {
    logCaptureLn(String("WiFi持续离线，重新初始化连接"));
    WiFi.disconnect(false, false, 200);
    startWifiConnection();
    lastRecoveryAction = now;
    reconnectAttempts = 0;
    wifiReinitialized = true;
    return;
  }

  if (offlineTime >= WIFI_RECONNECT_DELAY_MS && reconnectAttempts < WIFI_RECONNECT_ATTEMPTS &&
      now - lastRecoveryAction >= WIFI_RECONNECT_INTERVAL_MS) {
    reconnectAttempts++;
    lastRecoveryAction = now;
    logCaptureF("WiFi自动重连尝试: %u/%u\n", reconnectAttempts, WIFI_RECONNECT_ATTEMPTS);
    WiFi.reconnect();
  }
}
