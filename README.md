# Arduino 센서 데이터 수집 및 저장 시스템

FreeRTOS 기반의 다중 센서 실시간 데이터 수집·저장 프로젝트입니다.
기초 서보 제어부터 Flash 메모리 데이터 로깅까지 단계적으로 구성되어 있습니다.

---

## 프로젝트 구조

```
Arduino/
├── sketch_mar29a/   # 가변저항 서보 제어 (기초)
├── Sweep/           # 버튼 + 서보 + NeoPixel (기초)
├── sketch_jun14a/   # UART 서보/LED 제어 (기초)
├── FileSystem/      # SD카드 파일 I/O (중급)
├── I2C/             # 듀얼 센서 + FreeRTOS (고급)
└── SPI/             # Flash 데이터 로깅 + FreeRTOS (고급+)
```

---

## 스케치별 설명

### 1. `sketch_mar29a` — 가변저항 서보 제어
가장 기초적인 스케치. 아날로그 핀에서 가변저항 값을 읽어 서보 각도로 변환합니다.

| 항목 | 내용 |
|------|------|
| 하드웨어 | 가변저항 (A0), 서보모터 (Pin 9) |
| 동작 | 아날로그 값(0~1023) → 서보 각도(0~180°) 변환, 15ms 주기 업데이트 |
| 의존 라이브러리 | `Servo.h` |

---

### 2. `Sweep` — 버튼 토글 서보 + NeoPixel 상태 표시
버튼 입력으로 서보 위치와 RGB LED를 동시에 제어하는 상태 머신입니다.

| 항목 | 내용 |
|------|------|
| 하드웨어 | 버튼 (Pin 7), 서보 (Pin 9), NeoPixel 2체인 (Pin 2, 3) |
| 상태 1 | 서보 50°, NeoPixel 전체 빨간색 |
| 상태 0 | 서보 130°, NeoPixel 전체 OFF |
| 의존 라이브러리 | `Servo.h`, `Adafruit_NeoPixel.h` |

---

### 3. `sketch_jun14a` — UART 서보/LED 제어
시리얼 명령으로 서보와 NeoPixel을 원격 제어합니다.

| 항목 | 내용 |
|------|------|
| 통신 | UART 9600 baud |
| 명령 `'1'` | 서보 130°, NeoPixel 빨간색 |
| 명령 `'0'` | 서보 50°, NeoPixel OFF |
| 의존 라이브러리 | `Servo.h`, `Adafruit_NeoPixel.h` |

---

### 4. `FileSystem` — SD카드 파일 I/O
SdFat 라이브러리를 사용해 SD카드에 디렉토리를 생성하고 센서 식별 데이터를 저장·읽기합니다.

| 항목 | 내용 |
|------|------|
| 하드웨어 | SD카드 (SPI, CS Pin 10) |
| 동작 | `/HMC`, `/MPU` 디렉토리 생성 → 파일 쓰기 → 읽기 후 시리얼 출력 |
| 의존 라이브러리 | `SPI.h`, `SdFat.h` |

#### 트러블슈팅 이력

<details>
<summary>v1.0 ~ v2.0 문제 해결 과정 보기</summary>

##### [v1.0] 센서 데이터 전부 0으로 저장되는 버그
- **증상**: SD card 파일 및 시리얼 출력에서 X/Y/Z 가속도·자이로·자기장 값이 모두 0
- **상태**: 미해결 → 원인 분석 시작

##### [v1.1] FreeRTOS + Wire 인터럽트 충돌 원인 확인
FreeRTOS 스케줄러 시작 후 Wire(I2C) 인터럽트가 정상 동작하지 않는 것이 원인으로 확인.
`setup()`에서 `Wire.begin()` 및 센서 초기화는 정상이나, Task 내부에서 센서 읽기 시 0이 반환됨.

- **원인**: FreeRTOS 스케줄러가 TWI 인터럽트보다 높은 우선순위로 실행되어 Wire 통신 차단
- **시도한 해결 방법**:
  - `taskENTER_CRITICAL()` 적용 → Wire 인터럽트까지 차단되어 실패
  - NVIC TWI 우선순위 0으로 설정 → 효과 없음
  - `vTaskSuspendAll()` 적용 → 스케줄러만 정지, 인터럽트 허용 방식 시도했으나 실패
  - `Wire.begin()`을 Task 내부로 이동 → 실패
  - `task_sensor` 별도 분리 + Stack 1024 증가 → 실패
- **상태**: 미해결

##### [v1.2] SD card CS 핀(D4)이 I2C 버스 간섭 유발 확인
I2C 스캐너로 진단한 결과, CS 핀을 D4로 설정 시 SD card 초기화 후 I2C 버스에서 센서가 전혀 인식되지 않는 현상 확인.

- **진단 과정**:
  - SD 초기화 전 I2C 스캔: `0x2C`, `0x68` 정상 인식
  - SD 초기화 후 I2C 스캔: 아무것도 인식 안 됨
  - → `sd.begin(D4)`가 I2C 핀에 간섭
- **원인**: Arduino Due에서 D4가 SPI 관련 핀과 내부 충돌 발생
- **상태**: 원인 확인 완료, 해결 진행 중

##### [v1.3] CS 핀 D4 → D10 변경으로 해결 ✅

- **해결 방법**:
  ```cpp
  // 변경 전
  #define SD_CS_PIN 4

  // 변경 후
  #define SD_CS_PIN 10  // SPI 전용 CS 핀, I2C 간섭 없음
  ```
- **검증**:
  - SD 초기화 전 I2C 스캔: `0x2C`, `0x68` ✅
  - SD 초기화 후 I2C 스캔: `0x2C`, `0x68` ✅
  - `[HMC] X:-21376 Y:16639 Z:-12798` ✅
  - `[MPU Acc] X:-112 Y:236 Z:-17168` ✅
  - `[MPU Gyro] X:172 Y:354 Z:-138` ✅
- **상태**: 해결 완료 ✅

##### [v2.0] 최종 동작 확인 ✅
센서 데이터 정상 읽기 및 SD card 저장, UART 명령 출력까지 전체 기능 정상 동작 확인.

**최종 하드웨어 구성:**
```
Arduino Due
├── I2C 버스 (D20/D21)
│   ├── HMC5883  (0x2C) → /HMC/data.txt
│   └── MPU6050  (0x68) → /MPU/data.txt
└── SPI 버스 (MOSI/MISO/SCK + CS=D10)
    └── SD card
```

**FreeRTOS Task 구성:**
```
├── task_hmc5883 → i2cMutex → 5초 주기 읽기
├── task_mpu6050 → i2cMutex → 5초 주기 읽기
└── task_sd      → spiMutex → 10초 주기 저장
                            → 'r' 명령: 파일 출력
                            → 'c' 명령: 파일 초기화
```

**최종 출력 검증:**
```
SD task: saved                              ✅ 10초 주기 저장
=== /HMC/data.txt ===
X:-21376 Y:16639  Z:-12798                 ✅
X:-20864 Y:26623  Z:-11262                 ✅
=== /MPU/data.txt ===
Acc X:-112  Y:236  Z:-17168  Gyro X:172  Y:354  Z:-138   ✅
Acc X:-168  Y:324  Z:-17292  Gyro X:196  Y:365  Z:-120   ✅
=== 출력 완료 ===                           ✅
```

</details>

---

### 5. `I2C` — 듀얼 센서 실시간 측정 (FreeRTOS)
HMC5883L 지자기 센서와 MPU6050 IMU를 I2C로 동시에 읽는 멀티태스킹 시스템입니다.

#### 사용 센서
| 센서 | 타입 | I2C 주소 | 측정값 |
|------|------|----------|--------|
| HMC5883L | 3축 지자기 | 0x2C | X, Y, Z 자기장 |
| MPU6050 | 6축 IMU | 0x68 | 3축 가속도 + 3축 자이로 |

#### 아키텍처
- FreeRTOS Task 1: HMC5883 읽기 (100ms 주기)
- FreeRTOS Task 2: MPU6050 읽기 (100ms 주기)
- `i2cMutex` 세마포어로 I2C 버스 충돌 방지
- 스택: 태스크당 256 바이트 / 우선순위: 2

#### 센서 초기화
**HMC5883L:**
- 레지스터 0x29 = 0x06 (축 부호 설정)
- 레지스터 0x0B = 0x08 (범위 8 Gauss, Set/Reset 활성화)
- 레지스터 0x0A = 0xC3 (Suspend → Continuous Mode)

**MPU6050:**
- 레지스터 0x6B = 0x00 (Sleep Mode 해제)

| 의존 라이브러리 | `FreeRTOS_ARM.h`, `Wire.h` |
|---|---|

---

### 6. `SPI` — Flash 메모리 데이터 로깅 (FreeRTOS)
I2C 듀얼 센서 데이터를 SPI Flash에 실시간 저장하고, UART 명령으로 조회·초기화하는 시스템입니다.

#### 아키텍처
| 태스크 | 역할 | 주기 |
|--------|------|------|
| Write Task | I2C 센서 읽기 → Flash 기록 | 1000ms |
| Read Task | UART 명령 수신 및 처리 | 이벤트 기반 |

#### UART 명령
| 명령 | 동작 |
|------|------|
| `'r'` | 저장된 전체 데이터 읽어서 시리얼 출력 |
| `'c'` | Chip Erase (전체 초기화, 20~100초 소요) |

#### Flash 메모리 사양
| 항목 | 값 |
|------|----|
| 용량 | 8 MB (0x000000 ~ 0x7FFFFF) |
| CS 핀 | Pin 10 |
| SPI 설정 | 1 MHz, MSB-first, Mode 0 |
| 레코드 크기 | 32 바이트 (데이터 18 + 패딩 14) |
| 페이지 크기 | 256 바이트 (페이지당 8개 레코드) |
| 섹터 크기 | 4 KB (삭제 단위) |

#### 데이터 패킷 구조 (32 바이트)
```
바이트  0~ 1 : HMC X (int16)
바이트  2~ 3 : HMC Y (int16)
바이트  4~ 5 : HMC Z (int16)
바이트  6~ 7 : Accel X (int16)
바이트  8~ 9 : Accel Y (int16)
바이트 10~11 : Accel Z (int16)
바이트 12~13 : Gyro X (int16)
바이트 14~15 : Gyro Y (int16)
바이트 16~17 : Gyro Z (int16)
바이트 18~31 : 패딩 (0x00)
```

#### Flash 핵심 함수
| 함수 | 명령 코드 | 설명 |
|------|-----------|------|
| `waitBusy()` | 0x05 | 상태 레지스터 폴링 (BUSY 비트 대기) |
| `writeEnable()` | 0x06 | 쓰기/지우기 전 반드시 호출 |
| `sectorErase()` | 0x20 | 4 KB 섹터 삭제 |
| `pageProgram()` | 0x02 | 최대 256 바이트 쓰기 |
| `readData()` | 0x03 | 임의 길이 읽기 |
| `findLastAddr()` | - | 부팅 시 마지막 기록 주소 복구 |

`spiMutex` 세마포어로 Flash 접근 보호.

| 의존 라이브러리 | `FreeRTOS_ARM.h`, `Wire.h`, `SPI.h` |
|---|---|

---

## 사용 라이브러리

| 라이브러리 | 용도 | 사용 스케치 |
|------------|------|-------------|
| `FreeRTOS_ARM` | 실시간 멀티태스킹 | I2C, SPI |
| `Wire` | I2C 통신 | I2C, SPI |
| `SPI` | SPI 통신 | FileSystem, SPI |
| `Servo` | 서보모터 제어 | Sweep, sketch_jun14a, sketch_mar29a |
| `Adafruit_NeoPixel` | WS2812B RGB LED | Sweep, sketch_jun14a |
| `SdFat` | SD카드 파일 시스템 | FileSystem |

---

## 난이도별 학습 경로

```
[기초]
sketch_mar29a  →  Sweep  →  sketch_jun14a
     ↓
[중급]
FileSystem
     ↓
[고급]
I2C (FreeRTOS + 듀얼 센서)
     ↓
[고급+]
SPI (FreeRTOS + Flash 로깅 + 복구 로직)
```

---

## 핵심 설계 패턴

- **FreeRTOS 멀티태스킹**: 센서 읽기와 데이터 처리를 독립 태스크로 분리
- **뮤텍스 동기화**: I2C/SPI 버스 공유 자원 보호
- **상태 복구**: 전원 재시작 후 `findLastAddr()`로 Flash 마지막 위치 자동 탐색
- **섹터 단위 삭제**: Flash 쓰기 전 4 KB 섹터 자동 삭제 처리
- **빅엔디안 파싱**: MSB/LSB 바이트 쌍을 16비트 정수로 재구성
