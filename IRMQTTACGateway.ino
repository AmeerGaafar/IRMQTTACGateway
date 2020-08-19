
#include "IRMQTTACGateway.h"
#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#endif  // ESP8266
#if defined(ESP32)
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <Update.h>
#endif  // ESP32
#include <WiFiClient.h>
#include <DNSServer.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRtext.h>
#include <IRtimer.h>
#include <IRutils.h>
#include <IRac.h>
#include <PubSubClient.h>

#define ___HTTP_PROTECT___  { if (!server.authenticate(HttpUsername, HttpPassword)) {           \
                                    debug("Basic HTTP authentication failure for /.");          \
                                    return server.requestAuthentication();                      \
                                 }                                                              \
                            }

// --------------------------------------------------------------------
// * * * IMPORTANT * * *
// You must change <PubSubClient.h> to have the following value.
// #define MQTT_MAX_PACKET_SIZE 768
// --------------------------------------------------------------------
// Check that the user has set MQTT_MAX_PACKET_SIZE to an appropriate size.
#if MQTT_MAX_PACKET_SIZE < 1024
#error "MQTT_MAX_PACKET_SIZE in <PubSubClient.h> is too small. "\
  "Increase the value per comments."
#endif  // MQTT_MAX_PACKET_SIZE < 1024
#include <algorithm>  // NOLINT(build/include)
#include <memory>
#include <string>

using irutils::msToString;

// Globals
#if defined(ESP8266)
ESP8266WebServer server(kHttpPort);
#endif  // ESP8266
#if defined(ESP32)
WebServer server(kHttpPort);
#endif  // ESP32
#if MDNS_ENABLE
MDNSResponder mdns;
#endif  // MDNS_ENABLE
WiFiClient espClient;

char HttpUsername[kUsernameLength + 1] = "admin";  // Default HTT username.
char HttpPassword[kPasswordLength + 1] = "";  // No HTTP password by default.
char Hostname[kHostnameLength + 1] = "ir_server";  // Default hostname.

uint16_t *codeArray;
uint32_t lastReconnectAttempt = 0;  // MQTT last attempt reconnection number
bool boot = true;
volatile bool lockIr = false;  // Primitive locking for gating the IR LED.
uint32_t sendReqCounter = 0;
bool lastSendSucceeded = false;  // Store the success status of the last send.
uint32_t lastSendTime = 0;
int8_t offset;  // The calculated period offset for this chip and library.
IRsend *IrSendTable[kNrOfIrTxGpios];
int8_t txGpioTable[kNrOfIrTxGpios] = {kDefaultIrLed};
String lastClimateSource;

// Climate stuff
IRac *climate[kNrOfIrTxGpios];
String channel_re = "(";  // Will be built later.
uint16_t chan = 0;  // The channel to use for the aircon HTML page.

TimerMs lastClimateIr = TimerMs();  // When we last sent the IR Climate mesg.
uint32_t irClimateCounter = 0;  // How many have we sent?
// Store the success status of the last climate send.
bool lastClimateSucceeded = false;
bool hasClimateBeenSent = false;  // Has the Climate ever been sent?

PubSubClient mqtt_client(espClient);

String latest_current_temperature="unknown";
String latest_current_humidity="unknown";

String lastMqttCmd = "None";
String lastMqttCmdTopic = "None";
uint32_t lastMqttCmdTime = 0;
uint32_t lastConnectedTime = 0;
uint32_t lastDisconnectedTime = 0;
uint32_t mqttDisconnectCounter = 0;
uint32_t mqttSentCounter = 0;
uint32_t mqttRecvCounter = 0;
bool wasConnected = true;

char MqttServer[kHostnameLength + 1] = "10.0.0.4";
char MqttPort[kPortLength + 1] = "1883";
char MqttUsername[kUsernameLength + 1] = "";
char MqttPassword[kPasswordLength + 1] = "";
char MqttPrefix[kHostnameLength + 1] = "";

String MqttAck;  // Sub-topic we send back acknowledgements on.
String MqttSend;  // Sub-topic we get new commands from.
String MqttRecv;  // Topic we send received IRs to.
String MqttLog;  // Topic we send log messages to.
String MqttLwt;  // Topic for the Last Will & Testament.
String MqttClimate;  // Sub-topic for the climate topics.
String MqttClimateCmnd;  // Sub-topic for the climate command topics.
#if MQTT_DISCOVERY_ENABLE
String MqttDiscovery;
String MqttUniqueId;
#endif  // MQTT_DISCOVERY_ENABLE
String MqttHAName;
String MqttClientId;

// Primative lock file for gating MQTT state broadcasts.
bool lockMqttBroadcast = true;
TimerMs lastBroadcast = TimerMs();  // When we last sent a broadcast.
bool hasBroadcastBeenSent = false;
#if MQTT_DISCOVERY_ENABLE
TimerMs lastDiscovery = TimerMs();  // When we last sent a Discovery.
bool hasDiscoveryBeenSent = false;
#endif  // MQTT_DISCOVERY_ENABLE
TimerMs statListenTime = TimerMs();  // How long we've been listening for.


bool isSerialGpioUsedByIr(void) {
  const int8_t kSerialTxGpio = 1;  // The GPIO serial output is sent to.
                                   // Note: *DOES NOT* control Serial output.
#if defined(ESP32)
  const int8_t kSerialRxGpio = 3;  // The GPIO serial input is received on.
#endif  // ESP32
  // Ensure we are not trodding on anything IR related.
  for (uint16_t i = 0; i < kNrOfIrTxGpios; i++)
    switch (txGpioTable[i]) {
#if defined(ESP32)
      case kSerialRxGpio:
#endif  // ESP32
      case kSerialTxGpio:
        return true;  // Serial port is in use for IR sending. Abort.
    }
  return false;  // Not in use as far as we can tell.
}

// Debug messages get sent to the serial port.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void debug(const char *str) {
#if DEBUG
  if (isSerialGpioUsedByIr()) return;  // Abort.
  uint32_t now = millis();
  Serial.printf("%07u.%03u: %s\n", now / 1000, now % 1000, str);
#endif  // DEBUG
}
#pragma GCC diagnostic pop

String timeElapsed(uint32_t const msec) {
  String result = msToString(msec);
  if (result.equalsIgnoreCase(D_STR_NOW))
    return result;
  else
    return result + F(" ago");
}

String timeSince(uint32_t const start) {
  if (start == 0)
    return F("Never");
  uint32_t diff = 0;
  uint32_t now = millis();
  if (start < now)
    diff = now - start;
  else
    diff = UINT32_MAX - start + now;
  return msToString(diff) + F(" ago");
}

String gpioToString(const int16_t gpio) {
  if (gpio == kGpioUnused)
    return F(D_STR_UNUSED);
  else
    return String(gpio);
}

int8_t getDefaultTxGpio(void) {
  for (int16_t i = 0; i < kNrOfIrTxGpios; i++)
    if (txGpioTable[i] != kGpioUnused) return txGpioTable[i];
  return kGpioUnused;
}

// Return a string containing the comma separated list of sending gpios.
String listOfTxGpios(void) {
  bool found = false;
  String result = "";
  for (uint16_t i = 0; i < kNrOfIrTxGpios; i++) {
    if (i) result += ", ";
    result += gpioToString(txGpioTable[i]);
    if (!found && txGpioTable[i] == getDefaultTxGpio()) {
      result += " (default)";
      found = true;
    }
  }
  return result;
}

String htmlMenu(void) {
  String html = F("<center>");
  html += htmlButton(kUrlRoot, F("Home"));
  html += htmlButton(kUrlAircon, F("Aircon"));
  html += htmlButton(kUrlInfo, F("System Info"));
  html += htmlButton(kUrlAdmin, F("Admin"));
  html += F("</center><hr>");
  return html;
}

String htmlSelectAcStateProtocol(const String name, const decode_type_t def,
                                 const bool simple) {
  String html = "<select name='" + name + "'>";
  for (uint8_t i = 1; i <= decode_type_t::kLastDecodeType; i++) {
    if (simple ^ hasACState((decode_type_t)i)) {
      switch (i) {
        case decode_type_t::RAW:
        case decode_type_t::PRONTO:
        case decode_type_t::GLOBALCACHE:
          break;
        default:
          html += htmlOptionItem(String(i), typeToString((decode_type_t)i),
                                i == def);
      }
    }
  }
  html += F("</select>");
  return html;
}

// Root web page with example usage etc.
void handleRoot(void) {

  ___HTTP_PROTECT___
  
  String html = htmlHeader(F("ESP IR MQTT Server"));
  html += F("<center><small><i>" _MY_VERSION_ "</i></small></center>");
  html += htmlMenu();
  html += F(
    "<h3>Send a simple IR message</h3><p>"
    "<form method='POST' action='/ir' enctype='multipart/form-data'>"
      D_STR_PROTOCOL ": ");
  html += htmlSelectAcStateProtocol(KEY_TYPE, decode_type_t::NEC, true);
  html += F(
      " " D_STR_CODE ": 0x<input type='text' name='" KEY_CODE "' min='0' "
        "value='0' size='16' maxlength='16'> "
      D_STR_BITS ": "
      "<select name='" KEY_BITS "'>"
        "<option selected='selected' value='0'>Default</option>");  // Default
  for (uint8_t i = 0; i < sizeof(kCommonBitSizes); i++) {
    String num = String(kCommonBitSizes[i]);
    html += F("<option value='");
    html += num;
    html += F("'>");
    html += num;
    html += F("</option>");
  }
  html += F(
      "</select>"
      " " D_STR_REPEAT ": <input type='number' name='" KEY_REPEAT "' min='0' "
        "max='99' value='0' size='2' maxlength='2'>"
      " <input type='submit' value='Send " D_STR_CODE "'>"
    "</form>"
    "<br><hr>"
    "<h3>Send a complex (Air Conditioner) IR message</h3><p>"
    "<form method='POST' action='/ir' enctype='multipart/form-data'>"
      D_STR_PROTOCOL ": ");
  html += htmlSelectAcStateProtocol(KEY_TYPE, decode_type_t::KELVINATOR, false);
  html += F(
      " State " D_STR_CODE ": 0x"
      "<input type='text' name='" KEY_CODE "' size='");
  html += String(kStateSizeMax * 2);
  html += F("' maxlength='");
  html += String(kStateSizeMax * 2);
  html += F("'"
          " value='"
                "'>"
      " <input type='submit' value='Send A/C " D_STR_CODE "'>"
    "</form>"
    "<br><hr>"
    "<h3>Send an IRremote Raw IR message</h3><p>"
    "<form method='POST' action='/ir' enctype='multipart/form-data'>"
      "<input type='hidden' name='" KEY_TYPE "' value='30'>"
      "String: (freq,array data) <input type='text' name='" KEY_CODE "'"
      " size='132' value='"
          "'>"
      " <input type='submit' value='Send Raw'>"
    "</form>"
    "<br><hr>"
    "<h3>Send a <a href='https://irdb.globalcache.com/'>GlobalCache</a>"
        " IR message</h3><p>"
    "<form method='POST' action='/ir' enctype='multipart/form-data'>"
      "<input type='hidden' name='" KEY_TYPE "' value='31'>"
      "String: 1:1,1,<input type='text' name='" KEY_CODE "' size='132'"
      " value='"
          "'>"
      " <input type='submit' value='Send GlobalCache'>"
    "</form>"
    "<br><hr>"
    "<h3>Send a <a href='http://www.remotecentral.com/cgi-bin/files/rcfiles.cgi"
      "?area=pronto&db=discrete'>Pronto code</a> IR message</h3><p>"
    "<form method='POST' action='/ir' enctype='multipart/form-data'>"
      "<input type='hidden' name='" KEY_TYPE "' value='25'>"
      "String (comma separated): <input type='text' name='" KEY_CODE "'"
      " size='132' value='"
          "'>"
      " " D_STR_REPEAT ": <input type='number' name='" KEY_REPEAT "' min='0' "
          "max='99' value='0' size='2' maxlength='2'>"
      " <input type='submit' value='Send Pronto'>"
    "</form>"
    "<br>");
  html += htmlEnd();
  server.send(200, "text/html", html);
}

String addJsReloadUrl(const String url, const uint16_t timeout_s,
                      const bool notify) {
  String html = F(
      "<script type=\"text/javascript\">\n"
      "<!--\n"
      "  function Redirect() {\n"
      "    window.location=\"");
  html += url;
  html += F("\";\n"
      "  }\n"
      "\n");
  if (notify && timeout_s) {
    html += F("  document.write(\"You will be redirected to the main page in ");
    html += String(timeout_s);
    html += F(" " D_STR_SECONDS ".\");\n");
  }
  html += F("  setTimeout('Redirect()', ");
  html += String(timeout_s * 1000);  // Convert to mSecs
  html += F(");\n"
      "//-->\n"
      "</script>\n");
  return html;
}

String htmlOptionItem(const String value, const String text, bool selected) {
  String html = F("<option value='");
  html += value + '\'';
  if (selected) html += F(" selected='selected'");
  html += '>' + text + F("</option>");
  return html;
}

String htmlSelectBool(const String name, const bool def) {
  String html = "<select name='" + name + "'>";
  for (uint16_t i = 0; i < 2; i++)
    html += htmlOptionItem(IRac::boolToString(i), IRac::boolToString(i),
                           i == def);
  html += F("</select>");
  return html;
}

String htmlSelectClimateProtocol(const String name, const decode_type_t def) {
  String html = "<select name='" + name + "'>";
  for (uint8_t i = 1; i <= decode_type_t::kLastDecodeType; i++) {
    if (IRac::isProtocolSupported((decode_type_t)i)) {
      html += htmlOptionItem(String(i), typeToString((decode_type_t)i),
                             i == def);
    }
  }
  html += F("</select>");
  return html;
}

String htmlSelectModel(const String name, const int16_t def) {
  String html = "<select name='" + name + "'>";
  for (int16_t i = -1; i <= 6; i++) {
    String num = String(i);
    String text;
    if (i == -1)
      text = F("Default");
    else if (i == 0)
      text = F("Unknown");
    else
      text = num;
    html += htmlOptionItem(num, text, i == def);
  }
  html += F("</select>");
  return html;
}

String htmlSelectUint(const String name, const uint16_t max,
                      const uint16_t def) {
  String html = "<select name='" + name + "'>";
  for (uint16_t i = 0; i < max; i++) {
    String num = String(i);
    html += htmlOptionItem(num, num, i == def);
  }
  html += F("</select>");
  return html;
}

String htmlSelectMode(const String name, const stdAc::opmode_t def) {
  String html = "<select name='" + name + "'>";
  for (int8_t i = -1; i <= (int8_t)stdAc::opmode_t::kLastOpmodeEnum; i++) {
    String mode = IRac::opmodeToString((stdAc::opmode_t)i);
    html += htmlOptionItem(mode, mode, (stdAc::opmode_t)i == def);
  }
  html += F("</select>");
  return html;
}

String htmlSelectFanspeed(const String name, const stdAc::fanspeed_t def) {
  String html = "<select name='" + name + "'>";
  for (int8_t i = 0; i <= (int8_t)stdAc::fanspeed_t::kLastFanspeedEnum; i++) {
    String speed = IRac::fanspeedToString((stdAc::fanspeed_t)i);
    html += htmlOptionItem(speed, speed, (stdAc::fanspeed_t)i == def);
  }
  html += F("</select>");
  return html;
}

String htmlSelectSwingv(const String name, const stdAc::swingv_t def) {
  String html = "<select name='" + name + "'>";
  for (int8_t i = -1; i <= (int8_t)stdAc::swingv_t::kLastSwingvEnum; i++) {
    String swing = IRac::swingvToString((stdAc::swingv_t)i);
    html += htmlOptionItem(swing, swing, (stdAc::swingv_t)i == def);
  }
  html += F("</select>");
  return html;
}

String htmlSelectSwingh(const String name, const stdAc::swingh_t def) {
  String html = "<select name='" + name + "'>";
  for (int8_t i = -1; i <= (int8_t)stdAc::swingh_t::kLastSwinghEnum; i++) {
    String swing = IRac::swinghToString((stdAc::swingh_t)i);
    html += htmlOptionItem(swing, swing, (stdAc::swingh_t)i == def);
  }
  html += F("</select>");
  return html;
}

String htmlHeader(const String title, const String h1_text) {
  String html = F("<html><head><title>");
  html += title;
  html += F("</title><meta http-equiv=\"Content-Type\" "
            "content=\"text/html;charset=utf-8\">"
            "</head><body><center><h1>");
  if (h1_text.length())
    html += h1_text;
  else
    html += title;
  html += F("</h1></center>");
  return html;
}

String htmlEnd(void) {
  return F("</body></html>");
}

String htmlButton(const String url, const String button, const String text) {
  String html = F("<button type='button' onclick='window.location=\"");
  html += url;
  html += F("\"'>");
  html += button;
  html += F("</button> ");
  html += text;
  return html;
}

// Admin web page
void handleAirCon(void) {
  String html = htmlHeader(F("Air Conditioner Control"));
  html += htmlMenu();
  if (kNrOfIrTxGpios > 1) {
    html += "<form method='POST' action='/aircon/set'"
        " enctype='multipart/form-data'>"
        "<table>"
        "<tr><td><b>Climate #</b></td><td>" +
        htmlSelectUint(KEY_CHANNEL, kNrOfIrTxGpios, chan) +
        "<input type='submit' value='Change'>"
        "</td></tr>"
        "</table>"
        "</form>"
        "<hr>";
  }
  if (climate[chan] != NULL) {
    html += "<h3>Current Settings</h3>"
        "<form method='POST' action='/aircon/set'"
        " enctype='multipart/form-data'>"
        "<input type='hidden' name='" KEY_CHANNEL "' value='" + String(chan) +
            "'>" +
        "<table style='width:33%'>"
        "<tr><td>" D_STR_PROTOCOL "</td><td>" +
            htmlSelectClimateProtocol(KEY_PROTOCOL,
                                      climate[chan]->next.protocol) +
            "</td></tr>"
        "<tr><td>" D_STR_MODEL "</td><td>" +
            htmlSelectModel(KEY_MODEL, climate[chan]->next.model) +
            "</td></tr>"
        "<tr><td>" D_STR_POWER "</td><td>" +
            htmlSelectBool(KEY_POWER, climate[chan]->next.power) +
            "</td></tr>"
        "<tr><td>" D_STR_MODE "</td><td>" +
            htmlSelectMode(KEY_MODE, climate[chan]->next.mode) +
            "</td></tr>"
        "<tr><td>" D_STR_TEMP "</td><td>"
            "<input type='number' name='" KEY_TEMP "' min='16' max='90' "
            "step='0.5' value='" + String(climate[chan]->next.degrees, 1) + "'>"
            "<select name='" KEY_CELSIUS "'>"
                "<option value='on'" +
                (climate[chan]->next.celsius ? " selected='selected'" : "") +
                ">C</option>"
                "<option value='off'" +
                (!climate[chan]->next.celsius ? " selected='selected'" : "") +
                ">F</option>"
            "</select></td></tr>"
        "<tr><td>" D_STR_FAN "</td><td>" +
            htmlSelectFanspeed(KEY_FANSPEED, climate[chan]->next.fanspeed) +
            "</td></tr>"
        "<tr><td>" D_STR_SWINGV "</td><td>" +
            htmlSelectSwingv(KEY_SWINGV, climate[chan]->next.swingv) +
            "</td></tr>"
        "<tr><td>" D_STR_SWINGH "</td><td>" +
            htmlSelectSwingh(KEY_SWINGH, climate[chan]->next.swingh) +
            "</td></tr>"
        "<tr><td>" D_STR_QUIET "</td><td>" +
            htmlSelectBool(KEY_QUIET, climate[chan]->next.quiet) +
            "</td></tr>"
        "<tr><td>" D_STR_TURBO "</td><td>" +
            htmlSelectBool(KEY_TURBO, climate[chan]->next.turbo) +
            "</td></tr>"
        "<tr><td>" D_STR_ECONO "</td><td>" +
            htmlSelectBool(KEY_ECONO, climate[chan]->next.econo) +
            "</td></tr>"
        "<tr><td>" D_STR_LIGHT "</td><td>" +
            htmlSelectBool(KEY_LIGHT, climate[chan]->next.light) +
            "</td></tr>"
        "<tr><td>" D_STR_FILTER "</td><td>" +
            htmlSelectBool(KEY_FILTER, climate[chan]->next.filter) +
            "</td></tr>"
        "<tr><td>" D_STR_CLEAN "</td><td>" +
            htmlSelectBool(KEY_CLEAN, climate[chan]->next.clean) +
            "</td></tr>"
        "<tr><td>" D_STR_BEEP "</td><td>" +
            htmlSelectBool(KEY_BEEP, climate[chan]->next.beep) +
            "</td></tr>"
        "<tr><td>Force resend</td><td>" +
            htmlSelectBool(KEY_RESEND, false) +
            "</td></tr>"
        "</table>"
        "<input type='submit' value='Update & Send'>"
        "</form>";
  }
  html += htmlEnd();
  server.send(200, "text/html", html);
}

// Parse the URL args to find the Common A/C arguments.
void handleAirConSet(void) {
  
  ___HTTP_PROTECT___

  debug("New common a/c received via HTTP");
  uint16_t channel = chan;
  if (kNrOfIrTxGpios > 1) {
    // Scan for the channel number if needed.
    for (uint16_t i = 0; i < server.args(); i++) {
      if (server.argName(i).equals(KEY_CHANNEL)) {
        channel = server.arg(i).toInt();
      }
    }
  }
  // Change the HTML channel for the climate if it is within the correct range.
  if (channel < kNrOfIrTxGpios) chan = channel;

  IRac *ac_ptr = climate[chan];
  String html = htmlHeader(F("Aircon updated!"));
  if (ac_ptr != NULL) {
    bool force_resend = false;
    for (uint16_t i = 0; i < server.args(); i++) {
      if (server.argName(i).equals(KEY_RESEND))
        force_resend = IRac::strToBool(server.arg(i).c_str());
      else
        updateClimate(&(ac_ptr->next), server.argName(i), "", server.arg(i));
    }

    sendClimate(genStatTopic(chan), true, false, force_resend, true, ac_ptr);
    lastClimateSource = F("HTTP");
  } else {  // ac_ptr == NULL
    debug("No climate setup for the given channel. Aborting!");
    html = htmlHeader(F("Aircon update FAILED!"));
  }
  // Redirect back to the aircon page.
  html += addJsReloadUrl(kUrlAircon, kQuickDisplayTime, false);
  html += htmlEnd();
  server.send(200, "text/html", html);
}

String htmlDisabled(void) {
  String html = F(
      "<i>Updates disabled until you set a password. "
      "You will need to <a href='");
  html += kUrlWipe;
  html += F("'>wipe & reset</a> to set one.</i><br><br>");
  return html;
}

// Admin web page
void handleAdmin(void) {
  String html = htmlHeader(F("Administration"));
  html += htmlMenu();
  html += F("<h3>Special commands</h3>");
#if MQTT_DISCOVERY_ENABLE
  html += htmlButton(
      kUrlSendDiscovery, F("Send MQTT Discovery"),
      F("Send a Climate MQTT discovery message to Home Assistant.<br><br>"));
#endif  // MQTT_DISCOVERY_ENABLE
#if MQTT_CLEAR_ENABLE
  html += htmlButton(
      kUrlClearMqtt, F("Clear data saved to MQTT"),
      F("Clear all saved climate & discovery messages for this device & "
        "reboot.<br><br>"));
#endif  // MQTT_CLEAR_ENABLE
  html += htmlButton(
      kUrlReboot, F("Reboot"),
      F("A simple reboot of the ESP. <small>ie. No changes</small><br>"
        "<br>"));
  html += htmlButton(
      kUrlWipe, F("Wipe Settings"),
      F("<mark>Warning:</mark> Resets the device back to original settings. "
        "<small>ie. Goes back to AP/Setup mode.</small><br><br>"));
#if FIRMWARE_OTA
  html += F("<hr><h3>Update firmware</h3><p>"
            "<b><mark>Warning:</mark></b><br> ");
  if (!strlen(HttpPassword))  // Deny if password not set
    html += htmlDisabled();
  else  // default password has been changed, so allow it.
    html += F(
        "<i>Updating your firmware may screw up your access to the device. "
        "If you are going to use this, know what you are doing first "
        "(and you probably do).</i><br>"
        "<form method='POST' action='/update' enctype='multipart/form-data'>"
          "Firmware to upload: <input type='file' name='update'>"
          "<input type='submit' value='Update'>"
        "</form>");
#endif  // FIRMWARE_OTA
  html += htmlEnd();
  server.send(200, "text/html", html);
}

uint32_t maxSketchSpace(void) {
#if defined(ESP8266)
  return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
#else  // defined(ESP8266)
  return UPDATE_SIZE_UNKNOWN;
#endif  // defined(ESP8266)
}

// Info web page
void handleInfo(void) {
  String html = htmlHeader(F("IR MQTT server info"));
  html += htmlMenu();
  html +=
    "<h3>General</h3>"
    "<p>Hostname: " + String(Hostname) + "<br>"
    "IP address: " + WiFi.localIP().toString() + "<br>"
    "MAC address: " + WiFi.macAddress() + "<br>"
    "Booted: " + timeSince(1) + "<br>" +
    "Version: " _MY_VERSION_ "<br>"
    "Built: " __DATE__
      " " __TIME__ "<br>"
    "Period Offset: " + String(offset) + "us<br>"
    "IR Lib Version: " _IRREMOTEESP8266_VERSION_ "<br>"
#if defined(ESP8266)
    "ESP8266 Core Version: " + ESP.getCoreVersion() + "<br>"
    "Free Sketch Space: " + String(maxSketchSpace() >> 10) + "k<br>"
#endif  // ESP8266
#if defined(ESP32)
    "ESP32 SDK Version: " + ESP.getSdkVersion() + "<br>"
#endif  // ESP32
    "Cpu Freq: " + String(ESP.getCpuFreqMHz()) + "MHz<br>"
    "IR Send GPIO(s): " + listOfTxGpios() + "<br>"
    + irutils::addBoolToString(kInvertTxOutput,
                               "Inverting GPIO output", false) + "<br>"
    "Total send requests: " + String(sendReqCounter) + "<br>"
    "Last message sent: " + String(lastSendSucceeded ? "Ok" : "FAILED") +
    " <i>(" + timeSince(lastSendTime) + ")</i><br>"
        "%<br>"
    "Serial debugging: "
#if DEBUG
        + String(isSerialGpioUsedByIr() ? D_STR_OFF : D_STR_ON) +
#else  // DEBUG
        D_STR_OFF
#endif  // DEBUG
        "<br>"
    "</p>"
    "<h4>MQTT Information</h4>"
    "<p>Server: " + String(MqttServer) + ":" + String(MqttPort) + " <i>(" +
    (mqtt_client.connected() ? "Connected " + timeSince(lastDisconnectedTime)
                             : "Disconnected " + timeSince(lastConnectedTime)) +
    ")</i><br>"
    "Disconnections: " + String(mqttDisconnectCounter - 1) + "<br>"
    "Max Packet Size: " + MQTT_MAX_PACKET_SIZE + " bytes<br>"
    "Client id: " + MqttClientId + "<br>"
    "Command topic(s): " + listOfCommandTopics() + "<br>"
    "Acknowledgements topic: " + MqttAck + "<br>"
    "Log topic: " + MqttLog + "<br>"
    "LWT topic: " + MqttLwt + "<br>"
    "QoS: " + String(QOS) + "<br>"
    // lastMqttCmd* is unescaped untrusted input.
    // Avoid any possible HTML/XSS when displaying it.
    "Last MQTT command seen: (topic) '" +
        irutils::htmlEscape(lastMqttCmdTopic) +
         "' (payload) '" + irutils::htmlEscape(lastMqttCmd) + "' <i>(" +
         timeSince(lastMqttCmdTime) + ")</i><br>"
    "Total published: " + String(mqttSentCounter) + "<br>"
    "Total received: " + String(mqttRecvCounter) + "<br>"
    "</p>"
    "<h4>Climate Information</h4>"
    "<p>"
    "IR Send GPIO: " + String(txGpioTable[0]) + "<br>"
    "Last update source: " + lastClimateSource + "<br>"
    "Total sent: " + String(irClimateCounter) + "<br>"
    "Last send: " + String(hasClimateBeenSent ?
        (String(lastClimateSucceeded ? "Ok" : "FAILED") +
         " <i>(" + timeElapsed(lastClimateIr.elapsed()) + ")</i>") :
        "<i>Never</i>") + "<br>"
    "State listen period: " + msToString(kStatListenPeriodMs) + "<br>"
    "State broadcast period: " + msToString(kBroadcastPeriodMs) + "<br>"
    "Last state broadcast: " + (hasBroadcastBeenSent ?
        timeElapsed(lastBroadcast.elapsed()) :
        String("<i>Never</i>")) + "<br>"
#if MQTT_DISCOVERY_ENABLE
    "Last discovery sent: " + (lockMqttBroadcast ?
        String("<b>Locked</b>") :
        (hasDiscoveryBeenSent ?
            timeElapsed(lastDiscovery.elapsed()) :
            String("<i>Never</i>"))) +
        "<br>"
    "Discovery topic: " + MqttDiscovery + "<br>" +
#endif  // MQTT_DISCOVERY_ENABLE
    "Command topics: " + MqttClimate + channel_re + '/' + MQTT_CLIMATE_CMND +
        '/' + kClimateTopics +
    "State topics: " + MqttClimate + channel_re + '/' + MQTT_CLIMATE_STAT +
        '/' + kClimateTopics +
    "</p>"
    // Page footer
    "<hr><p><small><center>"
      "<i>(Note: Page will refresh every 60 " D_STR_SECONDS ".)</i>"
    "<centre></small></p>";
  html += addJsReloadUrl(kUrlInfo, 60, false);
  html += htmlEnd();
  server.send(200, "text/html", html);
}

void doRestart(const char* str, const bool serial_only) {
  if (!serial_only)
    mqttLog(str);
  else
    debug(str);
  delay(2000);  // Enough time for messages to be sent.
  ESP.restart();
  delay(5000);  // Enough time to ensure we don't return.
}

#if MQTT_CLEAR_ENABLE
// Clear any MQTT message that we might have set retain on.
bool clearMqttSavedStates(const String topic_base) {
  String channelStr = "";
  bool success = true;
  // Clear the Last Will & Testament.
  success &= mqtt_client.publish(MqttLwt.c_str(), "", true);
#if MQTT_DISCOVERY_ENABLE
  // Clear the HA climate discovery message.
  success &= mqtt_client.publish(MqttDiscovery.c_str(), "", true);
#endif  // MQTT_DISCOVERY_ENABLE
  for (size_t channel = 0;
       channel <= kNrOfIrTxGpios;
       channelStr = '_' + String(channel++)) {
    for (size_t i = 0; i < sizeof(kMqttTopics) / sizeof(char*); i++) {
      // Sending a retained "" message to the topic should clear previous values
      // in theory.
      String topic = topic_base + channelStr + '/' + F(MQTT_CLIMATE_STAT) +
          '/' + String(kMqttTopics[i]);
      success &= mqtt_client.publish(topic.c_str(), "", true);
    }
    channelStr = '_' + String(channel);
  }
  String logmesg = "Removing all possible settings saved in MQTT for '" +
      topic_base + "' ";
  logmesg += success ? F("succeeded") : F("failed");
  mqttLog(logmesg.c_str());
  return success;
}

// Clear settings from MQTT web page
void handleClearMqtt(void) {

  ___HTTP_PROTECT___
  
  server.send(200, "text/html",
    htmlHeader(F("Clearing saved info from MQTT"),
               F("Removing all saved settings for this device from "
                 "MQTT.")) +
    "<p>Device restarting. Try connecting in a few " D_STR_SECONDS ".</p>" +
    addJsReloadUrl(kUrlRoot, 10, true) +
    htmlEnd());
  // Do the clearing.
  mqttLog("Clearing all saved settings from MQTT.");
  clearMqttSavedStates(MqttClimate);
  doRestart("Rebooting...");
}
#endif  // MQTT_CLEAR_ENABLE

// Reset web page
void handleReset(void) {

  ___HTTP_PROTECT___

  server.send(200, "text/html",
    htmlHeader(F("Reset WiFi Config")) +
    "<p>Device restarting. Try connecting in a few " D_STR_SECONDS ".</p>" +
    addJsReloadUrl(kUrlRoot, 10, true) +
    htmlEnd());
  // Do the reset.
#if MQTT_CLEAR_ENABLE
  mqttLog("Clearing all saved climate settings from MQTT.");
  clearMqttSavedStates(MqttClimate);
#endif  // MQTT_CLEAR_ENABLE
  delay(1000);
  doRestart("Rebooting...");
}

// Reboot web page
void handleReboot() {

  ___HTTP_PROTECT___
  
  server.send(200, "text/html",
    htmlHeader(F("Device restarting.")) +
    "<p>Try connecting in a few " D_STR_SECONDS ".</p>" +
    addJsReloadUrl(kUrlRoot, kRebootTime, true) +
    htmlEnd());
  doRestart("Reboot requested");
}

// Parse an Air Conditioner A/C Hex String/code and send it.
// Args:
//   irsend: A Ptr to the IRsend object to transmit via.
//   irType: Nr. of the protocol we need to send.
//   str: A hexadecimal string containing the state to be sent.
// Returns:
//   bool: Successfully sent or not.
bool parseStringAndSendAirCon(IRsend *irsend, const decode_type_t irType,
                              const String str) {
  uint8_t strOffset = 0;
  uint8_t state[kStateSizeMax] = {0};  // All array elements are set to 0.
  uint16_t stateSize = 0;

  if (str.startsWith("0x") || str.startsWith("0X"))
    strOffset = 2;
  // Calculate how many hexadecimal characters there are.
  uint16_t inputLength = str.length() - strOffset;
  if (inputLength == 0) {
    debug("Zero length AirCon code encountered. Ignored.");
    return false;  // No input. Abort.
  }

  switch (irType) {  // Get the correct state size for the protocol.
    case DAIKIN:
      // Daikin has 2 different possible size states.
      // (The correct size, and a legacy shorter size.)
      // Guess which one we are being presented with based on the number of
      // hexadecimal digits provided. i.e. Zero-pad if you need to to get
      // the correct length/byte size.
      // This should provide backward compatiblity with legacy messages.
      stateSize = inputLength / 2;  // Every two hex chars is a byte.
      // Use at least the minimum size.
      stateSize = std::max(stateSize, kDaikinStateLengthShort);
      // If we think it isn't a "short" message.
      if (stateSize > kDaikinStateLengthShort)
        // Then it has to be at least the version of the "normal" size.
        stateSize = std::max(stateSize, kDaikinStateLength);
      // Lastly, it should never exceed the "normal" size.
      stateSize = std::min(stateSize, kDaikinStateLength);
      break;
    case FUJITSU_AC:
      // Fujitsu has four distinct & different size states, so make a best guess
      // which one we are being presented with based on the number of
      // hexadecimal digits provided. i.e. Zero-pad if you need to to get
      // the correct length/byte size.
      stateSize = inputLength / 2;  // Every two hex chars is a byte.
      // Use at least the minimum size.
      stateSize = std::max(stateSize,
                           (uint16_t) (kFujitsuAcStateLengthShort - 1));
      // If we think it isn't a "short" message.
      if (stateSize > kFujitsuAcStateLengthShort)
        // Then it has to be at least the smaller version of the "normal" size.
        stateSize = std::max(stateSize, (uint16_t) (kFujitsuAcStateLength - 1));
      // Lastly, it should never exceed the maximum "normal" size.
      stateSize = std::min(stateSize, kFujitsuAcStateLength);
      break;
    case HITACHI_AC3:
      // HitachiAc3 has two distinct & different size states, so make a best
      // guess which one we are being presented with based on the number of
      // hexadecimal digits provided. i.e. Zero-pad if you need to to get
      // the correct length/byte size.
      stateSize = inputLength / 2;  // Every two hex chars is a byte.
      // Use at least the minimum size.
      stateSize = std::max(stateSize,
                           (uint16_t) (kHitachiAc3MinStateLength));
      // If we think it isn't a "short" message.
      if (stateSize > kHitachiAc3MinStateLength)
        // Then it probably the "normal" size.
        stateSize = std::max(stateSize,
                             (uint16_t) (kHitachiAc3StateLength));
      // Lastly, it should never exceed the maximum "normal" size.
      stateSize = std::min(stateSize, kHitachiAc3StateLength);
      break;
    case MWM:
      // MWM has variable size states, so make a best guess
      // which one we are being presented with based on the number of
      // hexadecimal digits provided. i.e. Zero-pad if you need to to get
      // the correct length/byte size.
      stateSize = inputLength / 2;  // Every two hex chars is a byte.
      // Use at least the minimum size.
      stateSize = std::max(stateSize, (uint16_t) 3);
      // Cap the maximum size.
      stateSize = std::min(stateSize, kStateSizeMax);
      break;
    case SAMSUNG_AC:
      // Samsung has two distinct & different size states, so make a best guess
      // which one we are being presented with based on the number of
      // hexadecimal digits provided. i.e. Zero-pad if you need to to get
      // the correct length/byte size.
      stateSize = inputLength / 2;  // Every two hex chars is a byte.
      // Use at least the minimum size.
      stateSize = std::max(stateSize, (uint16_t) (kSamsungAcStateLength));
      // If we think it isn't a "normal" message.
      if (stateSize > kSamsungAcStateLength)
        // Then it probably the extended size.
        stateSize = std::max(stateSize,
                             (uint16_t) (kSamsungAcExtendedStateLength));
      // Lastly, it should never exceed the maximum "extended" size.
      stateSize = std::min(stateSize, kSamsungAcExtendedStateLength);
      break;
    default:  // Everything else.
      stateSize = IRsend::defaultBits(irType) / 8;
      if (!stateSize || !hasACState(irType)) {
        // Not a protocol we expected. Abort.
        debug("Unexpected AirCon protocol detected. Ignoring.");
        return false;
      }
  }
  if (inputLength > stateSize * 2) {
    debug("AirCon code to large for the given protocol.");
    return false;
  }

  // Ptr to the least significant byte of the resulting state for this protocol.
  uint8_t *statePtr = &state[stateSize - 1];

  // Convert the string into a state array of the correct length.
  for (uint16_t i = 0; i < inputLength; i++) {
    // Grab the next least sigificant hexadecimal digit from the string.
    uint8_t c = tolower(str[inputLength + strOffset - i - 1]);
    if (isxdigit(c)) {
      if (isdigit(c))
        c -= '0';
      else
        c = c - 'a' + 10;
    } else {
      debug("Aborting! Non-hexadecimal char found in AirCon state:");
      debug(str.c_str());
      return false;
    }
    if (i % 2 == 1) {  // Odd: Upper half of the byte.
      *statePtr += (c << 4);
      statePtr--;  // Advance up to the next least significant byte of state.
    } else {  // Even: Lower half of the byte.
      *statePtr = c;
    }
  }
  if (!irsend->send(irType, state, stateSize)) {
    debug("Unexpected AirCon type in send request. Not sent.");
    return false;
  }
  return true;  // We were successful as far as we can tell.
}

// Count how many values are in the String.
// Args:
//   str:  String containing the values.
//   sep:  Character that separates the values.
// Returns:
//   The number of values found in the String.
uint16_t countValuesInStr(const String str, char sep) {
  int16_t index = -1;
  uint16_t count = 1;
  do {
    index = str.indexOf(sep, index + 1);
    count++;
  } while (index != -1);
  return count;
}

// Dynamically allocate an array of uint16_t's.
// Args:
//   size:  Nr. of uint16_t's need to be in the new array.
// Returns:
//   A Ptr to the new array. Restarts the ESP if it fails.
uint16_t * newCodeArray(const uint16_t size) {
  uint16_t *result;

  result = reinterpret_cast<uint16_t*>(malloc(size * sizeof(uint16_t)));
  // Check we malloc'ed successfully.
  if (result == NULL)  // malloc failed, so give up.
    doRestart(
        "FATAL: Can't allocate memory for an array for a new message! "
        "Forcing a reboot!", true);  // Send to serial only as we are in low mem
  return result;
}

// Parse a GlobalCache String/code and send it.
// Args:
//   irsend: A ptr to the IRsend object to transmit via.
//   str: A GlobalCache formatted String of comma separated numbers.
//        e.g. "38000,1,1,170,170,20,63,20,63,20,63,20,20,20,20,20,20,20,20,20,
//              20,20,63,20,63,20,63,20,20,20,20,20,20,20,20,20,20,20,20,20,63,
//              20,20,20,20,20,20,20,20,20,20,20,20,20,63,20,20,20,63,20,63,20,
//              63,20,63,20,63,20,63,20,1798"
//        Note: The leading "1:1,1," of normal GC codes should be removed.
// Returns:
//   bool: Successfully sent or not.
bool parseStringAndSendGC(IRsend *irsend, const String str) {
  uint16_t count;
  uint16_t *code_array;
  String tmp_str;

  // Remove the leading "1:1,1," if present.
  if (str.startsWith("1:1,1,"))
    tmp_str = str.substring(6);
  else
    tmp_str = str;

  // Find out how many items there are in the string.
  count = countValuesInStr(tmp_str, ',');

  // Now we know how many there are, allocate the memory to store them all.
  code_array = newCodeArray(count);

  // Now convert the strings to integers and place them in code_array.
  count = 0;
  uint16_t start_from = 0;
  int16_t index = -1;
  do {
    index = tmp_str.indexOf(',', start_from);
    code_array[count] = tmp_str.substring(start_from, index).toInt();
    start_from = index + 1;
    count++;
  } while (index != -1);
  irsend->sendGC(code_array, count);  // All done. Send it.
  free(code_array);  // Free up the memory allocated.
  if (count > 0)
    return true;  // We sent something.
  return false;  // We probably didn't.
}

// Parse a Pronto Hex String/code and send it.
// Args:
//   irsend: A ptr to the IRsend object to transmit via.
//   str: A comma-separated String of nr. of repeats, then hexadecimal numbers.
//        e.g. "R1,0000,0067,0000,0015,0060,0018,0018,0018,0030,0018,0030,0018,
//              0030,0018,0018,0018,0030,0018,0018,0018,0018,0018,0030,0018,
//              0018,0018,0030,0018,0030,0018,0030,0018,0018,0018,0018,0018,
//              0030,0018,0018,0018,0018,0018,0030,0018,0018,03f6"
//              or
//              "0000,0067,0000,0015,0060,0018". i.e. without the Repeat value
//        Requires at least kProntoMinLength comma-separated values.
//        sendPronto() only supports raw pronto code types, thus so does this.
//   repeats:  Nr. of times the message is to be repeated.
//             This value is ignored if an embeddd repeat is found in str.
// Returns:
//   bool: Successfully sent or not.
bool parseStringAndSendPronto(IRsend *irsend, const String str,
                              uint16_t repeats) {
  uint16_t count;
  uint16_t *code_array;
  int16_t index = -1;
  uint16_t start_from = 0;

  // Find out how many items there are in the string.
  count = countValuesInStr(str, ',');

  // Check if we have the optional embedded repeats value in the code string.
  if (str.startsWith("R") || str.startsWith("r")) {
    // Grab the first value from the string, as it is the nr. of repeats.
    index = str.indexOf(',', start_from);
    repeats = str.substring(start_from + 1, index).toInt();  // Skip the 'R'.
    start_from = index + 1;
    count--;  // We don't count the repeats value as part of the code array.
  }

  // We need at least kProntoMinLength values for the code part.
  if (count < kProntoMinLength) return false;

  // Now we know how many there are, allocate the memory to store them all.
  code_array = newCodeArray(count);

  // Rest of the string are values for the code array.
  // Now convert the hex strings to integers and place them in code_array.
  count = 0;
  do {
    index = str.indexOf(',', start_from);
    // Convert the hexadecimal value string to an unsigned integer.
    code_array[count] = strtoul(str.substring(start_from, index).c_str(),
                                NULL, 16);
    start_from = index + 1;
    count++;
  } while (index != -1);

  irsend->sendPronto(code_array, count, repeats);  // All done. Send it.
  free(code_array);  // Free up the memory allocated.
  if (count > 0)
    return true;  // We sent something.
  return false;  // We probably didn't.
}

// Parse an IRremote Raw Hex String/code and send it.
// Args:
//   irsend: A ptr to the IRsend object to transmit via.
//   str: A comma-separated String containing the freq and raw IR data.
//        e.g. "38000,9000,4500,600,1450,600,900,650,1500,..."
//        Requires at least two comma-separated values.
//        First value is the transmission frequency in Hz or kHz.
// Returns:
//   bool: Successfully sent or not.
bool parseStringAndSendRaw(IRsend *irsend, const String str) {
  uint16_t count;
  uint16_t freq = 38000;  // Default to 38kHz.
  uint16_t *raw_array;

  // Find out how many items there are in the string.
  count = countValuesInStr(str, ',');

  // We expect the frequency as the first comma separated value, so we need at
  // least two values. If not, bail out.
  if (count < 2)  return false;
  count--;  // We don't count the frequency value as part of the raw array.

  // Now we know how many there are, allocate the memory to store them all.
  raw_array = newCodeArray(count);

  // Grab the first value from the string, as it is the frequency.
  int16_t index = str.indexOf(',', 0);
  freq = str.substring(0, index).toInt();
  uint16_t start_from = index + 1;
  // Rest of the string are values for the raw array.
  // Now convert the strings to integers and place them in raw_array.
  count = 0;
  do {
    index = str.indexOf(',', start_from);
    raw_array[count] = str.substring(start_from, index).toInt();
    start_from = index + 1;
    count++;
  } while (index != -1);

  irsend->sendRaw(raw_array, count, freq);  // All done. Send it.
  free(raw_array);  // Free up the memory allocated.
  if (count > 0)
    return true;  // We sent something.
  return false;  // We probably didn't.
}

uint8_t getDefaultIrSendIdx(void) {
  for (uint16_t i = 0; i < kNrOfIrTxGpios; i++)
    if (IrSendTable[i] != NULL) return i;
  return 0;
}

IRsend* getDefaultIrSendPtr(void) {
  return IrSendTable[getDefaultIrSendIdx()];
}

// Parse the URL args to find the IR code.
void handleIr(void) {

  ___HTTP_PROTECT___

  uint64_t data = 0;
  String data_str = "";
  decode_type_t ir_type = decode_type_t::NEC;  // Default to NEC codes.
  uint16_t nbits = 0;
  uint16_t repeat = 0;
  int16_t channel = -1;

  for (uint16_t i = 0; i < server.args(); i++) {
    if (server.argName(i).equals(KEY_TYPE) ||
        server.argName(i).equals(KEY_PROTOCOL)) {
      ir_type = strToDecodeType(server.arg(i).c_str());
    } else if (server.argName(i).equals(KEY_CODE)) {
      data = getUInt64fromHex(server.arg(i).c_str());
      data_str = server.arg(i);
    } else if (server.argName(i).equals(KEY_BITS)) {
      nbits = server.arg(i).toInt();
    } else if (server.argName(i).equals(KEY_REPEAT)) {
      repeat = server.arg(i).toInt();
    } else if (server.argName(i).equals(KEY_CHANNEL)) {
      channel = server.arg(i).toInt();
    }
  }
  debug("New code received via HTTP");
  IRsend *tx_ptr = getDefaultIrSendPtr();
  if (channel >= 0 && channel < kNrOfIrTxGpios && IrSendTable[channel] != NULL)
    tx_ptr = IrSendTable[channel];
  lastSendSucceeded = sendIRCode(tx_ptr, ir_type, data, data_str.c_str(), nbits,
                                 repeat);
  String html = htmlHeader(F("IR command sent!"));
  html += addJsReloadUrl(kUrlRoot, kQuickDisplayTime, true);
  html += htmlEnd();
  server.send(200, "text/html", html);
}

void handleNotFound(void) {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i < server.args(); i++)
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  server.send(404, "text/plain", message);
}

void setup_wifi(void) {

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }


  strncpy(Hostname, HOSTNAME, kHostnameLength);
  strncat(Hostname,String(kChipId, HEX).c_str(),kHostnameLength);
  strncpy(HttpUsername, HTTPUSERNAME, kUsernameLength);
  strncpy(HttpPassword, HTTPPASSWORD, kPasswordLength);

  strncpy(MqttServer, MQTTSERVER, kHostnameLength);
  strncpy(MqttPort, MQTTPORT, kPortLength);
  strncpy(MqttUsername, MQTTUSERNAME, kUsernameLength);
  strncpy(MqttPassword, MQTTPASSWORD, kPasswordLength);
  strncpy(MqttPrefix, "irhvacgws/", kHostnameLength);
  strncat(MqttPrefix, Hostname, kHostnameLength);
  

  debug(String(String("WiFi connected. IP address: ")+WiFi.localIP().toString()+String(" Hostname: ")+String(Hostname)).c_str());
  //debug(WiFi.localIP().toString().c_str());  
}

void setup_ac() {
  IRac *ac_ptr = climate[0];
  if (ac_ptr != NULL) {
        updateClimate(&(ac_ptr->next), KEY_PROTOCOL, "", AC_MAKE);
        updateClimate(&(ac_ptr->next), KEY_MODEL, "", AC_MODEL); 
  }
}

void init_vars(void) {
  // If we have a prefix already, use it. Otherwise use the hostname.
  if (!strlen(MqttPrefix)) strncpy(MqttPrefix, Hostname, kHostnameLength);
  // Topic we send back acknowledgements on.
  MqttAck = String(MqttPrefix) + '/' + MQTT_ACK;
  // Sub-topic we get new commands from.
  MqttSend = String(MqttPrefix) + '/' + MQTT_SEND;
  // Topic we send received IRs to.
  MqttRecv = String(MqttPrefix) + '/' + MQTT_RECV;
  // Topic we send log messages to.
  MqttLog = String(MqttPrefix) + '/' + MQTT_LOG;
  // Topic for the Last Will & Testament.
  MqttLwt = String(MqttPrefix) + '/' + MQTT_LWT;
  // Sub-topic for the climate topics.
  MqttClimate = String(MqttPrefix) + '/' + MQTT_CLIMATE;
  // Sub-topic for the climate command topics.
  MqttClimateCmnd = MqttClimate + '/' + MQTT_CLIMATE_CMND + '/';
  // Sub-topic for the climate stat topics.
#if MQTT_DISCOVERY_ENABLE
  MqttDiscovery = "homeassistant/climate/" + String(MqttPrefix) + "/config";
  MqttUniqueId = WiFi.macAddress();
  MqttUniqueId.replace(":", "");
#endif  // MQTT_DISCOVERY_ENABLE
  MqttHAName = String(Hostname) + "_aircon";
  // Create a unique MQTT client id.
  MqttClientId = String(Hostname);  // + String(kChipId, HEX); since Hostname already has that now..
}

void setup(void) {
#if DEBUG
  if (!isSerialGpioUsedByIr()) {
#if defined(ESP8266)
    // Use SERIAL_TX_ONLY so that the RX pin can be freed up for GPIO/IR use.
    Serial.begin(BAUD_RATE, SERIAL_8N1, SERIAL_TX_ONLY);
#else  // ESP8266
    Serial.begin(BAUD_RATE, SERIAL_8N1);
#endif  // ESP8266
    while (!Serial)  // Wait for the serial connection to be establised.
      delay(50);
    Serial.println();
    debug("IRMQTTServer " _MY_VERSION_ " has booted.");
  }
#endif  // DEBUG

  setup_wifi();

#if DEBUG
  // After the config has been loaded, check again if we are using a Serial GPIO
  if (isSerialGpioUsedByIr()) Serial.end();
#endif  // DEBUG

  channel_re.reserve(kNrOfIrTxGpios * 3);
  // Initialise all the IR transmitters.
  for (uint8_t i = 0; i < kNrOfIrTxGpios; i++) {
    if (txGpioTable[i] == kGpioUnused) {
      IrSendTable[i] = NULL;
      climate[i] = NULL;
    } else {
      IrSendTable[i] = new IRsend(txGpioTable[i], kInvertTxOutput);
      if (IrSendTable[i] != NULL) {
        IrSendTable[i]->begin();
        offset = IrSendTable[i]->calibrate();
      }
      climate[i] = new IRac(txGpioTable[i], kInvertTxOutput);
      if (climate[i] != NULL && i > 0) channel_re += '_' + String(i) + '|';
    }
  }

  debug(String("Setting up AC: "+String(AC_MAKE)+" Model: "+String(AC_MODEL)).c_str());
  setup_ac();
  
  lastClimateSource = F("None");
  if (channel_re.length() == 1) {
    channel_re = "";
  } else {
    channel_re.remove(channel_re.length() - 1, 1);  // Remove the last char.
    channel_re += F(")?");
  }
  // Wait a bit for things to settle.
  delay(500);

  lastReconnectAttempt = 0;

#if MDNS_ENABLE
#if defined(ESP8266)
  if (mdns.begin(Hostname, WiFi.localIP())) {
#else  // ESP8266
  if (mdns.begin(Hostname)) {
#endif  // ESP8266
    debug("MDNS responder started");
  }
#endif  // MDNS_ENABLE

  // Setup the root web page.
  server.on(kUrlRoot, handleRoot);
  // Setup the page to handle web-based IR codes.
  server.on("/ir", handleIr);
  // Setup the aircon page.
  server.on(kUrlAircon, handleAirCon);
  // Setup the aircon update page.
  server.on("/aircon/set", handleAirConSet);
  // Setup the info page.
  server.on(kUrlInfo, handleInfo);
  // Setup the admin page.
  server.on(kUrlAdmin, handleAdmin);
  // Setup a reset page to cause information to be reset.
  server.on(kUrlWipe, handleReset);
  // Reboot url
  server.on(kUrlReboot, handleReboot);
#if MQTT_CLEAR_ENABLE
  // Clear settings saved to MQTT as retained messages.
  server.on(kUrlClearMqtt, handleClearMqtt);
#endif  // MQTT_CLEAR_ENABLE
#if MQTT_DISCOVERY_ENABLE
  // MQTT Discovery url
  server.on(kUrlSendDiscovery, handleSendMqttDiscovery);
#endif  // MQTT_DISCOVERY_ENABLE
  // Finish setup of the mqtt clent object.
  mqtt_client.setServer(MqttServer, atoi(MqttPort));
  mqtt_client.setCallback(mqttCallback);
  // Set various variables
  init_vars();

#if FIRMWARE_OTA
  // Setup the URL to allow Over-The-Air (OTA) firmware updates.
  if (strlen(HttpPassword)) {  // Allow if password is set.
    server.on("/update", HTTP_POST, [](){
        mqttLog("Attempting firmware update & reboot");
        delay(1000);
        server.send(200, "text/html",
            htmlHeader(F("Updating firmware")) +
            "<hr>"
            "<h3>Warning! Don't " D_STR_POWER " " D_STR_OFF " the device for "
            "60 " D_STR_SECONDS "!</h3>"
            "<p>The firmware is uploading and will try to flash itself. "
            "It is important to not interrupt the process.</p>"
            "<p>The firmware upload seems to have " +
            String(Update.hasError() ? "FAILED!" : "SUCCEEDED!") +
            " Rebooting! </p>" +
            addJsReloadUrl(kUrlRoot, 20, true) +
            htmlEnd());
        doRestart("Post firmware reboot.");
      }, [](){
        if (!server.authenticate(HttpUsername, HttpPassword)) {
          debug("Basic HTTP authentication failure for /update.");
          return server.requestAuthentication();
        }
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          debug("Update:");
          debug(upload.filename.c_str());
#if defined(ESP8266)
          WiFiUDP::stopAll();
#endif  // defined(ESP8266)
          if (!Update.begin(maxSketchSpace())) {  // start with max available
#if DEBUG
            if (!isSerialGpioUsedByIr())
              Update.printError(Serial);
#endif  // DEBUG
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) !=
              upload.currentSize) {
#if DEBUG
            if (!isSerialGpioUsedByIr())
              Update.printError(Serial);
#endif  // DEBUG
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          // true to set the size to the current progress
          if (Update.end(true)) {
            debug("Update Success:");
            debug(String(upload.totalSize).c_str());
            debug("Rebooting...");
          }
        }
        yield();
      });
    }
#endif  // FIRMWARE_OTA

  // Set up an error page.
  server.onNotFound(handleNotFound);

  server.begin();
  debug("HTTP server started");
}

String genStatTopic(const uint16_t channel) {
  if (channel)  // Never use the '*_0' state channel.
    return MqttClimate + "_" + String(channel) + '/' + MQTT_CLIMATE_STAT + '/';
  else
    return MqttClimate + '/' + MQTT_CLIMATE_STAT + '/';
}

// MQTT subscribing to topic
void subscribing(const String topic_name) {
  // subscription to topic for receiving data with QoS.
  if (mqtt_client.subscribe(topic_name.c_str(), QOS))
    debug("Subscription OK to:");
  else
    debug("Subscription FAILED to:");
  debug(topic_name.c_str());
}

// Un-subscribe from a MQTT topic
void unsubscribing(const String topic_name) {
  // subscription to topic for receiving data with QoS.
  if (mqtt_client.unsubscribe(topic_name.c_str()))
    debug("Unsubscribed OK from:");
  else
    debug("FAILED to unsubscribe from:");
  debug(topic_name.c_str());
}

void mqttLog(const char* str) {
  debug(str);
  mqtt_client.publish(MqttLog.c_str(), str);
  mqttSentCounter++;
}

bool reconnect(void) {
  // Loop a few times or until we're reconnected
  uint16_t tries = 1;
  while (!mqtt_client.connected() && tries <= 3) {
    int connected = false;
    // Attempt to connect
    debug(("Attempting MQTT connection to " + String(MqttServer) + ":" +
           String(MqttPort) + "... ").c_str());
    if (strcmp(MqttUsername, "") && strcmp(MqttPassword, "")) {
      debug("Using mqtt username/password to connect.");
      connected = mqtt_client.connect(MqttClientId.c_str(),
                                      MqttUsername, MqttPassword,
                                      MqttLwt.c_str(),
                                      QOS, true, kLwtOffline);

    } else {
      debug("Using password-less mqtt connection.");
      connected = mqtt_client.connect(MqttClientId.c_str(), MqttLwt.c_str(),
                                      QOS, true, kLwtOffline);
    }
    if (connected) {
    // Once connected, publish an announcement...
      mqttLog("(Re)Connected.");

      // Update Last Will & Testament to say we are back online.
      mqtt_client.publish(MqttLwt.c_str(), kLwtOnline, true);
      mqttSentCounter++;

      // Subscribing to topic(s)
      subscribing(MqttSend);  // General base topic.
      subscribing(MqttClimateCmnd + '+');  // Base climate command topics
      // Per channel topics
      for (uint16_t i = 0; i < kNrOfIrTxGpios; i++) {
        // General
        if (IrSendTable[i] != NULL)
          subscribing(MqttSend + '_' + String(i));
        // Climate
        if (climate[i] != NULL)
          subscribing(MqttClimate + '_' + String(i) + '/' + MQTT_CLIMATE_CMND +
                      '/' + '+');
      }

      // subscribe to current temp sensors
      subscribing(CURRENT_HMDT_SENSOR_TOPIC);  // current humidity sensor
      subscribing(CURRENT_TEMP_SENSOR_TOPIC);  // current temp sensor
      
    } else {
      debug(("failed, rc=" + String(mqtt_client.state()) +
            " Try again in a bit.").c_str());
      // Wait for a bit before retrying
      delay(tries << 7);  // Linear increasing back-off (x128)
    }
    tries++;
  }
  return mqtt_client.connected();
}

// Return a string containing the comma separated list of MQTT command topics.
String listOfCommandTopics(void) {
  String result = MqttSend;
  for (uint16_t i = 0; i < kNrOfIrTxGpios; i++) {
    result += ", " + MqttSend + '_' + String(i);
  }
  return result;
}

#if MQTT_DISCOVERY_ENABLE
// MQTT Discovery web page
void handleSendMqttDiscovery(void) {

  ___HTTP_PROTECT___

  server.send(200, "text/html",
      htmlHeader(F("Sending MQTT Discovery message")) +
      htmlMenu() +
      "<p>The Home Assistant MQTT Discovery message is being sent to topic: " +
      MqttDiscovery + ". It will show up in Home Assistant in a few seconds."
      "</p>"
      "<h3>Warning!</h3>"
      "<p>Home Assistant's config for this device is reset each time this is "
      " is sent.</p>" +
      addJsReloadUrl(kUrlRoot, kRebootTime, true) +
      htmlEnd());
  sendMQTTDiscovery(MqttDiscovery.c_str());
}
#endif  // MQTT_DISCOVERY_ENABLE

void doBroadcast(TimerMs *timer, const uint32_t interval,
                 IRac *climate[], const bool retain,
                 const bool force) {
  if (force || (!lockMqttBroadcast && timer->elapsed() > interval)) {
    debug("Sending MQTT stat update broadcast.");
    for (uint16_t i = 0; i < kNrOfIrTxGpios; i++) {
      String stat_topic = genStatTopic(i);
      sendClimate(stat_topic, retain, true, false, true, climate[i]);
#if MQTT_CLIMATE_JSON
      sendJsonState(climate[i]->next, stat_topic + KEY_JSON);
#endif  // MQTT_CLIMATE_JSON
    }
    sendInt(genStatTopic(0) + KEY_CURR_TEMP, latest_current_temperature.toInt(), true);
    timer->reset();  // It's been sent, so reset the timer.
    hasBroadcastBeenSent = true;
  }
}

void receivingMQTT(String const topic_name, String const callback_str) {
  uint64_t code = 0;
  uint16_t nbits = 0;
  uint16_t repeat = 0;
  uint8_t channel = getDefaultIrSendIdx();  // Default to first usable channel.

  debug("Receiving data by MQTT topic:");
  debug(topic_name.c_str());
  debug("with payload:");
  debug(callback_str.c_str());
  
  
  if (topic_name.startsWith(CURRENT_HMDT_SENSOR_TOPIC)) {
    latest_current_humidity=callback_str;
    return;
  } else if (topic_name.startsWith(CURRENT_TEMP_SENSOR_TOPIC)) {
    latest_current_temperature=callback_str;
    return;
  }
  
  
  // Save the message as the last command seen (global).
  lastMqttCmdTopic = topic_name;
  lastMqttCmd = callback_str;
  lastMqttCmdTime = millis();
  mqttRecvCounter++;

  // Check if a specific channel was requested by looking for a "*_[0-9]" suffix
  // Or is for a specific ac/climate channel. e.g. "*/ac_[1-9]"
  debug(("Checking for channel number in " + topic_name).c_str());
  for (uint16_t i = 0; i < kNrOfIrTxGpios; i++) {
    if (topic_name.endsWith("_" + String(i)) ||
        (i > 0 && topic_name.startsWith(MqttClimate + "_" + String(i)))) {
      channel = i;
      break;
    }
  }
  debug(("Channel = " + String(channel)).c_str());
  // Is it a climate topic?
  if (topic_name.startsWith(MqttClimate)) {
    String alt_cmnd_topic = MqttClimate + "_" + String(channel) + '/' +
        MQTT_CLIMATE_CMND + '/';
    // Also accept climate commands on the '*_0' channel.
    String cmnd_topic = topic_name.startsWith(alt_cmnd_topic) ? alt_cmnd_topic
                                                              : MqttClimateCmnd;
    String stat_topic = genStatTopic(channel);
    if (topic_name.startsWith(cmnd_topic)) {
      debug("It's a climate command topic");
      updateClimate(&(climate[channel]->next), topic_name, cmnd_topic,
                    callback_str);
      // Handle the special command for forcing a resend of the state via IR.
      bool force_resend = false;
      if (topic_name.equals(cmnd_topic + KEY_RESEND) &&
          callback_str.equalsIgnoreCase(KEY_RESEND)) {
        force_resend = true;
        mqttLog("Climate resend requested.");
      }
      if (sendClimate(stat_topic, true, false, force_resend, true,
                      climate[channel]) && !force_resend)
        lastClimateSource = F("MQTT");
    } else if (topic_name.startsWith(stat_topic)) {
      debug("It's a climate state topic. Update internal state and DON'T send");
      updateClimate(&(climate[channel]->next), topic_name, stat_topic,
                    callback_str);
    }
    return;  // We are done for now.
  }

  debug(("Using transmit channel " + String(static_cast<int>(channel)) +
         " / GPIO " + String(static_cast<int>(txGpioTable[channel]))).c_str());
  // Make a copy of the callback string as strtok destroys it.
  char* callback_c_str = strdup(callback_str.c_str());
  debug("MQTT Payload (raw):");
  debug(callback_c_str);

  // Chop up the str into command chunks.
  // i.e. commands in a sequence are delimitered by ';'.
  char* sequence_tok_ptr;
  for (char* sequence_item = strtok_r(callback_c_str, kSequenceDelimiter,
                                      &sequence_tok_ptr);
       sequence_item != NULL;
       sequence_item = strtok_r(NULL, kSequenceDelimiter, &sequence_tok_ptr)) {
    // Now, process each command individually.
    char* tok_ptr;
    // Make a copy of the sequence_item str as strtok_r stomps on it.
    char* ircommand = strdup(sequence_item);
    // Check if it is a pause command.
    switch (ircommand[0]) {
      case kPauseChar:
        {  // It's a pause. Everything after the 'P' should be a number.
          int32_t msecs = std::min((int32_t) strtoul(ircommand + 1, NULL, 10),
                                   kMaxPauseMs);
          delay(msecs);
          mqtt_client.publish(MqttAck.c_str(),
                              String(kPauseChar + String(msecs)).c_str());
          mqttSentCounter++;
          break;
        }
      default:  // It's an IR command.
        {
          // Get the numeric protocol type.
          decode_type_t ir_type = (decode_type_t)atoi(strtok_r(
              ircommand, kCommandDelimiter, &tok_ptr));
          char* next = strtok_r(NULL, kCommandDelimiter, &tok_ptr);
          // If there is unparsed string left, try to convert it assuming it's
          // hex.
          if (next != NULL) {
            code = getUInt64fromHex(next);
            next = strtok_r(NULL, kCommandDelimiter, &tok_ptr);
          } else {
            // We require at least two value in the string. Give up.
            break;
          }
          // If there is still string left, assume it is the bit size.
          if (next != NULL) {
            nbits = atoi(next);
            next = strtok_r(NULL, kCommandDelimiter, &tok_ptr);
          }
          // If there is still string left, assume it is the repeat count.
          if (next != NULL)
            repeat = atoi(next);
          // send received MQTT value by IR signal
          lastSendSucceeded = sendIRCode(
              IrSendTable[channel], ir_type, code,
              strchr(sequence_item, kCommandDelimiter[0]) + 1, nbits, repeat);
        }
    }
    free(ircommand);
  }
  free(callback_c_str);
}

// Callback function, when we receive an MQTT value on the topics
// subscribed this function is called
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // In order to republish this payload, a copy must be made
  // as the orignal payload buffer will be overwritten whilst
  // constructing the PUBLISH packet.
  // Allocate the correct amount of memory for the payload copy
  byte* payload_copy = reinterpret_cast<byte*>(malloc(length + 1));
  if (payload_copy == NULL) {
    debug("Can't allocate memory for `payload_copy`. Skipping callback!");
    return;
  }
  // Copy the payload to the new buffer
  memcpy(payload_copy, payload, length);

  // Conversion to a printable string
  payload_copy[length] = '\0';
  String callback_string = String(reinterpret_cast<char*>(payload_copy));
  String topic_name = String(reinterpret_cast<char*>(topic));

  // launch the function to treat received data
  receivingMQTT(topic_name, callback_string);

  // Free the memory
  free(payload_copy);
}

#if MQTT_DISCOVERY_ENABLE
void sendMQTTDiscovery(const char *topic) {
  String discovery_message=String(
      "{"
      "\"~\":\"" + MqttClimate + "\","
      "\"name\":\"" + MqttHAName + "\","
      "\"pow_cmd_t\":\"~/" MQTT_CLIMATE_CMND "/" KEY_POWER "\","
      "\"mode_cmd_t\":\"~/" MQTT_CLIMATE_CMND "/" KEY_MODE "\","
      "\"mode_stat_t\":\"~/" MQTT_CLIMATE_STAT "/" KEY_MODE "\","
      // I don't know why, but the modes need to be lower case to work with
      // Home Assistant & Google Home.
      "\"modes\":[\"off\",\"auto\",\"cool\",\"heat\",\"dry\",\"fan_only\"],"
      "\"temp_cmd_t\":\"~/" MQTT_CLIMATE_CMND "/" KEY_TEMP "\","
      "\"temp_stat_t\":\"~/" MQTT_CLIMATE_STAT "/" KEY_TEMP "\","
      "\"curr_temp_t\":\"~/" MQTT_CLIMATE_STAT "/" KEY_CURR_TEMP "\","
      "\"min_temp\":\"16\","
      "\"max_temp\":\"30\","
      "\"temp_step\":\"1\","
      "\"fan_mode_cmd_t\":\"~/" MQTT_CLIMATE_CMND "/" KEY_FANSPEED "\","
      "\"fan_mode_stat_t\":\"~/" MQTT_CLIMATE_STAT "/" KEY_FANSPEED "\","
      "\"fan_modes\":[\"" D_STR_AUTO "\",\"" D_STR_MIN "\",\"" D_STR_LOW "\",\""
                      D_STR_MEDIUM "\",\"" D_STR_HIGH "\",\"" D_STR_MAX "\"],"
      "\"swing_mode_cmd_t\":\"~/" MQTT_CLIMATE_CMND "/" KEY_SWINGV "\","
      "\"swing_mode_stat_t\":\"~/" MQTT_CLIMATE_STAT "/" KEY_SWINGV "\","
      "\"swing_modes\":[\"" D_STR_OFF "\",\"" D_STR_AUTO "\",\"" D_STR_HIGHEST
                        "\",\"" D_STR_HIGH "\",\"" D_STR_MIDDLE "\",\""
                        D_STR_LOW "\",\"" D_STR_LOWEST "\"],"
      "\"uniq_id\":\"" + MqttUniqueId + "\","
      "\"device\":{"
        "\"identifiers\":[\"" + MqttUniqueId + "\"],"
        "\"connections\":[[\"mac\",\"" + WiFi.macAddress() + "\"]],"
        "\"manufacturer\":\"IRremoteESP8266\","
        "\"model\":\"IRMQTTServer\","
        "\"name\":\"" + Hostname + "\","
        "\"sw_version\":\"" _MY_VERSION_ "\""
        "}"
      "}");

  mqttLog(String(String("sending the following discovery message to: ")+String(topic)).c_str());
  mqttLog(discovery_message.c_str());        
  if (mqtt_client.publish(topic, discovery_message.c_str(),true)) {
    mqttLog("MQTT climate discovery successful sent.");
    hasDiscoveryBeenSent = true;
    lastDiscovery.reset();
    mqttSentCounter++;
  } else {
    mqttLog("MQTT climate discovery FAILED to send.");
  }
}
#endif  // MQTT_DISCOVERY_ENABLE

void loop(void) {
  server.handleClient();  // Handle any web activity

  uint32_t now = millis();
  // MQTT client connection management
  if (!mqtt_client.connected()) {
    if (wasConnected) {
      lastDisconnectedTime = now;
      wasConnected = false;
      mqttDisconnectCounter++;
    }
    // Reconnect if it's longer than kMqttReconnectTime since we last tried.
    if (now - lastReconnectAttempt > kMqttReconnectTime) {
      lastReconnectAttempt = now;
      debug("client mqtt not connected, trying to connect");
      // Attempt to reconnect
      if (reconnect()) {
        lastReconnectAttempt = 0;
        wasConnected = true;
        if (boot) {
          mqttLog("IRMQTTServer " _MY_VERSION_ " just booted");
          boot = false;
        } else {
          mqttLog(String(
              "IRMQTTServer just (re)connected to MQTT. Lost connection about "
              + timeSince(lastConnectedTime)).c_str());
        }
        lastConnectedTime = now;
        debug("successful client mqtt connection");
        if (lockMqttBroadcast) {
          // Attempt to fetch back any Climate state stored in MQTT retained
          // messages on the MQTT broker.
          mqttLog("Started listening for previous state.");
          for (uint16_t i = 0; i < kNrOfIrTxGpios; i++)
            subscribing(genStatTopic(i) + '+');
          statListenTime.reset();
        }
      }
    }
  } else {
    // MQTT loop
    lastConnectedTime = now;
    mqtt_client.loop();
    if (lockMqttBroadcast && statListenTime.elapsed() > kStatListenPeriodMs) {
      for (uint16_t i = 0; i < kNrOfIrTxGpios; i++) {
        String stat_topic = genStatTopic(i);
        unsubscribing(stat_topic + '+');
        // Did something change?
        if (climate[i] != NULL && climate[i]->hasStateChanged()) {
          sendClimate(stat_topic, true, false, false,
                      MQTT_CLIMATE_IR_SEND_ON_RESTART, climate[i]);
          lastClimateSource = F("MQTT (via retain)");
          mqttLog("The state was recovered from MQTT broker.");
        }
      }
      mqttLog("Finished listening for previous state.");
      lockMqttBroadcast = false;  // Release the lock so we can broadcast again.
    }
    // Periodically send all of the climate state via MQTT.
    doBroadcast(&lastBroadcast, kBroadcastPeriodMs, climate, false, false);
  }
  delay(100);
}

// Arduino framework doesn't support strtoull(), so make our own one.
uint64_t getUInt64fromHex(char const *str) {
  uint64_t result = 0;
  uint16_t offset = 0;
  // Skip any leading '0x' or '0X' prefix.
  if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) offset = 2;
  for (; isxdigit((unsigned char)str[offset]); offset++) {
    char c = str[offset];
    result *= 16;
    if (isdigit(c))
      result += c - '0';  // '0' .. '9'
    else if (isupper(c))
      result += c - 'A' + 10;  // 'A' .. 'F'
    else
      result += c - 'a' + 10;  // 'a' .. 'f'
  }
  return result;
}

// Transmit the given IR message.
//
// Args:
//   irsend:   A pointer to a IRsend object to transmit via.
//   ir_type:  enum of the protocol to be sent.
//   code:     Numeric payload of the IR message. Most protocols use this.
//   code_str: The unparsed code to be sent. Used by complex protocol encodings.
//   bits:     Nr. of bits in the protocol. 0 means use the protocol's default.
//   repeat:   Nr. of times the message is to be repeated. (Not all protocols.)
// Returns:
//   bool: Successfully sent or not.
bool sendIRCode(IRsend *irsend, decode_type_t const ir_type,
                uint64_t const code, char const * code_str, uint16_t bits,
                uint16_t repeat) {
  if (irsend == NULL) return false;
  bool success = true;  // Assume success.
  // Ensure we have enough repeats.
  repeat = std::max(IRsend::minRepeats(ir_type), repeat);
  if (bits == 0) bits = IRsend::defaultBits(ir_type);
  // Create a pseudo-lock so we don't try to send two codes at the same time.
  while (lockIr)
    delay(20);
  lockIr = true;

  // send the IR message.
  switch (ir_type) {
    case decode_type_t::PRONTO:  // 25
      success = parseStringAndSendPronto(irsend, code_str, repeat);
      break;
    case decode_type_t::RAW:  // 30
      success = parseStringAndSendRaw(irsend, code_str);
      break;
    case decode_type_t::GLOBALCACHE:  // 31
      success = parseStringAndSendGC(irsend, code_str);
      break;
    default:  // Everything else.
      if (hasACState(ir_type))  // protocols with > 64 bits
        success = parseStringAndSendAirCon(irsend, ir_type, code_str);
      else  // protocols with <= 64 bits
        success = irsend->send(ir_type, code, bits, repeat);
  }
  lastSendTime = millis();
  // Release the lock.
  lockIr = false;

  // Indicate that we sent the message or not.
  if (success) {
    sendReqCounter++;
    debug("Sent the IR message:");
  } else {
    debug("Failed to send IR Message:");
  }
  debug(D_STR_PROTOCOL ": ");
  debug(String(ir_type).c_str());
  // For "long" codes we basically repeat what we got.
  if (hasACState(ir_type) || ir_type == PRONTO || ir_type == RAW ||
      ir_type == GLOBALCACHE) {
    debug(D_STR_CODE ": ");
    debug(code_str);
    // Confirm what we were asked to send was sent.

    if (success) {
      if (ir_type == PRONTO && repeat > 0)
        mqtt_client.publish(MqttAck.c_str(), (String(ir_type) +
                                              kCommandDelimiter[0] + 'R' +
                                              String(repeat) +
                                              kCommandDelimiter[0] +
                                              String(code_str)).c_str());
      else
        mqtt_client.publish(MqttAck.c_str(), (String(ir_type) +
                                              kCommandDelimiter[0] +
                                              String(code_str)).c_str());
      mqttSentCounter++;
    }
  } else {  // For "short" codes, we break it down a bit more before we report.
    debug((D_STR_CODE ": 0x" + uint64ToString(code, 16)).c_str());
    debug((D_STR_BITS ": " + String(bits)).c_str());
    debug((D_STR_REPEAT ": " + String(repeat)).c_str());
    if (success) {
      mqtt_client.publish(MqttAck.c_str(), (String(ir_type) +
                                            kCommandDelimiter[0] +
                                            uint64ToString(code, 16) +
                                            kCommandDelimiter[0] +
                                            String(bits) +
                                            kCommandDelimiter[0] +
                                            String(repeat)).c_str());
      mqttSentCounter++;
    }
  }
  return success;
}

bool sendInt(const String topic, const int32_t num, const bool retain) {
  mqttSentCounter++;
  return mqtt_client.publish(topic.c_str(), String(num).c_str(), retain);
}

bool sendBool(const String topic, const bool on, const bool retain) {
  mqttSentCounter++;
  return mqtt_client.publish(topic.c_str(), (on ? "on" : "off"), retain);
}

bool sendString(const String topic, const String str, const bool retain) {
  mqttSentCounter++;
  return mqtt_client.publish(topic.c_str(), str.c_str(), retain);
}

bool sendFloat(const String topic, const float_t temp, const bool retain) {
  mqttSentCounter++;
  return mqtt_client.publish(topic.c_str(), String(temp, 1).c_str(), retain);
}

#if MQTT_CLIMATE_JSON
void sendJsonState(const stdAc::state_t state, const String topic,
                   const bool retain, const bool ha_mode) {
  DynamicJsonDocument json(kJsonAcStateMaxSize);
  json[KEY_PROTOCOL] = typeToString(state.protocol);
  json[KEY_MODEL] = state.model;
  json[KEY_POWER] = IRac::boolToString(state.power);
  json[KEY_MODE] = IRac::opmodeToString(state.mode);
  // Home Assistant wants mode to be off if power is also off & vice-versa.
  if (ha_mode && (state.mode == stdAc::opmode_t::kOff || !state.power)) {
    json[KEY_MODE] = IRac::opmodeToString(stdAc::opmode_t::kOff);
    json[KEY_POWER] = IRac::boolToString(false);
  }
  json[KEY_CELSIUS] = IRac::boolToString(state.celsius);
  json[KEY_TEMP] = state.degrees;
  json[KEY_CURR_TEMP] = latest_current_temperature;
  json[KEY_FANSPEED] = IRac::fanspeedToString(state.fanspeed);
  json[KEY_SWINGV] = IRac::swingvToString(state.swingv);
  json[KEY_SWINGH] = IRac::swinghToString(state.swingh);
  json[KEY_QUIET] = IRac::boolToString(state.quiet);
  json[KEY_TURBO] = IRac::boolToString(state.turbo);
  json[KEY_ECONO] = IRac::boolToString(state.econo);
  json[KEY_LIGHT] = IRac::boolToString(state.light);
  json[KEY_FILTER] = IRac::boolToString(state.filter);
  json[KEY_CLEAN] = IRac::boolToString(state.clean);
  json[KEY_BEEP] = IRac::boolToString(state.beep);
  json[KEY_SLEEP] = state.sleep;

  String payload = "";
  payload.reserve(200);
  serializeJson(json, payload);
  sendString(topic, payload, retain);
}

bool validJsonStr(DynamicJsonDocument doc, const char* key) {
  return doc.containsKey(key) && doc[key].is<char*>();
}

bool validJsonInt(DynamicJsonDocument doc, const char* key) {
  return doc.containsKey(key) && doc[key].is<signed int>();
}

stdAc::state_t jsonToState(const stdAc::state_t current, const char *str) {
  DynamicJsonDocument json(kJsonAcStateMaxSize);
  if (deserializeJson(json, str, kJsonAcStateMaxSize)) {
    debug("json MQTT message did not parse. Skipping!");
    return current;
  }
  stdAc::state_t result = current;
  if (validJsonStr(json, KEY_PROTOCOL))
    result.protocol = strToDecodeType(json[KEY_PROTOCOL]);
  else if (validJsonInt(json, KEY_PROTOCOL))
    result.protocol = (decode_type_t)json[KEY_PROTOCOL].as<signed int>();
  if (validJsonStr(json, KEY_MODEL))
    result.model = IRac::strToModel(json[KEY_MODEL].as<char*>());
  else if (validJsonInt(json, KEY_MODEL))
    result.model = json[KEY_MODEL];
  if (validJsonStr(json, KEY_MODE))
    result.mode = IRac::strToOpmode(json[KEY_MODE]);
  if (validJsonStr(json, KEY_FANSPEED))
    result.fanspeed = IRac::strToFanspeed(json[KEY_FANSPEED]);
  if (validJsonStr(json, KEY_SWINGV))
    result.swingv = IRac::strToSwingV(json[KEY_SWINGV]);
  if (validJsonStr(json, KEY_SWINGH))
    result.swingh = IRac::strToSwingH(json[KEY_SWINGH]);
  if (json.containsKey(KEY_TEMP))
    result.degrees = json[KEY_TEMP];
  if (validJsonInt(json, KEY_SLEEP))
    result.sleep = json[KEY_SLEEP];
  if (validJsonStr(json, KEY_POWER))
    result.power = IRac::strToBool(json[KEY_POWER]);
  if (validJsonStr(json, KEY_QUIET))
    result.quiet = IRac::strToBool(json[KEY_QUIET]);
  if (validJsonStr(json, KEY_TURBO))
    result.turbo = IRac::strToBool(json[KEY_TURBO]);
  if (validJsonStr(json, KEY_ECONO))
    result.econo = IRac::strToBool(json[KEY_ECONO]);
  if (validJsonStr(json, KEY_LIGHT))
    result.light = IRac::strToBool(json[KEY_LIGHT]);
  if (validJsonStr(json, KEY_CLEAN))
    result.clean = IRac::strToBool(json[KEY_CLEAN]);
  if (validJsonStr(json, KEY_FILTER))
    result.filter = IRac::strToBool(json[KEY_FILTER]);
  if (validJsonStr(json, KEY_BEEP))
    result.beep = IRac::strToBool(json[KEY_BEEP]);
  if (validJsonStr(json, KEY_CELSIUS))
    result.celsius = IRac::strToBool(json[KEY_CELSIUS]);
  return result;
}
#endif  // MQTT_CLIMATE_JSON

void updateClimate(stdAc::state_t *state, const String str,
                   const String prefix, const String payload) {
#if MQTT_CLIMATE_JSON
  if (str.equals(prefix + KEY_JSON))
    *state = jsonToState(*state, payload.c_str());
  else
#endif  // MQTT_CLIMATE_JSON
  if (str.equals(prefix + KEY_PROTOCOL)) {
    state->protocol = strToDecodeType(payload.c_str());
    debug(String(String("AC Protocol set to: ")+ payload+String(" ->  ")+typeToString((decode_type_t)state->protocol)+String(" [")+String(state->protocol)+String("]")).c_str());
    
  } else if (str.equals(prefix + KEY_MODEL)) {
    state->model = IRac::strToModel(payload.c_str());
    debug(String(String("AC Model set to: ")+payload+String(" -> ")+typeToString((decode_type_t)state->model)+String(" [")+String(state->model)+String("]")).c_str());
  } else if (str.equals(prefix + KEY_POWER)) {
    state->power = IRac::strToBool(payload.c_str());
#if MQTT_CLIMATE_HA_MODE
    if (!state->power) state->mode = stdAc::opmode_t::kOff;
#endif  // MQTT_CLIMATE_HA_MODE
  } else if (str.equals(prefix + KEY_MODE)) {
    state->mode = IRac::strToOpmode(payload.c_str());
#if MQTT_CLIMATE_HA_MODE
    state->power = (state->mode != stdAc::opmode_t::kOff);
#endif  // MQTT_CLIMATE_HA_MODE
  } else if (str.equals(prefix + KEY_TEMP)) {
    state->degrees = payload.toFloat();
  } else if (str.equals(prefix + KEY_FANSPEED)) {
    state->fanspeed = IRac::strToFanspeed(payload.c_str());
  } else if (str.equals(prefix + KEY_SWINGV)) {
    state->swingv = IRac::strToSwingV(payload.c_str());
  } else if (str.equals(prefix + KEY_SWINGH)) {
    state->swingh = IRac::strToSwingH(payload.c_str());
  } else if (str.equals(prefix + KEY_QUIET)) {
    state->quiet = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_TURBO)) {
    state->turbo = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_ECONO)) {
    state->econo = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_LIGHT)) {
    state->light = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_BEEP)) {
    state->beep = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_FILTER)) {
    state->filter = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_CLEAN)) {
    state->clean = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_CELSIUS)) {
    state->celsius = IRac::strToBool(payload.c_str());
  } else if (str.equals(prefix + KEY_SLEEP)) {
    state->sleep = payload.toInt();
  }
}

bool sendClimate(const String topic_prefix, const bool retain,
                 const bool forceMQTT, const bool forceIR,
                 const bool enableIR, IRac *ac) {
  bool diff = false;
  bool success = true;
  const stdAc::state_t next = ac->getState();
  const stdAc::state_t prev = ac->getStatePrev();
  if (prev.protocol != next.protocol || forceMQTT) {
    diff = true;
    success &= sendString(topic_prefix + KEY_PROTOCOL,
                          typeToString(next.protocol), retain);
  }
  if (prev.model != next.model || forceMQTT) {
    diff = true;
    success &= sendInt(topic_prefix + KEY_MODEL, next.model, retain);
  }
  String mode_str = IRac::opmodeToString(next.mode);
  // I don't know why, but the modes need to be lower case to work with
  // Home Assistant & Google Home.
  mode_str.toLowerCase();
#if MQTT_CLIMATE_HA_MODE
  // Home Assistant want's these two bound together.
  if (prev.power != next.power || prev.mode != next.mode || forceMQTT) {
    success &= sendBool(topic_prefix + KEY_POWER, next.power, retain);
    if (!next.power) mode_str = F("off");
#else  // MQTT_CLIMATE_HA_MODE
  // In non-Home Assistant mode, power and mode are not bound together.
  if (prev.power != next.power || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_POWER, next.power, retain);
  }
  if (prev.mode != next.mode || forceMQTT) {
#endif  // MQTT_CLIMATE_HA_MODE
    success &= sendString(topic_prefix + KEY_MODE, mode_str, retain);
    diff = true;
  }
  if (prev.degrees != next.degrees || forceMQTT) {
    diff = true;
    success &= sendFloat(topic_prefix + KEY_TEMP, next.degrees, retain);
  }
  if (prev.celsius != next.celsius || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_CELSIUS, next.celsius, retain);
  }
  if (prev.fanspeed != next.fanspeed || forceMQTT) {
    diff = true;
    success &= sendString(topic_prefix + KEY_FANSPEED,
                          IRac::fanspeedToString(next.fanspeed), retain);
  }
  if (prev.swingv != next.swingv || forceMQTT) {
    diff = true;
    success &= sendString(topic_prefix + KEY_SWINGV,
                          IRac::swingvToString(next.swingv), retain);
  }
  if (prev.swingh != next.swingh || forceMQTT) {
    diff = true;
    success &= sendString(topic_prefix + KEY_SWINGH,
                          IRac::swinghToString(next.swingh), retain);
  }
  if (prev.quiet != next.quiet || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_QUIET, next.quiet, retain);
  }
  if (prev.turbo != next.turbo || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_TURBO, next.turbo, retain);
  }
  if (prev.econo != next.econo || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_ECONO, next.econo, retain);
  }
  if (prev.light != next.light || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_LIGHT, next.light, retain);
  }
  if (prev.filter != next.filter || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_FILTER, next.filter, retain);
  }
  if (prev.clean != next.clean || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_CLEAN, next.clean, retain);
  }
  if (prev.beep != next.beep || forceMQTT) {
    diff = true;
    success &= sendBool(topic_prefix + KEY_BEEP, next.beep, retain);
  }
  if (prev.sleep != next.sleep || forceMQTT) {
    diff = true;
    success &= sendInt(topic_prefix + KEY_SLEEP, next.sleep, retain);
  }
  if (diff && !forceMQTT) {
    debug("Difference in common A/C state detected.");
#if MQTT_CLIMATE_JSON
    sendJsonState(next, topic_prefix + KEY_JSON);
#endif  // MQTT_CLIMATE_JSON
  } else {
    debug("NO difference in common A/C state detected.");
  }
  // Only send an IR message if we need to.
  if (enableIR && ((diff && !forceMQTT) || forceIR)) {
    sendReqCounter++;
    if (ac == NULL) {  // No climate object is available.
      debug("Can't send climate state as common A/C object doesn't exist!");
      return false;
    }
    debug("Sending common A/C state via IR.");
    lastClimateSucceeded = ac->sendAc();
    if (lastClimateSucceeded) hasClimateBeenSent = true;
    success &= lastClimateSucceeded;
    lastClimateIr.reset();
    irClimateCounter++;
  }
  // Mark the "next" value as old/previous.
  if (ac != NULL) {
    ac->markAsSent();
  }
  return success;
}
