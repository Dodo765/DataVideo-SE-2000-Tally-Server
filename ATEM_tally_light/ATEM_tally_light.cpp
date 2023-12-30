#include "ATEM_tally_light.hpp"

#ifndef VERSION
#define VERSION "dev"
#endif

#define FASTLED_ALLOW_INTERRUPTS 0

#ifndef CHIP_FAMILY
#define CHIP_FAMILY "Unknown"
#endif

#define DISPLAY_NAME "Tally Test server"

// Include libraries:

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#include <EEPROM.h>
#include <ATEMmin.h>
#include <TallyServer.h>
#include <FastLED.h>

// Map "old" LED colors to CRGB colors
// CRGB color_led[8] = {CRGB::Black, CRGB::Red, CRGB::Lime, CRGB::Blue, CRGB::Yellow, CRGB::Fuchsia, CRGB::White, CRGB::Orange};

// Define states
#define STATE_STARTING 0
#define STATE_CONNECTING_TO_WIFI 1
#define STATE_CONNECTING_TO_SWITCHER 2
#define STATE_RUNNING 3

// Define modes of operation
#define MODE_NORMAL 1
#define MODE_PREVIEW_STAY_ON 2
#define MODE_PROGRAM_ONLY 3
#define MODE_ON_AIR 4

#define TALLY_FLAG_OFF 0
#define TALLY_FLAG_PROGRAM 1
#define TALLY_FLAG_PREVIEW 2

// Define Neopixel status-LED options
#define NEOPIXEL_STATUS_FIRST 1
#define NEOPIXEL_STATUS_LAST 2
#define NEOPIXEL_STATUS_NONE 3

// FastLED
#ifndef TALLY_DATA_PIN
#if ESP32
#define TALLY_DATA_PIN 12
#elif ARDUINO_ESP8266_NODEMCU
#define TALLY_DATA_PIN 7
#else
#define TALLY_DATA_PIN 13 // D7
#endif
#endif
int numTallyLEDs;
int numStatusLEDs;
CRGB *leds;
CRGB *tallyLEDs;
CRGB *statusLED;
bool neopixelsUpdated = false;

// Initialize global variables

ESP8266WebServer server(80);

int tallyFlag = TALLY_FLAG_OFF;

TallyServer tallyServer;

ImprovWiFi improv(&Serial);

uint8_t state = STATE_STARTING;

// Define struct for holding tally settings (mostly to simplify EEPROM read and write, in order to persist settings)
struct Settings
{
    char tallyName[32] = "";
    uint8_t tallyNo;
    uint8_t tallyModeLED1;
    uint8_t tallyModeLED2;
    bool staticIP;
    IPAddress tallyIP;
    IPAddress tallySubnetMask;
    IPAddress tallyGateway;
    IPAddress switcherIP;
    uint16_t neopixelsAmount;
    uint8_t neopixelStatusLEDOption;
    uint8_t neopixelBrightness;
    uint8_t ledBrightness;
};

Settings settings;

bool firstRun = true;

int bytesAvailable = false;
uint8_t readByte;

void onImprovWiFiErrorCb(ImprovTypes::Error err)
{
}

void onImprovWiFiConnectedCb(const char *ssid, const char *password)
{
}

// tally preview
int liveFlag = 0;
int prevFlag = 0;

// set tally state to live and prev
void tallySetState()
{
    for (int i = 0; i < 41; i++)
    {
        tallyServer.setTallyFlag(i, 0);
    }
    if (liveFlag == prevFlag)
    {
        tallyServer.setTallyFlag(liveFlag, 3);
    }
    else
    {
        tallyServer.setTallyFlag(liveFlag, 1);
        tallyServer.setTallyFlag(prevFlag, 2);
    }
    Serial.print("Live: ");
    Serial.print(liveFlag + 1);
    Serial.print(" Prev: ");
    Serial.println(prevFlag + 1);
}

// set flags with redundancy check
void setPreviewFlag(int number)
{
    if (prevFlag != number)
    {
        prevFlag = number;
        tallySetState();
    }
}

void setLiveFlag(int number)
{
    if (liveFlag != number)
    {
        liveFlag = number;
        tallySetState();
    }
}

// Perform initial setup on power on
void setup()
{
    pinMode(D0, INPUT_PULLUP);
    pinMode(D1, INPUT_PULLUP);
    pinMode(D2, INPUT_PULLUP);
    pinMode(D3, INPUT_PULLUP);
    pinMode(D4, INPUT_PULLUP);
    pinMode(D5, INPUT_PULLUP);
    pinMode(D6, INPUT_PULLUP);
    pinMode(D7, INPUT_PULLUP);

    // Start Serial
    Serial.begin(115200);
    Serial.println("########################");
    Serial.println("Serial started");

    // Read settings from EEPROM. WIFI settings are stored separately by the ESP
    EEPROM.begin(sizeof(settings)); // Needed on ESP8266 module, as EEPROM lib works a bit differently than on a regular Arduino
    EEPROM.get(0, settings);

    Serial.println(settings.tallyName);

    if (settings.staticIP && settings.tallyIP != IPADDR_NONE)
    {
        WiFi.config(settings.tallyIP, settings.tallyGateway, settings.tallySubnetMask);
    }
    else
    {
        settings.staticIP = false;
    }

    // Put WiFi into station mode and make it connect to saved network
    WiFi.mode(WIFI_STA);
    WiFi.hostname(settings.tallyName);
    WiFi.setAutoReconnect(true);
    WiFi.begin();

    Serial.println("------------------------");
    Serial.println("Connecting to WiFi...");
    Serial.println("Network name (SSID): " + getSSID());

    // Initialize and begin HTTP server for handeling the web interface
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.onNotFound(handleNotFound);
    server.begin();

    tallyServer.begin();

    improv.setDeviceInfo(CHIP_FAMILY, DISPLAY_NAME, VERSION, "Tally Light", "");
    improv.onImprovError(onImprovWiFiErrorCb);
    improv.onImprovConnected(onImprovWiFiConnectedCb);

    // Wait for result from first attempt to connect - This makes sure it only activates the softAP if it was unable to connect,
    // and not just because it hasn't had the time to do so yet. It's blocking, so don't use it inside loop()
    unsigned long start = millis();
    while ((!WiFi.status() || WiFi.status() >= WL_DISCONNECTED) && (millis() - start) < 10000LU)
    {
        bytesAvailable = Serial.available();
        if (bytesAvailable > 0)
        {
            readByte = Serial.read();
            improv.handleByte(readByte);
        }
    }

    // Set state to connecting before entering loop
    changeState(STATE_CONNECTING_TO_WIFI);

    tallyServer.setTallySources(40);
}

void loop()
{
    bytesAvailable = Serial.available();
    if (bytesAvailable > 0)
    {
        readByte = Serial.read();
        improv.handleByte(readByte);
    }

    switch (state)
    {
    case STATE_CONNECTING_TO_WIFI:
        if (WiFi.status() == WL_CONNECTED)
        {
            WiFi.mode(WIFI_STA); // Disable softAP if connection is successful
            Serial.println("------------------------");
            Serial.println("Connected to WiFi:   " + getSSID());
            Serial.println("IP:                  " + WiFi.localIP().toString());
            Serial.println("Subnet Mask:         " + WiFi.subnetMask().toString());
            Serial.println("Gateway IP:          " + WiFi.gatewayIP().toString());

            Serial.println("Press enter (\\r) to loop through tally states.");
            changeState(STATE_RUNNING);
        }
        else if (firstRun)
        {
            firstRun = false;
            Serial.println("Unable to connect. Serving \"Tally Light setup\" WiFi for configuration, while still trying to connect...");
            WiFi.softAP((String)DISPLAY_NAME + " setup");
            WiFi.mode(WIFI_AP_STA); // Enable softAP to access web interface in case of no WiFi
            // setBothLEDs(LED_WHITE);
            // setStatusLED(LED_WHITE);
        }
        break;

    case STATE_RUNNING:
        if ((bytesAvailable && readByte == 'i'))
        {
            Serial.println("Info:");
            Serial.print("Live: ");
            Serial.print(liveFlag + 1);
            Serial.print(" Prev: ");
            Serial.println(prevFlag + 1);
        }
        if ((bytesAvailable && readByte == 'r'))
        {
            tallyFlag++;
            tallyFlag %= 4;

            switch (tallyFlag)
            {
            case TALLY_FLAG_OFF:
                Serial.println("Off");
                break;
            case TALLY_FLAG_PROGRAM:
                Serial.println("Program");
                break;
            case TALLY_FLAG_PREVIEW:
                Serial.println("Preview");
                break;
            case TALLY_FLAG_PROGRAM | TALLY_FLAG_PREVIEW:
                Serial.println("Program and preview");
                break;
            default:
                Serial.println("Invalid tally state...");
                break;
            }

            for (int i = 0; i < 41; i++)
            {
                tallyServer.setTallyFlag(i, tallyFlag);
            }
        }
        if ((bytesAvailable && readByte == '1') || !digitalRead(D0))
        {
            setLiveFlag(0);
        }
        if ((bytesAvailable && readByte == '2') || !digitalRead(D1))
        {
            setLiveFlag(1);
        }
        if ((bytesAvailable && readByte == '3') || !digitalRead(D2))
        {
            setLiveFlag(2);
        }
        if ((bytesAvailable && readByte == '4') || !digitalRead(D3))
        {
            setLiveFlag(3);
        }
        if ((bytesAvailable && readByte == '6') || !digitalRead(D4))
        {
            setPreviewFlag(0);
        }
        if ((bytesAvailable && readByte == '7') || !digitalRead(D5))
        {
            setPreviewFlag(1);
        }
        if ((bytesAvailable && readByte == '8') || !digitalRead(D6))
        {
            setPreviewFlag(2);
        }
        if ((bytesAvailable && readByte == '9') || !digitalRead(D7))
        {
            setPreviewFlag(3);
        }

        delay(10);

        // Handle Tally Server
        tallyServer.runLoop();

        break;
    }

    // Switch state if WiFi connection is lost...
    if (WiFi.status() != WL_CONNECTED && state != STATE_CONNECTING_TO_WIFI)
    {
        Serial.println("------------------------");
        Serial.println("WiFi connection lost...");
        changeState(STATE_CONNECTING_TO_WIFI);

        // Reset tally server's tally flags, They won't get the message, but it'll be reset for when the connectoin is back.
        tallyServer.resetTallyFlags();
    }

    // Handle web interface
    server.handleClient();
}

// Handle the change of states in the program
void changeState(uint8_t stateToChangeTo)
{
    firstRun = true;
    switch (stateToChangeTo)
    {
    case STATE_CONNECTING_TO_WIFI:
        state = STATE_CONNECTING_TO_WIFI;
        break;
    case STATE_CONNECTING_TO_SWITCHER:
        state = STATE_CONNECTING_TO_SWITCHER;
        break;
    case STATE_RUNNING:
        state = STATE_RUNNING;
        break;
    }
}

void analogWriteWrapper(uint8_t pin, uint8_t value)
{
    analogWrite(pin, value);
}

int getTallyState(uint16_t tallyNo)
{
    if (tallyFlag & TALLY_FLAG_PROGRAM)
    {
        return TALLY_FLAG_PROGRAM;
    }
    else if (tallyFlag & TALLY_FLAG_PREVIEW)
    {
        return TALLY_FLAG_PREVIEW;
    }
    else
    {
        return TALLY_FLAG_OFF;
    }
}

// Serve setup web page to client, by sending HTML with the correct variables
void handleRoot()
{
    String html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\"content=\"width=device-width,initial-scale=1.0\"><title>DataVideo Tally Serwer</title>";
    html += "<style>#staticIP {accent-color: #07b50c;}.s777777 h1,.s777777 h2 {color: #07b50c;}.fr{float: right}body {display: flex;align-items: center;justify-content: center;width: 100vw;overflow-x: hidden;font-family: \"Arial\", sans-serif;background-color: #242424;color: #fff;table {width: 80%;max-width: 1200px;background-color: #3b3b3b;padding: 20px;margin: 20px;border-radius: 10px;box-shadow: 0 0 10px rgba(0, 0, 0, 0.5);border-radius: 12px;overflow: hidden;border-spacing: 0;padding: 5px 45px;box-sizing: border-box;}tr.s777777 {background-color: transparent;color: #07b50c !important;}tr.cccccc {background-color: transparent;} tr.cccccc p {font-size: 16px;}input[type=\"checkbox\"] {width: 17.5px;aspect-ratio: 1;cursor: pointer;}td {cursor: default;user-select: none;}input {border-radius: 6px;cursor: text;}select {border-radius: 6px;cursor: pointer;}td.fr input {background-color: #07b50c !important; -webkit-appearance: none; accent-color: #07b50c !important;color: white;padding: 7px 17px;cursor: pointer;}* {line-height: 1.2;}@media screen and (max-width: 730px) {body {width: 100vw;margin: 0;padding: 10px;}table {width: 100%;padding: 0 10px;margin: 0;}}</style>";
    html += "</head><script>function switchIpField(e){console.log(\"switch\");console.log(e);var target=e.srcElement||e.target;var maxLength=parseInt(target.attributes[\"maxlength\"].value,10);var myLength=target.value.length;if(myLength>=maxLength){var next=target.nextElementSibling;if(next!=null){if(next.className.includes(\"IP\")){next.focus();}}}else if(myLength==0){var previous=target.previousElementSibling;if(previous!=null){if(previous.className.includes(\"IP\")){previous.focus();}}}}function ipFieldFocus(e){console.log(\"focus\");console.log(e);var target=e.srcElement||e.target;target.select();}function load(){var containers=document.getElementsByClassName(\"IP\");for(var i=0;i<containers.length;i++){var container=containers[i];container.oninput=switchIpField;container.onfocus=ipFieldFocus;}containers=document.getElementsByClassName(\"tIP\");for(var i=0;i<containers.length;i++){var container=containers[i];container.oninput=switchIpField;container.onfocus=ipFieldFocus;}toggleStaticIPFields();}function toggleStaticIPFields(){var enabled=document.getElementById(\"staticIP\").checked;document.getElementById(\"staticIPHidden\").disabled=enabled;var staticIpFields=document.getElementsByClassName('tIP');for(var i=0;i<staticIpFields.length;i++){staticIpFields[i].disabled=!enabled;}}</script><style>a{color:#0F79E0}</style><body style=\"font-family:Verdana;white-space:nowrap;\"onload=\"load()\"><table cellpadding=\"2\"style=\"width:100%\"><tr class=\"s777777\"style=\"font-size:.8em;\"><td colspan=\"3\"><h1>&nbsp;DataVideo Tally Serwer</h1><h2>&nbsp;Status:</h2></td></tr><tr><td><br></td><td></td><td style=\"width:100%;\"></td></tr><tr><td>Status połączenia:</td><td colspan=\"2\">";
    switch (WiFi.status())
    {
    case WL_CONNECTED:
        html += "Połączono do sieci";
        break;
    case WL_NO_SSID_AVAIL:
        html += "Sieć nie znaleziona";
        break;
    case WL_CONNECT_FAILED:
        html += "Nie poprawne hasło";
        break;
    case WL_IDLE_STATUS:
        html += "Zmiana stanu...";
        break;
    case WL_DISCONNECTED:
        html += "Tryb stacji niedostępny i nie wiem co to znaczy";
        break;
    case WL_CONNECTION_LOST:
        html += "Utracono połączenie WiFi";
        break;
    default:
        html += "Timeout";
        break;
    }

    html += "</td></tr><tr><td>Nazwa sieci (SSID):</td><td colspan=\"2\">";
    html += getSSID();
    html += "</td></tr><tr><td><br></td></tr><tr><td>Siła sygnału:</td><td colspan=\"2\">";
    html += WiFi.RSSI();
    html += " dBm</td></tr>";
    html += "<tr><td>Statyczny adres IP:</td><td colspan=\"2\">";
    html += settings.staticIP == true ? "Tak" : "Nie";
    html += "</td></tr><tr><td> Adres IP:</td><td colspan=\"2\">";
    html += WiFi.localIP().toString();
    html += "</td></tr><tr><td>Maska sieciowa: </td><td colspan=\"2\">";
    html += WiFi.subnetMask().toString();
    html += "</td></tr><tr><td>Brama domyślna: </td><td colspan=\"2\">";
    html += WiFi.gatewayIP().toString();
    html += "</td></tr><tr><td><br></td></tr>";

    html += "<tr class=\"s777777\"style=\"font-size:.8em;\"><td colspan=\"3\"><h2>&nbsp;Ustawienia:</h2></td></tr><tr><td><br></td></tr><form action=\"/save\"method=\"post\"><tr><td>Nazwa urządzenia: </td><td><input type=\"text\"size=\"30\"maxlength=\"30\"name=\"tName\"value=\"";
    html += WiFi.hostname();
    html += "\"required/></td></tr><tr style=\"display:none;\"><td><br></td></tr><tr style=\"display:none;\"><td>Numer urządzenia Tally: </td><td><input type=\"number\"size=\"5\"min=\"1\"max=\"41\"name=\"tNo\"value=\"";
    settings.tallyNo = 1;
    html += (settings.tallyNo + 1);
    html += "\"required/></td></tr><tr style=\"display:none;\"><td>Tryb Tally (LED 1):&nbsp;</td><td><select name=\"tModeLED1\"><option value=\"";
    html += (String)MODE_NORMAL + "\"";
    if (settings.tallyModeLED1 == MODE_NORMAL)
        html += "selected";
    html += ">Normalny</option><option value=\"";
    html += (String)MODE_PREVIEW_STAY_ON + "\"";
    if (settings.tallyModeLED1 == MODE_PREVIEW_STAY_ON)
        html += "selected";
    html += ">Podgląd zostaje włączony</option><option value=\"";
    html += (String)MODE_PROGRAM_ONLY + "\"";
    if (settings.tallyModeLED1 == MODE_PROGRAM_ONLY)
        html += "selected";
    html += ">Tylko program</option><option value=\"";
    html += (String)MODE_ON_AIR + "\"";
    if (settings.tallyModeLED1 == MODE_ON_AIR)
        html += "selected";
    html += ">Na żywo</option></select></td></tr><tr style=\"display:none;\"><td>Tryb Tally (LED 2):</td><td><select name=\"tModeLED2\"><option value=\"";
    html += (String)MODE_NORMAL + "\"";
    if (settings.tallyModeLED2 == MODE_NORMAL)
        html += "selected";
    html += ">Normalny</option><option value=\"";
    html += (String)MODE_PREVIEW_STAY_ON + "\"";
    if (settings.tallyModeLED2 == MODE_PREVIEW_STAY_ON)
        html += "selected";
    html += ">Podgląd zostaje włączony</option><option value=\"";
    html += (String)MODE_PROGRAM_ONLY + "\"";
    if (settings.tallyModeLED2 == MODE_PROGRAM_ONLY)
        html += "selected";
    html += ">Tylko program</option><option value=\"";
    html += (String)MODE_ON_AIR + "\"";
    if (settings.tallyModeLED2 == MODE_ON_AIR)
        html += "selected";
    html += ">Na żywo</option></select></td></tr><tr style=\"display:none;\"><td> Jasność ledów: </td><td><input type=\"number\"size=\"5\"min=\"0\"max=\"255\"name=\"ledBright\"value=\"";
    html += settings.ledBrightness;
    html += "\"required/></td></tr><tr style=\"display:none;\"><td><br></td></tr><tr style=\"display:none;\"><td>Ilość ledów:</td><td><input type=\"number\"size=\"5\"min=\"0\"max=\"1000\"name=\"neoPxAmount\"value=\"";
    html += settings.neopixelsAmount;
    html += "\"required/></td></tr><tr style=\"display:none;\"><td>Dioda statusu: </td><td><select name=\"neoPxStatus\"><option value=\"";
    html += (String)NEOPIXEL_STATUS_FIRST + "\"";
    if (settings.neopixelStatusLEDOption == NEOPIXEL_STATUS_FIRST)
        html += "selected";
    html += ">Pierwsza</option><option value=\"";
    html += (String)NEOPIXEL_STATUS_LAST + "\"";
    if (settings.neopixelStatusLEDOption == NEOPIXEL_STATUS_LAST)
        html += "selected";
    html += ">Ostatnia</option><option value=\"";
    html += (String)NEOPIXEL_STATUS_NONE + "\"";
    if (settings.neopixelStatusLEDOption == NEOPIXEL_STATUS_NONE)
        html += "selected";
    html += ">Żadna</option></select></td></tr><tr style=\"display:none;\"><td> Neopixel brightness: </td><td><input type=\"number\"size=\"5\"min=\"0\"max=\"255\"name=\"neoPxBright\"value=\"";
    html += settings.neopixelBrightness;
    html += "\"required/></td></tr><tr><td><br></td></tr><tr><td>Nazwa sieci (SSID): </td><td><input type =\"text\"size=\"30\"maxlength=\"30\"name=\"ssid\"value=\"";
    html += getSSID();
    html += "\"required/></td></tr><tr><td>Hasło do sieci: </td><td><input type=\"password\"size=\"30\"maxlength=\"30\"name=\"pwd\"pattern=\"^$|.{8,32}\"value=\"";
    if (WiFi.isConnected()) // As a minimum security meassure, to only send the wifi password if it's currently connected to the given network.
        html += WiFi.psk();
    html += "\"/></td></tr><tr><td><br></td></tr><tr><td>Użyj statycznego adresu IP: </td><td><input type=\"hidden\"id=\"staticIPHidden\"name=\"staticIP\"value=\"false\"/><input id=\"staticIP\"type=\"checkbox\"name=\"staticIP\"value=\"true\"onchange=\"toggleStaticIPFields()\"";
    if (settings.staticIP)
        html += "checked";
    html += "/></td></tr><tr><td>Adres IP: </td><td><input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP1\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[0];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP2\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[1];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP3\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[2];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"tIP4\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyIP[3];
    html += "\"required/></td></tr><tr><td>Maska sieciowa: </td><td><input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask1\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[0];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask2\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[1];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask3\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[2];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"mask4\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallySubnetMask[3];
    html += "\"required/></td></tr><tr><td>Brama domyślna: </td><td><input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate1\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[0];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate2\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[1];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate3\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[2];
    html += "\"required/>. <input class=\"tIP\"type=\"text\"size=\"3\"maxlength=\"3\"name=\"gate4\"pattern=\"\\d{0,3}\"value=\"";
    html += settings.tallyGateway[3];
    html += "\"required/></td></tr>";

    html += "<tr><td><br></td></tr><tr><td/><td class=\"fr\"><input type=\"submit\"value=\"Zapisz zmiany\"/></td></tr></form><tr class=\"cccccc\" style=\"font-size: .8em;\"><td colspan=\"3\"><p>&nbsp;Stworzone przez <a href=\"https://github.com/Dodo765\" target=\"_blank\">Dominik Kawalec</a></p><p>&nbsp;Napisane w oparciu o bibliotekę <a href=\"https://github.com/kasperskaarhoj/SKAARHOJ-Open-Engineering/tree/master/ArduinoLibs\" target=\"_blank\">SKAARHOJ</a></p></td></tr></table></body></html>";
    server.send(200, "text/html", html);
}

// Save new settings from client in EEPROM and restart the ESP8266 module
void handleSave()
{
    if (server.method() != HTTP_POST)
    {
        server.send(405, "text/html", "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\"content=\"width=device-width, initial-scale=1.0\"><title>DataVideo Tally Serwer</title></head><body style=\"font-family:Verdana;\"><table class=\"s777777\"border=\"0\"width=\"100%\"cellpadding=\"1\"style=\"color:#ffffff;font-size:.8em;\"><tr><td><h1>&nbsp;DataVideo Tally Serwer</h1></td></tr></table><br>Żądanie bez zmiany ustawień nie jest możliwe</body></html>");
    }
    else
    {
        String ssid;
        String pwd;
        bool change = false;
        for (uint8_t i = 0; i < server.args(); i++)
        {
            change = true;
            String var = server.argName(i);
            String val = server.arg(i);

            if (var == "tName")
            {
                val.toCharArray(settings.tallyName, (uint8_t)32);
            }
            else if (var == "tModeLED1")
            {
                settings.tallyModeLED1 = val.toInt();
            }
            else if (var == "tModeLED2")
            {
                settings.tallyModeLED2 = val.toInt();
            }
            else if (var == "ledBright")
            {
                settings.ledBrightness = val.toInt();
            }
            else if (var == "neoPxAmount")
            {
                settings.neopixelsAmount = val.toInt();
            }
            else if (var == "neoPxStatus")
            {
                settings.neopixelStatusLEDOption = val.toInt();
            }
            else if (var == "neoPxBright")
            {
                settings.neopixelBrightness = val.toInt();
            }
            else if (var == "tNo")
            {
                settings.tallyNo = val.toInt() - 1;
            }
            else if (var == "ssid")
            {
                ssid = String(val);
            }
            else if (var == "pwd")
            {
                pwd = String(val);
            }
            else if (var == "staticIP")
            {
                settings.staticIP = (val == "true");
            }
            else if (var == "tIP1")
            {
                settings.tallyIP[0] = val.toInt();
            }
            else if (var == "tIP2")
            {
                settings.tallyIP[1] = val.toInt();
            }
            else if (var == "tIP3")
            {
                settings.tallyIP[2] = val.toInt();
            }
            else if (var == "tIP4")
            {
                settings.tallyIP[3] = val.toInt();
            }
            else if (var == "mask1")
            {
                settings.tallySubnetMask[0] = val.toInt();
            }
            else if (var == "mask2")
            {
                settings.tallySubnetMask[1] = val.toInt();
            }
            else if (var == "mask3")
            {
                settings.tallySubnetMask[2] = val.toInt();
            }
            else if (var == "mask4")
            {
                settings.tallySubnetMask[3] = val.toInt();
            }
            else if (var == "gate1")
            {
                settings.tallyGateway[0] = val.toInt();
            }
            else if (var == "gate2")
            {
                settings.tallyGateway[1] = val.toInt();
            }
            else if (var == "gate3")
            {
                settings.tallyGateway[2] = val.toInt();
            }
            else if (var == "gate4")
            {
                settings.tallyGateway[3] = val.toInt();
            }
            else if (var == "aIP1")
            {
                settings.switcherIP[0] = val.toInt();
            }
            else if (var == "aIP2")
            {
                settings.switcherIP[1] = val.toInt();
            }
            else if (var == "aIP3")
            {
                settings.switcherIP[2] = val.toInt();
            }
            else if (var == "aIP4")
            {
                settings.switcherIP[3] = val.toInt();
            }
        }

        if (change)
        {
            EEPROM.put(0, settings);
            EEPROM.commit();

            server.send(200, "text/html", (String) "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\"content=\"width=device-width, initial-scale=1.0\"><title>DataVideo Tally Serwer</title></head><body><table class=\"s777777\"border=\"0\"width=\"100%\"cellpadding=\"1\"style=\"font-family:Verdana;color:#ffffff;font-size:.8em;\"><tr><td><h1>&nbsp;DataVideo Tally Serwer</h1></td></tr></table><br>Ustawienia zapisane pomyślnie!</body></html>");

            // Delay to let data be saved, and the response to be sent properly to the client
            server.close(); // Close server to flush and ensure the response gets to the client
            delay(100);

            // Change into STA mode to disable softAP
            WiFi.mode(WIFI_STA);
            delay(100); // Give it time to switch over to STA mode (this is important on the ESP32 at least)

            if (ssid && pwd)
            {
                WiFi.persistent(true); // Needed by ESP8266
                // Pass in 'false' as 5th (connect) argument so we don't waste time trying to connect, just save the new SSID/PSK
                // 3rd argument is channel - '0' is default. 4th argument is BSSID - 'NULL' is default.
                WiFi.begin(ssid.c_str(), pwd.c_str(), 0, NULL, false);
            }

            // Delay to apply settings before restart
            delay(100);
            ESP.restart();
        }
    }
}

// Send 404 to client in case of invalid webpage being requested.
void handleNotFound()
{
    server.send(404, "text/html", "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\"content=\"width=device-width, initial-scale=1.0\"><title>DataVideo Tally Serwer</title></head><body style=\"font-family:Verdana;\"><table class=\"s777777\"border=\"0\"width=\"100%\"cellpadding=\"1\"style=\"color:#ffffff;font-size:.8em;\"><tr><td><h1>&nbsp DataVideo Tally Serwer</h1></td></tr></table><br>404 - strona nie znaleziona</body></html>");
}

String getSSID()
{
    return WiFi.SSID();
}