
#define BUTTON 7 // input pin where the button is connected
#include <Adafruit_NeoPixel.h>
#include <Servo.h>
#define NUMPIXELS 2
#define PIN1 3
#define PIN2 2
Adafruit_NeoPixel pixels1(NUMPIXELS, PIN1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels2(NUMPIXELS, PIN2, NEO_GRB + NEO_KHZ800);

int val = 0; // it is used to store the state of the input pin
int old_val = 0; // it is used to maintain state in the previous step of the input pin
int state = 0; // stores the motor status: 0 for off, 1 for on

Servo myservo1; // create servo object to control a servo
Servo myservo2;

void setup() {
  Serial.begin(9600);
  myservo1.attach(9); // attaches the servo on pin 9 to the servo object
  myservo1.write(0);
  pinMode(BUTTON, INPUT); // sets the pin input 
  pixels1.begin();
  pixels2.begin();
}  
  
void loop() {  
  val = digitalRead(BUTTON); // reads the input value and saves into val
  
  // checks if something happened 
  if ((val == HIGH) && (old_val == LOW)){  
    state = 1 - state;  
  }   
  
  old_val = val; // ricordiamo il valore precedente di val  
  
  if (state == 1) {  
    Serial.print('1');
    myservo1.write(50);
    pixels1.setPixelColor(0, 255, 0, 0);
    pixels1.show();
    pixels1.setPixelColor(1, 255, 0, 0);
    pixels1.show();
    pixels2.setPixelColor(0, 255, 0, 0);
    pixels2.show();
    pixels2.setPixelColor(1, 255, 0, 0);
    pixels2.show(); // starts the motor. 225 is the speed value. You can modify it from 155 to 255
  }  
  else {
    Serial.print('0'); 
    pixels1.clear();
    pixels1.show();
    pixels2.clear();
    pixels2.show();
    myservo1.write(130);
 
  }  
} 
