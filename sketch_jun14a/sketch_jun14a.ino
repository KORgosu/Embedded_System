#define BUTTON 7 // input pin where the button is connected
#include <Adafruit_NeoPixel.h>
#include <Servo.h>
#define NUMPIXELS 2
#define PIN1 3
#define PIN2 2
Adafruit_NeoPixel pixels1(NUMPIXELS, PIN1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels2(NUMPIXELS, PIN2, NEO_GRB + NEO_KHZ800);


Servo myservo1; // create servo object to control a servo
Servo myservo2;

void setup() {  
  Serial.begin(9600);
  myservo1.attach(9); // attaches the servo on pin 9 to the servo object
  myservo1.write(0);
  pixels1.begin();
  pixels2.begin();

}  

char data;

void loop() {  
  if(Serial.available())
  {
    data = Serial.read();
  }
  if(data == '1')
  {
    myservo1.write(130);
    pixels1.setPixelColor(0, 255, 0, 0);
    pixels1.show();
    pixels1.setPixelColor(1, 255, 0, 0);
    pixels1.show();
    pixels2.setPixelColor(0, 255, 0, 0);
    pixels2.show();
    pixels2.setPixelColor(1, 255, 0, 0);
    pixels2.show();
  }
  else if(data == '0')
  {
    pixels1.clear();
    pixels1.show();
    pixels2.clear();
    pixels2.show();
    myservo1.write(50); 
  }  
} 
