#include <Arduino.h>
#include <EEPROM.h>
#include <ArduinoJson.h>




const byte REMOTE_RF_PIN  = 2; // Input from RF module output
const byte newRemoteButtonPressMaxCount = 5; // Number of times a new remote must be pressed during programming window.
const byte newRemoteWindownInSeconds = 10; // New Remote must be programmed within this number of seconds after startup.
const unsigned long Remote_Repeat_Time_ns = 100000; //100 ms. Wait this long before reporting another press of the same button.
const byte brightnessStepAmount = 20; // Amount to increase/decrease brightness
/************************
 * End of configuration *
 * **********************/

// If a pulse is between these two values, consider it a Header pulse
const unsigned long MIN_HEADER_LENGTH  = 4000;
const unsigned int MAX_HEADER_LENGTH = 8000;
 
// between these two values, cosider it a 0
const unsigned int MIN_SPACE_LENGTH = 150;
const unsigned int MAX_SPACE_LENGTH = 250;

// between these two values, consider it a 1
const unsigned int MIN_MARK_LENGTH = 500;
const unsigned int MAX_MARK_LENGTH = 750;

enum remote_buttons : byte{
  UNKNOWN       = 0,
  ON            = 0x01,
  OFF           = 0x02,
  UP            = 0x03,
  MINUS         = 0x04,
  RED           = 0x05,
  DOUBLE_ARROW  = 0x06,
  DOWN          = 0x07,
  PLUS          = 0x08,
  WHITE         = 0x09,
  UNKNOWN_0X0A  = 0x0A,
  BLUE          = 0x0B,
  GREEN         = 0x0C
};

// Contains data about the RF packet
union RFPacket {
  struct{
    remote_buttons command :8;
    unsigned int remote :16;
  } id;
  unsigned long value;
} ;

// Used when receiving/building a packet
volatile struct RemoteCommand
{
  byte count;
  union RFPacket packet;
  byte isReady = 0;
  unsigned long receiveTime;
} receivedCommand;

const byte EEPROM_ADDR_REMOTE_ID = 0x00; // EEPROM address to write remote id
unsigned int remote_id = 0xffff;  // Remote Id to respond to.
byte brightness = 255;
unsigned int transistionSpeed = 0x7fff;

unsigned long getRemoteIdFromEeprom(); // Fetch the last remote id from EEPROM
void writeRemoteIdToEeprom(unsigned long remoteId); // Write remote id to EEPROM (if changed.)
void HandleProgrammingNewRemoteId(void);
void WriteJsonPower(bool turnOn);
void WriteJsonColor(remote_buttons button);
void WriteIteratePresets(void);
void WriteJsonBrightness(bool makeBrighter);
void WriteJsonTransistionSpeed(bool makeFaster);
void TryParseWledStatus(void);

void handleRfInterrupt()
{
  static union RFPacket workingPacket; // static means retain the state of these variables between method calls.
  static unsigned long lastChange = 0;
  unsigned long now = micros();
  static bool isHighBitPosition = false;
  static bool isCapturing = false;
  static byte bitPosition = 0;
  bool value = 0;
  unsigned long duration = now - lastChange; // How long since the previous pin state change
  
  // Reset the command count when the current button has been released. 
  if (duration > MAX_HEADER_LENGTH){
    receivedCommand.count = 0;
  }
  
  lastChange = now; // Remember for next time around

  // Do we have a Header pulse (Long duration. We know a duration this long is HIGH)
  if (duration >= MIN_HEADER_LENGTH && duration <= MAX_HEADER_LENGTH)
  {
    workingPacket.value = 0; // Reset our values
    isCapturing = true; // Start capturing
    isHighBitPosition = false; // Ignore the next (low) pulse
    bitPosition = 0; // Start building the value from 0
    return;
  }

  if (isCapturing == false)
  {
    return; // If we aren't capturing, nothing to do
  }
  if (duration >= MIN_SPACE_LENGTH && duration <= MAX_SPACE_LENGTH)
  {
    value = 0; // Found a small pulse (high or low, doesn't matter)
  }else if (duration >= MIN_MARK_LENGTH && duration <= MAX_MARK_LENGTH){
    value = 1; // Found a long pulse (has to be high,
  }else{
    isCapturing = 0; // Our packet encountered interferrence. Abort.
    return;
  }
  if (isHighBitPosition)
  { // Are we be interested in this pulse?
    bitPosition++; // Keep track of how many bits we've collected.
    workingPacket.value = (workingPacket.value << 1) | value; // Shift left and append value to least significant bit

    if (bitPosition == 24) // Do we have the entire packet?
    {
      isCapturing = 0; // Don't capture until next header
      if (receivedCommand.isReady == 0)
      {
        if ((receivedCommand.packet.value != workingPacket.value) )
        { // Different Packet as last time OR timeout occurred
          receivedCommand.packet.value = workingPacket.value;
          receivedCommand.count = 0;
        }else{
          receivedCommand.count++;
        }
        receivedCommand.receiveTime = now;    
        receivedCommand.isReady = 1;
      }
    }
  }
  isHighBitPosition = !isHighBitPosition;
};

unsigned long newRemoteTimeout;

void setup() {
  Serial.begin(115200);
  while (!Serial) continue;
  attachInterrupt(digitalPinToInterrupt(REMOTE_RF_PIN), handleRfInterrupt, CHANGE);
  Serial.write("Started\n");
  remote_id = getRemoteIdFromEeprom();
  Serial.write("REMOTE ID: 0x"); Serial.println(remote_id, HEX);
  newRemoteTimeout = micros() + ( newRemoteWindownInSeconds * 1000000); // seconds 
  //startupTime = micros();
}

byte newRemoteButtonPressCount = newRemoteButtonPressMaxCount;
unsigned int newRemoteId = 0xffff; 

void loop() {
  if (newRemoteTimeout > 0 && (micros() > newRemoteTimeout)){
    newRemoteTimeout = 0; // New Remote window expired. Completely disable new remote functionality.
  }

  TryParseWledStatus();

  if (receivedCommand.isReady == 1)
  {
    HandleProgrammingNewRemoteId();
    if (remote_id == receivedCommand.packet.id.remote)
    {
      if (receivedCommand.count == 1){ // Don't allow holding down the button.
        switch (receivedCommand.packet.id.command){
          case remote_buttons::RED:
          case remote_buttons::GREEN:
          case remote_buttons::BLUE:
          case remote_buttons::WHITE:
            WriteJsonColor(receivedCommand.packet.id.command);
            break;
          case remote_buttons::ON:
            WriteJsonPower(true);
            break;          
          case remote_buttons::OFF:
            WriteJsonPower(false);
            break;
          case remote_buttons::UP:
            WriteJsonTransistionSpeed(true);
            break;
          case remote_buttons::DOWN:
            WriteJsonTransistionSpeed(false);
            break;
          case remote_buttons::MINUS:
            WriteJsonBrightness(false);
            break;
          case remote_buttons::PLUS:
            WriteJsonBrightness(true);
            break;
          case remote_buttons::DOUBLE_ARROW:
            WriteIteratePresets();
            break;
          default:
            break;
        }        
      } 
    }
    receivedCommand.isReady = 0; // notify the interrupt handler this packet has been handled.
  }
}

void dump(){
  Serial.write("bri/transition "); Serial.print(brightness); Serial.write(" / ");Serial.println(transistionSpeed);
}

// {"state": {"bri": 128, "transition": 7634}}
// {"state": {"bri": 80}}
// {"state": {"transition": 5555}}
void TryParseWledStatus(){
  if (Serial.available() == false){return;}
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, Serial);
    // Test if parsing succeeds.
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }
  dump();
  brightness = doc["state"]["bri"] | brightness;
  transistionSpeed = doc["state"]["transition"] | transistionSpeed;
  dump();
}



void HandleProgrammingNewRemoteId(){
  if (newRemoteTimeout > 0 && (micros() < newRemoteTimeout) && (remote_id != receivedCommand.packet.id.remote)){
    // If in the timeout window, and a compatible remote has the same button pushed 5 times, use that remote.
    if (receivedCommand.count == 1){
      if (newRemoteButtonPressCount == newRemoteButtonPressMaxCount){
        newRemoteId = receivedCommand.packet.id.remote;
      }
      if (newRemoteId == receivedCommand.packet.id.remote){
        if (newRemoteButtonPressCount > 0) {
          newRemoteButtonPressCount--;
        }
        if (newRemoteButtonPressCount == 0){
          remote_id = receivedCommand.packet.id.remote;
          Serial.write("*** NEW REMOTE ID: 0x"); Serial.println(remote_id, HEX);
          writeRemoteIdToEeprom(remote_id);
          newRemoteTimeout = 0; // Completely disable new remote functionality.
        }
      }
    }
  }
}

void WriteJsonPower(bool turnOn){
  const size_t capacity = JSON_OBJECT_SIZE(1);
  StaticJsonDocument<capacity> doc;
  doc["on"] = turnOn;
  serializeJson(doc, Serial);
}

void WriteJsonBrightness(bool makeBrighter){
  const size_t capacity = JSON_OBJECT_SIZE(1);
  StaticJsonDocument<capacity> doc;
  byte currentBrightness = brightness;
  if (makeBrighter){
    brightness += brightnessStepAmount;
    if (brightness < currentBrightness){brightness=255;}
  }else{
    brightness -= brightnessStepAmount;
    if (brightness > currentBrightness){brightness = 0;}
  }
  doc["bri"] = brightness;
  serializeJson(doc, Serial);
}

void WriteJsonTransistionSpeed(bool makeFaster){
  const size_t capacity = JSON_OBJECT_SIZE(1);
  StaticJsonDocument<capacity> doc;
  unsigned int currentSpeed = transistionSpeed;
  if (makeFaster){
    transistionSpeed++;
    if (transistionSpeed < currentSpeed){transistionSpeed=0xffff;}
  }else{
    transistionSpeed--;
    if (transistionSpeed > currentSpeed){transistionSpeed = 0;}
  }
  doc["transition"] = transistionSpeed;
  serializeJson(doc, Serial);
}

void WriteJsonColor(remote_buttons button){
  StaticJsonDocument<64> doc;
  byte r=0, g=0, b=0, w=0;
  switch(button){
    case remote_buttons::BLUE:
      b=255; break;
    case remote_buttons::GREEN:
      g=255;break;
    case remote_buttons::RED:
      r=255; break;
    case remote_buttons::WHITE:
      w=255; break;
    default:
      return;
  }
  doc["on"] = true;
  JsonArray seg0 = doc["seg"][0].createNestedArray("col");
  seg0.add(r);
  seg0.add(g);
  seg0.add(b);
  seg0.add(w);
  serializeJson(doc, Serial);
}

void WriteIteratePresets(){
  const size_t capacity = JSON_OBJECT_SIZE(1);
  StaticJsonDocument<capacity> doc;
  doc["ps"] = "1~10~";
  serializeJson(doc, Serial);  
}


unsigned long getRemoteIdFromEeprom()
{
  unsigned long remoteId = 0xffff;
  EEPROM.get(EEPROM_ADDR_REMOTE_ID, remoteId);
  return remoteId;
}

void writeRemoteIdToEeprom(unsigned long remoteId){
  EEPROM.put(EEPROM_ADDR_REMOTE_ID, remoteId);
}

