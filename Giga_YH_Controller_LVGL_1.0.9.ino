//Giga YH Controller with LVGL
//Author: Noah Christian (C) 2024
//Version 1.0.9 - GitHub version posting
//
//Purpose: This controls power flow to a home based on time-of-use rates that
//  can vary as much as 8x from peak to off-peak. Thus it can act as an arbitrator
//  of power rates for a household with appropriate hardware. THe Y-H hardware is
//  designed out-of-the-box to feed power from downstream current monitoring. This
//  is not an easy connection in the US grid because the current monitor really
//  needs to be upstream at the panel. Thus the math and control system are different,
//  and it needs a signed value of power flow to maintain levels of export close
//  to zero.
//
//This controller is designed to manage power flow to a home based on inputs 
//  from the grid. It receives commands from current monitors provided by
//  the Y-H grid tied inverter running from a battery to feed to the house.
//  Home Assistant runs a current monitor that sends MQTT messages to this software
//  to inform it of the grid power with polarity, as the Y-H controller sends
//  an absolute value without a sign. To prevent the system from exporting power
//  to the grid, the controller will adjust the output on two lines (single phase)
//  using two grid tied converters attached to line 1 (L1) and line 2 (L2) on
//  a house system. The controllers plug into the wall and attach to a battery.
//  the software here processes the MQQT messages, it applies a conversion factor
//  from the Y-H current loops, because the provided loops were the wrong diameter
//  for a home system, and the current loops used had a different winding factor.
//  The software then sends commands to the grid tied converters at the needed rate to
//  send updates, and it displays information on a Giga Display. The controller can adjust 
//  power output based on setpoints received via MQTT, and it includes a startup 
//  delay to ensure that the grid tie is active before sending power commands.
//  The grid tied converters are set to a maximum output of 900W but can be adjusted.
//  Grid tied inverters can be put in parallel to increase capacity beyond this.
//  
//Hardware: Arduino Giga R1 WiFi, Giga Display Shield, RS485 converters to 3.3V TTL
//  for interfacing power current meters provided by Y-H in their feedback system.
//  measurement, MQTT broker (e.g., Home Assistant), Y-H grid tied inverter 
//  (2 for single phase 220V US system),  whole house battery.
//  Currently the grid tied converters need to be on smart outlets for time-of-use,
//  but the software can be adjusted to control the power output based on time of day,
//  to eliminate this hardware.
//
//Software: Arduino IDE, LVGL for display management, ArduinoMqttClient for MQTT
//  communication, Home Assistant for MQTT broker and smart home management, 
//  NTP for time synchronization.
//
//Note: This code is a starting point and may require adjustments based on specific 
//  hardware configurations and requirements. It is important to test the code in a
// safe environment, especially when dealing with power control.
//
//Disclaimer: This code is provided as-is for educational purposes. The author is not 
//  responsible for any damage or harm caused by the use of this code. Always 
//  ensure proper safety measures are in place when working with electrical systems.
//
// arduino_secrets.h should contain the following definitions:
// #define SECRET_SSID "YourWiFiSSID"
// #define SECRET_PASS "YourWiFiPassword"
// #define HOME_ASSISTANT_IP "YourHomeAssistantIP"
// #define MQTT_USERNAME "YourMQTTUsername"
// #define MQTT_PASSWORD "YourMQTTPassword"
//
//

#include "Arduino_H7_Video.h"
#include "lvgl.h"
#include "Arduino_GigaDisplayTouch.h"
#include "lv_conf.h"

// Include the RTC library
#include "mbed.h"
#include <mbed_mktime.h>

#include <WiFi.h>
#include <WiFiUdp.h>
#include "arduino_secrets.h"
#include <ArduinoMqttClient.h>

//Unit 2 is only MQTT subscriber, not publisher Unit 1 is sub/pubber
#define UNIT_NUMBER 1
#define MAX_POWER 900
#define STARTUP_DELAY 10 //Startup Delay in Seconds (e.g. 10 minutes as 600 seconds)

//2026-07 oscillation fix: total_power_from_grid is one whole-household reading that
//  Home Assistant publishes identically to both Line1Set and Line2Set, so each line's
//  accumulator only claims half of it (see the /2.0 in the b_New_L{1,2}_Power blocks)
//  -- otherwise the two lines' corrections double-count the same signal.
//EMA_ALPHA smooths onMqttMessage's incoming reading before it reaches the accumulator;
//  derived from the measured real Refoss->HA report cadence (~15.8s between readings in
//  logged data) for an effective ~35s smoothing time constant.
#define EMA_ALPHA 0.35
//MAX_STEP_PER_UPDATE caps how much a single 30s update may change output by. Default is
//  a conservative starting point grounded in the incident log, not a hardware-verified
//  limit -- revisit once real-world behavior after this fix can be observed.
#define MAX_STEP_PER_UPDATE 100.0

bool trace = true;

#define VERSION_POWER "1.0.10"
//version 1.0.0 - clock showing UTC
//version 1.0.1 - fixed clock and included version referencing
//version 1.0.2 - include MQTT publishing and pubsub
//version 1.0.3 - include reading of data, putting to screen, publishing, and taking commands
//                some code cleanup
//version 1.0.4 - Output all values to screen Power In, Corrected, Output, SetPoint, LastCommand
//                added verbosity
//version 1.0.5 - Feedback working through MQTT, add readout of local power on 485
//                Add startup delay of 5 minutes of sending 0W to ensure that grid tie is active
//                Include watchdog timer on RS485 transmissions to ensure that heartbeat is active if sensor isn't
//version 1.0.6 - Bug fixes
//version 1.0.7 - First production version
//version 1.0.8 - Running version minor fixes including watchdog on inputs
//version 1.0.9 - GitHub version posting
//version 1.0.10 - Fix output oscillation traced to a logged 2026-07-11 incident: total_power_from_grid
//                 is one whole-household reading published to both Line1Set/Line2Set, so each line
//                 was double-applying the same correction. Now halved, EMA-filtered, and slew-rate
//                 limited per update -- see EMA_ALPHA/MAX_STEP_PER_UPDATE above.
//
//TODO: Report on output
//      Time of Use
//      Holiday schedule


uint8_t verbosity = 255;
// 0 = silent
// 1 = basics
// 4 = regular chit-chat
// 8 = talky talky
// 255 = oversharing

int timezone = -7;  //this is GMT -7. Pacific Time (Los Angelese)

///////please enter your sensitive data in the Secret tab/arduino_secrets.h
char ssid[] = SECRET_SSID;        // your network SSID (name)
char password[] = SECRET_PASS;    // your network password (use for WPA, or use as key for WEP)

//uint32_t currentTime;
uint32_t lastPowerChangeL1=millis();
uint32_t lastPowerChangeL2=millis();

int wifiStatus = WL_IDLE_STATUS;
WiFiUDP Udp; // A UDP instance to let us send and receive packets over UDP
//NTPClient timeClient(Udp);
Arduino_H7_Video Display(800, 480, GigaDisplayShield);
Arduino_GigaDisplayTouch TouchDetector;

//MQTT (Message Queuing Telemetry Transport)
//create the objects
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

//define broker, port and topic
const char broker[] = HOME_ASSISTANT_IP; //not sure why not working name on local: "homeassistant";
int        port     = 1883;
//Version/Enterprise/Site/Area/Line/Device
const char pubtopic1[]  = "V1.0/Home/PowerFeeder/Line1";
const char pubtopic2[]  = "V1.0/Home/PowerFeeder/Line2";
const char subtopic1[]  = "V1.0/Home/PowerFeeder/Line1Set";
const char subtopic2[]  = "V1.0/Home/PowerFeeder/Line2Set";

//end of Mosquitto

unsigned int localPort = 2390; // local port to listen for UDP packets

// IPAddress timeServer(162, 159, 200, 123); // pool.ntp.org NTP server

constexpr auto timeServer { "pool.ntp.org" };

const int NTP_PACKET_SIZE = 48; // NTP timestamp is in the first 48 bytes of the message

byte packetBuffer[NTP_PACKET_SIZE]; // buffer to hold incoming and outgoing packets


//Declarations for LVGL Elements
lv_obj_t* obj1;
lv_obj_t* obj2;
lv_obj_t* obj3;
lv_obj_t* obj4;
lv_obj_t * label1;
lv_obj_t * label2;
lv_obj_t * label3;
lv_obj_t * label4;

//Non-blocking timer tracking
unsigned long previousTime = millis();
unsigned long currentTime = millis();
unsigned long timer485_L1 = millis();
unsigned long timer485_L2 = millis();
int tStart = millis(); //startup counter see STARTUP_DELAY define
bool b_Start = true; //true=incorporate startup delay (note startup delay starts after code is running)

uint8_t buffer1[12];//to do: should be 8
uint8_t highByte1,lowByte1,cksum1;
uint8_t receivedChar1;
bool messageValid1 = false;
bool bSendMessage1 = false;
uint8_t tail1 = 0;

uint8_t buffer2[12];
uint8_t highByte2,lowByte2,cksum2;
uint8_t receivedChar2;
bool messageValid2 = false;
bool bSendMessage2 = false;
bool bDisplayMessage = true; //print 0 to start
uint8_t tail2 = 0;
const float L1_CorrectionFactor=0.683966;
const float L2_CorrectionFactor=0.683966;
uint16_t L1_Output = 0;
uint16_t L2_Output = 0;

//use float Power1 and 2 for display
float f_L1_Power=0.0;
float f_L2_Power=0.0;
unsigned int Power1=0;
unsigned int PowerCk1=0;
unsigned int Power2=0;
unsigned int PowerCk2=0;
bool b_New_L1_Power=false;
bool b_New_L2_Power=false;

//Giga Method
char * getLocaltime(char buffer[])
{
    tm t;
    _rtc_localtime(time(NULL), &t, RTC_FULL_LEAP_YEAR_SUPPORT);
    strftime(buffer, 32, "%Y-%m-%d %k:%M:%S", &t);
    if (UNIT_NUMBER > 1)
      strcat(buffer, "\n#FF0000 Not Publishing#");    
    return buffer;
}

char * wifi_info(char buffer[]) {
  char currentline[60]="";
  //char buffer[200]="";
  // print the SSID of the network you're attached to:
  sprintf(currentline, "SSID: %s\n", WiFi.SSID());
  strcpy(buffer, currentline);
  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  sprintf(currentline, "IPAddress: %03d.%03d.%03d.%03d\n", ip[0],ip[1],ip[2],ip[3]);
  strcat(buffer, currentline);
  
  ip = WiFi.dnsIP();
  sprintf(currentline, "DNS: %03d.%03d.%03d.%03d\n", ip[0],ip[1],ip[2],ip[3]);
  strcat(buffer, currentline);
  
  ip = WiFi.gatewayIP();
  sprintf(currentline, "Gateway: %03d.%03d.%03d.%03d\n", ip[0],ip[1],ip[2],ip[3]);
  strcat(buffer, currentline);
  
  sprintf(currentline,"RSSI : %d dBm\n",WiFi.RSSI());
  strcat(buffer, currentline);
  return buffer;

}

void printWifiStatus() {
  if (verbosity > 0){
    // print the SSID of the network you're attached to:
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    // print your board's IP address:
    IPAddress ip = WiFi.localIP();
    Serial.print("IP Address: ");
    Serial.println(ip);

    // print the received signal strength:
    long rssi = WiFi.RSSI();
    Serial.print("signal strength (RSSI):");
    Serial.print(rssi);
    Serial.println(" dBm");
  }
}

void connectToWiFi(void){
  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    if (verbosity > 0) Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true);
  }

  String fv = WiFi.firmwareVersion();

  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    if (verbosity > 0) Serial.println("Please upgrade the firmware");
  }

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_SHIELD) {
    if (verbosity > 0) Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true);
  }

  // attempt to connect to WiFi network:
  while (wifiStatus != WL_CONNECTED) {
    if (verbosity > 0){
      Serial.print("Attempting to connect to SSID: ");
      Serial.println(ssid);
    }
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    wifiStatus = WiFi.begin(ssid, password);

    // wait 10 seconds for connection:
    delay(10000);
  }
  setNtpTime();
  if (verbosity > 0) Serial.println("Connected to WiFi");
  printWifiStatus();
}

void setNtpTime()
{
  Udp.begin(localPort);
  sendNTPpacket(timeServer);
  delay(1000);
  parseNtpPacket();
}

// send an NTP request to the time server at the given address
unsigned long sendNTPpacket(const char * address)
{
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011; // LI, Version, Mode
  packetBuffer[1] = 0; // Stratum, or type of clock
  packetBuffer[2] = 6; // Polling Interval
  packetBuffer[3] = 0xEC; // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;

  Udp.beginPacket(address, 123); // NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

unsigned long parseNtpPacket()
{
  if (!Udp.parsePacket())
      return 0;

  Udp.read(packetBuffer, NTP_PACKET_SIZE);
  const unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
  const unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
  const unsigned long secsSince1900 = highWord << 16 | lowWord;
  constexpr unsigned long seventyYears = 2208988800UL;
  const unsigned long epoch = secsSince1900 - seventyYears;

  const unsigned long new_epoch = epoch + (3600 * timezone);  //multiply the timezone with 3600 (1 hour)
  set_time(new_epoch);

  if (verbosity > 4){
    Serial.print("Seconds since Jan 1 1900 = ");
    Serial.println(secsSince1900);

    // now convert NTP time into everyday time:
    Serial.print("Unix time = ");
    // print Unix time:
    Serial.println(epoch);

    // print the hour, minute and second:
    Serial.print("The UTC time is "); // UTC is the time at Greenwich Meridian (GMT)
    Serial.print((epoch % 86400L) / 3600); // print the hour (86400 equals secs per day)
    Serial.print(':');
    if (((epoch % 3600) / 60) < 10) {
        // In the first 10 minutes of each hour, we'll want a leading '0'
        Serial.print('0');
    }
    Serial.print((epoch % 3600) / 60); // print the minute (3600 equals secs per minute)
    Serial.print(':');
    if ((epoch % 60) < 10) {
        // In the first 10 seconds of each minute, we'll want a leading '0'
        Serial.print('0');
    }
    Serial.println(epoch % 60); // print the second
  }
  return epoch;
}

void onMqttMessage(int messageSize) {
  char tbuf[256]="";
  int size=0;
  String topic = mqttClient.messageTopic();
  // we received a message, print out the topic and contents
  if (verbosity > 4) {
    Serial.print("Received a message with topic '");
    Serial.print(topic);
    Serial.print("', length ");
    Serial.print(messageSize);
    Serial.print(" bytes: ");
  }
  if(topic.equals(subtopic1)){
    // use the Stream interface to print the contents
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size]=(char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read(); //discard anything beyond tbuf's capacity so the next message stays in sync
    tbuf[size]=NULL;
    if (verbosity > 4) Serial.print(String(tbuf));
    if (verbosity > 4) Serial.println();
    //smoothed (EMA) rather than a raw overwrite -- see EMA_ALPHA note above
    f_L1_Power = EMA_ALPHA * atof(tbuf) + (1 - EMA_ALPHA) * f_L1_Power; //atof avoids a heap-allocating String just to parse a float
    if (trace) {Serial.print("f_L1_Power = "); Serial.println(f_L1_Power,3);}
    b_New_L1_Power = true;
  }
  if(topic.equals(subtopic2)){
    // use the Stream interface to print the contents
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size]=(char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read(); //discard anything beyond tbuf's capacity so the next message stays in sync
    tbuf[size]=NULL;
    if (verbosity > 4) Serial.print(String(tbuf));
    if (verbosity > 4) Serial.println();
    //smoothed (EMA) rather than a raw overwrite -- see EMA_ALPHA note above
    f_L2_Power = EMA_ALPHA * atof(tbuf) + (1 - EMA_ALPHA) * f_L2_Power; //atof avoids a heap-allocating String just to parse a float
    b_New_L2_Power = true;
  }
}

void displayInValues(float L1, float L2, uint16_t L1PM, uint16_t L2PM) {
  char buffer[128]; // Ensure the buffer is large enough
  sprintf(buffer, "L1 in = %.3f W\nL2 in = %.3f W\nCorrected Local\nL1 = %.3f\nL2 = %.3f\n", L1, L2, (float) L1PM*L1_CorrectionFactor, (float) L2PM*L2_CorrectionFactor);
  lv_label_set_text(label2, buffer);  
}
void displayOutValues(uint16_t L1, uint16_t L2) {
  char buffer[128]; // Ensure the buffer is large enough
  sprintf(buffer, "L1 out =\n %d W\nL2 out =\n %d W\n", L1, L2);
  lv_label_set_text(label4, buffer);  
}

void setup() {
  // initialize both serial ports:
  //Serial.begin(9600); //the COM PORT on USB Monitor
  Serial.begin(115200);
  Serial1.begin(4800);
  Serial2.begin(4800);
  
  delay(3000);
  Display.begin();
  TouchDetector.begin();

  //Display & Grid Setup
  lv_obj_t* screen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(screen, Display.width(), Display.height());

  static lv_coord_t col_dsc[] = { 370, 370, LV_GRID_TEMPLATE_LAST };
  static lv_coord_t row_dsc[] = { 215, 215, 215, 215, LV_GRID_TEMPLATE_LAST };

  lv_obj_t* grid = lv_obj_create(lv_scr_act());
  lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
  lv_obj_set_size(grid, Display.width(), Display.height());

  //top left
  obj1 = lv_obj_create(grid);
  lv_obj_set_grid_cell(obj1, LV_GRID_ALIGN_STRETCH, 0, 1,  //column
                       LV_GRID_ALIGN_STRETCH, 0, 1);      //row

  //bottom left
  obj2 = lv_obj_create(grid);
  lv_obj_set_grid_cell(obj2, LV_GRID_ALIGN_STRETCH, 0, 1,  //column
                       LV_GRID_ALIGN_STRETCH, 1, 1);      //row
  //top right
  obj3 = lv_obj_create(grid);
  lv_obj_set_grid_cell(obj3, LV_GRID_ALIGN_STRETCH, 1, 1,  //column
                       LV_GRID_ALIGN_STRETCH, 0, 1);      //row

  //bottom right
  obj4 = lv_obj_create(grid);
  lv_obj_set_grid_cell(obj4, LV_GRID_ALIGN_STRETCH, 1, 1,  //column
                       LV_GRID_ALIGN_STRETCH, 1, 1);      //row


  label1 = lv_label_create(obj1);
  lv_obj_set_align(label1, LV_ALIGN_CENTER);
  label2 = lv_label_create(obj2);
  lv_obj_set_align(label2, LV_ALIGN_CENTER);
  label3 = lv_label_create(obj3);
  lv_obj_set_align(label3, LV_ALIGN_TOP_LEFT);
  label4 = lv_label_create(obj4);
  lv_obj_set_align(label4, LV_ALIGN_CENTER);
  char subject_text[128];
  sprintf(subject_text,"Power Controller\nV%s",VERSION_POWER);
  lv_label_set_recolor(label1, true);
  lv_label_set_recolor(label2, true);
  lv_label_set_recolor(label3, true);
  lv_label_set_recolor(label4, true);
  lv_label_set_text(label1, subject_text);
  lv_label_set_text(label2, "Power In");
  lv_label_set_text(label3, "Connecting to WiFi");
  lv_label_set_text(label4, "Power Out");
  
  //Try to not leave screen in the dark
  lv_timer_handler();

  connectToWiFi();
  if (verbosity > 0) Serial.println("\nStarting connection to server...");
  if (verbosity > 0) {
    Serial.print("Running version ");
    Serial.println(VERSION_POWER);
  }

  if (verbosity > 0) {
    Serial.print("Attempting to connect to the MQTT broker: ");
    Serial.println(broker);
  }
  
  mqttClient.setUsernamePassword(MQTT_USERNAME,MQTT_PASSWORD);
  if (!mqttClient.connect(broker, port)) {
    if (verbosity > 0) Serial.print("MQTT connection failed! Error code = ");
    if (verbosity > 0) Serial.println(mqttClient.connectError());

    while (1);
  }

  if (verbosity > 0) Serial.println("You're connected to the MQTT broker!");
  
  // set the message receive callback
  mqttClient.onMessage(onMqttMessage);

  // Subscribe to a topic
  if (verbosity > 0) {
    Serial.print("Subscribing to topic: ");
    Serial.println(subtopic1);
    Serial.println();
  }
  // subscribe to a topic
  mqttClient.subscribe(subtopic1);

  // Subscribe to a topic
  if (verbosity > 0) {
    Serial.print("Subscribing to topic: ");
    Serial.println(subtopic2);
    Serial.println();
  }
  // subscribe to a topic
  mqttClient.subscribe(subtopic2);

  tStart = millis();
}

int tick = 0;

static char subject_text[256];
char buffer[200]="";
unsigned long watchdog = millis();
  
void loop() {

  
  currentTime = millis();
  if (Serial1.available() > 0) {
    receivedChar1 = Serial1.read();
    buffer1[tail1] = receivedChar1;
    tail1++;
    if(tail1==8){ //incrementing tail before
      if ( (buffer1[0] == 0x24) && (buffer1[1] == 0x56) ){
        messageValid1=true;
        if (trace) Serial.println("Message 1 Valid");
      }
      else tail1=0;
    }
  }

  if (Serial2.available() > 0) {
    receivedChar2 = Serial2.read();
    buffer2[tail2] = receivedChar2;
    tail2++;
    if(tail2==8){ //incrementing tail before
      if ( (buffer2[0] == 0x24) && (buffer2[1] == 0x56) ){
        messageValid2=true;
      }
      else tail2=0; //discard the message read eight bytes but invalid
    }
   }
  
  watchdog = millis();
  if ((watchdog-timer485_L1)>550){//output at 2 Hz plus a little
      //watchdog the input
       if (trace) {Serial.print("Watchdog timer485_L1 hit at elapsed milliseconds ");Serial.print(watchdog-timer485_L1);Serial.print(" ms bSendMessage1 = ");Serial.println(bSendMessage1);}
      if (!bSendMessage1) bSendMessage1 = true;
  }
  
  if ((watchdog-timer485_L2)>550){//output at 2 Hz plus a little
      //watchdog the input
       if (trace) {Serial.print("Watchdog timer485_L2 hit at elapsed milliseconds ");Serial.print(watchdog-timer485_L2);Serial.print(" ms bSendMessage2 = ");Serial.println(bSendMessage2);}
      if (!bSendMessage2) bSendMessage2 = true;
  }
    
  if(messageValid1) ///valid message and received from Port, now we can write but don't send msg yet
  {
    Power1 =buffer1[4]*256 + buffer1[5];
    PowerCk1 = (264 - (buffer1[4] + buffer1[5])) & 0xFF;
    if (PowerCk1 == buffer1[7] ){
      if (verbosity > 4) {
        Serial.print(Power1);
        Serial.print(" W ");
        Serial.print("VALID\n");
      }
      bSendMessage1 = true;
    } else {
      if (verbosity > 0) {
        Serial.print(Power1);
        Serial.print(" W ");
        Serial.print("NOT VALID\n");
        Serial.print(PowerCk1, HEX);
        Serial.print(" vs. ");
        Serial.print(buffer1[7], HEX);
        Serial.println();
      }
    }
    messageValid1=false; //message has been consumed
    tail1 = 0; //frame fully consumed either way; re-arm for next sync sequence (previously only reset on success, allowing tail1 to walk past the buffer on repeated checksum failures)
  }

  if(messageValid2) ///valid message
  {
    Power2 =buffer2[4]*256 + buffer2[5];
    PowerCk2 = (264 - (buffer2[4] + buffer2[5])) & 0xFF;
    if (PowerCk2 == buffer2[7] ){
      if (verbosity > 0) {
        Serial.print(Power2);
        Serial.print(" W ");
        Serial.print("VALID L2\n");
      }
      bSendMessage2 = true;
    } else {
      if (verbosity > 0) {
        Serial.print(Power2);
        Serial.print(" W ");
        Serial.print("NOT VALID L2\n");
        Serial.print(PowerCk2, HEX);
        Serial.print(" vs. ");
        Serial.print(buffer2[7], HEX);
        Serial.println();
      }
    }
    messageValid2=false;
    tail2 = 0; //frame fully consumed either way; re-arm for next sync sequence
  }

  if (bSendMessage1){
    if (trace) {Serial.print("trying to write output to RS485 Port 1 = ");Serial.println(L1_Output);}
    highByte1 = (uint8_t)(L1_Output >> 8);
    lowByte1 = ( L1_Output - highByte1*256 );
    cksum1 = 264 - ((highByte1 + lowByte1) & 0xFF);
    {
      buffer1[0]=0x24; buffer1[1]=0x56;
      buffer1[2]=0x0; buffer1[3]='!'; buffer1[4]=highByte1;
      buffer1[5]=lowByte1; buffer1[6]=0x80;
      buffer1[7]=cksum1;
      {
        Serial1.write(buffer1, 8);  //write outputs bytes at low level, not null terminated
        if (verbosity > 4) {
          Serial.print("L1_Output = ");Serial.println(L1_Output);
        }
      }
    }
    messageValid1=false; //reset status
    tail1 = 0;
    bSendMessage1=false;
    bDisplayMessage=true;
    timer485_L1=millis();
  }

  if (bSendMessage2){
    if (trace) {Serial.print("trying to write output to RS485 Port 2 = ");Serial.println(L2_Output);}
    highByte2 = (uint8_t)(L2_Output >> 8);
    lowByte2 = ( L2_Output - highByte2*256 );
    cksum2 = 264 - ((highByte2 + lowByte2) & 0xFF);
    {
      buffer2[0]=0x24; buffer2[1]=0x56;
      buffer2[2]=0x0; buffer2[3]=0x21; buffer2[4]=highByte2;//!=0x21; 0x22 is trial also tried 0x20
      buffer2[5]=lowByte2; buffer2[6]=0x80;
      buffer2[7]=cksum2;
      {
        Serial2.write(buffer2, 8);  //write outputs bytes at low level, not null terminated
        if (verbosity > 4) {
          Serial.print("L2_Output = ");Serial.println(L2_Output);
        }
      }
    }
    messageValid2=false; //reset status
    tail2 = 0;
    bSendMessage2=false;
    bDisplayMessage=true;
    timer485_L2=millis();
  }

  if(bDisplayMessage) {
    displayOutValues(L1_Output, L2_Output);
    displayInValues(f_L1_Power, f_L2_Power, Power1, Power2);
    bDisplayMessage=false;
  }
  
  if (currentTime - previousTime >= 1000) {
    tick++;
    strcpy(subject_text,getLocaltime(buffer));
    if(b_Start){
      char buf[60]="";
      int sec = STARTUP_DELAY - (currentTime - tStart)/1000;
      sprintf(buf, "\n #FF0000 Startup in# %d#FF0000  sec#", sec );
      strcat(subject_text, buf);
    }
    if (currentTime > (tStart + STARTUP_DELAY*1000)) b_Start=false; //one shot
    lv_label_set_text(label1, subject_text);

    strcpy(subject_text,wifi_info(buffer));
    lv_label_set_text(label3, subject_text);

    previousTime = currentTime;
  } 
  
  if(b_New_L1_Power){
    if(!b_Start) { //account for delays in readout as well as power feeder 30 sec otherwise throw out update
      float tOutput; //use positive and negative values
      if (currentTime > (lastPowerChangeL1 + 30000)){
        //f_L1_Power is total_power_from_grid, one whole-household reading published
        //  identically to both Line1Set and Line2Set -- halve it so the two lines
        //  don't each independently apply the full correction (2026-07 oscillation fix)
        tOutput = L1_Output + f_L1_Power / 2.0; //if f_L1_Power is negative, it will lower power out
        //TODO(L1/L2 balance): with the feeder running, L1 reads positive and L2 reads
        //  negative such that L1+L2 should trend toward 0. The previous cross-line
        //  correction here always fired because it gated on L1_Power/L2_Power, which
        //  were declared but never assigned (always 0), rather than the real Power1/
        //  Power2 readback. Re-derive the correct coupling (and any P/I/D constants)
        //  from logged input/output data before reintroducing it.
        if (tOutput > MAX_POWER) tOutput = MAX_POWER;
        if (tOutput<0) tOutput = 0;
        if (tOutput - L1_Output > MAX_STEP_PER_UPDATE) tOutput = L1_Output + MAX_STEP_PER_UPDATE;
        else if (L1_Output - tOutput > MAX_STEP_PER_UPDATE) tOutput = L1_Output - MAX_STEP_PER_UPDATE;
        lastPowerChangeL1 = currentTime;
      }else{
        tOutput = L1_Output;
      }
      L1_Output = (int) tOutput;
    }
    b_New_L1_Power=false;//fully processed power
    bSendMessage1=true; //output the new value to the feeder
    bDisplayMessage=true;
  }

  if(b_New_L2_Power){
    if(!b_Start) { //account for delays in readout as well as power feeder 30 sec otherwise throw out update
      float tOutput; //use positive and negative values
      if (currentTime > (lastPowerChangeL2 + 30000)){
        //halved for the same reason as L1 above (2026-07 oscillation fix)
        tOutput = L2_Output + f_L2_Power / 2.0; //if f_L2_Power is negative, it will lower power out
        //TODO(L1/L2 balance): see matching note in the b_New_L1_Power block above.
        if (tOutput > MAX_POWER) tOutput = MAX_POWER;
        if (tOutput<0) tOutput = 0;
        if (tOutput - L2_Output > MAX_STEP_PER_UPDATE) tOutput = L2_Output + MAX_STEP_PER_UPDATE;
        else if (L2_Output - tOutput > MAX_STEP_PER_UPDATE) tOutput = L2_Output - MAX_STEP_PER_UPDATE;
        lastPowerChangeL2 = currentTime;
      }else{
        tOutput = L2_Output;
      }
      L2_Output = (int) tOutput;
    }
    b_New_L2_Power=false;//fully processed power
    bSendMessage2=true; //output the new value to the feeder
    bDisplayMessage=true;
  }

  lv_timer_handler();

    if ((tick > 4)  && (UNIT_NUMBER < 2)) {
    //read value from A0
    //int Rvalue = random(2000);

    //print to serial monitor
    if (verbosity > 4){
      Serial.print("Sending message to topic: '");
      Serial.print(pubtopic1+String("' :"));
      Serial.print((uint16_t) L1_Output);
      Serial.println();
      Serial.print("Sending message to topic: '");
      Serial.print(pubtopic2+String("' :"));
      Serial.print((uint16_t) L2_Output);
      Serial.println();
    }
      //publish the message to the specific topic
      mqttClient.beginMessage(pubtopic1);
      mqttClient.print((uint16_t) L1_Output);
      mqttClient.endMessage();
      //publish the message to the specific topic
      mqttClient.beginMessage(pubtopic2);
      mqttClient.print((uint16_t) L2_Output);
      mqttClient.endMessage();
      tick=0;
  }


  // call poll() regularly to allow the library to send MQTT keep alives which
  // avoids being disconnected by the broker

  //if mqtt gets a different number, recalculate new setpoint
  //L1_Output <= Power going out to feeder
  //Power1 <= The measured local output at L1, read back over RS485
  //b_New_L1_Power <= new L1 power just read
  mqttClient.poll();

} //loop

