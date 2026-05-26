#include <FreeRTOS_ARM.h>
#include <Wire.h>

// RTClib 설정 vs Wire 기반 설정 변경부분
// 아래 숫자를 1로 설정하면 RTClib 사용
// 0으로 설정하면 Wire로 DS3231 직접 제어 수행
#define USE_RTCLIB 1

#if USE_RTCLIB
  #include "RTClib.h"
  RTC_DS3231 rtc;
#endif

#define DS3231_ADDR 0x68

// FreeRTOS 핸들
TaskHandle_t handle_rtc_task;
TaskHandle_t handle_command_task;

SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t timeDataMutex;

// 공유 시간 데이터
struct RtcTimeData {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  float temperature;
};

RtcTimeData g_rtcData;

// 공통 유틸
uint8_t bcdToDec(uint8_t val) {
  return ((val >> 4) * 10) + (val & 0x0F);
}

uint8_t decToBcd(uint8_t val) {
  return ((val / 10) << 4) | (val % 10);
}

uint8_t monthStringToNumber(const char *monthStr) {
  if (!strcmp(monthStr, "Jan")) return 1;
  if (!strcmp(monthStr, "Feb")) return 2;
  if (!strcmp(monthStr, "Mar")) return 3;
  if (!strcmp(monthStr, "Apr")) return 4;
  if (!strcmp(monthStr, "May")) return 5;
  if (!strcmp(monthStr, "Jun")) return 6;
  if (!strcmp(monthStr, "Jul")) return 7;
  if (!strcmp(monthStr, "Aug")) return 8;
  if (!strcmp(monthStr, "Sep")) return 9;
  if (!strcmp(monthStr, "Oct")) return 10;
  if (!strcmp(monthStr, "Nov")) return 11;
  if (!strcmp(monthStr, "Dec")) return 12;
  return 1;
}

// Zeller 공식 기반 요일 계산
// DS3231 day register는 보통 1~7 사용
uint8_t calculateDayOfWeek(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 3) {
    month += 12;
    year--;
  }

  uint16_t k = year % 100;
  uint16_t j = year / 100;

  uint8_t h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;

  // h: 0=Saturday, 1=Sunday, 2=Monday ...
  // DS3231용으로 1=Sunday, 2=Monday ... 7=Saturday 형태로 변환
  uint8_t dow = ((h + 6) % 7) + 1;
  return dow;
}

// __DATE__, __TIME__ 파싱
void parseCompileDateTime(uint16_t &year, uint8_t &month, uint8_t &day, uint8_t &hour, uint8_t &minute, uint8_t &second) {
  char monthStr[4];
  int dayInt, yearInt;
  int hourInt, minuteInt, secondInt;

  sscanf(__DATE__, "%3s %d %d", monthStr, &dayInt, &yearInt);
  sscanf(__TIME__, "%d:%d:%d", &hourInt, &minuteInt, &secondInt);

  year = yearInt;
  month = monthStringToNumber(monthStr);
  day = dayInt;
  hour = hourInt;
  minute = minuteInt;
  second = secondInt;
}

// Wire 직접 구현용 DS3231 함수
// 위에서 USE_RTCLIB == 0 으로 설정한경우
#if !USE_RTCLIB

bool ds3231WriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool ds3231ReadRegisters(uint8_t startReg, uint8_t *buffer, uint8_t length) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(startReg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t received = Wire.requestFrom(DS3231_ADDR, length);

  if (received != length) {
    return false;
  }

  for (uint8_t i = 0; i < length; i++) {
    buffer[i] = Wire.read();
  }

  return true;
}

bool ds3231LostPower() {
  uint8_t statusReg;

  if (!ds3231ReadRegisters(0x0F, &statusReg, 1)) {
    return true;
  }

  // OSF: Oscillator Stop Flag, bit 7
  return statusReg & 0x80;
}

void ds3231ClearOSF() {
  uint8_t statusReg;

  if (ds3231ReadRegisters(0x0F, &statusReg, 1)) {
    statusReg &= ~0x80;
    ds3231WriteRegister(0x0F, statusReg);
  }
}

void ds3231EnableOscillator() {
  uint8_t controlReg;

  if (ds3231ReadRegisters(0x0E, &controlReg, 1)) {
    // EOSC bit = 0 이면 oscillator 동작
    controlReg &= ~0x80;
    ds3231WriteRegister(0x0E, controlReg);
  }
}

bool ds3231AdjustToCompileTime() {
  uint16_t year;
  uint8_t month, day, hour, minute, second;

  parseCompileDateTime(year, month, day, hour, minute, second);

  uint8_t dow = calculateDayOfWeek(year, month, day);

  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);                       // seconds register부터 쓰기 시작
  Wire.write(decToBcd(second));            // 00H seconds
  Wire.write(decToBcd(minute));            // 01H minutes
  Wire.write(decToBcd(hour));              // 02H hours, 24시간 모드
  Wire.write(decToBcd(dow));               // 03H day of week
  Wire.write(decToBcd(day));               // 04H date
  Wire.write(decToBcd(month));             // 05H month
  Wire.write(decToBcd(year - 2000));        // 06H year

  if (Wire.endTransmission() != 0) {
    return false;
  }

  ds3231ClearOSF();
  return true;
}

bool ds3231ReadTime(RtcTimeData &data) {
  uint8_t raw[7];

  if (!ds3231ReadRegisters(0x00, raw, 7)) {
    return false;
  }

  data.second = bcdToDec(raw[0] & 0x7F);
  data.minute = bcdToDec(raw[1] & 0x7F);

  // 24시간 모드 기준
  data.hour = bcdToDec(raw[2] & 0x3F);

  data.day = bcdToDec(raw[4] & 0x3F);
  data.month = bcdToDec(raw[5] & 0x1F);
  data.year = 2000 + bcdToDec(raw[6]);

  return true;
}

bool ds3231ReadTemperature(float &temperature) {
  uint8_t raw[2];

  if (!ds3231ReadRegisters(0x11, raw, 2)) {
    return false;
  }

  int8_t msb = (int8_t)raw[0];
  uint8_t lsb = raw[1];

  // DS3231 온도 LSB 상위 2비트가 0.25도 단위
  temperature = msb + ((lsb >> 6) * 0.25);

  return true;
}

#endif

// RTC 초기화
bool initRTC() {
#if USE_RTCLIB

  if (!rtc.begin()) {
    Serial.println("RTC not found");
    return false;
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power. Adjusting to compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  } else {
    Serial.println("RTC power normal. Current time is preserved.");
  }

  return true;

#else
  ds3231EnableOscillator();

  if (ds3231LostPower()) {
    Serial.println("RTC lost power. Adjusting to compile time.");

    if (!ds3231AdjustToCompileTime()) {
      Serial.println("RTC adjust failed");
      return false;
    }
  } else {
    Serial.println("RTC power normal. Current time is preserved.");
  }

  return true;

#endif
}

// RTC 읽기 함수
bool readRTC(RtcTimeData &data) {
#if USE_RTCLIB

  DateTime now = rtc.now();

  data.year = now.year();
  data.month = now.month();
  data.day = now.day();
  data.hour = now.hour();
  data.minute = now.minute();
  data.second = now.second();
  data.temperature = rtc.getTemperature();

  return true;

#else

  if (!ds3231ReadTime(data)) {
    return false;
  }

  if (!ds3231ReadTemperature(data.temperature)) {
    return false;
  }

  return true;

#endif
}

// 출력 함수
void printRtcData(const RtcTimeData &data) {
  Serial.print(data.year);
  Serial.print('/');

  if (data.month < 10) Serial.print('0');
  Serial.print(data.month);
  Serial.print('/');

  if (data.day < 10) Serial.print('0');
  Serial.print(data.day);
  Serial.print(' ');

  if (data.hour < 10) Serial.print('0');
  Serial.print(data.hour);
  Serial.print(':');

  if (data.minute < 10) Serial.print('0');
  Serial.print(data.minute);
  Serial.print(':');

  if (data.second < 10) Serial.print('0');
  Serial.print(data.second);

  Serial.print(" | Temp: ");
  Serial.print(data.temperature);
  Serial.println(" C");
}

// Task 1: RTC 주기적 읽기
void task_rtc(void *pvParameters) {
  Serial.println("RTC task started");

  for (;;) {
    RtcTimeData localData;

    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      bool ok = readRTC(localData);
      xSemaphoreGive(i2cMutex);

      if (ok) {
        if (xSemaphoreTake(timeDataMutex, portMAX_DELAY) == pdTRUE) {
          g_rtcData = localData;
          xSemaphoreGive(timeDataMutex);
        }

        printRtcData(localData);
      } else {
        Serial.println("RTC read failed");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ======================================================
// Task 2: UART 명령 처리
// r: 현재 저장된 RTC 데이터 출력
// s: 컴파일 시각으로 RTC 재설정
// ======================================================

void task_command(void *pvParameters) {
  Serial.println("Command task started");
  Serial.println("Command: r = read current data, s = set compile time");

  for (;;) {
    if (Serial.available()) {
      char cmd = Serial.read();

      if (cmd == 'r') {
        RtcTimeData localData;

        if (xSemaphoreTake(timeDataMutex, portMAX_DELAY) == pdTRUE) {
          localData = g_rtcData;
          xSemaphoreGive(timeDataMutex);
        }

        Serial.println("=== Current RTC Data ===");
        printRtcData(localData);
      }

      if (cmd == 's') {
        if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {

#if USE_RTCLIB
          rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
          Serial.println("RTC adjusted to compile time using RTClib.");
#else
          if (ds3231AdjustToCompileTime()) {
            Serial.println("RTC adjusted to compile time using Wire.");
          } else {
            Serial.println("RTC adjust failed.");
          }
#endif

          xSemaphoreGive(i2cMutex);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ======================================================
// setup / loop
// ======================================================

void setup() {
  Serial.begin(115200);

  while (!Serial) {
    ;
  }

  Serial.println("RTC FreeRTOS Practice Start");

  Wire.begin();

  i2cMutex = xSemaphoreCreateMutex();
  timeDataMutex = xSemaphoreCreateMutex();

  if (i2cMutex == NULL || timeDataMutex == NULL) {
    Serial.println("Mutex creation failed");
    while (1);
  }

  if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
    bool rtcOk = initRTC();
    xSemaphoreGive(i2cMutex);

    if (!rtcOk) {
      Serial.println("RTC initialization failed");
      while (1);
    }
  }

#if USE_RTCLIB
  Serial.println("Mode: RTClib");
#else
  Serial.println("Mode: Wire direct");
#endif

  xTaskCreate(
    task_rtc,
    "RTC Task",
    512,
    NULL,
    2,
    &handle_rtc_task
  );

  xTaskCreate(
    task_command,
    "Command Task",
    512,
    NULL,
    1,
    &handle_command_task
  );

  vTaskStartScheduler();

  Serial.println("Failed to start scheduler");
  while (1);
}

void loop() {
  // FreeRTOS 사용 시 loop는 사용하지 않음
}