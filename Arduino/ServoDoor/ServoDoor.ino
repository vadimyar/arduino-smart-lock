#include <Servo.h>
#include <avr/wdt.h>
#include <EEPROM.h>

// ==========================================
// 1. ОБЪЯВЛЕНИЕ ПИНОВ И КОНСТАНТ
// ==========================================

// --- Пины выхода ---
const byte SERVO_PIN = 10;         // Собственно сервопривод
const byte LED_PIN = 8;            // Индикатор состояния
const byte BUZZER_PIN = 9;         // Активная пикалка
const byte STATUS_LED_PIN = 13;    // тупо мигает когда комият собаку
const byte BREATHING_LED_PIN = 11; // дыхание подсветки главной кнопки

// --- Пины входов ---
const byte TRIG_PINS[] = {2, 3, 4, 5};  // пины входов с датчиков активный низкий уровень
const byte NUM_TRIGS = sizeof(TRIG_PINS) / sizeof(TRIG_PINS[0]); // назначение пинов
const byte SENSOR_LATCH_PIN = 6;        // вход сигнал с датчика Холла (открыто/закрыто)

const byte CONFIG_BUTTON_INDEX = 2;     // для D4 и меню по кнопке настройки 

// --- Механика и скорость ---
const int ANGLE_DOWN = 160;      // начальный угол сервы больше 160 нельзя, упремся!
const int ANGLE_UP = 105;        // конечный угол сервы меньше 40 нельзя, угол раскрытия макс: 160 - 40 = 120 это 55 мм хода тросика!
const int MOVE_DELAY = 7;        // скважность импульсов сервы от этого зависит его скорость

// --- Звуки и время ---        
const unsigned long HOLD_TIME = 10000;   // ⚠️ МЕНЯЙ ТОЛЬКО ЗДЕСЬ - паузы пересчитаются автоматически!
const byte HOLD_TICK_COUNT = 30;         // Количество тиков в мелодии удержания
const unsigned long DEBOUNCE_TIMEOUTS[] = { 3000, 3000, 3000, 3000 }; 
const unsigned long ALARM_DELAY = 15000; // Время до НАЧАЛА тревоги

// ⚠️ НОВОЕ: Максимальная продолжительность ЗВУКА тревоги (мс)
// После этого времени звук выключится, но LED продолжит мигать, пока дверь открыта.
const unsigned long ALARM_SOUND_DURATION = 15000; // 15 секунд звучания

const unsigned long LONG_PRESS_DURATION = 1500; 
const int EEPROM_VOLUME_INDEX = 0; 

// ==========================================
// 🔇 НАСТРОЙКИ "ТИХОГО РЕЖИМА" ЗУММЕРА
// ==========================================
#define USE_SAFE_PULSE_MODE 

const unsigned int VOLUME_LEVELS[] = {100, 10000, 100000}; // значения для меню выбора громкости пищалки
const byte MAX_VOLUME_LEVELS = sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0]); //сюда сохраняем выбранный уровеньгромкости

byte currentVolumeIndex = 0;

// ==========================================
// 🎵 МЕЛОДИИ (Партитуры) 🎵
// ==========================================

const unsigned int MELODY_OPEN[] = {50, 50, 0}; // "." один короткий тик и короткая пауза

const unsigned int MELODY_CLOSE[] = {50, 50, 0}; // {300, 102, 300, 102, 300, 503, 0}; // "_ _ _" три длинных тика

const unsigned int MORSE_AR[] = {100, 102, 300, 102, 100, 102, 300, 102, 100, 503, 0};  // "._._." ти-таа-ти-таа-ти

const unsigned int MELODY_ALARM[] = {100, 102, 100, 102, 100, 303, 300, 102, 300, 102, 300, 303, 100, 102, 100, 102, 100, 603, 0};  // "... _ _ _ ..." ну ты понял

// ==========================================
// 🎼 ДИНАМИЧЕСКАЯ МЕЛОДИЯ УДЕРЖАНИЯ
// ==========================================
// Массивы рассчитываются автоматически при старте на основе HOLD_TIME
unsigned int holdMelodyBuffer[HOLD_TICK_COUNT * 2 + 1];

// Функция расчета ускоряющихся пауз (геометрическая прогрессия)
void calculateHoldMelody() {
  const float q = 0.9;
  const unsigned int MIN_PAUSE_MS = 10; // Минимальная пауза между тиками
  const unsigned int TICK_DURATION_MS = 1; // Минимальная длительность (реальная определяется в setBuzzerOn)
  
  float a1 = (float)HOLD_TIME * (1.0 - q) / (1.0 - pow(q, HOLD_TICK_COUNT));
  
  for (byte i = 0; i < HOLD_TICK_COUNT; i++) {
    holdMelodyBuffer[i * 2] = TICK_DURATION_MS; // 1 мс (минимум, реальная длительность из VOLUME_LEVELS)
    
    unsigned int calculatedPause = (unsigned int)(a1 * pow(q, i));
    holdMelodyBuffer[i * 2 + 1] = (calculatedPause < MIN_PAUSE_MS) ? MIN_PAUSE_MS : calculatedPause;
  }
  holdMelodyBuffer[HOLD_TICK_COUNT * 2] = 0;
  
  Serial.print(F("Мелодия удержания рассчитана: "));
  Serial.print(HOLD_TICK_COUNT);
  Serial.print(F(" тиков за "));
  Serial.print(HOLD_TIME);
  Serial.println(F(" мс"));
}

// --- Дыхание подсветки ---
const unsigned long BREATH_PERIOD_MS = 2000; 
const byte MIN_BRIGHTNESS = 5;  
const byte MAX_BRIGHTNESS = 255; 

// ==========================================
// 2. ДВИЖОК ВОСПРОИЗВЕДЕНИЯ МЕЛОДИЙ
// ==========================================
const unsigned int* playingMelody = nullptr;
int melodyIndex = 0;
unsigned long melodyStepStart = 0;
bool isMelodyPlaying = false;
bool loopCurrentMelody = false;
bool isHoldMelodyStarted = false; 

void setBuzzerOn() {
  #ifdef USE_SAFE_PULSE_MODE
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(VOLUME_LEVELS[currentVolumeIndex]); 
    digitalWrite(BUZZER_PIN, LOW);
  #else
    analogWrite(BUZZER_PIN, 255);
  #endif
}

void setBuzzerOff() {
  analogWrite(BUZZER_PIN, 0);
  digitalWrite(BUZZER_PIN, LOW);
}

void playMelody(const unsigned int* melody, bool loop = false) {
  playingMelody = melody;
  melodyIndex = 0;
  melodyStepStart = millis();
  isMelodyPlaying = true;
  loopCurrentMelody = loop;
  if (playingMelody[0] > 0) setBuzzerOn();
}

void stopMelody() {
  isMelodyPlaying = false;
  playingMelody = nullptr;
  setBuzzerOff();
}

void updateMelody(unsigned long currentTime) {
  if (!isMelodyPlaying || playingMelody == nullptr) return;
  unsigned int duration = playingMelody[melodyIndex];
  
  if (duration == 0) {
    if (loopCurrentMelody) {
      melodyIndex = 0; duration = playingMelody[0]; melodyStepStart = currentTime;
      setBuzzerOn();
    } else {
      isMelodyPlaying = false; playingMelody = nullptr; setBuzzerOff();
    }
    return;
  }

  if (currentTime - melodyStepStart >= duration) {
    melodyStepStart = currentTime;
    melodyIndex++;
    if (playingMelody[melodyIndex] == 0) {
      if (loopCurrentMelody) { 
        melodyIndex = 0; 
        setBuzzerOn(); 
      } else { 
        isMelodyPlaying = false; playingMelody = nullptr; setBuzzerOff(); 
      }
    } else {
      if (melodyIndex % 2 == 0) {
        setBuzzerOn();
      } else {
        setBuzzerOff();
      }
    }
  }
}

// ==========================================
// 3. КОНЕЧНЫЙ АВТОМАТ И ПЕРЕМЕННЫЕ
// ==========================================
enum SystemState {
  STATE_IDLE,           
  STATE_MOVING_UP,      
  STATE_HOLDING,        
  STATE_MOVING_DOWN,
  STATE_CONFIG          
};

SystemState currentState = STATE_IDLE;
unsigned long stateStartTime = 0;
int currentAngle = ANGLE_DOWN;
int targetAngle = ANGLE_DOWN;
unsigned long lastStepTime = 0; 

unsigned long lastLedToggleTime = 0;
bool ledState = false;

unsigned long latchOpenTime = 0;      
bool isAlarming = false;              
unsigned long alarmSoundStartTime = 0; // ⚠️ НОВОЕ: Время начала звука тревоги

bool lastState[NUM_TRIGS];
unsigned long lastTriggerTime[NUM_TRIGS];

unsigned long configButtonPressTime = 0;
bool configButtonIsPressed = false;
bool configLongPressHandled = false; 

Servo doorLock;

// ==========================================
// 4. ФУНКЦИЯ НАСТРОЙКИ
// ==========================================
void setup() {
  Serial.begin(115200);
  wdt_enable(WDTO_2S); 
  
  for (byte i = 0; i < NUM_TRIGS; i++) {
    pinMode(TRIG_PINS[i], INPUT_PULLUP);
    lastState[i] = HIGH;
    lastTriggerTime[i] = 0;
  }
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(BREATHING_LED_PIN, OUTPUT); 
  pinMode(SENSOR_LATCH_PIN, INPUT_PULLUP);

  setBuzzerOff();

  doorLock.attach(SERVO_PIN);
  doorLock.write(ANGLE_DOWN);
  
  unsigned long setupStart = millis();
  while (millis() - setupStart < 500) { wdt_reset(); }
  
  doorLock.detach();
  digitalWrite(LED_PIN, LOW);
  
  // --- ЧТЕНИЕ УРОВНЯ ГРОМКОСТИ ИЗ EEPROM ---
  byte savedVolume = EEPROM.read(EEPROM_VOLUME_INDEX);
  if (savedVolume < MAX_VOLUME_LEVELS) {
    currentVolumeIndex = savedVolume;
    Serial.print(F("Загружен уровень громкости из EEPROM: ")); 
    Serial.println(currentVolumeIndex + 1);
  } else {
    currentVolumeIndex = 0;
    EEPROM.update(EEPROM_VOLUME_INDEX, currentVolumeIndex);
    Serial.println(F("EEPROM пуст. Установлен уровень громкости 1 по умолчанию."));
  }
  
  // --- РАСЧЕТ ДИНАМИЧЕСКОЙ МЕЛОДИИ УДЕРЖАНИЯ ---
  calculateHoldMelody();
  
  Serial.println(F("Система готова. Замок заблокирован."));
}

// ==========================================
// 5. ОСНОВНОЙ ЦИКЛ
// ==========================================
void loop() {
  unsigned long currentTime = millis();
  wdt_reset(); 

  updateMelody(currentTime);

  // --- ОБРАБОТКА СЕРВИСНОЙ КНОПКИ (D4) ---
  bool configBtnState = digitalRead(TRIG_PINS[CONFIG_BUTTON_INDEX]);
  
  if (configBtnState == LOW && lastState[CONFIG_BUTTON_INDEX] == HIGH) {
    configButtonPressTime = currentTime;
    configButtonIsPressed = true;
    configLongPressHandled = false; 
  }
  
  if (configBtnState == HIGH && lastState[CONFIG_BUTTON_INDEX] == LOW) {
    if (configButtonIsPressed) {
      unsigned long pressDuration = currentTime - configButtonPressTime;
      
      if (!configLongPressHandled && pressDuration < LONG_PRESS_DURATION) {
        if (currentState == STATE_CONFIG) {
          currentVolumeIndex++;
          if (currentVolumeIndex >= MAX_VOLUME_LEVELS) currentVolumeIndex = 0;
          
          setBuzzerOn();
          digitalWrite(LED_PIN, HIGH);
          delay(100);
          setBuzzerOff();
          digitalWrite(LED_PIN, LOW);
          
          Serial.print(F("Уровень громкости: ")); Serial.println(currentVolumeIndex + 1);
        } else if (currentState == STATE_IDLE) {
          Serial.println(F("Сервисная кнопка (короткое нажатие в обычном режиме)."));
        }
      }
      configButtonIsPressed = false;
    }
  }
  
  if (configButtonIsPressed && !configLongPressHandled) {
    if ((currentTime - configButtonPressTime) >= LONG_PRESS_DURATION) {
      configLongPressHandled = true; 
      
      if (currentState == STATE_CONFIG) {
        currentState = STATE_IDLE;
        EEPROM.update(EEPROM_VOLUME_INDEX, currentVolumeIndex);
        playMelody(MORSE_AR, false); 
        Serial.print(F("Настройка завершена. Сохранен уровень громкости: "));
        Serial.println(currentVolumeIndex + 1);
      } else if (currentState == STATE_IDLE) {
        currentState = STATE_CONFIG;
        Serial.println(F("Вход в режим настройки громкости зуммера."));
        Serial.print(F("Текущий уровень: ")); Serial.println(currentVolumeIndex + 1);
      }
    }
  }
  lastState[CONFIG_BUTTON_INDEX] = configBtnState;

  // --- ОБРАБОТКА ОСТАЛЬНЫХ КНОПОК (D2, D3, D5) ---
  for (byte i = 0; i < NUM_TRIGS; i++) {
    if (i == CONFIG_BUTTON_INDEX) continue;
    
    bool currentStatePin = digitalRead(TRIG_PINS[i]);
    if (lastState[i] == HIGH && currentStatePin == LOW) {
      if (currentTime - lastTriggerTime[i] >= DEBOUNCE_TIMEOUTS[i]) {
        lastTriggerTime[i] = currentTime;
        lastState[i] = currentStatePin;
        if (currentState == STATE_IDLE) {
          Serial.print(F("Сработал источник на пине ")); Serial.println(TRIG_PINS[i]);
          startUnlockCycle(currentTime);
        }
      }
    }
    lastState[i] = currentStatePin;
  }

  // ==========================================
  // ⚠️ НОВОЕ: ЛОГИКА КОНТРОЛЯ ПОЛОЖЕНИЯ СОБАЧКИ С ТАЙМАУТОМ ЗВУКА
  // ==========================================
  bool latchIsUp = (digitalRead(SENSOR_LATCH_PIN) == LOW);
  
  if (latchIsUp) {
    if (latchOpenTime == 0) latchOpenTime = currentTime;
    
    // 1. Запуск тревоги (звук + начало отсчета времени звука)
    if ((currentTime - latchOpenTime >= ALARM_DELAY) && !isAlarming) {
      isAlarming = true;
      alarmSoundStartTime = currentTime;
      playMelody(MELODY_ALARM, true);
      Serial.println(F("ВНИМАНИЕ: Замок открыт слишком долго! Включена тревога."));
    }
    
    // 2. Принудительная остановка ЗВУКА по истечении ALARM_SOUND_DURATION
    // Флаг isAlarming при этом остается true, чтобы продолжало мигать LED
    if (isAlarming && (currentTime - alarmSoundStartTime >= ALARM_SOUND_DURATION)) {
      if (isMelodyPlaying) {
        stopMelody();
        Serial.println(F("Звук тревоги выключен по таймауту. Дверь всё ещё открыта!"));
      }
    }
  } else {
    // Датчик Холла в покое (дверь закрыта)
    latchOpenTime = 0;
    alarmSoundStartTime = 0;
    if (currentState == STATE_IDLE) {
      if (isAlarming) {
        stopMelody(); // На всякий случай гарантируем остановку
        isAlarming = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println(F("Дверь закрыта. Тревога снята."));
      }
    }
  }

  // 3. Управление миганием LED во время тревоги (работает независимо от звука!)
  if (isAlarming) {
    if (currentTime - lastLedToggleTime >= 300) { // Мигание каждые 300 мс
      lastLedToggleTime = currentTime;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
  } else if (currentState == STATE_IDLE) {
    // Если не тревога и мы в простое, гасим LED (на случай, если он остался висеть)
    digitalWrite(LED_PIN, LOW);
  }
  // ==========================================

  // --- ИНДИКАЦИЯ LED (Дыхание или Быстрое мигание в конфиге) ---
  if (currentState == STATE_CONFIG) {
    if (currentTime - lastLedToggleTime >= 100) {
      lastLedToggleTime = currentTime;
      ledState = !ledState;
      analogWrite(BREATHING_LED_PIN, ledState ? MAX_BRIGHTNESS : MIN_BRIGHTNESS);
    }
  } else if (!isAlarming) { // Дыхание работает только если нет тревоги
    unsigned long timeInPeriod = currentTime % BREATH_PERIOD_MS;
    float phase = (timeInPeriod * 2.0 * PI) / BREATH_PERIOD_MS;
    int offset = (MAX_BRIGHTNESS + MIN_BRIGHTNESS) / 2;
    int amplitude = (MAX_BRIGHTNESS - MIN_BRIGHTNESS) / 2;
    int currentBrightness = offset + (amplitude * sin(phase));
    analogWrite(BREATHING_LED_PIN, currentBrightness);
  }

  // --- МИГАНИЕ СТАТУСНОГО LED (D13) ---
  static unsigned long d13Timer = 0;
  static bool d13State = false;
  if (currentTime - d13Timer >= 150) {
    d13Timer = currentTime;
    d13State = !d13State;
    digitalWrite(STATUS_LED_PIN, d13State);
  }

  // --- ОБРАБОТКА СОСТОЯНИЙ АВТОМАТА ---
  switch (currentState) {
    case STATE_IDLE:
      break;
    case STATE_MOVING_UP:
      handleMovingUpState(currentTime);
      break;
    case STATE_HOLDING:
      handleHoldingState(currentTime);
      break;
    case STATE_MOVING_DOWN:
      handleMovingDownState(currentTime);
      break;
    case STATE_CONFIG:
      break;
  }
}

// ==========================================
// 6. ФУНКЦИИ ОБРАБОТКИ СОСТОЯНИЙ
// ==========================================

void handleMovingUpState(unsigned long currentTime) {
  if (currentTime - lastStepTime >= MOVE_DELAY) {
    lastStepTime = currentTime;
    if (currentAngle != targetAngle) {
      if (currentAngle < targetAngle) currentAngle++;
      else currentAngle--;
      doorLock.write(currentAngle);
    } else {
      currentState = STATE_HOLDING;
      stateStartTime = currentTime;
      digitalWrite(LED_PIN, HIGH); 
      playMelody(MELODY_OPEN, false);
      isHoldMelodyStarted = false; 
    }
  }
}

void handleHoldingState(unsigned long currentTime) {
  if (!isMelodyPlaying && !isHoldMelodyStarted) {
    playMelody(holdMelodyBuffer, true); // ⚠️ Используем динамический массив!
    isHoldMelodyStarted = true;
  }

  if ((currentTime - stateStartTime) >= HOLD_TIME) {
    currentState = STATE_MOVING_DOWN;
    targetAngle = ANGLE_DOWN;
    lastStepTime = currentTime;
    stopMelody(); 
    isHoldMelodyStarted = false; 
  }
}

void handleMovingDownState(unsigned long currentTime) {
  if (currentTime - lastStepTime >= MOVE_DELAY) {
    lastStepTime = currentTime;
    if (currentAngle != targetAngle) {
      if (currentAngle < targetAngle) currentAngle++;
      else currentAngle--;
      doorLock.write(currentAngle);
    } else {
      finishUnlockCycle();
    }
  }
}

// ==========================================
// 7. ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ==========================================

void startUnlockCycle(unsigned long currentTime) {
  currentState = STATE_MOVING_UP;
  stateStartTime = currentTime;
  targetAngle = ANGLE_UP;
  lastStepTime = currentTime;
  digitalWrite(LED_PIN, HIGH); 
  doorLock.attach(SERVO_PIN);
}

void finishUnlockCycle() {
  doorLock.detach();
  currentState = STATE_IDLE;
  digitalWrite(LED_PIN, LOW); 
  playMelody(MELODY_CLOSE, false);
  Serial.println(F("Замок заблокирован."));
}
