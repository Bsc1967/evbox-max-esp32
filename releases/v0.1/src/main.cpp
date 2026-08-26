#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

namespace Pins {
constexpr int Relay1 = 1;
constexpr int Relay2 = 2;
constexpr int Relay3 = 41;
constexpr int Relay4 = 42;
constexpr int Relay5 = 45;
constexpr int Relay6 = 46;
constexpr int Rs485Tx = 17;
constexpr int Rs485Rx = 18;
constexpr int Buzzer = 21;
}

constexpr uint8_t ADDR_NEW = 0x00;
constexpr uint8_t ADDR_CP = 0x80;
constexpr uint8_t ADDR_BROADCAST = 0xBC;
constexpr uint16_t ACK = 0xAA00;
constexpr uint8_t SOF = 0x02;
constexpr uint8_t EOF_BYTE = 0x03;
constexpr uint8_t TRAILER_BYTE = 0xFE;
constexpr uint16_t JANITZA_LIVE_START = 1317;
constexpr uint16_t JANITZA_LIVE_COUNT = 124;

enum class Mode : uint8_t { Disabled, Manual, Load, Pv };
enum class EvState : uint8_t { Init, WaitRegistration, AssignAddress, Available, Connected, Ready, Charging, PvWait, Fault };

struct Config {
  String ssid;
  String password;
  String janitzaHost = "192.168.1.30";
  uint16_t janitzaPort = 502;
  uint8_t janitzaUnit = 1;
  int16_t janitzaOffset = 0;
  Mode mode = Mode::Pv;
  float manualCurrent = 6.0f;
  float maxCurrent = 16.0f;
  float breakerCurrent = 16.0f;
  float mainFuseCurrent = 25.0f;
  float phaseDetectCurrent = 5.0f;
  uint16_t janitzaTimeoutSeconds = 10;
  float janitzaFallbackCurrent = 6.0f;
};

struct JanitzaValues {
  bool valid = false;
  bool online = false;
  float voltage[3] = {};
  float current[3] = {};
  float activePowerTotal = 0.0f;
  float importPower = 0.0f;
  float exportPower = 0.0f;
  float frequency = 0.0f;
  uint32_t lastReadMs = 0;
  String lastError;
};

struct EvboxStatus {
  uint8_t cbAddress = 0;
  String serial;
  uint16_t firmware = 0;
  uint16_t hardware = 0;
  EvState state = EvState::Init;
  uint8_t cbStateCode = 0;
  float phaseCurrent[3] = {};
  float lastLimit = NAN;
  float desiredCurrent = 0.0f;
  String desiredReason = "opstart";
  uint32_t session = 1;
  uint32_t lastFrameMs = 0;
  uint32_t lastCbFrameMs = 0;
  uint32_t lastCurrentSentMs = 0;
  uint32_t pvWaitSinceMs = 0;
  bool chargeActive = false;
};

Config cfg;
JanitzaValues janitza;
EvboxStatus evbox;
Preferences prefs;
WebServer server(80);
HardwareSerial EvSerial(1);
String logLines[60];
uint8_t logHead = 0;
uint8_t logCount = 0;
uint8_t rxBuf[220];
size_t rxLen = 0;
uint16_t modbusTransaction = 0;
uint32_t nextJanitzaPollMs = 0;
uint32_t nextControlMs = 0;
uint32_t nextStatusPollMs = 0;

String hexByte(uint8_t value) {
  char out[3];
  snprintf(out, sizeof(out), "%02X", value);
  return String(out);
}

String hexWord(uint16_t value) {
  char out[5];
  snprintf(out, sizeof(out), "%04X", value);
  return String(out);
}

String hexDword(uint32_t value) {
  char out[9];
  snprintf(out, sizeof(out), "%08lX", static_cast<unsigned long>(value));
  return String(out);
}

uint8_t parseHexByte(const String &text, size_t index) {
  return static_cast<uint8_t>(strtoul(text.substring(index, index + 2).c_str(), nullptr, 16));
}

uint32_t parseHexUInt(const String &text, size_t index, size_t count) {
  return strtoul(text.substring(index, index + count).c_str(), nullptr, 16);
}

void addLog(const String &line) {
  String row = String(millis() / 1000) + "s " + line;
  logLines[logHead] = row;
  logHead = (logHead + 1) % 60;
  if (logCount < 60) logCount++;
  Serial.println(row);
}

const char *modeName(Mode mode) {
  switch (mode) {
    case Mode::Disabled: return "disabled";
    case Mode::Manual: return "manual";
    case Mode::Load: return "load";
    case Mode::Pv: return "pv";
  }
  return "unknown";
}

Mode parseMode(const String &value) {
  if (value == "disabled") return Mode::Disabled;
  if (value == "manual") return Mode::Manual;
  if (value == "load") return Mode::Load;
  return Mode::Pv;
}

const char *stateName(EvState state) {
  switch (state) {
    case EvState::Init: return "INIT";
    case EvState::WaitRegistration: return "WAIT_REGISTRATION";
    case EvState::AssignAddress: return "ASSIGN_ADDRESS";
    case EvState::Available: return "AVAILABLE";
    case EvState::Connected: return "CONNECTED";
    case EvState::Ready: return "READY";
    case EvState::Charging: return "CHARGING";
    case EvState::PvWait: return "PV_WAIT";
    case EvState::Fault: return "FAULT";
  }
  return "UNKNOWN";
}

const char *cbStateName(uint8_t value) {
  switch (value) {
    case 0x02: return "available";
    case 0x0A: return "error";
    case 0x17: return "in use";
    case 0x47: return "preparing";
    case 0x48: return "charging";
    case 0x4A: return "ready";
    case 0x4B: return "finished";
  }
  return "unknown";
}

void setState(EvState state, const String &reason) {
  if (evbox.state != state) addLog(String("state ") + stateName(evbox.state) + " > " + stateName(state) + " (" + reason + ")");
  evbox.state = state;
}

uint32_t secondsSince2000() {
  return (millis() / 1000UL) + 840000000UL;
}

bool janitzaFresh() {
  if (!janitza.valid || janitza.lastReadMs == 0) return false;
  return millis() - janitza.lastReadMs <= static_cast<uint32_t>(cfg.janitzaTimeoutSeconds) * 1000UL;
}

float janitzaAgeSeconds() {
  if (!janitza.lastReadMs) return NAN;
  return (millis() - janitza.lastReadMs) / 1000.0f;
}

float clampCurrent(float amps) {
  if (!isfinite(amps)) return 0.0f;
  return constrain(amps, 0.0f, 32.0f);
}

uint16_t deciAmp(float amps) {
  return static_cast<uint16_t>(roundf(clampCurrent(amps) * 10.0f));
}

uint8_t checksum(const String &payload) {
  uint8_t sum = 0;
  for (size_t i = 0; i < payload.length(); i++) sum += static_cast<uint8_t>(payload[i]);
  return sum;
}

uint8_t parity(const String &payload) {
  uint8_t x = 0;
  for (size_t i = 0; i < payload.length(); i++) x ^= static_cast<uint8_t>(payload[i]);
  return x;
}

void sendPacket(uint8_t src, uint8_t dst, uint8_t cmd, const String &data, const String &reason) {
  String payload = hexByte(dst) + hexByte(src) + hexByte(cmd) + data;
  String frameText = payload + hexByte(checksum(payload)) + hexByte(parity(payload));
  EvSerial.write(SOF);
  EvSerial.print(frameText);
  EvSerial.write(EOF_BYTE);
  EvSerial.flush();
  addLog(String("TX ") + hexByte(src) + "->" + hexByte(dst) + " cmd " + hexByte(cmd) + " " + reason + " data=" + (data.length() ? data : "-"));
}

void sendCurrentLimit(float amps) {
  if (!evbox.cbAddress) return;
  float current = clampCurrent(amps);
  String value = hexWord(deciAmp(current));
  sendPacket(ADDR_CP, evbox.cbAddress, 0x6B, String("01") + hexWord(60) + value + value + value, String("current limit ") + String(current, 1) + "A");
  evbox.lastLimit = current;
  evbox.lastCurrentSentMs = millis();
}

void sendRestartRegistration() {
  sendPacket(ADDR_CP, ADDR_BROADCAST, 0x1E, "", "restart registration");
  setState(EvState::WaitRegistration, "restart registration");
}

void sendConnectionState() {
  if (evbox.cbAddress) sendPacket(ADDR_CP, evbox.cbAddress, 0x1B, "0000038400", "connection state");
}

void sendStatusUpdateRequest() {
  if (evbox.cbAddress) sendPacket(ADDR_CP, evbox.cbAddress, 0x18, "02", "request state update");
}

float regFloat(const uint16_t *regs, uint16_t address) {
  uint16_t index = address - JANITZA_LIVE_START;
  uint32_t raw = (static_cast<uint32_t>(regs[index]) << 16) | regs[index + 1];
  float value;
  memcpy(&value, &raw, sizeof(value));
  return isfinite(value) ? value : NAN;
}

bool readExact(WiFiClient &client, uint8_t *buffer, size_t count, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < count && millis() - start < timeoutMs) {
    int available = client.available();
    if (available > 0) got += client.read(buffer + got, count - got);
    delay(1);
  }
  return got == count;
}

bool readJanitza() {
  if (WiFi.status() != WL_CONNECTED) {
    janitza.online = false;
    janitza.lastError = "WiFi niet verbonden";
    return false;
  }

  WiFiClient client;
  client.setTimeout(2500);
  if (!client.connect(cfg.janitzaHost.c_str(), cfg.janitzaPort)) {
    janitza.online = false;
    janitza.lastError = "Modbus TCP connect fout";
    return false;
  }

  modbusTransaction++;
  uint8_t request[12] = {
    static_cast<uint8_t>(modbusTransaction >> 8), static_cast<uint8_t>(modbusTransaction),
    0x00, 0x00, 0x00, 0x06, cfg.janitzaUnit, 0x03,
    static_cast<uint8_t>((JANITZA_LIVE_START + cfg.janitzaOffset) >> 8),
    static_cast<uint8_t>(JANITZA_LIVE_START + cfg.janitzaOffset),
    static_cast<uint8_t>(JANITZA_LIVE_COUNT >> 8), static_cast<uint8_t>(JANITZA_LIVE_COUNT)
  };
  client.write(request, sizeof(request));

  uint8_t header[7];
  if (!readExact(client, header, sizeof(header), 3000)) {
    janitza.online = false;
    janitza.lastError = "Modbus header timeout";
    return false;
  }
  uint16_t responseId = (header[0] << 8) | header[1];
  uint16_t length = (header[4] << 8) | header[5];
  if (responseId != modbusTransaction || header[2] || header[3] || header[6] != cfg.janitzaUnit || length < 3) {
    janitza.online = false;
    janitza.lastError = "Ongeldig Modbus antwoord";
    return false;
  }

  uint8_t payload[260];
  if (length - 1 > sizeof(payload) || !readExact(client, payload, length - 1, 3000)) {
    janitza.online = false;
    janitza.lastError = "Modbus payload timeout";
    return false;
  }
  if (payload[0] & 0x80) {
    janitza.online = false;
    janitza.lastError = String("Modbus foutcode ") + String(payload[1]);
    return false;
  }
  if (payload[0] != 0x03 || payload[1] != JANITZA_LIVE_COUNT * 2) {
    janitza.online = false;
    janitza.lastError = "Onverwachte Modbus lengte";
    return false;
  }

  uint16_t regs[JANITZA_LIVE_COUNT];
  for (uint16_t i = 0; i < JANITZA_LIVE_COUNT; i++) regs[i] = (payload[2 + i * 2] << 8) | payload[3 + i * 2];
  janitza.voltage[0] = regFloat(regs, 1317);
  janitza.voltage[1] = regFloat(regs, 1319);
  janitza.voltage[2] = regFloat(regs, 1321);
  janitza.current[0] = regFloat(regs, 1325);
  janitza.current[1] = regFloat(regs, 1327);
  janitza.current[2] = regFloat(regs, 1329);
  janitza.activePowerTotal = regFloat(regs, 1369);
  janitza.importPower = max(janitza.activePowerTotal, 0.0f);
  janitza.exportPower = max(-janitza.activePowerTotal, 0.0f);
  janitza.frequency = regFloat(regs, 1439);
  janitza.valid = true;
  janitza.online = true;
  janitza.lastReadMs = millis();
  janitza.lastError = "";
  return true;
}

uint8_t activePhaseMask(uint8_t &count) {
  uint8_t mask = 0;
  count = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (janitza.current[i] >= cfg.phaseDetectCurrent) {
      mask |= (1 << i);
      count++;
    }
  }
  if (count == 0) {
    mask = 0x07;
    count = 3;
  }
  return mask;
}

bool chargeFlowActive() {
  return evbox.state == EvState::Ready || evbox.state == EvState::Charging || evbox.state == EvState::PvWait;
}

float rawRequestedCurrent() {
  if (cfg.mode == Mode::Disabled) return 0.0f;
  if (cfg.mode == Mode::Manual) return cfg.manualCurrent;
  if (!janitzaFresh()) return cfg.janitzaFallbackCurrent;
  if (cfg.mode == Mode::Load) return cfg.maxCurrent;
  uint8_t phaseCount = 1;
  activePhaseMask(phaseCount);
  return janitza.exportPower / (230.0f * max<uint8_t>(1, phaseCount));
}

float calculateAllowedCurrent(float requested) {
  uint8_t phaseCount = 1;
  uint8_t mask = activePhaseMask(phaseCount);
  float allowed = min(requested, min(cfg.maxCurrent, cfg.breakerCurrent));
  if (janitzaFresh()) {
    float exportCurrent = janitza.exportPower / (230.0f * max<uint8_t>(1, phaseCount));
    for (uint8_t i = 0; i < 3; i++) {
      if (mask & (1 << i)) allowed = min(allowed, evbox.phaseCurrent[i] + exportCurrent + (cfg.mainFuseCurrent - janitza.current[i]));
    }
  }
  return clampCurrent(allowed);
}

float updateDesiredCurrent() {
  bool fresh = janitzaFresh();
  float desired = calculateAllowedCurrent(rawRequestedCurrent());
  cfg.mode == Mode::Pv ? evbox.desiredReason = "pv" : evbox.desiredReason = modeName(cfg.mode);

  if (!fresh && (cfg.mode == Mode::Load || cfg.mode == Mode::Pv)) {
    desired = min(desired, cfg.janitzaFallbackCurrent);
    evbox.desiredReason = "Janitza stale fallback";
  }

  if (fresh && cfg.mode == Mode::Pv) {
    if (desired < 6.0f) {
      if (evbox.pvWaitSinceMs == 0) evbox.pvWaitSinceMs = millis();
      if (chargeFlowActive() && millis() - evbox.pvWaitSinceMs < 30000UL) {
        desired = 6.0f;
        setState(EvState::PvWait, "PV dip anti-dender");
        evbox.desiredReason = "PV dip, 30s min 6A";
      } else {
        desired = 0.0f;
        evbox.desiredReason = "PV te laag, stop";
      }
    } else {
      evbox.pvWaitSinceMs = 0;
    }
  } else {
    evbox.pvWaitSinceMs = 0;
  }

  evbox.desiredCurrent = clampCurrent(desired);
  return evbox.desiredCurrent;
}

void decodeCbState(const String &data) {
  if (data.length() < 2) return;
  uint8_t code = parseHexByte(data, 0);
  evbox.cbStateCode = code;
  EvState next = evbox.state;
  switch (code) {
    case 0x02: next = EvState::Available; break;
    case 0x17: next = EvState::Connected; break;
    case 0x47:
    case 0x4A: next = EvState::Ready; break;
    case 0x48: next = EvState::Charging; break;
    case 0x0A: next = EvState::Fault; break;
    default: break;
  }
  setState(next, String("cmd26 status ") + hexByte(code) + " " + cbStateName(code));

  if (data.length() >= 128) {
    uint32_t rawLimit = parseHexUInt(data, 124, 4);
    if (rawLimit > 0 && rawLimit < 1000) evbox.lastLimit = rawLimit / 10.0f;
  }
}

void handlePacket(uint8_t dst, uint8_t src, uint8_t cmd, const String &data) {
  evbox.lastFrameMs = millis();
  if (src >= 1 && src <= 20) evbox.lastCbFrameMs = millis();
  addLog(String("RX ") + hexByte(src) + "->" + hexByte(dst) + " cmd " + hexByte(cmd) + " data=" + (data.length() ? data : "-"));

  if (cmd == 0x11 && src == ADDR_NEW && dst == ADDR_CP && data.length() >= 15) {
    evbox.serial = data.substring(0, 7);
    evbox.firmware = data.substring(7, 11).toInt();
    evbox.hardware = data.substring(11, 15).toInt();
    evbox.cbAddress = 0x01;
    setState(EvState::AssignAddress, "CB registratie");
    sendPacket(ADDR_CP, ADDR_BROADCAST, 0x11, evbox.serial + hexByte(evbox.cbAddress) + "03", "adres toewijzen");
    delay(300);
    sendConnectionState();
    delay(300);
    sendStatusUpdateRequest();
    return;
  }

  if (!evbox.cbAddress && src >= 1 && src <= 20) evbox.cbAddress = src;

  if (cmd == 0x6A && dst == ADDR_CP && src >= 1 && src <= 20) {
    evbox.cbAddress = src;
    evbox.chargeActive = true;
    sendPacket(ADDR_CP, src, 0x6A, hexWord(ACK), "request current ack");
    if (!chargeFlowActive()) setState(EvState::Ready, "cmd6A current request");
    updateDesiredCurrent();
    sendCurrentLimit(evbox.desiredCurrent);
    return;
  }

  if (cmd == 0x26 && dst == ADDR_CP && src >= 1 && src <= 20) {
    evbox.cbAddress = src;
    decodeCbState(data);
    sendPacket(ADDR_CP, src, 0x26, hexDword(evbox.session) + hexDword(secondsSince2000()), "state update ack");
    return;
  }

  if (cmd == 0x23 && dst == ADDR_CP && src >= 1 && src <= 20) {
    sendPacket(ADDR_CP, src, 0x23, String("01") + hexDword(evbox.session) + hexDword(secondsSince2000()), "metering start ack");
    return;
  }
  if (cmd == 0x24 && dst == ADDR_CP && src >= 1 && src <= 20) {
    sendPacket(ADDR_CP, src, 0x24, "01", "metering end ack");
    return;
  }
  if (cmd == 0x66 && dst == ADDR_CP && src >= 1 && src <= 20) {
    sendPacket(ADDR_CP, src, 0x66, "", "meter push ack");
    return;
  }
}

void parseFrame(const uint8_t *frame, size_t len) {
  bool hasTrailer = len >= 2 && frame[len - 1] == TRAILER_BYTE && frame[len - 2] == EOF_BYTE;
  size_t eofIndex = hasTrailer ? len - 2 : len - 1;
  if (len < 12 || frame[0] != SOF || frame[eofIndex] != EOF_BYTE || eofIndex < 5) return;

  String payload;
  for (size_t i = 1; i < eofIndex - 4; i++) payload += static_cast<char>(frame[i]);
  String gotChecksum;
  gotChecksum += static_cast<char>(frame[eofIndex - 4]);
  gotChecksum += static_cast<char>(frame[eofIndex - 3]);
  String gotParity;
  gotParity += static_cast<char>(frame[eofIndex - 2]);
  gotParity += static_cast<char>(frame[eofIndex - 1]);
  if (gotChecksum != hexByte(checksum(payload)) || gotParity != hexByte(parity(payload)) || payload.length() < 6) {
    addLog("RX frame checksum/parity fout");
    return;
  }
  uint8_t dst = parseHexByte(payload, 0);
  uint8_t src = parseHexByte(payload, 2);
  uint8_t cmd = parseHexByte(payload, 4);
  handlePacket(dst, src, cmd, payload.substring(6));
}

void feedEvboxByte(uint8_t byte) {
  if (byte == SOF) {
    rxLen = 0;
    rxBuf[rxLen++] = byte;
    return;
  }
  if (!rxLen) return;
  if (rxLen < sizeof(rxBuf)) rxBuf[rxLen++] = byte;
  if (byte == EOF_BYTE) {
    parseFrame(rxBuf, rxLen);
    rxLen = 0;
  } else if (rxLen >= sizeof(rxBuf)) {
    addLog("RX frame te lang");
    rxLen = 0;
  }
}

String htmlEscape(const String &value) {
  String out = value;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

String statusJson() {
  String json = "{";
  json += String("\"wifi\":\"") + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "offline") + "\",";
  json += String("\"ap\":\"") + WiFi.softAPIP().toString() + "\",";
  json += String("\"state\":\"") + String(stateName(evbox.state)) + "\",";
  json += String("\"cbAddress\":\"") + (evbox.cbAddress ? hexByte(evbox.cbAddress) : String("--")) + "\",";
  json += String("\"cbState\":\"") + hexByte(evbox.cbStateCode) + " " + cbStateName(evbox.cbStateCode) + "\",";
  json += String("\"desired\":") + String(evbox.desiredCurrent, 1) + ",";
  json += String("\"lastLimit\":") + (isnan(evbox.lastLimit) ? String("null") : String(evbox.lastLimit, 1)) + ",";
  json += String("\"reason\":\"") + jsonEscape(evbox.desiredReason) + "\",";
  json += String("\"janitzaFresh\":") + String(janitzaFresh() ? "true" : "false") + ",";
  json += String("\"janitzaAge\":") + (isnan(janitzaAgeSeconds()) ? String("null") : String(janitzaAgeSeconds(), 0)) + ",";
  json += String("\"janitzaError\":\"") + jsonEscape(janitza.lastError) + "\",";
  json += String("\"importPower\":") + String(janitza.importPower, 0) + ",";
  json += String("\"exportPower\":") + String(janitza.exportPower, 0) + ",";
  json += String("\"jCurrent\":[") + String(janitza.current[0], 2) + "," + String(janitza.current[1], 2) + "," + String(janitza.current[2], 2) + "],";
  json += String("\"evCurrent\":[") + String(evbox.phaseCurrent[0], 2) + "," + String(evbox.phaseCurrent[1], 2) + "," + String(evbox.phaseCurrent[2], 2) + "],";
  json += "\"log\":[";
  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (logHead + 60 - logCount + i) % 60;
    if (i) json += ",";
    json += String("\"") + jsonEscape(logLines[idx]) + "\"";
  }
  json += "]}";
  return json;
}

void saveConfig() {
  prefs.begin("evbalance", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.password);
  prefs.putString("jhost", cfg.janitzaHost);
  prefs.putUShort("jport", cfg.janitzaPort);
  prefs.putUChar("junit", cfg.janitzaUnit);
  prefs.putShort("joff", cfg.janitzaOffset);
  prefs.putUChar("mode", static_cast<uint8_t>(cfg.mode));
  prefs.putFloat("manual", cfg.manualCurrent);
  prefs.putFloat("max", cfg.maxCurrent);
  prefs.putFloat("breaker", cfg.breakerCurrent);
  prefs.putFloat("main", cfg.mainFuseCurrent);
  prefs.putFloat("detect", cfg.phaseDetectCurrent);
  prefs.putUShort("timeout", cfg.janitzaTimeoutSeconds);
  prefs.putFloat("fallback", cfg.janitzaFallbackCurrent);
  prefs.end();
}

void loadConfig() {
  prefs.begin("evbalance", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.password = prefs.getString("pass", "");
  cfg.janitzaHost = prefs.getString("jhost", cfg.janitzaHost);
  cfg.janitzaPort = prefs.getUShort("jport", cfg.janitzaPort);
  cfg.janitzaUnit = prefs.getUChar("junit", cfg.janitzaUnit);
  cfg.janitzaOffset = prefs.getShort("joff", cfg.janitzaOffset);
  cfg.mode = static_cast<Mode>(prefs.getUChar("mode", static_cast<uint8_t>(cfg.mode)));
  cfg.manualCurrent = prefs.getFloat("manual", cfg.manualCurrent);
  cfg.maxCurrent = prefs.getFloat("max", cfg.maxCurrent);
  cfg.breakerCurrent = prefs.getFloat("breaker", cfg.breakerCurrent);
  cfg.mainFuseCurrent = prefs.getFloat("main", cfg.mainFuseCurrent);
  cfg.phaseDetectCurrent = prefs.getFloat("detect", cfg.phaseDetectCurrent);
  cfg.janitzaTimeoutSeconds = prefs.getUShort("timeout", cfg.janitzaTimeoutSeconds);
  cfg.janitzaFallbackCurrent = prefs.getFloat("fallback", cfg.janitzaFallbackCurrent);
  prefs.end();
}

String rootPage() {
  String page = F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>EV Load Balance</title><style>body{font-family:system-ui;margin:20px;background:#10151f;color:#e8edf7}"
                  "input,select,button{font:inherit;margin:4px;padding:8px;background:#182233;color:#e8edf7;border:1px solid #344156;border-radius:6px}"
                  "label{display:block;margin:8px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px}"
                  ".panel{border:1px solid #344156;border-radius:8px;padding:14px;background:#151d2a}pre{white-space:pre-wrap;font-size:12px}</style></head><body>"
                  "<h1>EV Load Balance</h1><div class='grid'><div class='panel'><h2>Status</h2><pre id='status'>laden...</pre></div>"
                  "<div class='panel'><h2>Config</h2><form method='post' action='/config'>");
  page += String("WiFi SSID<label><input name='ssid' value='") + htmlEscape(cfg.ssid) + "'></label>";
  page += String("WiFi wachtwoord<label><input name='password' type='password' value='") + htmlEscape(cfg.password) + "'></label>";
  page += String("Janitza host<label><input name='jhost' value='") + htmlEscape(cfg.janitzaHost) + "'></label>";
  page += String("Janitza port<label><input name='jport' type='number' value='") + String(cfg.janitzaPort) + "'></label>";
  page += String("Unit id<label><input name='junit' type='number' value='") + String(cfg.janitzaUnit) + "'></label>";
  page += "Mode<label><select name='mode'>";
  for (const char *m : {"disabled", "manual", "load", "pv"}) page += String("<option value='") + m + "'" + (String(m) == modeName(cfg.mode) ? " selected" : "") + ">" + m + "</option>";
  page += "</select></label>";
  page += String("Manual A<label><input name='manual' type='number' step='0.1' value='") + String(cfg.manualCurrent, 1) + "'></label>";
  page += String("Max A<label><input name='max' type='number' step='0.1' value='") + String(cfg.maxCurrent, 1) + "'></label>";
  page += String("Hoofdzekering A<label><input name='main' type='number' step='0.1' value='") + String(cfg.mainFuseCurrent, 1) + "'></label>";
  page += String("Janitza timeout s<label><input name='timeout' type='number' value='") + String(cfg.janitzaTimeoutSeconds) + "'></label>";
  page += String("Fallback A<label><input name='fallback' type='number' step='0.1' value='") + String(cfg.janitzaFallbackCurrent, 1) + "'></label>";
  page += F("<button>Opslaan</button></form><p><a href='/restart'>Herstart EVBox registratie</a></p></div></div>"
            "<script>async function tick(){const r=await fetch('/status');const j=await r.json();status.textContent=JSON.stringify(j,null,2)}setInterval(tick,1000);tick()</script></body></html>");
  return page;
}

void handleConfigPost() {
  cfg.ssid = server.arg("ssid");
  cfg.password = server.arg("password");
  cfg.janitzaHost = server.arg("jhost");
  cfg.janitzaPort = server.arg("jport").toInt();
  cfg.janitzaUnit = server.arg("junit").toInt();
  cfg.mode = parseMode(server.arg("mode"));
  cfg.manualCurrent = server.arg("manual").toFloat();
  cfg.maxCurrent = server.arg("max").toFloat();
  cfg.breakerCurrent = cfg.maxCurrent;
  cfg.mainFuseCurrent = server.arg("main").toFloat();
  cfg.janitzaTimeoutSeconds = server.arg("timeout").toInt();
  cfg.janitzaFallbackCurrent = server.arg("fallback").toFloat();
  saveConfig();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send(200, "text/html", rootPage()); });
  server.on("/status", HTTP_GET, [] { server.send(200, "application/json", statusJson()); });
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/restart", HTTP_GET, [] {
    sendRestartRegistration();
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "");
  });
  server.begin();
}

void setupWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("EVLoadBalance-setup");
  if (cfg.ssid.length()) {
    WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
    addLog(String("WiFi verbinden met ") + cfg.ssid);
  }
  addLog(String("Setup AP: ") + WiFi.softAPIP().toString());
}

void setupRelays() {
  for (int pin : {Pins::Relay1, Pins::Relay2, Pins::Relay3, Pins::Relay4, Pins::Relay5, Pins::Relay6}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  pinMode(Pins::Buzzer, OUTPUT);
}

void updateRelays() {
  digitalWrite(Pins::Relay1, evbox.cbAddress ? HIGH : LOW);
  digitalWrite(Pins::Relay2, janitzaFresh() ? HIGH : LOW);
  digitalWrite(Pins::Relay3, chargeFlowActive() ? HIGH : LOW);
  digitalWrite(Pins::Relay4, (!janitzaFresh() && (cfg.mode == Mode::Load || cfg.mode == Mode::Pv)) ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  loadConfig();
  setupRelays();
  EvSerial.begin(38400, SERIAL_8N1, Pins::Rs485Rx, Pins::Rs485Tx);
  setupWifi();
  setupWeb();
  setState(EvState::WaitRegistration, "opstart");
  sendRestartRegistration();
}

void loop() {
  server.handleClient();
  while (EvSerial.available()) feedEvboxByte(static_cast<uint8_t>(EvSerial.read()));

  uint32_t now = millis();
  if (now >= nextJanitzaPollMs) {
    nextJanitzaPollMs = now + 1000UL;
    if (!readJanitza()) addLog(String("Janitza fout: ") + janitza.lastError);
  }
  if (now >= nextControlMs) {
    nextControlMs = now + 1000UL;
    float desired = updateDesiredCurrent();
    if (evbox.cbAddress && chargeFlowActive() && (isnan(evbox.lastLimit) || fabsf(evbox.lastLimit - desired) >= 0.1f) && now - evbox.lastCurrentSentMs >= 5000UL) {
      sendCurrentLimit(desired);
    }
  }
  if (evbox.cbAddress && now >= nextStatusPollMs) {
    nextStatusPollMs = now + 10000UL;
    sendStatusUpdateRequest();
  }
  updateRelays();
}
