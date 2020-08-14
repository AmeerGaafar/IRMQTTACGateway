
Send & receive arbitrary IR codes via a web server or MQTT.
Copyright David Conran 2016, 2017, 2018, 2019

Copyright:
  Code for this has been borrowed from lots of other OpenSource projects &
  resources. I'mNOT* claiming complete Copyright ownership of all the code.
  Likewise, feel free to borrow from this as much as you want.

NOTE: An IR LED circuit SHOULD be connected to the ESP if
      you want to send IR messages. e.g. GPIO4 (D2)
      A compatible IR RX modules SHOULD be connected to ESP
      if you want to capture & decode IR nessages. e.g. GPIO14 (D5)
      See 'IR_RX' in IRMQTTServer.h.
      GPIOs are configurable from the http://<your_esp's_ip_address>/gpio
      page.

WARN: This isvery* advanced & complicated example code. Not for beginners.
      You are strongly suggested to try & look at other example code first
      to understand how this library works.

# Instructions

## Before First Boot (i.e. Compile time)
- Disable MQTT if desired. (see '#define MQTT_ENABLE' in IRMQTTServer.h).

- Site specific settings:
  o Search for 'CHANGE_ME' in IRMQTTServer.h for the things you probably
    need to change for your particular situation.
  o All user changable settings are in the file IRMQTTServer.h.

- Arduino IDE:
  o Install the following libraries via Library Manager
    - ArduinoJson (https://arduinojson.org/) (Version >= 6.0)
    - PubSubClient (https://pubsubclient.knolleary.net/)
    - WiFiManager (https://github.com/tzapu/WiFiManager)
                  (ESP8266: Version >= 0.14, ESP32: 'development' branch.)
  o You MUST change <PubSubClient.h> to have the following (or larger) value:
    (with REPORT_RAW_UNKNOWNS 1024 or more is recommended)
    #define MQTT_MAX_PACKET_SIZE 768
  o Use the smallest non-zero FILESYSTEM size you can for your board.
    (See the Tools -> Flash Size menu)

- PlatformIO IDE:
    If you are using PlatformIO, this should already been done for you in
    the accompanying platformio.ini file.

## First Boot (Initial setup)
The ESP board will boot into the WiFiManager's AP mode.
i.e. It will create a WiFi Access Point with a SSID like: "ESP123456" etc.
Connect to that SSID. Then point your browser to http://192.168.4.1/ and
configure the ESP to connect to your desired WiFi network and associated
required settings. It will remember these details on next boot if the device
connects successfully.
More information can be found here:
  https://github.com/tzapu/WiFiManager#how-it-works

If you need to reset the WiFi and saved settings to go back to "First Boot",
visit:  http://<your_esp's_ip_address>/reset

## Normal Use (After initial setup)
Enter 'http://<your_esp's_ip_address/' in your browser & follow the
instructions there to send IR codes via HTTP/HTML.
Visit the http://<your_esp's_ip_address>/gpio page to configure the GPIOs
for the IR LED(s) and/or IR RX demodulator.

You can send URLs like the following, with similar data type limitations as
the MQTT formating in the next section. e.g:
  http://<your_esp's_ip_address>/ir?type=7&code=E0E09966
  http://<your_esp's_ip_address>/ir?type=4&code=0xf50&bits=12
  http://<your_esp's_ip_address>/ir?code=C1A2E21D&repeats=8&type=19
  http://<your_esp's_ip_address>/ir?type=31&code=40000,1,1,96,24,24,24,48,24,24,24,24,24,48,24,24,24,24,24,48,24,24,24,24,24,24,24,24,1058
  http://<your_esp's_ip_address>/ir?type=18&code=190B8050000000E0190B8070000010f0
  http://<your_esp's_ip_address>/ir?repeats=1&type=25&code=0000,006E,0022,0002,0155,00AA,0015,0040,0015,0040,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0040,0015,0040,0015,0015,0015,0040,0015,0015,0015,0015,0015,0015,0015,0040,0015,0015,0015,0015,0015,0040,0015,0040,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0040,0015,0015,0015,0015,0015,0040,0015,0040,0015,0040,0015,0040,0015,0040,0015,0640,0155,0055,0015,0E40
If you have enabled more than 1 TX GPIO, you can use the "channel" argument:
  http://<your_esp's_ip_address>/ir?channel=0&type=7&code=E0E09966
  http://<your_esp's_ip_address>/ir?channel=1&type=7&code=E0E09966

or

Send a MQTT message to the topic 'ir_server/send'
(or 'ir_server/send_1' etc if you have enabled more than 1 TX GPIO)
using the following format (Order is important):
  protocol_num,hexcode
    e.g. 7,E0E09966
         which is: Samsung(7), Power On code, default bit size,
                   default nr. of repeats.

  protocol_num,hexcode,bits
    e.g. 4,f50,12
         which is: Sony(4), Power Off code, 12 bits & default nr. of repeats.

  protocol_num,hexcode,bits,repeats
    e.g. 19,C1A2E21D,0,8
         which is: Sherwood(19), Vol Up, default bit size & repeated 8 times.

  30,frequency,raw_string
    e.g. 30,38000,9000,4500,500,1500,500,750,500,750
         which is: Raw (30) @ 38kHz with a raw code of
           "9000,4500,500,1500,500,750,500,750"

  31,code_string
    e.g. 31,40000,1,1,96,24,24,24,48,24,24,24,24,24,48,24,24,24,24,24,48,24,24,24,24,24,24,24,24,1058
         which is: GlobalCache (31) & "40000,1,1,96,..." (Sony Vol Up)

  25,Rrepeats,hex_code_string
    e.g. 25,R1,0000,006E,0022,0002,0155,00AA,0015,0040,0015,0040,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0040,0015,0040,0015,0015,0015,0040,0015,0015,0015,0015,0015,0015,0015,0040,0015,0015,0015,0015,0015,0040,0015,0040,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0015,0040,0015,0015,0015,0015,0015,0040,0015,0040,0015,0040,0015,0040,0015,0040,0015,0640,0155,0055,0015,0E40
         which is: Pronto (25), 1 repeat, & "0000 006E 0022 0002 ..."
            aka a "Sherwood Amp Tape Input" message.

  ac_protocol_num,really_long_hexcode
    e.g. 18,190B8050000000E0190B8070000010F0
         which is: Kelvinator (18) Air Con on, Low Fan, 25 deg etc.
         NOTE: Ensure you zero-pad to the correct number of digits for the
               bit/byte size you want to send as some A/C units have units
               have different sized messages. e.g. Fujitsu A/C units.

  Sequences.
    You can send a sequence of IR messages via MQTT using the above methods
    if you separate them with a ';' character. In addition you can add a
    pause/gap between sequenced messages by using 'P' followed immediately by
    the number of milliseconds you wish to wait (up to a max of kMaxPauseMs).
      e.g. 7,E0E09966;4,f50,12
        Send a Samsung(7) TV Power on code, followed immediately by a Sony(4)
        TV power off message.
      or:  19,C1A28877;P500;19,C1A25AA5;P500;19,C1A2E21D,0,30
        Turn on a Sherwood(19) Amplifier, Wait 1/2 a second, Switch the
        Amplifier to Video input 2, wait 1/2 a second, then send the Sherwood
        Amp the "Volume Up" message 30 times.

  In short:
    No spaces after/before commas.
    Values are comma separated.
    The first value is always in Decimal.
    For simple protocols, the next value (hexcode) is always hexadecimal.
    The optional bit size is in decimal.
    CAUTION: Some AC protocols DO NOT use the really_long_hexcode method.
             e.g. < 64bit AC protocols.

  Unix command line usage example:
    # Install a MQTT client
    $ sudo apt install mosquitto-clients
    # Send a 32-bit NEC code of 0x1234abcd via MQTT.
    $ mosquitto_pub -h 10.0.0.4 -t ir_server/send -m '3,1234abcd,32'

This server will send (back) what ever IR message it just transmitted to
the MQTT topic 'ir_server/sent' to confirm it has been performed. This works
for messages requested via MQTT or via HTTP.

  Unix command line usage example:
    # Listen to MQTT acknowledgements.
    $ mosquitto_sub -h 10.0.0.4 -t ir_server/sent

Incoming IR messages (from an IR remote control) will be transmitted to
the MQTT topic 'ir_server/received'. The MQTT message will be formatted
similar to what is required to for the 'sent' topic.
e.g. "3,C1A2F00F,32" (Protocol,Value,Bits) for simple codes
  or "18,110B805000000060110B807000001070" (Protocol,Value) for complex codes
Note: If the protocol is listed as -1, then that is an UNKNOWN IR protocol.
      You can't use that to recreate/resend an IR message. It's only for
      matching purposes and shouldn't be trusted.

  Unix command line usage example:
    # Listen via MQTT for IR messages captured by this server.
    $ mosquitto_sub -h 10.0.0.4 -t ir_server/received

Note: General logging messages are also sent to 'ir_server/log' from
      time to time.

## Climate (AirCon) interface. (Advanced use)
You can now control Air Conditioner devices that have full/detailed support
from the IRremoteESP8266 library. See the "Aircon" page for list of supported
devices. You can do this via HTTP/HTML or via MQTT.

NOTE: It will only change the attributes you change/set. It's up to you to
      maintain a consistent set of attributes for your particular aircon.

TIP: Use "-1" for 'model' if your A/C doesn't have a specific `setModel()`
     or IR class attribute. Most don't. Some do.
     e.g. PANASONIC_AC, FUJITSU_AC, WHIRLPOOL_AC

### via MQTT:
The code listen for commands (via wildcard) on the MQTT topics at the
`ir_server/ac/cmnd/+` level (or ir_server/ac_1/cmnd/+` if multiple TX GPIOs)
such as:x
i.e. protocol, model, power, mode, temp, fanspeed, swingv, swingh, quiet,
     turbo, light, beep, econo, sleep, filter, clean, use_celsius
e.g. ir_server/ac/cmnd/power, ir_server/ac/cmnd/temp,
     ir_server/ac_0/cmnd/mode, ir_server/ac_2/cmnd/fanspeed, etc.
It will process them, and if successful and it caused a change, it will
acknowledge this via the relevant state topic for that command.
e.g. If the aircon/climate changes from power off to power on, it will
     send an "on" payload to "ir_server/ac/stat/power"

There is a special command available to force the ESP to resend the current
A/C state in an IR message. To do so use the `resend` command MQTT topic,
e.g. `ir_server/ac/cmnd/resend` with a payload message of `resend`.
There is no corresponding "stat" message update for this particular topic,
but a log message is produced indicating it was received.

NOTE: These "stat" messages have the MQTT retain flag set to on. Thus the
      MQTT broker will remember them until reset/restarted etc.

The code will also periodically broadcast all possible aircon/climate state
attributes to their corresponding "ir_server/ac/stat" topics. This ensures
any updates to the ESP's knowledge that may have been lost in transmission
are re-communicated. e.g. The MQTT broker being offline.
This also helps with Home Assistant MQTT discovery.

The program on boot & first successful connection to the MQTT broker, will
try to re-acquire any previous aircon/climate state information and act
accordingly. This will typically result in A/C IR message being sent as and
saved state will probably be different from the defaults.

NOTE: Command attributes are processed sequentially.
      e.g. Going from "25C, cool, fan low" to "27C, heat, fan high" may go
      via "27C, cool, fan low" & "27C, heat, fan low" depending on the order
      of arrival & processing of the MQTT commands.

### Home Assistant (HA) MQTT climate integration
After you have set the Protocol (required) & Model (if needed) and any of
the other misc aircon settings you desire, you can then add the following to
your Home Assistant configuration, and it should allow you to
control most of the important settings. Google Home/Assistant (via HA)
can also control the device, but you will need to configure Home Assistant
via it's documentation for that. It has even more limited control.
It's far beyond the scope of these instructions to guide you through setting
up HA and Google Home integration. See https://www.home-assistant.io/

In HA's configuration.yaml, add:

climate:
  - platform: mqtt
    name: Living Room Aircon
    modes:
      - "off"
      - "auto"
      - "cool"
      - "heat"
      - "dry"
      - "fan_only"
    fan_modes:
      - "Auto"
      - "Min"
      - "Low"
      - "Medium"
      - "High"
      - "Max"
    swing_modes:
      - "Off"
      - "Auto"
      - "Highest"
      - "High"
      - "Middle"
      - "Low"
      - "Lowest"
    power_command_topic: "ir_server/ac/cmnd/power"
    mode_command_topic: "ir_server/ac/cmnd/mode"
    mode_state_topic: "ir_server/ac/stat/mode"
    temperature_command_topic: "ir_server/ac/cmnd/temp"
    temperature_state_topic: "ir_server/ac/stat/temp"
    fan_mode_command_topic: "ir_server/ac/cmnd/fanspeed"
    fan_mode_state_topic: "ir_server/ac/stat/fanspeed"
    swing_mode_command_topic: "ir_server/ac/cmnd/swingv"
    swing_mode_state_topic: "ir_server/ac/stat/swingv"
    min_temp: 16
    max_temp: 32
    temp_step: 1
    retain: false

#### Home Assistant MQTT Discovery
  There is an option for this: 'Send MQTT Discovery' under the 'Admin' menu.
  It will produce a single MQTT Climate Discovery message for Home Assistant
  provided you have everything configured correctly here and in HA.
  This message has MQTT RETAIN set on it, so it only ever needs to be sent
  once or if the config details change etc.

  If you no longer want it, manually remove it from your MQTT broker.
  e.g.
    `mosquitto_pub -t homeassistant/climate/ir_server/config -n -r -d`

  NOTE: If you have multiple TX GPIOs configured, itONLY* works for the
  first TX GPIO climate. You will need to manually configure the others.

### via HTTP:
  Use the "http://<your_esp's_ip_address>/aircon/set" URL and pass on
  the arguments as needed to control your device. See the `KEY_*` #defines
  in the code for all the parameters.
  i.e. protocol, model, power, mode, temp, fanspeed, swingv, swingh, quiet,
       turbo, light, beep, econo, sleep, filter, clean, use_celsius, channel
  Example:
    http://<your_esp's_ip_address>/aircon/set?channel=0&protocol=PANASONIC_AC&model=LKE&power=on&mode=auto&fanspeed=min&temp=23

  NOTE: If you don't set the channel, the first GPIO (Channel 0) is used.

## Debugging & Logging
If DEBUG is turned on, there is additional information printed on the Serial
Port. Serial Port output may be disabled if the GPIO is used for IR.

If MQTT is enabled, some information/logging is sent to the MQTT topic:
  `ir_server/log`

## Updates
You can upload new firmware Over The Air (OTA) via the form on the device's
"Admin" page. No need to connect to the device again via USB. \o/
Your settings should be remembered between updates. \o/ \o/

On boards with 1 Meg of flash should use an SPIFFS size of 64k if you want a
hope of being able to load a firmware via OTA.
Boards with only 512k flash have no chance of OTA with this firmware.

## Security
<security-hat="on">
There is NO authentication set on the HTTP/HTML interface by default (see
`HTML_PASSWORD_ENABLE` to change that), and there is NO SSL/TLS (encryption)
used by this example code.
  i.e. All usernames & passwords are sent in clear text.
       All communication to the MQTT server is in clear text.
  e.g. This on/using the public Internet is a 'Really Bad Idea<tm>'!
You should NOT have or use this code or device exposed on an untrusted and/or
unprotected network.
If you allow access to OTA firmware updates, then a 'Bad Guy<tm>' could
potentially compromise your network. OTA updates are password protected by
default. If you are sufficiently paranoid, you SHOULD disable uploading
firmware via OTA. (see 'FIRMWARE_OTA')
You SHOULD also set/change all usernames & passwords.
For extra bonus points: Use a separate untrusted SSID/vlan/network/ segment
for your IoT stuff, including this device.
            Caveat Emptor. You have now been suitably warned.
</security-hat>
