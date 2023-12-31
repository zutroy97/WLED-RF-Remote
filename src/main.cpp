#include <Arduino.h>
#include <EEPROM.h>

const byte REMOTE_RF_PIN  = 2; // Input from RF module output
const byte newRemoteButtonPressMaxCount = 5; // Number of times a new remote must be pressed during programming window.
const byte newRemoteWindownInSeconds = 10; // New Remote must be programmed within this number of seconds after startup.
const unsigned long Remote_Repeat_Time_ns = 100000; //100 ms. Wait this long before reporting another press of the same button.

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

const byte EEPROM_ADDR_REMOTE_ID = 0x00; // EEPROM address to write remote id
unsigned int remote_id = 0xffff;  // Remote Id to respond to.

unsigned long getRemoteIdFromEeprom(); // Fetch the last remote id from EEPROM
void writeRemoteIdToEeprom(unsigned long remoteId); // Write remote id to EEPROM (if changed.)
void WriteToSerialHumanReadable(void);
void HandleProgrammingNewRemoteId(void);

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

  if (receivedCommand.isReady == 1)
  {
    HandleProgrammingNewRemoteId();
    if (remote_id == receivedCommand.packet.id.remote)
    {
      if (receivedCommand.count == 1){ // Don't allow holding down the button.
        WriteToSerialHumanReadable();
      } 
    }
    receivedCommand.isReady = 0; // notify the interrupt handler this packet has been handled.
  }
}

void HandleProgrammingNewRemoteId(void){
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

void WriteToSerialHumanReadable(){
  Serial.write("REMOTE ID: 0x"); Serial.print(receivedCommand.packet.id.remote, HEX);
  Serial.write(" COMMAND: 0x"); Serial.print(receivedCommand.packet.id.command, HEX);Serial.write(" ");
  switch (receivedCommand.packet.id.command){
    case remote_buttons::RED:
      Serial.write("RED");
      break;
    case remote_buttons::GREEN:
      Serial.write("GREEN");
      break;
    case remote_buttons::ON:
      Serial.write("ON");
      break;          
    case remote_buttons::WHITE:
      Serial.write("WHITE");
      break;
    case remote_buttons::BLUE:
      Serial.write("BLUE");
      break;
    case remote_buttons::OFF:
      Serial.write("OFF");
      break;
    case remote_buttons::UP:
      Serial.write("UP");
      break;
    case remote_buttons::DOWN:
      Serial.write("DOWN");
      break;
    case remote_buttons::MINUS:
      Serial.write("MINUS");
      break;
    case remote_buttons::PLUS:
      Serial.write("PLUS");
      break;
    case remote_buttons::DOUBLE_ARROW:
      Serial.write("Double Arrow");
      break;          
    default:
      Serial.write(receivedCommand.packet.id.command);
      break;
  }
  Serial.write("\n");  
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