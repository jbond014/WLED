#include "wled.h"

namespace {
constexpr unsigned long WS_CLIENT_HEARTBEAT_INTERVAL_MS = 30000UL;
constexpr unsigned long WS_CLIENT_PONG_TIMEOUT_MS = 15000UL;
constexpr unsigned long WS_CLIENT_RECONNECT_BASE_MS = 5000UL;
constexpr unsigned long WS_CLIENT_RECONNECT_MAX_MS = 300000UL;
constexpr uint8_t WS_CLIENT_RECONNECT_SHIFT_CAP = 6;
constexpr uint8_t WS_CLIENT_RECONNECT_ATTEMPT_CAP = 16;

bool wsClientStarted = false;

unsigned long wsClientReconnectDelay()
{
  uint8_t shift = min(wsClientReconnectAttempts, WS_CLIENT_RECONNECT_SHIFT_CAP);
  unsigned long delayMs = WS_CLIENT_RECONNECT_BASE_MS;
  while (shift-- > 0 && delayMs < WS_CLIENT_RECONNECT_MAX_MS) {
    if (delayMs > WS_CLIENT_RECONNECT_MAX_MS / 2) {
      delayMs = WS_CLIENT_RECONNECT_MAX_MS;
      break;
    }
    delayMs *= 2;
  }
  return delayMs;
}

void scheduleWsClientReconnect(bool resetBackoff = false)
{
  if (resetBackoff) {
    wsClientReconnectAttempts = 0;
    wsClientNextReconnectAttempt = 0;
  } else {
    if (wsClientReconnectAttempts < WS_CLIENT_RECONNECT_ATTEMPT_CAP) wsClientReconnectAttempts++;
    unsigned long delayMs = wsClientReconnectDelay();
    wsClientNextReconnectAttempt = millis() + delayMs;
    DEBUG_PRINTF_P(PSTR("External WS reconnect in %lus.\n"), delayMs / 1000);
  }
  lastWsClientReconnectAttempt = millis();
}

void disconnectWsClient()
{
  wsClientConnected = false;
  wsClientStarted = false;
  if (wsClient != nullptr) wsClient->disconnect();
}

bool sendDataWsClient()
{
  if (wsClient == nullptr || !wsClientConnected) return false;
  if (!requestJSONBufferLock(JSON_LOCK_WS_SEND)) return false;

  JsonObject state = pDoc->createNestedObject("state");
  serializeState(state);
  JsonObject info  = pDoc->createNestedObject("info");
  serializeInfo(info);

  size_t payloadLen = measureJson(*pDoc);
  std::vector<char> payload(payloadLen + 1, '\0');
  serializeJson(*pDoc, payload.data(), payload.size());
  releaseJSONBufferLock();
  return wsClient->sendTXT(payload.data(), payloadLen);
}

void wsClientEvent(WStype_t type, uint8_t *payload, size_t length)
{
  switch (type) {
    case WStype_CONNECTED:
      wsClientConnected = true;
      wsClientStarted = true;
      scheduleWsClientReconnect(true);
      DEBUG_PRINTLN(F("External WS connected."));
      break;
    case WStype_DISCONNECTED:
      if (wsClientConnected || wsClientStarted) DEBUG_PRINTLN(F("External WS disconnected."));
      wsClientConnected = false;
      wsClientStarted = false;
      if (isWsClientConfigured() && WLED_CONNECTED) scheduleWsClientReconnect();
      break;
    case WStype_ERROR:
      DEBUG_PRINTLN(F("External WS error."));
      wsClientConnected = false;
      wsClientStarted = false;
      if (isWsClientConfigured() && WLED_CONNECTED) scheduleWsClientReconnect();
      break;
    case WStype_PONG:
      DEBUG_PRINTLN(F("External WS pong."));
      break;
    case WStype_TEXT: {
      if (!payload || !length) break;
      if (!requestJSONBufferLock(JSON_LOCK_WS_RECEIVE)) break;
      DeserializationError error = deserializeJson(*pDoc, payload, length);
      JsonObject root = pDoc->as<JsonObject>();
      bool sendState = false;
      if (!error && !root.isNull()) {
        if (root["v"] && root.size() == 1) {
          sendState = true;
        } else {
          JsonObject state = root[F("state")];
          if (state.isNull()) state = root;
          deserializeState(state, CALL_MODE_WS_SEND);
        }
      }
      releaseJSONBufferLock();
      if (sendState) sendDataWsClient();
      break;
    }
    default:
      break;
  }
}
} // namespace

bool isWsClientConfigured()
{
  return wsClientEnabled && wsClientHost[0] != '\0';
}

void normalizeWsClientPath()
{
  if (!wsClientPath[0]) strlcpy(wsClientPath, "/", sizeof(wsClientPath));
  if (wsClientPath[0] == '/') return;
  char tmp[sizeof(wsClientPath)];
  strlcpy(tmp, wsClientPath, sizeof(tmp));
  wsClientPath[0] = '/';
  wsClientPath[1] = '\0';
  strlcat(wsClientPath, tmp, sizeof(wsClientPath));
}

void initWsClient(bool forceReconnect)
{
  if (!isWsClientConfigured()) {
    disconnectWsClient();
    delete wsClient;
    wsClient = nullptr;
    scheduleWsClientReconnect(true);
    return;
  }

  if (!WLED_CONNECTED) {
    disconnectWsClient();
    return;
  }

  if (forceReconnect && wsClient != nullptr) {
    disconnectWsClient();
    delete wsClient;
    wsClient = nullptr;
    scheduleWsClientReconnect(true);
  }

  if (wsClient == nullptr) {
    wsClient = new WebSocketsClient();
    if (wsClient == nullptr) return;
    wsClient->onEvent(wsClientEvent);
    wsClient->enableHeartbeat(WS_CLIENT_HEARTBEAT_INTERVAL_MS, WS_CLIENT_PONG_TIMEOUT_MS, 2);
  }

  if (wsClientConnected || wsClientStarted) return;

  DEBUG_PRINTF_P(PSTR("Connecting external WS to %s:%u%s\n"), wsClientHost, wsClientPort, wsClientPath);
  wsClient->begin(wsClientHost, wsClientPort, wsClientPath);
  wsClientStarted = true;
  lastWsClientReconnectAttempt = millis();
  wsClientNextReconnectAttempt = millis() + wsClientReconnectDelay();
}

void handleWsClient()
{
  const unsigned long now = millis();

  // millis() wraps roughly every 49 days, so reset the reconnect schedule on rollover.
  if (lastWsClientReconnectAttempt > now) {
    scheduleWsClientReconnect(true);
  }

  if (wsClient != nullptr) wsClient->loop();

  if (!isWsClientConfigured()) {
    disconnectWsClient();
    delete wsClient;
    wsClient = nullptr;
    return;
  }

  if (!WLED_CONNECTED) {
    disconnectWsClient();
    return;
  }

  if (wsClientConnected || wsClientStarted) return;

  if (wsClientNextReconnectAttempt == 0 || now >= wsClientNextReconnectAttempt) {
    initWsClient();
  }
}
