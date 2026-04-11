# 핀 매핑 — NUCLEO-F411RE

## 액추에이터 / 센서 핀 매핑

| 부품 | GPIO | Arduino | Morpho | 방향 | CubeMX 설정 | 비고 |
|------|------|---------|--------|------|------------|------|
| 택트 스위치 | PB5 | D4 | CN9-5 | Input | GPIO_Input Pull-Up / EXTI Falling | 한쪽 → PB5, 반대쪽 → GND |
| 액티브 부저 | PB10 | D6 | CN9-7 | Output | GPIO_Output | S → PB10, VCC → 3.3V |
| 포텐셔미터 | PA0 | A0 | CN8-1 | Analog In | ADC1_IN0 | SIG → PA0, VCC → 3.3V |
| INA219 SDA | PB9 | D14 | CN5-9 | I2C | I2C1_SDA | 4.7kΩ 풀업 → 3.3V |
| INA219 SCL | PB8 | D15 | CN5-10 | I2C | I2C1_SCL | 4.7kΩ 풀업 → 3.3V |
| SHT31 SDA | PB9 | D14 | CN5-9 | I2C | I2C1_SDA (INA219와 버스 공유) | I2C 주소 0x44 (ADDR=GND) |
| SHT31 SCL | PB8 | D15 | CN5-10 | I2C | I2C1_SCL (INA219와 버스 공유) | |
| IRF520 SIG (팬) | PB4 | D5 | CN9-6 | Output | GPIO_Output → TIM3_CH1 PWM | 1kΩ 직렬 → SIG, 팬 전원 5V |

### 저항 정리

| 위치 | 값 | 역할 |
|------|----|------|
| IRF520 SIG 직렬 | 1kΩ | 게이트 과전류 / 노이즈 보호 |
| I2C SDA 풀업 | 4.7kΩ | I2C 버스 풀업 |
| I2C SCL 풀업 | 4.7kΩ | I2C 버스 풀업 |

---

## 전원 핀 매핑

| 전압 | Morpho | 연결 부품 |
|------|--------|----------|
| 3.3V | CN6-4 | 포텐셔미터, INA219, SHT31, 액티브 부저 |
| 5V | CN6-5 | IRF520 모듈 VCC, 쿨링 팬 전원 |
| GND | CN6-6 / CN6-7 | 모든 부품 공통 GND |

---

## 브레드보드 전원 레일 배분

```
빨간 레일 (상단) ── 3.3V  →  포텐셔미터 / INA219 / 부저
빨간 레일 (하단) ── 5V    →  IRF520 / 팬
파란 레일        ── GND   →  전 부품 공통
```

> 3.3V 와 5V 를 같은 레일에 섞지 말 것.  
> MB-102 브레드보드는 상/하 전원 레일이 독립되어 있어 분리에 적합하다.

---

## 사용 중인 핀 (변경 금지)

| GPIO | 기능 | 비고 |
|------|------|------|
| PA2 | USART2_TX | EC 통신 |
| PA3 | USART2_RX | EC 통신 |
| PA5 | LD2 (내장 LED) | |
| PA13 | TMS (SWD) | 디버거 |
| PA14 | TCK (SWD) | 디버거 |
| PB3 | SWO | SWV 디버그 출력 |
| PC13 | B1 (내장 버튼) | |
