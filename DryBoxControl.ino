/***
 * 
 * Pins:
 * 
 * A4,A5  - I2C Display control (A4 - SDA, A5 - SCL)
 * 
 * 

 * D2     - not used
 * D1     - not used
 * D3     - Tachometer pin
 * D4     - not used
 *
 * D5     - Rotary Encoder Switch
 * D6     - Rotary Encoder DT_pin                 Note: some encoder are different and you may have to change DT and CLK Pin.
 * D7     - Rotary Encoder CLK_pin                      Clockwise spin should increase all values.
 * D8     - Digital in DHT11 sensor
 * D9     - Analog out PWM Fan Air exchange
 * D10    - Analog out PWM Fan Heating
 * D11    - Analog out PWM Heating
 * D12    - 
 * D13    - Digital out LEDPIN 13 (interne LED)
 */

#include <SPI.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Arduino.h>
#include <DHT.h>
#include "DryBoxControl.h"
#include "WRKeyStateDef.h"
#include "DryBoxDisplay.h"
#include "HeatingData.h"

// defines for DHT11 Temp and Humitidy Sensor
#define DHTPIN 8        // Digital pin connected to the DHT sensor
//#define DHTTYPE DHT11   // chose the DHT type you are using
#define DHTTYPE DHT22 

// Define the tachometer pin
#define FAN_PIN 3 // Pin where the fan's tachometer output is connected
int tRPM = -1;  //Threshold of fan rpm error. -1 == Off feature
volatile int rpmCounter = 0; // Counter for the tachometer pulses
int rpm = 0;
bool rpmUpdate = false;
bool isFanfails = false;
unsigned long lastMillis = 0; // Variable to store the last time the RPM was calculated
const unsigned long interval = 1000; // Interval at which to calculate RPM (1 second)

volatile uint8_t B100HzToggle = 0;  // 100 Hertz Signal
uint8_t ui10MilliSekCount = 0;

int encoder_value = 500;
int last_encoder_value  = 500;
int EncSwitch = 5;
int DT_pin = 6;   //swap lines to change direction on the encoder
int CLK_pin = 7;  //swap lines to change direction on the encoder
int DT_pinstate;
int last_DT_pinstate;

int AirFanSpeed = 100;    // Extract Fan Speed in Percent
int HeatFanSpeed = 50;    // HeatFan Speed in Percent
int HeatingPower = 25;    // Heating Power in Percent

int testAirFanSpeed = 0;    // Extract Fan Speed in Percent for testing mode
int testHeatFanSpeed = 0;    // HeatFan Speed in Percent for testing mode
int testHeatingPower = 0;    // Heating Power in Percent for testing mode

// default values at startup, if nothing is stored in the eeprom
int DryTemperature = 35;    // Destination Dry Temperature in degree celsius
int DryTime_Hours = 1;      
int DryTime_Minutes = 30;
int curDryTime_Hours = 1;
int curDryTime_Minutes = 30;

//-- adjustable seetings for drying process, stored in HeatingData class
HeatingData heatingData;    // see HeatingData class


//-- State var for rotary encoder weitch  ----
uint8_t encoderBUTTON_State=0;

//-- Remember State on for Heater, Heater Fan and Ventilation
boolean StateHeaterOn = false;
boolean StateHeaterFanOn = false;
boolean StateVentilationOn = false;
boolean turboMode = false;

DryBoxDisplay display;
DHT dht(DHTPIN, DHTTYPE);


/***
 * Saves the current settings for temperature, drying hours and minutes
 */
void SaveSettings() {
  EEPROM.write(0, (uint8_t) 17);
  EEPROM.write(1, (uint8_t) DryTemperature);
  EEPROM.write(2, (uint8_t) DryTime_Hours);
  EEPROM.write(3, (uint8_t) DryTime_Minutes);
  uint16_t rpmValue = (uint16_t) tRPM;
  EEPROM.write(4, (uint8_t) (rpmValue & 0xFF));
  EEPROM.write(5, (uint8_t) ((rpmValue >> 8) & 0xFF));
}


/***
 * Reads the last saved settings for temperature, drying hours and minutes
 */
void ReadSettings() {
  uint8_t isInit = EEPROM.read(0);

  if(isInit == 17) {
    DryTemperature = EEPROM.read(1);
    DryTime_Hours = EEPROM.read(2);
    DryTime_Minutes = EEPROM.read(3);
    uint8_t lowByte = EEPROM.read(4);
    uint8_t highByte = EEPROM.read(5);
    uint16_t rpmValue = lowByte | (highByte << 8);
    tRPM = (int16_t) rpmValue;
  }
}


// Interrupt is called once a millisecond, 
SIGNAL(TIMER0_COMPA_vect) 
{
  unsigned long currentMillis = millis();
  ui10MilliSekCount ++;

  if(ui10MilliSekCount >= 10 ) {
    ui10MilliSekCount = 0;
    B100HzToggle ^= 1;
  }

  ReadEncoder();  
}

void setup() {
  
  //setup fan tachometer
  pinMode(FAN_PIN, INPUT_PULLUP); // Set the tachometer pin as input with internal pull-up resistor
  attachInterrupt(digitalPinToInterrupt(FAN_PIN), countPulse, FALLING); // Attach an interrupt to count the pulses
  
  // setup Rotary encoder
  pinMode (DT_pin, INPUT);
  pinMode (CLK_pin, INPUT);
  digitalWrite(DT_pin, HIGH);
  digitalWrite(CLK_pin, HIGH);

  // setup Heating and air 
  pinMode(FANAIR_PIN, OUTPUT);  
  analogWrite(FANAIR_PIN, 0);            // PWM Extrcat Fan off
  pinMode(FANHEATING_PIN, OUTPUT);  
  analogWrite(FANHEATING_PIN, 0);        // PWM Heating Fan  off
  pinMode(HEATING_PIN, OUTPUT);
  analogWrite(HEATING_PIN, 0);           // PWM Heating off  

  ReadSettings();

  // Reads the initial state of DT
  last_DT_pinstate = digitalRead(DT_pin);

  display.Setup();
  display.SetVersion(APP_VERSION);
  display.ScreenOut(SCR_WELCOME);
  dht.begin();                      // start DHT

  // Timer setup --------------------------
  // Timer0 is already used for millis() - we'll just interrupt somewhere
  // in the middle and call the "Compare A" function below
  OCR0A = 0xAF;
  TIMSK0 |= _BV(OCIE0A);    

}

void ReadEncoder() {
    DT_pinstate = digitalRead(DT_pin);
    if (DT_pinstate != last_DT_pinstate) { //did DT changed state?
      if (digitalRead(CLK_pin) == DT_pinstate) { // if DT changed state, check CLK
        encoder_value--; // rotation is counter-clockwise, decrement the value
      }else{
        encoder_value++; // rotation is clockwise, increment the value
      }
    last_DT_pinstate = DT_pinstate; //save the last state of DT
    }
}


/***
 * EncoderValueChange - increments or decrements the value of given var depending
 * from the direction the rotary encoder is moved. The Value of var is kept in range
 * of rangeMin and rangeMax.
 * 
 * param int * valToModify : pointer of var to modify
 * param int rangeMin: minimum value 
 * param int rangeMax: maximum value
*/
void EncoderValueChange(int * valToModify, int rangeMin, int rangeMax, int increment, bool off) {
    int aktValue = * valToModify;

    if(last_encoder_value != encoder_value)
    {

      if(encoder_value > last_encoder_value + 1)
      {
        last_encoder_value = encoder_value;
        if(aktValue < rangeMax && (aktValue+increment <= rangeMax))
        {
          if (aktValue == -1)
            aktValue++;
          aktValue += increment;
        }
      }
      
      if(encoder_value + 1  < last_encoder_value )
      {
        last_encoder_value = encoder_value;
        if(aktValue > rangeMin && (aktValue-increment >= rangeMin))
          aktValue -= increment;
        else if (off)
          aktValue = -1;
      }
    }

    * valToModify = aktValue;
}


boolean IsPWMStateOn(int pwmValue) {
  if(pwmValue > 0)
    return true;
  else  
    return false;
}

void SetPWMRate(uint8_t pin, int ratePercent)
{
  int PWMVal = 255 * ratePercent / 100;
  analogWrite(pin,PWMVal);

  // check which Pin (heater, heater fan or ventilation) and store the state on or off
  switch (pin) {
  case FANAIR_PIN: StateVentilationOn = IsPWMStateOn(PWMVal);
  break;
  case FANHEATING_PIN: StateHeaterFanOn = IsPWMStateOn(PWMVal);
  break;
  case HEATING_PIN: StateHeaterOn = IsPWMStateOn(PWMVal);
  break;
  }
}

void calculateRPM() {
  unsigned long currentMillis = millis(); // Get the current time
  
  if (currentMillis - lastMillis >= interval) {
    // One second has passed, calculate the RPM
    noInterrupts(); // Disable interrupts temporarily
    int rpmReading = rpmCounter; // Copy the pulse count
    rpmCounter = 0; // Reset the pulse count
    interrupts(); // Re-enable interrupts
    
    // Calculate RPM: 
    // Fan gives 2 pulses per revolution (for typical 3-pin fans)
    rpm = (rpmReading * 30); // 60 seconds per minute / 2 pulses per revolution = 30
    rpmUpdate = true;
    if (rpm < tRPM) {
      isFanfails = true;
    } else {
      isFanfails = false;
    }
    lastMillis = currentMillis; // Update the last time the RPM was calculated
  }
}

char* RollingMessage(boolean StateHeaterOn, boolean StateHeaterFanOn, boolean StateVentilationOn, boolean turboMode, int curDryTime_Hours, int curDryTime_Minutes)
{
    static char buf[96]; 
    buf[0] = '\0'; 
    
    snprintf(buf, sizeof(buf), "Heater: %s, Fan: %s, Vent: %s, Turbo: %s, Time: %02d:%02d ",
            StateHeaterOn ? "On" : "Off",
            StateHeaterFanOn ? "On" : "Off",
            StateVentilationOn ? "On" : "Off",
            turboMode ? "On" : "Off",
            curDryTime_Hours,
            curDryTime_Minutes);
               
    if (tRPM != -1)
    {
        char rpmBuf[24];
        sprintf(rpmBuf, ", PTC FAN RPM = %d, ", rpm);
        strcat(buf, rpmBuf);
    }
    return buf;
}

void countPulse() {
  rpmCounter++;
}

// Main loop -----------------------------------------------------------
void loop() {
  static uint8_t AppState=0;
  static uint8_t StateTrigger = 0;
  uint8_t aktStateTrigger;
  static int aktModeNo = 1;
  static int aktTestModeNo = 1;
  static int aktTimeEdMode = 1;  // 1= edit hour, 2= minutes, 3= exit edit
  static int aktBreakModNo = 1;

  static uint8_t ui100HzSecCounter=0;   // counter for a second
  static uint8_t ui100HzSensorTimer=200;  // read DHT11 sensor every 2 seconds
  static int runMinuteTimer = 6000;
  static int rDelay = 1;
  static int airExChgEndCounter = 2000;
  static float humidity = 0.0;
  static float temperature = 0.0;
  
  int oldHeatFanSpeed=0;
  int oldAirFanSpeed=0;
  int oldHeatingPower=0;
  int oldModeNo=0;
  int oldTestModeNo=0;
  int oldTimeEdMode=0;  
  int oldDryTemperature=0;
  int old_tRPM=0;
  int oldTimeVal=0;
  int oldBreakModNo = 1;
  
  char szBuf[8];

  aktStateTrigger = B100HzToggle;

  // Processing State-Machine. ----------------------------------------------------------
  // The state machine is triggered on every Change of flank of the timer toogle.
  // This happens 100 times in a second.

  if(aktStateTrigger != StateTrigger) {
    StateTrigger = aktStateTrigger;

    CheckKeyState(&encoderBUTTON_State, EncSwitch);

    // The DHT11 is a little slow,. So we shouldn't
    // receive values too quickly
    if(ui100HzSensorTimer > 0) {
      ui100HzSensorTimer--;
    } else {
      ui100HzSensorTimer = 200;
      humidity = dht.readHumidity();
      temperature = dht.readTemperature();

     if (AppState == AST_RUNDRYING){
        // Check for NaN values
        if (isnan(humidity) || isnan(temperature)) {
          // Stop the drying process if NaN values are detected
          dryController(DST_TEARDOWN, temperature);
          display.ScreenOut(SCR_ERROR); 
          display.PrintError("DHT Sensor Error"); // Display error message
          AppState = AST_IDLE; // Transition to a safe state
        }        
      }
      
      if(AppState == AST_MODE_SELECT || AppState == AST_TESTMODE) {
        display.PrintTHValue(temperature, humidity);
      }
    }   

    //Read fan tachometer    
    if (tRPM != -1)
    { 
      calculateRPM();      
      if (isFanfails && AppState == AST_RUNDRYING)
      {
        dryController(DST_TEARDOWN, temperature);
        display.ScreenOut(SCR_ERROR); 
        display.PrintError("FAN at low RPM!"); // Display error message
        AppState = AST_IDLE; // Transition to a safe state
        isFanfails = false;
      }          
    }   

    // App-State-Machine processing and step through
    switch(AppState) {

      case AST_IDLE:
          
        if(encoderBUTTON_State == 1 )
        {
        AppState = AST_PREPARE_SELECT;
        }
        break;

      case AST_PREPARE_SELECT:
        display.ScreenOut(SCR_MENUBASE);
        display.updateModSelect(aktModeNo);
        if(aktModeNo == 1)
        {
          display.PrintDestTemp(DryTemperature, 6);
        }
        if(aktModeNo == 2)
          display.PrintDestTime(DryTime_Hours, DryTime_Minutes, 6);
        if(aktModeNo == 3)
          display.PrintDestRPM(tRPM, 6);
        AppState = AST_MODE_SELECT;      
        break;

      case AST_MODE_SELECT:
        oldModeNo = aktModeNo;
        //adjust menue size
        if (tRPM != -1)   
          EncoderValueChange(&aktModeNo, 1, 8, 1, false);
        else
          EncoderValueChange(&aktModeNo, 1, 7, 1, false);  

        if(oldModeNo != aktModeNo)  // there is a change
        {
          display.updateModSelect(aktModeNo);
          if(aktModeNo == 1)
            display.PrintDestTemp(DryTemperature, 6);
          if(aktModeNo == 2)
            display.PrintDestTime(DryTime_Hours, DryTime_Minutes, 6);
          if(aktModeNo == 3)
            display.PrintDestRPM(tRPM, 6);          
        }
        if(aktModeNo == 8 && rpmUpdate){
            display.FanRPM(rpm);
            rpmUpdate = false; 
        }        
        
        if(encoderBUTTON_State == 1 && aktModeNo == SELMOD_DRYTEMP)  // encoder switch pressed, set temperature
        {
          display.ScreenOut(SCR_SETTEMP);
          display.PrintDestTemp(DryTemperature, 0);
          display.CursorPos(1, 1);
          display.CursorOn();
          AppState = AST_SET_DRYTEMP;
        }

        if(encoderBUTTON_State == 1 && aktModeNo == SELMOD_DRYTIME)  // encoder switch pressed, set time
        {
          display.ScreenOut(SCR_SETTIME);
          display.PrintDestTime(DryTime_Hours, DryTime_Minutes, 0);
          display.CursorPos(0, 1);
          display.BlinkOn();          
          aktTimeEdMode = 1;
          AppState = AST_SET_DRYTIME;
        }

        if(encoderBUTTON_State == 1 && aktModeNo == SELMOD_DRYSTART)  // encoder switch pressed, start drying
        {
          display.ScreenOut(SCR_RUNNING);
          display.PrintHFVState(temperature, humidity);	
          curDryTime_Hours = DryTime_Hours;
          curDryTime_Minutes = DryTime_Minutes;
          runMinuteTimer = 6000;
          dryController(DST_STARTUP, temperature);
          AppState = AST_RUNDRYING;
        }

        if(encoderBUTTON_State == 1 && aktModeNo == SELMOD_RPM)  // encoder switch pressed, set RPM
        {
          display.ScreenOut(SCR_SET_RPM);
          display.PrintDestRPM(tRPM, 0);
          display.CursorPos(1, 1);
          display.CursorOn();
          AppState = AST_SET_RPM;
        }

        // save actual settings
        if(encoderBUTTON_State == 1 && aktModeNo == SELMOD_SAVE)
        {
          display.ScreenOut(SCR_SAVED);
          SaveSettings();
          AppState = AST_IDLE;
        }

        if(encoderBUTTON_State == 1 && aktModeNo == SELMOD_VERSION)
        {
          display.ScreenOut(SCR_WELCOME);
          AppState = AST_IDLE;
        }

        if(encoderBUTTON_State == 1 && aktModeNo == SELMOD_TESTING)  // encoder switch pressed, start testing mode
        {
          display.ScreenOut(SCR_TESTING);
          aktTestModeNo = 1;
          display.updateTestModSelect(aktTestModeNo);
          testAirFanSpeed = 0;    
          testHeatFanSpeed =0;    
          testHeatingPower =0;  
          AppState = AST_TESTMODE;
        }
        break;

      case AST_SET_DRYTEMP:
        oldDryTemperature = DryTemperature;
        // with one Heater, 57° is maximum. For more temperature, a second Heater 
        // and a bigger power supply is necessary.
        EncoderValueChange(&DryTemperature, 1, 55, 1, false);   
        if(oldDryTemperature != DryTemperature)
        {
          display.PrintDestTemp(DryTemperature, 0);    
          display.CursorPos(1, 1);      
        }

        if(encoderBUTTON_State == 1)
        {
          display.CursorOff();
          AppState = AST_PREPARE_SELECT;
        }
        break;

      case AST_SET_RPM:
        old_tRPM = tRPM;
        EncoderValueChange(&tRPM, 0, 1500, 100, true);   
        if(old_tRPM != tRPM)
        {
          display.PrintDestRPM(tRPM, 0);   
          display.CursorPos(1, 1);      
        }

        if(encoderBUTTON_State == 1)
        {
          display.CursorOff();
          AppState = AST_PREPARE_SELECT;
        }
        break;

      case AST_SET_DRYTIME: // select hour, minutes or Return
        oldTimeEdMode = aktTimeEdMode;
        EncoderValueChange(&aktTimeEdMode, 1, 3, 1, false);
        if(oldTimeEdMode != aktTimeEdMode)
        {
          display.SetEdTimeCursorPos(aktTimeEdMode);
          display.BlinkOn();        
        }

        if(encoderBUTTON_State == 1 && aktTimeEdMode == 1) // change hours
        {
          display.BlinkOff();
          display.CursorOn();
          AppState = AST_ED_DRYHOUR;
        } 

        if(encoderBUTTON_State == 1 && aktTimeEdMode == 2) // change minutes
        {
          display.BlinkOff();
          display.CursorOn();
          AppState = AST_ED_DRYMINUTE;
        }                       

        if(encoderBUTTON_State == 1 && aktTimeEdMode == 3)
        {
          display.BlinkOff();
          AppState = AST_PREPARE_SELECT;
        }
        break;

      case AST_ED_DRYHOUR:
        oldTimeVal = DryTime_Hours;
        EncoderValueChange(&DryTime_Hours, 0, 99, 1, false);
        if(oldTimeVal != DryTime_Hours)
        {
          display.PrintDestTime(DryTime_Hours, DryTime_Minutes, 0);
          display.SetEdTimeCursorPos(aktTimeEdMode);         
        }

        if(encoderBUTTON_State == 1) // return to Edit select
        {
          display.SetEdTimeCursorPos(aktTimeEdMode);
          display.CursorOff();
          display.BlinkOn();
          AppState = AST_SET_DRYTIME;
        }
        break;

      case AST_ED_DRYMINUTE:
        oldTimeVal = DryTime_Minutes;
        EncoderValueChange(&DryTime_Minutes, 0, 59, 1, false);
        if(oldTimeVal != DryTime_Minutes)
        {
          display.PrintDestTime(DryTime_Hours, DryTime_Minutes, 0);
          display.SetEdTimeCursorPos(aktTimeEdMode);         
        }

        if(encoderBUTTON_State == 1) // return to Edit select
        {
          display.SetEdTimeCursorPos(aktTimeEdMode);
          display.CursorOff();
          display.BlinkOn();
          AppState = AST_SET_DRYTIME;
        }      
        break;        

      case AST_RUNDRYING:
        // Timer check and display -------------------------------
        // The active state is called 100 times per second. So 6000 equals one minute
        if(rDelay > 0) //need a small delay or it will break stuff
          rDelay--;        
        else 
        {
          rDelay = 1;
          char* scrolMsg = RollingMessage(StateHeaterOn, StateHeaterFanOn, StateVentilationOn, turboMode, curDryTime_Hours, curDryTime_Minutes);
          if (scrolMsg != NULL) 
          {
            display.DisScrollText(scrolMsg);
            //free(scrolMsg);
          } 
          else            
            display.DisScrollText("Error: No memory");
            //display.DisScrollText("Memory allocation failed");
          }
        
        if(runMinuteTimer > 0) {
          runMinuteTimer--;
        }
        else {
          runMinuteTimer = 6000;
          if(curDryTime_Minutes > 0) {
            curDryTime_Minutes--;
            display.PrintHFVState(temperature, humidity);
          } else {
            if(curDryTime_Hours > 0) {
              curDryTime_Hours--;
              curDryTime_Minutes = 59;
              display.PrintDestTime(curDryTime_Hours, curDryTime_Minutes, 5);
            } else {
              // drying ready
              dryController(DST_TEARDOWN, temperature);
              display.PrintHFVState(temperature, humidity);
              AppState = AST_ENDVENTILATION;
              //final fresh air ventilation after drying
              airExChgEndCounter = (int) heatingData.finalAirExtractionTime * 100;  // fresh air for defined amount of seconds
              SetPWMRate(FANAIR_PIN, 80);              
              display.PrintHFVState(temperature, humidity);
            }
          }
        }

        // Heating control ------------------------------------
        if(AppState == AST_RUNDRYING) {// only while AST_RUNDRYING is active, call dryController
          dryController(0, temperature);
          display.PrintHFVState(temperature, humidity);
        }

        if(encoderBUTTON_State == 1)
        {
          AppState = AST_RUNPAUSE;
          dryController(DST_BREAK, temperature);
          display.ScreenOut(SCR_RUNBREAK);
          display.CursorPos(0, 1);          
          display.BlinkOn();
          aktBreakModNo=1;
        }
        break;        

      case AST_ENDVENTILATION:
        if(airExChgEndCounter > 0) {
          airExChgEndCounter--;
        } 
        else {
          SetPWMRate(FANAIR_PIN, 0);
          AppState = AST_PREPARE_SELECT;
        }
        break;

      case AST_RUNPAUSE:
        oldBreakModNo = aktBreakModNo;
        EncoderValueChange(&aktBreakModNo, 1, 2, 1, false);
        if(oldBreakModNo != aktBreakModNo) {
          display.SetBreakCursorPos(aktBreakModNo);
        }

        if(encoderBUTTON_State == 1 && aktBreakModNo == 1) // continue
        {
          display.BlinkOff();
          dryController(DST_CONTINUE, temperature);
          dryController(0, temperature);
          AppState = AST_RUNDRYING;
          display.ScreenOut(SCR_RUNNING);
          display.PrintHFVState(temperature, humidity);  											 
        }    

        if(encoderBUTTON_State == 1 && aktBreakModNo == 2) // stop
        {
          display.BlinkOff();
          dryController(DST_TEARDOWN, temperature);
          AppState = AST_PREPARE_SELECT;           
        }      
        break;
      
      case AST_TESTMODE:
        oldTestModeNo = aktTestModeNo;
        EncoderValueChange(&aktTestModeNo, 1, 4, 1, false);
        if(oldTestModeNo != aktTestModeNo) {
          display.updateTestModSelect(aktTestModeNo);
        if(aktTestModeNo == 2)
          display.PrintPercentValue(testHeatFanSpeed);
        if(aktTestModeNo == 3)
          display.PrintPercentValue(testHeatingPower);
        if(aktTestModeNo == 4)
          display.PrintPercentValue(testAirFanSpeed);
        }    

        if(encoderBUTTON_State == 1 && aktTestModeNo == 1) { // exit testing
          SetPWMRate(HEATING_PIN, 0);
          SetPWMRate(FANHEATING_PIN, 0);
          SetPWMRate(FANAIR_PIN, 0);

          AppState = AST_PREPARE_SELECT;
        }

        if(encoderBUTTON_State == 1 && aktTestModeNo == 2) { // set test Fan speed
          display.PrintPercentValue(testHeatFanSpeed);
          display.CursorPos(6, 1);
          display.CursorOn();          
          AppState = AST_FAN_CONTROL;
        }

        if(encoderBUTTON_State == 1 && aktTestModeNo == 3) { // set test Heat power
          display.PrintPercentValue(testHeatingPower);
          display.CursorPos(6, 1);
          display.CursorOn();          
          AppState = AST_HEAT_CONTROL;
        }

        if(encoderBUTTON_State == 1 && aktTestModeNo == 4) { // set test Air Fan speed
          display.PrintPercentValue(testAirFanSpeed);
          display.CursorPos(6, 1);
          display.CursorOn();          
          AppState = AST_AIR_CONTROL;
        }        
        break;
      
      // Fan Speed, on exit switch off Fan
      case AST_FAN_CONTROL:
        oldHeatFanSpeed = testHeatFanSpeed;
        EncoderValueChange(&testHeatFanSpeed, 0, 99, 1, false);
        if(oldHeatFanSpeed != testHeatFanSpeed) {
          display.PrintPercentValue(testHeatFanSpeed);
          SetPWMRate(FANHEATING_PIN, testHeatFanSpeed);
        }

        if(encoderBUTTON_State == 1 )
        { 
        display.CursorOff();
        display.updateTestModSelect(aktTestModeNo);
        display.PrintPercentValue(testHeatFanSpeed);
        AppState = AST_TESTMODE;
        }
        break;

      case AST_AIR_CONTROL:
        oldAirFanSpeed = testAirFanSpeed;
        EncoderValueChange(&testAirFanSpeed, 0, 99, 1, false);
        if(oldAirFanSpeed != testAirFanSpeed) {
          display.PrintPercentValue(testAirFanSpeed);
          SetPWMRate(FANAIR_PIN, testAirFanSpeed);
        }

        if(encoderBUTTON_State == 1 )
        {
        display.CursorOff();
        display.updateTestModSelect(aktTestModeNo);
        display.PrintPercentValue(testAirFanSpeed);
        AppState = AST_TESTMODE;
        }
        break;

      case AST_HEAT_CONTROL:
        oldHeatingPower = testHeatingPower;
        EncoderValueChange(&testHeatingPower, 0, 99, 1, false);
        if(oldHeatingPower != testHeatingPower) {
          display.PrintPercentValue(testHeatingPower);
          SetPWMRate(HEATING_PIN, testHeatingPower);
        }

        if(encoderBUTTON_State == 1 )
        {
        display.CursorOff();
        display.updateTestModSelect(aktTestModeNo);
        display.PrintPercentValue(testHeatingPower);
        AppState = AST_TESTMODE;
        }
        break;        
    }

  }

}

// --- functions for temperature control ------------------

/***
 * dryController(uint8_t doState, float aktTemperature)
 *
 * Controlling the drying process with heating and fresh air.
 * 
 * param uint8_t doState: force action from external caller
 * param float aktTemperature: actual temperature given from extern
 */
 void dryController(uint8_t doState, float aktTemperature)
 {
   static uint8_t DryState=0;
   static int oneSecondCounter=100; // The Controller is called 100 times every second
   static uint8_t rampSecCounter=0;
   static int     airExChgOneMinuteCounter=6000; // Controller is called 100 times every second, 6000 counts equals 1 minute
   static uint8_t airExChgMinutesCounter=0;

   //static HeatingData heatingData;    // see HeatingData class

   if(doState > 0) {
     DryState = doState;
   } else {
     if(DryState != DST_AIR_EXCHANGE) {
        if(airExChgOneMinuteCounter > 0 ) {
          airExChgOneMinuteCounter--;
        } else {
          airExChgOneMinuteCounter=6000;
          airExChgMinutesCounter++;          
        }
     }
   }

   switch(DryState) {
     case DST_STARTUP:
        oneSecondCounter=100;
        rampSecCounter=0;
        airExChgOneMinuteCounter=6000;
        airExChgMinutesCounter=0;
        heatingData.SetupHeatingValues(DryTemperature);
        turboMode = true;
        DryState = DST_RAMPUP_HEATER;
        break;

     case DST_TEARDOWN:
        SetPWMRate(FANHEATING_PIN, 0);
        SetPWMRate(HEATING_PIN, 0);
        SetPWMRate(FANAIR_PIN, 0);
        break;

     case DST_RAMPUP_HEATER:
        if(oneSecondCounter > 0) {
          oneSecondCounter--;
        } else {
          oneSecondCounter=100;
          if(rampSecCounter < 2) { // soft power on the heater
            rampSecCounter++;
            SetPWMRate(FANHEATING_PIN, heatingData.defaultHeaterFanPWM);
            SetPWMRate(HEATING_PIN, heatingData.rampUpHeatPWM[rampSecCounter]);              
          } else {
            oneSecondCounter=250; // set default power to heater and wait in next step for 2,5 seconds
            SetPWMRate(HEATING_PIN, heatingData.defaultHeaterPWM);
            if(turboMode == true) { // set heater and heater fan to turbo mode at first heat up
              SetPWMRate(FANHEATING_PIN, heatingData.turboHeaterFanPWM);
              SetPWMRate(HEATING_PIN, heatingData.turboHeaterPWM);        
            }
            DryState = DST_WAIT_DEST_TEMP;
          }
        }        
        break;

     case DST_WAIT_DEST_TEMP:
        if(oneSecondCounter > 0) {
          oneSecondCounter--;
        } else { 
            if(turboMode == true) { // turbo mode power
              if(aktTemperature + heatingData.compareOffsetTurboMode >= DryTemperature) { // turbo mode offset temperature reached?
                SetPWMRate(HEATING_PIN, heatingData.defaultHeaterPWM);
                SetPWMRate(FANHEATING_PIN, heatingData.defaultHeaterFanPWM);
                turboMode = false;         
              } else {
                SetPWMRate(HEATING_PIN, heatingData.turboHeaterPWM); // in case of temperature drop, switch heater to high power again
                SetPWMRate(FANHEATING_PIN, heatingData.turboHeaterFanPWM);
              }
            } else { // default heating and heater fan power
              if(aktTemperature + heatingData.compareOffset >= DryTemperature) {
                SetPWMRate(HEATING_PIN, heatingData.nearDestHeaterPWM);             
              } else {
                SetPWMRate(HEATING_PIN, heatingData.defaultHeaterPWM); // in case of temperature drop, switch heater to high power again
              }
            }

        }

        if(aktTemperature >= DryTemperature) {
          if (tRPM > -1)
            SetPWMRate(FANHEATING_PIN, heatingData.defaultHeaterPWM); 
          else
            SetPWMRate(FANHEATING_PIN, heatingData.lowHeaterFanPWM); // heating fan continue with lower speed. Lowest defined speed for the range
          SetPWMRate(HEATING_PIN, 0);                  // heater off
          DryState = DST_TEMP_REACHED;         
        }        
        break;        

     case DST_TEMP_REACHED:
        if(aktTemperature < DryTemperature) {
          oneSecondCounter=100;
          rampSecCounter=0;
          DryState = DST_RAMPUP_HEATER;               // go back to ramp up the heater
        }

        if(airExChgMinutesCounter >= heatingData.airExchangeIntervallMinutes) {
          airExChgOneMinuteCounter = (int) heatingData.airExtractionTime * 100;  // fresh air for 20 seconds
          SetPWMRate(FANAIR_PIN, heatingData.ventilationFanPWN);     // speed for ventilation fan in percent. Adjust value here, if you need more or less
          SetPWMRate(HEATING_PIN, heatingData.ventilationHeaterPWM);  // additional power to the heater for smaller temperature drop
          DryState = DST_AIR_EXCHANGE;
        }
        break;

      case DST_AIR_EXCHANGE:
        if(airExChgOneMinuteCounter > 0) {
          airExChgOneMinuteCounter--;
        } else {
          airExChgOneMinuteCounter = 6000; //set for next count
          airExChgMinutesCounter = 0;
          SetPWMRate(HEATING_PIN, 0); // heater has to switch off. Otherwise the temp will continue to raise.
          SetPWMRate(FANAIR_PIN, 0);
          DryState = DST_TEMP_REACHED;
        }
        break;

      case DST_BREAK:
        SetPWMRate(FANHEATING_PIN, 50);  // Generally in not a good idea to turn of PTC heaters fan right after shutdown.ToDo: add 5 min timer
        SetPWMRate(HEATING_PIN, 0);      
        break;

      case DST_CONTINUE:
        oneSecondCounter=100;
        rampSecCounter=0;      
        DryState = DST_RAMPUP_HEATER;
        break;        
   }
 }