//Name: Joel Molina
//Date: 4/20/2024
//Purpose: Create an evaporation cooling system (Swamp Cooler)

#include <LiquidCrystal.h>
#include <dht.h>
#include <Stepper.h>
#include <Wire.h>
#include <RTClib.h>

//time setup
DS1307 rtc;

//fan motor setup
int speedPin = 5;
int dir1 = 6; 
int dir2 = 7;

//stepper motor setup
const int stepsPerRevolution = 2048;
Stepper myStepper = Stepper(stepsPerRevolution, 8, 10, 9, 11); //In1-In3-In2-In4 for proper step sequence

//DHT setup
dht DHT;
#define DHT11_PIN 4

//LCD setup
const int RS = 45, EN = 43, D4 = 47, D5 = 49, D6 = 51, D7 = 53;
LiquidCrystal lcd(RS, EN, D4, D5, D6, D7);
  
//UART setup 
//UART macros
#define RDA 0x80
#define TBE 0x20

//UART pointers
volatile unsigned char* myUCSR0A  = (unsigned char*) 0x00C0;
volatile unsigned char* myUCSR0B  = (unsigned char*) 0x00C1;
volatile unsigned char* myUCSR0C  = (unsigned char*) 0x00C2;
volatile unsigned int*  myUBRR0   = (unsigned int*) 0x00C4;
volatile unsigned char* myUDR0    = (unsigned char*) 0x00C6;

//ADC pointers
volatile unsigned char* my_ADMUX = (unsigned char*) 0x7C;
volatile unsigned char* my_ADCSRB = (unsigned char*) 0x7B;
volatile unsigned char* my_ADCSRA = (unsigned char*) 0x7A;
volatile unsigned int* my_ADC_DATA = (unsigned int*) 0x78;

//water sensor setup
#define POWER_PIN 52
#define SIGNAL_PIN A0
int waterSensorValue = 0;

//general global variables
int mode;
volatile bool startButtonPressed = 0;
volatile bool stopButtonPressed = 0;
volatile bool resetButtonPressed = 0;
unsigned long previousTime = 0; //Stores last LCD update time.
const long updateInterval = 60000; //60000 milliseconds (1-minute per update)
float fWaterLevel = 0;
float waterLevelThreshold = 0.2;
int temperature = 0;
int temperatureThreshold = 25;
bool firstTime = -1;
bool toggleDirFlag = 0;

//port A Register Pointers (used for LED's)
volatile unsigned char* port_a = (unsigned char*) 0x22; 
volatile unsigned char* ddr_a  = (unsigned char*) 0x21; 
volatile unsigned char* pin_a  = (unsigned char*) 0x20;

//port E Register Pointers (used for start & stop buttons) | PE3 used for fan motor
volatile unsigned char* port_e = (unsigned char*) 0x2E; 
volatile unsigned char* ddr_e  = (unsigned char*) 0x2D; 
volatile unsigned char* pin_e  = (unsigned char*) 0x2C;

//port B Register Pointers (used for water sensor output)
volatile unsigned char* port_b = (unsigned char*) 0x25; 
volatile unsigned char* ddr_b  = (unsigned char*) 0x24; 
volatile unsigned char* pin_b  = (unsigned char*) 0x23;

//port D Register Pointers (used for reset and changePos buttons)
volatile unsigned char* port_d = (unsigned char*) 0x2B; 
volatile unsigned char* ddr_d  = (unsigned char*) 0x2A; 
volatile unsigned char* pin_d  = (unsigned char*) 0x29;

//port H Register Pointers (used for fan motor)
volatile unsigned char* port_h = (unsigned char*) 0x102; 
volatile unsigned char* ddr_h  = (unsigned char*) 0x101; 
volatile unsigned char* pin_h  = (unsigned char*) 0x100;

void setup() 
{
  //Initialize serial port
  U0Init(9600);

  //set LED's to output mode 
  //pinA(1,3,5,7)
  *ddr_a |= 0x02; //blue LED
  *ddr_a |= 0x08; //green LED
  *ddr_a |= 0x20; //yellow LED
  *ddr_a |= 0x80; //red LED

  //set buttons to input mode
  *ddr_e &= 0xEF; //start | PE4
  *ddr_e &= 0xDF; //stop | PE5
  *ddr_d &= 0xFB; //reset | PD2
  *ddr_a &= 0xF1; //changePos | PA0


  //set fan motor dir & speed pins to output mode
  *ddr_h |= 0x10;//PH4 dir2 pin (IN4)
  *ddr_h |= 0x08;//PH3 dir1 pin (IN3)
  *ddr_e |= 0x08;//PE3 speed pin (ENB)

  //time setup
  Wire.begin();
  rtc.begin();
  if(!rtc.isrunning())
  {
    rtc.adjust(DateTime(__DATE__, __TIME__));
  }

  //initialize LCD
  lcd.begin(16, 2);
  
  //interrupt for stop, start, reset, and toggle vent direction buttons
  attachInterrupt(digitalPinToInterrupt(2), start, RISING);
  attachInterrupt(digitalPinToInterrupt(3), stop, RISING);
  attachInterrupt(digitalPinToInterrupt(19), reset, RISING);
  attachInterrupt(digitalPinToInterrupt(18), start, RISING);

  //water sensor pin as output
  *ddr_b |= 0x02;

  //initialize ADC
  adc_init();

  //start in disabled mode
  mode = 0;
}

void loop()
{
  //Change vent position if allowed
  if((*pin_a & 0x01)) //&& (toggleDirFlag == 0))
  {
    myStepper.setSpeed(5);
    myStepper.step(stepsPerRevolution);
    printTime();
    writeString("vent position changed");
  }

  //disabled mode
  if(mode == 0)
  {
    //yellow LED on, rest off
    *port_a |= 0x20; //y
    *port_a &= 0xFD; //b
    *port_a &= 0xF7; //g
    *port_a &= 0x7F; //r

    //reset firstTime flag for LCD updates
    if(firstTime == 1)
    {
      firstTime = 0;
    }

    //disable water sensor, no reason to be polling the water level in disabled mode
    *port_b &= 0xFD;

    //fan motor off
    *port_e &= 0xF7;

    //path 1: Start button pressed, go to idle mode
    if(startButtonPressed)
    {
      mode = 2;
      startButtonPressed = 0;
      printTime();
      writeString("Switched to idle mode");
      writeChar('\n');
    }

    //allowed to change vent position
    toggleDirFlag = 0;
  }
  else
  {
    //gather DHT11 data
    DHT.read11(DHT11_PIN);
    temperature = DHT.temperature;
    int humidity = DHT.humidity;

    //Update LCD with current humidity and temperature every minute
    //From disabled to idle want to update immediately so there isnt a 1-minute wait at the start.
    if(firstTime == 0)
    {
      unsigned long currentTime = millis();
      if(currentTime - previousTime >= 1)
      {
        previousTime = currentTime;
        updateLCD(temperature, humidity);
      }
      firstTime = 1;
    }
    else
    {
      unsigned long currentTime = millis();
      if(currentTime - previousTime >= updateInterval)
      {
        previousTime = currentTime;
        updateLCD(temperature, humidity);
      }
    }

    //Gather water sensor value
    *port_b |= 0x02;
    int waterLevel = adc_read(0);
    //holds a value between 0-1 for ease of use of the values
    fWaterLevel = ((float) waterLevel / 1023) * 5.0; 
    //writeWaterLevel(fWaterLevel); function call for test.

    //error mode
    if(mode == 1)
    {
      //red LED on, rest off
      *port_a |= 0x80; //r
      *port_a &= 0xFD; //b
      *port_a &= 0xF7; //g
      *port_a &= 0xDF; //y

      //Display error Message
      errorLCD();

      //fan motor off
      *port_e &= 0xF7;

      //Lock ability to adjust vent position
      toggleDirFlag = 1;

      //path 1: stop button pressed, go to disabled mode
      if(stopButtonPressed)
      {
        mode = 0;
        stopButtonPressed = 0;

        printTime();
        writeString("Switched to disabled mode");
        writeChar('\n');

      }

      //path 2: reset button pressed, go to idle mode
      if(resetButtonPressed && (fWaterLevel > waterLevelThreshold))
      {
        mode = 2;
        resetButtonPressed = 0;

        printTime();
        writeString("Switched to idle mode");
        writeChar('\n');
      }
    }

    //idle mode
    if(mode == 2)
    {
      //green LED on, rest off
      *port_a |= 0x08; //g
      *port_a &= 0xFD; //b
      *port_a &= 0x7F; //r
      *port_a &= 0xDF; //y

      //fan motor off
      *port_e &= 0xF7;

      //path 1: stop button pressed, go to disabled mode
      if(stopButtonPressed)
      {
        mode = 0;
        stopButtonPressed = 0;

        printTime();
        writeString("Switched to disabled mode");
        writeChar('\n');
      }

      //path 2: water level <= threshold, go to error mode
      if(fWaterLevel <= waterLevelThreshold)
      {
        mode = 1;

        printTime();
        writeString("Switched to error mode");
        writeChar('\n');
      }

      //path 3: temperature > threshold: go to running mode
      if(temperature > temperatureThreshold)
      {
        mode = 3;

        printTime();
        writeString("Switched to running mode");
        writeChar('\n');
        writeString("fan on");
        writeChar('\n');
      }

      //allowed to change vent position
      toggleDirFlag = 0;
    }

    //running mode
    if(mode == 3)
    {
      //blue LED on, rest off
      *port_a |= 0x02; //b
      *port_a &= 0x7F; //r
      *port_a &= 0xF7; //g
      *port_a &= 0xDF; //y

      //start fan motor, by default CW
      *port_e |= 0x08;
      *port_h |= 0x10;
      *port_h &= 0xF7;

      //path 1: stop button pressed, go to disabled mode
      if(stopButtonPressed)
      {
        mode = 0;
        stopButtonPressed = 0;

        printTime();
        writeString("Switched to disabled mode");
        writeChar('\n');
        writeString("fan off");
        writeChar('\n');
      }

      //path 2: temperature <= threshold: go to idle mode
      if(temperature <= temperatureThreshold)
      {
        mode = 2;

        printTime();
        writeString("Switched to idle mode");
        writeChar('\n');
        writeString("fan off");
        writeChar('\n');
      }

      //path 3: water level < threshold, go to error mode
      if(fWaterLevel < waterLevelThreshold)
      {
        mode = 1;

        printTime();
        writeString("Switched to error mode");
        writeChar('\n');
        writeString("fan off");
        writeChar('\n');
      }

      //allowed to change vent position
      toggleDirFlag = 0;
    }
  }
}


//interrupt functions for start & stop functions
void start()
{
  startButtonPressed = 1;
}

void stop()
{
  stopButtonPressed = 1;
}

void reset()
{
  resetButtonPressed = 1;
}

//writes humidity and temperature to LCD
void updateLCD(int t, int h)
{
  lcd.clear();
  //int -> string
  char tempStr[3]; 
  char humStr[3];

  sprintf(tempStr, "%d", t);
  sprintf(humStr, "%d", h);

  lcd.setCursor(0, 0);
  lcd.write("temperature:");
  lcd.write(tempStr);

  lcd.setCursor(0, 1);
  lcd.write("humidity:");
  lcd.write(humStr);
}

//writes error message to LCD
void errorLCD()
{
  lcd.clear();
  
  lcd.setCursor(0, 0);
  lcd.write("Water level is");

  lcd.setCursor(0, 1);
  lcd.write("too low");
}

//UART functions
void U0Init(int U0baud) //Serial.begin
{
 unsigned long FCPU = 16000000;
 unsigned int tbaud;
 tbaud = (FCPU / 16 / U0baud - 1);
 *myUCSR0A = 0x20;
 *myUCSR0B = 0x18;
 *myUCSR0C = 0x06;
 *myUBRR0  = tbaud;
}

unsigned char kbhit() //Serial.available
{
  return *myUCSR0A & RDA;
}

unsigned char getChar() //Serial.read
{
  return myUDR0;
}

void writeChar(unsigned char U0pdata) //Serial.write
{
  while((*myUCSR0A & TBE) == 0);
  *myUDR0 = U0pdata;
}

//convert numbers to char
void writeNumber(int number) 
{
  if (number < 10) 
  {
    writeChar('0'); //0 padding
    writeChar('0' + number);
  } else 
  {
    writeChar('0' + number / 10); 
    writeChar('0' + number % 10);
  }
}

//special case for printing the year
void writeYear(int number)
{
  writeChar('0' + (number / 1000) % 10);
  writeChar('0' + (number / 100) % 10);
  writeChar('0' + (number / 10) % 10); 
  writeChar('0' + number % 10);
}

//write strings using char function
void writeString(const char* s) 
{
  //break at null terminator
  for (int i = 0; s[i] != '\0'; i++) 
  {
    writeChar(s[i]); // Write each character
  }
}


//ADC functions
void adc_init()
{
  // setup the A register
  *my_ADCSRA |= 0b10000000; //set bit 7 to 1 to enable ADC
  *my_ADCSRA &= 0b11011111; //clear bit 6 to 0 to disable ADC trigger mode
  *my_ADCSRA &= 0b11110111; //clear bit 5 to 0 to disable ADC interrupt
  *my_ADCSRA &= 0b11111000; //clear bit 0-2 to 0 to set prescaler selection to slow reading

  // setup the B register
  *my_ADCSRB &= 0b11110111; //clear bit 3 to 0 to reset the channel and gain bits
  *my_ADCSRB &= 0b11111000; //clear bit 2-0 to 0 to set free running mode

  // setup the MUX Register
  *my_ADMUX  &= 0b01111111; //clear bit 7 to 0 for AVCC analog reference
  *my_ADMUX  |= 0b01000000; //set bit   6 to 1 for AVCC analog reference
  *my_ADMUX  &= 0b11011111; //clear bit 5 to 0 for right adjust result
  *my_ADMUX  &= 0b11100000; //clear bit 4-0 to 0 to reset the channel and gain bits
}

unsigned int adc_read(unsigned char adc_channel_num)
{
  //clear the channel selection bits (MUX 4:0)
  *my_ADMUX  &= 0b11100000;

  //clear the channel selection bits (MUX 5)
  *my_ADCSRB &= 0b11110111;

  //set the channel number
  if(adc_channel_num > 7)
  {
    //set the channel selection bits, but remove the most significant bit (bit 3)
    adc_channel_num -= 8;
    // set MUX bit 5
    *my_ADCSRB |= 0b00001000;
  }
  //set the channel selection bits
  *my_ADMUX  += adc_channel_num;

  //set bit 6 of ADCSRA to 1 to start a conversion
  *my_ADCSRA |= 0x40;

  //wait for the conversion to complete
  while((*my_ADCSRA & 0x40) != 0);

  //return the result in the ADC data register
  return *my_ADC_DATA;
}

//write time of events
void printTime() 
{
  DateTime now = rtc.now();
  writeString("Date:" );

  writeYear(now.year());
  writeChar('-');
  writeNumber(now.month());
  writeChar('-');
  writeNumber(now.day());
  writeChar(' ');

  writeString("Time: ");

  writeNumber(now.hour());
  writeChar(':');
  writeNumber(now.minute());
  writeChar(':');
  writeNumber(now.second());
  writeChar('\n');
}

//test function: write water level values to serial monitor
/*
void writeWaterLevel(float v)
{
  int ones = (int) v;
  int tenths = (int) (v * 10);
  tenths = tenths % 10;
  int hundreths = (int) (v * 100);
  hundreths = hundreths % 10;

  writeChar(ones + '0');
  writeChar('.');
  writeChar(tenths + '0');
  writeChar(hundreths + '0');
  writeChar('\n');
}
*/




