/* 
  WirringCutter - DIY Arduino Wire Feeder + Cutter
 *
 * เวอร์ชันนี้: สเต็ปป้อนลวด + สเต็ปตัดลวด (2 motors) เลือกความยาว/จำนวนด้วยปุ่มกด 5 ปุ่ม + LCD I2C
 *
 * Required libraries (ติดตั้งใน Arduino IDE ก่อนคอมไพล์):
 *   - StepperDriver by laurb9         -> BasicStepperDriver.h
 *     (Sketch > Include Library > Manage Libraries... ค้นหา "StepperDriver")
 *   - LiquidCrystal_I2C by Frank de Brabander -> สำหรับ LCD I2C 16x2
 *     (ค้นหา "LiquidCrystal I2C" ใน Library Manager)
 *   - Wire.h                          -> มาพร้อม Arduino IDE แล้ว
 *
 * Board: Arduino Uno (Mega/Nano ได้)
 *
 * ===== Pinout =====
 *  Stepper X (ป้อนลวด): DIR=10, STEP=11
 *  Stepper Y (ตัดลวด):  DIR=8,  STEP=9
 *  LCD I2C 16x2:        SDA=A4, SCL=A5, VCC=5V, GND=GND
 *  ปุ่ม 5 ตัว (INPUT_PULLUP, อีกด้านต่อ GND):
 *    UP    = ขา 3
 *    DOWN  = ขา 4
 *    NEXT  = ขา 5
 *    START = ขา 6
 *    RESET = ขา 7
 */

#include <Arduino.h>
#include "BasicStepperDriver.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===== Motor =====
#define MOTOR_STEPS_X 4000
#define DIR_X         10
#define STEP_X        11
#define MICROSTEPS_X  16
#define MOTOR_X_RPM   50

// ===== Cutter =====
#define MOTOR_STEPS_Y 200
#define DIR_Y         8
#define STEP_Y        9
#define MICROSTEPS_Y  16
#define MOTOR_Y_RPM   50

// ===== Buttons (INPUT_PULLUP) =====
#define BTN_UP        3
#define BTN_DOWN      4
#define BTN_NEXT      5
#define BTN_START     6
#define BTN_RESET     7

// ===== Calibration: ปรับค่านี้ให้ตรงกับเครื่องจริง =====
// จำนวนสเต็ปต่อ 1 ซม. ของลวดที่ป้อนออกมา
// (ค่าจากโค้ดเดิม STEPS_PER_MM = 25 -> STEPS_PER_CM = 25 × 10 = 250)
#define STEPS_PER_CM       250
#define LENGTH_STEP_CM     1      // ปุ่ม UP/DOWN ปรับทีละ 1 ซม.
#define LENGTH_MIN_CM      1
#define LENGTH_MAX_CM      200
#define COUNT_MAX          9999
#define CUTTER_STEPS       6400    // จำนวนสเต็ปต่อการตัด 1 ครั้ง (ปรับให้ตรงเครื่องจริง)

// ===== ลำดับปุ่มในอาร์เรย์ (สำหรับฟังก์ชัน getPressedButton) =====
const int BTN_PINS[5] = { BTN_UP, BTN_DOWN, BTN_NEXT, BTN_START, BTN_RESET };
#define IDX_UP      0
#define IDX_DOWN    1
#define IDX_NEXT    2
#define IDX_START   3
#define IDX_RESET   4

// ===== Menu state =====
enum MenuPage { PAGE_LENGTH, PAGE_COUNT };
MenuPage currentPage = PAGE_LENGTH;

// ===== Operation state =====
enum OperationState { OP_IDLE, OP_FEEDING, OP_CUTTING };
OperationState currentOp = OP_IDLE;

// ===== ค่าตั้ง =====

int  wireLength = 1;     // เริ่มต้น 1 cm 
int  pieceCount = 1;       // เริ่มต้น 1 ชิ้น

// ===== สถานะการทำงาน =====
bool running = false;
int  piecesDone = 0;

// ===== ตัวออบเจกต์ =====
BasicStepperDriver stepperX(MOTOR_STEPS_X, DIR_X, STEP_X);
BasicStepperDriver stepperY(MOTOR_STEPS_Y, DIR_Y, STEP_Y);
LiquidCrystal_I2C lcd(0x27, 16, 2);   // ถ้าจอไม่ขึ้น ลองเปลี่ยน 0x27 -> 0x3F

// ===== สถานะปุ่มครั้งก่อน (สำหรับ edge detection) =====
bool lastBtnState[5] = { HIGH, HIGH, HIGH, HIGH, HIGH };


void setup() {
  Serial.begin(9600);

  // ตั้งขาปุ่มเป็น INPUT_PULLUP (กด = LOW, ปล่อย = HIGH)
  for (int i = 0; i < 5; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
  }

  stepperX.begin(MOTOR_X_RPM, MICROSTEPS_X);
  stepperY.begin(MOTOR_Y_RPM, MICROSTEPS_Y);

  lcd.init();
  lcd.backlight();
  updateLCD();

  delay(500);
}

void loop() {
  int btn = getPressedButton();

  // RESET มี priority สูงสุด — หยุดได้ทุกเมื่อ + คืนค่าทุกอย่างเป็นค่าเริ่มต้น
  if (btn == IDX_RESET) {
    running = false;
    piecesDone = 0;
    currentOp = OP_IDLE;
    wireLength = LENGTH_MIN_CM;   // กลับเป็น 1 cm
    pieceCount = 1;               // กลับเป็น 1 ชิ้น
    currentPage = PAGE_LENGTH;    // กลับหน้าเมนู LENGTH
    updateLCD();
    return;
  }

  if (running) {
    // START กดซ้ำระหว่างทำงาน = หยุด (stop)
    if (btn == IDX_START) {
      running = false;
      piecesDone = 0;
      currentOp = OP_IDLE;
      updateLCD();
      return;
    }

    if (currentOp == OP_IDLE) {
      currentOp = OP_FEEDING;
      updateLCD();
    } else if (currentOp == OP_FEEDING) {
      feedOnePiece();
      currentOp = OP_CUTTING;
      updateLCD();
    } else if (currentOp == OP_CUTTING) {
      cutOnePiece();
      currentOp = OP_IDLE;
      piecesDone++;
      if (piecesDone >= pieceCount) {
        running = false;
        piecesDone = 0;
      }
      updateLCD();
    }
    return;
  }

  // ===== โหมดตั้งค่า =====

  // NEXT: สลับเมนู LENGTH <-> COUNT (กดซ้ำไป-กลับ)
  if (btn == IDX_NEXT) {
    currentPage = (currentPage == PAGE_LENGTH) ? PAGE_COUNT : PAGE_LENGTH;
    updateLCD();
  }
  // UP/DOWN: ปรับค่าในเมนูปัจจุบัน
  else if (currentPage == PAGE_LENGTH) {
    if (btn == IDX_UP) {
      wireLength += LENGTH_STEP_CM;
      if (wireLength > LENGTH_MAX_CM) wireLength = LENGTH_MAX_CM;
      updateLCD();
    } else if (btn == IDX_DOWN) {
      wireLength -= LENGTH_STEP_CM;
      if (wireLength < LENGTH_MIN_CM) wireLength = LENGTH_MIN_CM;
      updateLCD();
    }
  } else { // PAGE_COUNT
    if (btn == IDX_UP) {
      pieceCount += 1;
      if (pieceCount > COUNT_MAX) pieceCount = COUNT_MAX;
      updateLCD();
    } else if (btn == IDX_DOWN) {
      pieceCount -= 1;
      if (pieceCount < 1) pieceCount = 1;
      updateLCD();
    }
  }

  // START: (ขณะไม่ได้ทำงาน) เริ่มงานใหม่ นับชิ้นที่ 1 ใหม่เสมอ
  if (btn == IDX_START) {
    piecesDone = 0;
    currentOp = OP_IDLE;
    running = true;
    updateLCD();
  }
}


// =========================================================
// ฟังก์ชันอ่านปุ่ม — return index ปุ่มที่ "เพิ่งกด" (edge), หรือ -1
// =========================================================
int getPressedButton() {
  int result = -1;
  for (int i = 0; i < 5; i++) {
    bool cur = (digitalRead(BTN_PINS[i]) == LOW);
    if (cur && !lastBtnState[i]) {
      result = i;   // ตรวจพบขอบลง (เพิ่งกด)
    }
    lastBtnState[i] = cur;
  }
  if (result != -1) {
    delay(30);   // debounce แบบง่าย
  }
  return result;
}


// =========================================================
// ฟังก์ชันป้อนลวด 1 ชิ้น: ป้อนตามความยาว
// =========================================================
void feedOnePiece() {
  long steps = (long)wireLength * STEPS_PER_CM;

  stepperX.move(steps);
}


// =========================================================
// ฟังก์ชันตัดลวด 1 ครั้ง: ขับมอเตอร์ Y ไปทิศทางเดียว (ไม่คืนตำแหน่ง)
// =========================================================
void cutOnePiece() {
  stepperY.move(CUTTER_STEPS);
}


// =========================================================
// ฟังก์ชันอัปเดต LCD
// =========================================================
void updateLCD() {
  lcd.clear();

  if (running) {
    // หน้าจอขณะทำงาน
    lcd.setCursor(0, 0);
    if (currentOp == OP_FEEDING) {
      lcd.print("FEEDING ");
    } else if (currentOp == OP_CUTTING) {
      lcd.print("CUTTING ");
    }
    lcd.print(piecesDone);
    lcd.print("/");
    lcd.print(pieceCount);
    lcd.setCursor(0, 1);
    lcd.print("Len ");
    lcd.print(wireLength);
    lcd.print("cm");
    return;
  }

  // หน้าจอตั้งค่า
  lcd.setCursor(0, 0);
  if (currentPage == PAGE_LENGTH) {
    lcd.print("1:LENGTH  2:COUNT");
  } else {
    lcd.print("1:LENGTH  2:COUNT");
  }

  lcd.setCursor(0, 1);
  if (currentPage == PAGE_LENGTH) {
    lcd.print(">");
    lcd.print(wireLength);
    lcd.print("cm   ");
    lcd.setCursor(10, 1);
    lcd.print(" ");
    lcd.print(pieceCount);
    lcd.print("pcs");
  } else {
    lcd.print(" ");
    lcd.print(wireLength);
    lcd.print("cm   ");
    lcd.setCursor(10, 1);
    lcd.print(">");
    lcd.print(pieceCount);
    lcd.print("pcs");
  }
}
