// ============================================================================
//  SignalTracker — Firmware Lengkap
//  Versi  : 2.0
//  Fitur  : DWIN HMI · Password Keypad · Kalibrasi Multi-Titik
//           PZEM AC Monitoring · Proteksi HC/OV/VD · HM-30 Sequence
// ============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PZEM004Tv30.h>

// --- Pin Output ---
#define PIN_MOSFET_1    12   // MOSFET 1 (Signal Tracker)      | HIGH=ON  LOW=OFF
#define PIN_MOSFET_2    22   // MOSFET 2 (HM-30 Power Enable)  | HIGH=ON  LOW=OFF
#define PIN_OPTO        27   // Optocoupler HM-30 Sequence     | HIGH=ON  LOW=OFF  (Active-High)
#define PIN_RELAY       32   // Relay Proteksi Utama           | LOW=ON   HIGH=OFF (Active-Low)
#define RS485_DIR_PIN    4   // Arah RS-485 DWIN
#define SSR1            13   // Solid State Relay 1            | HIGH=ON  LOW=OFF
#define SSR2            15   // Solid State Relay 2            | HIGH=ON  LOW=OFF



// --- Pin Sensor Analog ---
#define PIN_V1           34  // Sensor Tegangan Channel 1
#define PIN_V2           35  // Sensor Tegangan Channel 2
#define PIN_SENSE_CUR_1  36  // Sensor Arus ACS758 Channel 1
#define PIN_SENSE_CUR_2  39  // Sensor Arus ACS758 Channel 2

// --- PZEM (AC Monitoring) ---
#define PZEM_RX_PIN      25  // ESP32 RX <- TX PZEM
#define PZEM_TX_PIN      26  // ESP32 TX -> RX PZEM

//Pin SSR
#define SSR1              13
#define SSR2            15

// --- DWIN HMI ---
#define DWIN_RX_PIN      16
#define DWIN_TX_PIN      17
#define DWIN_BAUD        115200

// --- Threshold Proteksi ---
#define THRESHOLD_OVERCURRENT   8.0f   // Ampere
#define THRESHOLD_OVERVOLTAGE  15.0f   // Volt
#define THRESHOLD_UNDERVOLTAGE  8.0f   // Volt (Voltage Drop)

// --- Timing ---
#define HEARTBEAT_INTERVAL     200UL   // ms — interval ping ke DWIN
#define DATA_UPDATE_INTERVAL  1000UL   // ms — interval update sensor ke DWIN
#define IDLE_DIM_MS          15000UL   // ms — layar mulai redup
#define IDLE_SLEEP_MS        30000UL   // ms — layar mati total
#define POPUP_DURATION        1500UL   // ms — durasi popup password

// --- HM-30 Sequence Timing ---
#define HM30_POWER_ON_WAIT    1000UL   // ms — tunggu setelah power on
#define HM30_OPTO_PULSE1       300UL   // ms — durasi pulsa pertama (ON)
#define HM30_OPTO_PAUSE        100UL   // ms — jeda antar pulsa (OFF)
#define HM30_OPTO_PULSE2      2000UL   // ms — durasi pulsa kedua (ON)

// --- ADC Sampling ---
#define ADC_SAMPLES             16     // jumlah sampel rata-rata per pembacaan

// --- Password Default ---
#define DEFAULT_PASSWORD      "123456"
#define PASSWORD_LENGTH            6

// --- Macro logika pin (agar tidak bingung Active-Low vs Active-High) ---
#define RELAY_ON()   digitalWrite(PIN_RELAY, LOW)
#define RELAY_OFF()  digitalWrite(PIN_RELAY, HIGH)
#define OPTO_ON()    digitalWrite(PIN_OPTO, HIGH)
#define OPTO_OFF()   digitalWrite(PIN_OPTO, LOW)

// ============================================================================
//  TABEL KALIBRASI MULTI-TITIK
//  Format: {rawADC, nilaiNyata}
//  Tambahkan atau ubah titik sesuai hasil pengukuran Anda
// ============================================================================
struct CalPoint {
  int rawADC;
  float realValue;
};

CalPoint calV1[] = {                          // Tegangan Channel 1
  {0, 0.00}, {490, 5.00}, {752, 7.00},
  {1001, 9.00}, {1141, 10.00}, {1403, 12.00}
};

CalPoint calV2[] = {                          // Tegangan Channel 2
  {0, 0.00}, {492, 5.00}, {753, 7.00},
  {1002, 9.00}, {1136, 10.00}, {1400, 12.00}
};

CalPoint calI1[] = {                          // Arus Channel 1
  {2838, 0.00}, {2883, 0.49}, {2901, 0.68}, {2918, 0.87}, {2927, 0.99},
  {2945, 1.25}, {2968, 1.48}, {3012, 2.01}, {3111, 3.00}, {3203, 4.01},
  {3317, 5.01}, {3377, 5.49}, {3432, 6.02}
};

CalPoint calI2[] = {                          // Arus Channel 2
  {2836, 0.00}, {2875, 0.48}, {2888, 0.69}, {2898, 0.90}, {2906, 1.00},
  {2921, 1.24}, {2941, 1.50}, {2974, 2.00}, {3052, 3.01}, {3130, 4.00},
  {3220, 5.01}, {3256, 5.50}, {3296, 6.00}
};

// Batas bawah arus (noise/bocoran di bawah ini dianggap 0)
#define CURRENT_DEADBAND        0.15f

// ============================================================================
//  REGISTER & HALAMAN DWIN HMI
//  Sesuaikan VP address jika Anda mengubah desain DGUS
// ============================================================================

// Sistem
#define VP_BRIGHTNESS     0x0082
#define VP_CLOCK_TEXT     0x3010

// Halaman
#define HAL_LANDING           0
#define HAL_DASHBOARD         1
#define HAL_KEYPAD            2
#define HAL_SETTING           3
#define HAL_POPUP_GAGAL       4
#define HAL_POPUP_SUKSES      5
#define HAL_ALARM_VD          6
#define HAL_ALARM_HC          7
#define HAL_ALARM_OV          8
#define HAL_STANDBY_STATIS   19
#define HAL_CONFIRM_NEW_PASS 20
#define HAL_ENTER_OLD_PASS   21
#define HAL_ENTER_NEW_PASS   22
#define HAL_POPUP_SUCCESS_NEW 23

// VP Data DC
#define VP_TEGANGAN_1    0x5000
#define VP_TEGANGAN_2    0x5100
#define VP_ARUS_1        0x5200
#define VP_ARUS_2        0x5300
#define VP_DAYA          0x5400
#define VP_SUHU          0x5500
#define VP_ENERGI        0x5600

// VP Data AC (PZEM)
#define VP_TEGANGAN_AC   0x7250
#define VP_DAYA_AC       0x7260
#define VP_ARUS_AC       0x7270
#define VP_ENERGI_AC     0x7280

// VP Toggle & Keypad
#define VP_LANDING_TOUCH   0x1900
#define VP_TOGGLE_1_ICON   0x6000
#define VP_TOGGLE_2_ICON   0x6100
#define VP_TOGGLE_1_TOUCH  0x6910
#define VP_TOGGLE_2_TOUCH  0x6920
#define VP_SETTING_TEXT    0x7100

const uint16_t vpDot[PASSWORD_LENGTH] = {0x1005, 0x1010, 0x1020, 0x1030, 0x1040, 0x1050};
const uint16_t VP_KEYS[10] = {0x1110, 0x1120, 0x1130, 0x1140, 0x1150, 0x1160, 0x1170, 0x1180, 0x1190, 0x1210};
#define VP_KEY_BACKSPACE   0x1230
#define VP_KEY_CLEAR       0x1220

// ============================================================================
//  INISIALISASI OBJEK
// ============================================================================
Preferences    preferences;
HardwareSerial dwin(2);
PZEM004Tv30    pzem(&Serial1, PZEM_RX_PIN, PZEM_TX_PIN);

const byte HEADER1 = 0x5A;
const byte HEADER2 = 0xA5;

// ============================================================================
//  VARIABEL STATE
// ============================================================================

// --- HMI ---
bool     isLayarOnline     = false;
bool     isSystemActive    = false;
bool     isLayarDisentuh   = false;
uint16_t halamanAktif      = 999;
uint16_t halamanTujuanGagal = HAL_KEYPAD;
uint16_t halamanTerkunci   = 999;   // 999 = tidak terkunci
byte     powerState        = 0;     // 0=aktif 1=redup 2=mati

// --- Password ---
String KUNCI_SANDI     = DEFAULT_PASSWORD;
String inputPassword   = "";
String tempNewPassword = "";
byte   targetToggle    = 0;

// --- Status Output ---
bool  statusToggle1 = true;    // Normally ON
bool  statusSSR1    = false;   // SSR1 default OFF
bool  statusSSR2    = false;   // SSR2 default OFF
bool  statusToggle2 = true;    // Normally ON

// --- API / LattePanda ---
unsigned long lastApiStatusTime = 0;
const unsigned long API_STATUS_INTERVAL = 1500UL;

// Cache sensor agar STATUS/API tidak perlu membaca ADC lagi
float sensorVoltage1 = 0.0f;
float sensorVoltage2 = 0.0f;
float sensorCurrent1 = 0.0f;
float sensorCurrent2 = 0.0f;
float sensorPower    = 0.0f;

float acVoltage = 0.0f;
float acCurrent = 0.0f;
float acPower   = 0.0f;
float acEnergy  = 0.0f;
bool  acDataValid = false;

bool stateRelay = false;

// --- Energi ---
float totalEnergiKwh = 0.0;

// --- Timing ---
unsigned long lastHeartbeatTime = 0;
unsigned long lastTouchActivity = 0;
unsigned long lastDwinUpdate    = 0;
unsigned long lastTickJam       = 0;

// --- HM-30 State Machine ---
bool          hm30_active = false;
int           hm30_state  = 0;
unsigned long hm30_timer  = 0;

// --- RTC Dummy ---
int ntpTahun = 2026, ntpBulan = 8, ntpTanggal = 22;
int ntpJam = 10, ntpMenit = 0, ntpDetik = 0;

// ============================================================================
//  FUNGSI UTILITAS RS-485 & DWIN
// ============================================================================
void rs485Tx() {
  digitalWrite(RS485_DIR_PIN, HIGH);
}
void rs485Rx() {
  dwin.flush();
  delay(2);
  digitalWrite(RS485_DIR_PIN, LOW);
}

void tulisIntDWIN(uint16_t vp, uint16_t nilai) {
  byte f[] = {HEADER1, HEADER2, 0x05, 0x82, highByte(vp), lowByte(vp), highByte(nilai), lowByte(nilai)};
  rs485Tx(); dwin.write(f, sizeof(f)); dwin.flush(); rs485Rx();
}

void tulisFloatDWIN(uint16_t vp, float val) {
  union {
    float f;
    byte b[4];
  } u; u.f = val;
  byte f[] = {HEADER1, HEADER2, 0x07, 0x82, highByte(vp), lowByte(vp), u.b[3], u.b[2], u.b[1], u.b[0]};
  rs485Tx(); dwin.write(f, sizeof(f)); dwin.flush(); rs485Rx();
}

void tulisTextDWIN(uint16_t vp, const String &text) {
  int tLen = text.length();
  int nBytes = (tLen % 2 != 0) ? tLen + 1 : tLen;
  if (nBytes > 30) nBytes = 30;
  byte f[40];
  f[0] = HEADER1; f[1] = HEADER2; f[2] = 3 + nBytes; f[3] = 0x82; f[4] = highByte(vp); f[5] = lowByte(vp);
  for (int i = 0; i < nBytes; i++) f[6 + i] = (i < tLen) ? text.charAt(i) : 0x20;
  rs485Tx(); dwin.write(f, 6 + nBytes); dwin.flush(); rs485Rx();
}

void gantiHalaman(uint16_t id) {
  byte f[] = {HEADER1, HEADER2, 0x07, 0x82, 0x00, 0x84, 0x5A, 0x01, highByte(id), lowByte(id)};
  rs485Tx(); dwin.write(f, sizeof(f)); dwin.flush(); rs485Rx();
  halamanAktif = id;
}

void tulisSemuaCurveDWIN(uint16_t vTeg, uint16_t vArus, uint16_t vDaya, uint16_t vEnergi, uint16_t vSuhu) {
  byte f[30] = {
    HEADER1, HEADER2, 0x1B, 0x82,
    0x03, 0x10, 0x5A, 0xA5, 0x05, 0x00,
    0x00, 0x01, highByte(vTeg),    lowByte(vTeg),
    0x01, 0x01, highByte(vArus),   lowByte(vArus),
    0x02, 0x01, highByte(vDaya),   lowByte(vDaya),
    0x03, 0x01, highByte(vEnergi), lowByte(vEnergi),
    0x04, 0x01, highByte(vSuhu),   lowByte(vSuhu)
  };
  rs485Tx(); dwin.write(f, 30); dwin.flush(); rs485Rx();
}

void wakeUpScreen() {
  if (powerState != 0) {
    tulisIntDWIN(VP_BRIGHTNESS, 0x6464);
    powerState = 0;
  }
  lastTouchActivity = millis();
}

// ============================================================================
//  FUNGSI SENSOR (Kalibrasi Multi-Titik)
// ============================================================================
float kalibrasiMultiTitik(int raw, CalPoint pts[], int n) {
  if (raw <= pts[0].rawADC)   return pts[0].realValue;
  if (raw >= pts[n - 1].rawADC) return pts[n - 1].realValue;
  for (int i = 0; i < n - 1; i++) {
    if (raw >= pts[i].rawADC && raw <= pts[i + 1].rawADC) {
      float x1 = pts[i].rawADC, y1 = pts[i].realValue, x2 = pts[i + 1].rawADC, y2 = pts[i + 1].realValue;
      return y1 + (raw - x1) * (y2 - y1) / (x2 - x1);
    }
  }
  return 0.0;
}

void readAllSensors(float &c1, float &c2, float &v1, float &v2) {
  long sC1 = 0, sC2 = 0, sV1 = 0, sV2 = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sC1 += analogRead(PIN_SENSE_CUR_1);
    sC2 += analogRead(PIN_SENSE_CUR_2);
    sV1 += analogRead(PIN_V1);
    sV2 += analogRead(PIN_V2);
  }
#define CAL(val, tbl) kalibrasiMultiTitik((val)/ADC_SAMPLES, tbl, sizeof(tbl)/sizeof(tbl[0]))
  v1 = CAL(sV1, calV1); v2 = CAL(sV2, calV2);
  c1 = CAL(sC1, calI1); c2 = CAL(sC2, calI2);
#undef CAL
  if (c1 < CURRENT_DEADBAND) c1 = 0.0f;
  if (c2 < CURRENT_DEADBAND) c2 = 0.0f;
}

// ============================================================================
//  PARSER DWIN HMI (Non-Blocking Ring Buffer)
// ============================================================================
void prosesToggle(byte toggle) {
  if (toggle == 1) {
    statusToggle1 = !statusToggle1;
    tulisIntDWIN(VP_TOGGLE_1_ICON, statusToggle1 ? 1 : 0);
    digitalWrite(PIN_MOSFET_1, statusToggle1 ? HIGH : LOW);
    Serial.printf("[TOGGLE] MOSFET 1 -> %s\n", statusToggle1 ? "ON" : "OFF");
  } else if (toggle == 2) {
    statusToggle2 = !statusToggle2;
    tulisIntDWIN(VP_TOGGLE_2_ICON, statusToggle2 ? 1 : 0);
    if (statusToggle2) {
      hm30_active = true;
      hm30_state = 0;
    }
    else               {
      hm30_active = false;
      digitalWrite(PIN_MOSFET_2, LOW);
      OPTO_OFF();
    }
    Serial.printf("[TOGGLE] MOSFET 2 / HM-30 -> %s\n", statusToggle2 ? "ON" : "OFF");
  }
}

void bersihkanInputPassword() {
  inputPassword = "";
  for (int i = 0; i < PASSWORD_LENGTH; i++) tulisIntDWIN(vpDot[i], 0);
}

void bacaSerialDWIN() {
  static byte rxBuf[64];
  static int  rxIdx = 0;

  while (dwin.available() > 0) {
    byte b = dwin.read();
    rxBuf[rxIdx++] = b;

    // Jaga agar header selalu di posisi 0
    if (rxIdx >= 3 && (rxBuf[0] != HEADER1 || rxBuf[1] != HEADER2)) {
      for (int k = 0; k < rxIdx - 1; k++) rxBuf[k] = rxBuf[k + 1];
      rxIdx--; continue;
    }

    if (rxIdx < 3) continue;
    int pktLen = rxBuf[2] + 3;
    if (rxIdx < pktLen) continue;

    byte     cmd  = rxBuf[3];
    uint16_t vp   = (rxBuf[4] << 8) | rxBuf[5];
    byte     wLen = rxBuf[6];

    if (cmd == 0x83 || cmd == 0x82) {
      // --- Heartbeat / Status Halaman ---
      if (vp == 0x0014 && wLen >= 5) {
        isLayarOnline = true;
        uint16_t newPage = (rxBuf[7] << 8) | rxBuf[8];
        bool disentuh    = ((rxBuf[11] << 8) | rxBuf[12]) & 0xFF00;
        if (disentuh) {
          wakeUpScreen();
          tulisIntDWIN(0x0016, 0x0000);
        }
        if (newPage != halamanAktif) {
          bersihkanInputPassword();
          halamanAktif = newPage;
          wakeUpScreen();
          if (halamanAktif == HAL_DASHBOARD) {
            isSystemActive = true;
            tulisIntDWIN(VP_TOGGLE_1_ICON, statusToggle1 ? 1 : 0);
            tulisIntDWIN(VP_TOGGLE_2_ICON, statusToggle2 ? 1 : 0);
          }
        }
      }
      // --- Touch / Komponen ---
      else if (!(cmd == 0x82 && vp == 0x4F4B)) {
        if (vp >= 0x1000) wakeUpScreen();

        if      (vp == VP_LANDING_TOUCH)  gantiHalaman(HAL_DASHBOARD);
        else if (vp == VP_TOGGLE_1_TOUCH) targetToggle = 1;
        else if (vp == VP_TOGGLE_2_TOUCH) targetToggle = 2;

        // Proses keypad (hanya saat di halaman keypad)
        bool diHalamanKeypad = (halamanAktif == HAL_KEYPAD      ||
                                halamanAktif == HAL_ENTER_OLD_PASS  ||
                                halamanAktif == HAL_ENTER_NEW_PASS  ||
                                halamanAktif == HAL_CONFIRM_NEW_PASS);
        if (diHalamanKeypad) {
          // Cek tombol angka
          int num = -1;
          for (int i = 0; i <= 9; i++) if (vp == VP_KEYS[i]) {
              num = i;
              break;
            }

          if (num != -1 && (int)inputPassword.length() < PASSWORD_LENGTH) {
            inputPassword += String(num);
            tulisIntDWIN(vpDot[inputPassword.length() - 1], 1);

            if ((int)inputPassword.length() == PASSWORD_LENGTH) {
              if (halamanAktif == HAL_ENTER_OLD_PASS) {
                if (inputPassword == KUNCI_SANDI) gantiHalaman(HAL_ENTER_NEW_PASS);
                else {
                  halamanTujuanGagal = halamanAktif;
                  gantiHalaman(HAL_POPUP_GAGAL);
                }
              }
              else if (halamanAktif == HAL_ENTER_NEW_PASS) {
                tempNewPassword = inputPassword; gantiHalaman(HAL_CONFIRM_NEW_PASS);
              }
              else if (halamanAktif == HAL_CONFIRM_NEW_PASS) {
                if (inputPassword == tempNewPassword) {
                  KUNCI_SANDI = inputPassword;
                  preferences.putString("password", KUNCI_SANDI);
                  gantiHalaman(HAL_POPUP_SUCCESS_NEW);
                } else {
                  halamanTujuanGagal = halamanAktif;
                  gantiHalaman(HAL_POPUP_GAGAL);
                }
              }
              else { // Kontrol Toggle MOSFET
                if (inputPassword == KUNCI_SANDI) {
                  prosesToggle(targetToggle);
                  targetToggle = 0;
                  gantiHalaman(HAL_POPUP_SUKSES);
                }
                else {
                  halamanTujuanGagal = halamanAktif;
                  gantiHalaman(HAL_POPUP_GAGAL);
                }
              }
              bersihkanInputPassword();
            }
          }
          else if (vp == VP_KEY_BACKSPACE && inputPassword.length() > 0) {
            tulisIntDWIN(vpDot[inputPassword.length() - 1], 0);
            inputPassword.remove(inputPassword.length() - 1);
          }
          else if (vp == VP_KEY_CLEAR) {
            bersihkanInputPassword();
          }
        }
      }
    }
    rxIdx = 0;
  }
  if (rxIdx >= 64) rxIdx = 0;
}

// ============================================================================
//  HEARTBEAT & TIMER
// ============================================================================
void kirimHeartbeatPing() {
  if (millis() - lastHeartbeatTime < HEARTBEAT_INTERVAL) return;
  lastHeartbeatTime = millis();
  byte f[] = {HEADER1, HEADER2, 0x04, 0x83, 0x00, 0x14, 0x05};
  rs485Tx(); dwin.write(f, sizeof(f)); dwin.flush(); rs485Rx();
}

void prosesTriggerAnimasiLanding() {
  static unsigned long t = 0;
  static bool done = false;
  if (halamanAktif == HAL_LANDING) {
    if (!t) {
      t = millis();
      done = false;
    }
    if (!done && millis() - t > 4800) {
      gantiHalaman(HAL_STANDBY_STATIS);
      done = true;
    }
  } else {
    t = 0;
    done = false;
  }
}

void prosesPowerManagement() {
  if (!isLayarOnline || !isSystemActive) return;
  if (halamanTerkunci != 999) {
    wakeUpScreen();
    return;
  }
  unsigned long idle = millis() - lastTouchActivity;
  if      (idle > IDLE_DIM_MS   && idle <= IDLE_SLEEP_MS && powerState == 0) {
    gantiHalaman(HAL_STANDBY_STATIS); delay(10);
    while (dwin.available()) dwin.read();
    tulisIntDWIN(VP_BRIGHTNESS, 0x1414); powerState = 1;
  }
  else if (idle > IDLE_SLEEP_MS && powerState == 1) {
    tulisIntDWIN(VP_BRIGHTNESS, 0x0000); powerState = 2;
  }
}

void prosesPopupPassword() {
  static unsigned long t = 0;
  bool isPopup = (halamanAktif == HAL_POPUP_GAGAL || halamanAktif == HAL_POPUP_SUKSES || halamanAktif == HAL_POPUP_SUCCESS_NEW);
  if (isPopup) {
    if (!t) t = millis();
    if (millis() - t > POPUP_DURATION) {
      gantiHalaman((halamanAktif == HAL_POPUP_SUKSES || halamanAktif == HAL_POPUP_SUCCESS_NEW) ? HAL_DASHBOARD : halamanTujuanGagal);
      t = 0;
    }
  } else {
    t = 0;
  }
}

// ============================================================================
//  HM-30 NON-BLOCKING SEQUENCE STATE MACHINE
// ============================================================================
void handleHM30Sequence() {
  if (!hm30_active) {
    if (hm30_state != 0) {
      digitalWrite(PIN_MOSFET_2, LOW); OPTO_OFF();
      hm30_state = 0;
    }
    return;
  }
  unsigned long now = millis();
  switch (hm30_state) {
    case 0: // Power ON
      Serial.println("[HM-30] Power ON → tunggu " + String(HM30_POWER_ON_WAIT) + "ms...");
      digitalWrite(PIN_MOSFET_2, HIGH);
      hm30_timer = now; hm30_state = 1; break;
    case 1: // Pulsa 1 ON
      if (now - hm30_timer >= HM30_POWER_ON_WAIT) {
        Serial.println("[HM-30] OPTO ON (" + String(HM30_OPTO_PULSE1) + "ms)");
        OPTO_ON(); hm30_timer = now; hm30_state = 2;
      } break;
    case 2: // Pulsa 1 OFF
      if (now - hm30_timer >= HM30_OPTO_PULSE1) {
        Serial.println("[HM-30] OPTO OFF (" + String(HM30_OPTO_PAUSE) + "ms)");
        OPTO_OFF(); hm30_timer = now; hm30_state = 3;
      } break;
    case 3: // Pulsa 2 ON
      if (now - hm30_timer >= HM30_OPTO_PAUSE) {
        Serial.println("[HM-30] OPTO ON (" + String(HM30_OPTO_PULSE2) + "ms)");
        OPTO_ON(); hm30_timer = now; hm30_state = 4;
      } break;
    case 4: // Pulsa 2 OFF → Selesai
      if (now - hm30_timer >= HM30_OPTO_PULSE2) {
        Serial.println("[HM-30] Sekuens Selesai!");
        OPTO_OFF(); hm30_state = 5;
      } break;
    case 5: break; // HM-30 aktif normal
  }
}

// ============================================================================
//  JAM RTC DUMMY
// ============================================================================
void updateJamTextDWIN() {
  if (!isLayarOnline || millis() - lastTickJam < 1000) return;
  lastTickJam = millis();
  if (++ntpDetik >= 60) {
    ntpDetik = 0;
    if (++ntpMenit >= 60) {
      ntpMenit = 0;
      if (++ntpJam >= 24) {
        ntpJam = 0;
        ntpTanggal++;
      }
    }
  }
  char buf[25];
  sprintf(buf, "%02d-%02d-%04d %02d:%02d:%02d", ntpTanggal, ntpBulan, ntpTahun, ntpJam, ntpMenit, ntpDetik);
  tulisTextDWIN(VP_CLOCK_TEXT, buf);
}

// ============================================================================
//  UPDATE DATA REAL-TIME KE DWIN
// ============================================================================
void updateRealTimeData() {
  if (millis() - lastDwinUpdate < DATA_UPDATE_INTERVAL) return;
  lastDwinUpdate = millis();

  float c1, c2, v1, v2;
  readAllSensors(c1, c2, v1, v2);

  // --- Proteksi (cek sebelum nilai ditimpa oleh status toggle) ---
  if (halamanTerkunci == 999) {
    if ((c1 + c2) > THRESHOLD_OVERCURRENT) {
      Serial.println(">>> [ALARM] High Current!");
      RELAY_ON(); stateRelay = true; halamanTerkunci = HAL_ALARM_HC; wakeUpScreen(); gantiHalaman(halamanTerkunci);
    } else if (v1 > THRESHOLD_OVERVOLTAGE || v2 > THRESHOLD_OVERVOLTAGE) {
      Serial.println(">>> [ALARM] Over Voltage!");
      RELAY_ON(); stateRelay = true; halamanTerkunci = HAL_ALARM_OV; wakeUpScreen(); gantiHalaman(halamanTerkunci);
    } else if ((statusToggle1 && v1 < THRESHOLD_UNDERVOLTAGE) || (statusToggle2 && v2 < THRESHOLD_UNDERVOLTAGE)) {
      Serial.println(">>> [ALARM] Voltage Drop!");
      RELAY_ON(); stateRelay = true; halamanTerkunci = HAL_ALARM_VD; wakeUpScreen(); gantiHalaman(halamanTerkunci);
    }
  }

  // --- Sembunyikan parameter jika MOSFET OFF ---
  if (!statusToggle1) {
    v1 = 0.0;
    c1 = 0.0;
  }
  if (!statusToggle2) {
    v2 = 0.0;
    c2 = 0.0;
  }

  // --- Hitung daya & energi ---
  float dayaTotal = max(v1 * c1, 0.0f) + max(v2 * c2, 0.0f);
  totalEnergiKwh += dayaTotal / 3600000.0f;

  // --- Cache untuk API / STATUS ---
  sensorVoltage1 = v1;
  sensorVoltage2 = v2;
  sensorCurrent1 = c1;
  sensorCurrent2 = c2;
  sensorPower    = dayaTotal;

  // --- Kirim data DC ke HMI ---
  tulisFloatDWIN(VP_TEGANGAN_1, v1);
  tulisFloatDWIN(VP_TEGANGAN_2, v2);
  tulisFloatDWIN(VP_ARUS_1, c1);
  tulisFloatDWIN(VP_ARUS_2, c2);
  tulisFloatDWIN(VP_DAYA, dayaTotal);
  tulisFloatDWIN(VP_ENERGI, totalEnergiKwh);
  tulisSemuaCurveDWIN((uint16_t)(v1 * 10), (uint16_t)((c1 + c2) * 10),
                      (uint16_t)(dayaTotal * 10), (uint16_t)(totalEnergiKwh * 10), 350);

  // --- Kirim data AC (PZEM) ---
  float acV = pzem.voltage();
  float acI = pzem.current();
  float acP = pzem.power();
  float acE = pzem.energy();

  if (!isnan(acV)) {
    acVoltage = acV;
    acDataValid = true;
    tulisFloatDWIN(VP_TEGANGAN_AC, acV);
  }

  if (!isnan(acI)) {
    acCurrent = acI;
    tulisFloatDWIN(VP_ARUS_AC, acI);
  }

  if (!isnan(acP)) {
    acPower = acP;
    tulisFloatDWIN(VP_DAYA_AC, acP);
  }

  if (!isnan(acE)) {
    acEnergy = acE;
    tulisFloatDWIN(VP_ENERGI_AC, acE);
  }

  // --- Uptime di halaman Setting ---
  if (halamanAktif == HAL_SETTING) {
    unsigned long s = millis() / 1000;
    char buf[9]; snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
    tulisTextDWIN(VP_SETTING_TEXT, buf);
  }
}

// ============================================================================
//  API / LATTEPANDA SERIAL PROTOCOL
// ============================================================================
//
// Request:
//   STATUS
//   TRACKER ON
//   TRACKER OFF
//   HM30 ON
//   HM30 OFF
//
// Response:
//   JSON satu baris
//
//
// Catatan:
//   Serial USB ESP32 <-> LattePanda/API menggunakan newline (\n)
// ============================================================================

void setTracker(bool enabled) {
  statusToggle1 = enabled;
  digitalWrite(PIN_MOSFET_1, enabled ? HIGH : LOW);

  if (isLayarOnline) {
    tulisIntDWIN(VP_TOGGLE_1_ICON, enabled ? 1 : 0);
  }
}

void setHM30(bool enabled) {
  statusToggle2 = enabled;
  hm30_active = enabled;

  if (!enabled) {
    digitalWrite(PIN_MOSFET_2, LOW);
    OPTO_OFF();
    hm30_state = 0;
  } else {
    // Mulai ulang sequence HM30 dari awal.
    hm30_state = 0;
  }

  if (isLayarOnline) {
    tulisIntDWIN(VP_TOGGLE_2_ICON, enabled ? 1 : 0);
  }
}

void sendStatusJson() {
  float c1, c2, v1, v2;
  readAllSensors(c1, c2, v1, v2);
  
  // float v = pzem.voltage(); // <--- BARIS INI DIHAPUS KARENA BIKIN TIMEOUT

  Serial.print("{\"ok\":true,\"type\":\"status\"");

  // ==========================================================
  // TRACKER
  // ==========================================================
  Serial.print(",\"tracker\":{");

  Serial.print("\"enabled\":");
  Serial.print(statusToggle1 ? "true" : "false");

  Serial.print(",\"voltage\":");
  Serial.print(v1, 2);

  Serial.print(",\"current\":");
  Serial.print(c1, 2);

  Serial.print(",\"power\":");
  Serial.print(max(v1 * c1, 0.0f), 2);

  Serial.print("}");

  // ==========================================================
  // HM30
  // ==========================================================
  Serial.print(",\"hm30\":{");

  Serial.print("\"enabled\":");
  Serial.print(statusToggle2 ? "true" : "false");

  Serial.print(",\"voltage\":");
  Serial.print(v2, 2);

  Serial.print(",\"current\":");
  Serial.print(c2, 2);

  Serial.print(",\"power\":");
  Serial.print(max(v2 * c2, 0.0f), 2);

  Serial.print("}");

  // ==========================================================
  // AC / PZEM
  // ==========================================================
  Serial.print(",\"ac\":{");
  Serial.print("\"valid\":");
  Serial.print(acDataValid ? "true" : "false");

  // --- BAGIAN INI YANG DIUBAH: MENGGUNAKAN VARIABEL CACHE ---
  Serial.print(",\"voltage\":");
  Serial.print(acVoltage, 2);

  Serial.print(",\"current\":");
  Serial.print(acCurrent, 2);

  Serial.print(",\"power\":");
  Serial.print(acPower, 2);

  Serial.print(",\"energy\":");
  Serial.print(acEnergy, 3);
  // ----------------------------------------------------------

  Serial.print("}");

  // ==========================================================
  // RELAY PROTECTION
  // ==========================================================
  Serial.print(",\"relay_protection\":");
  Serial.print(stateRelay ? "true" : "false");

  // ==========================================================
  // ALARM
  // ==========================================================
  Serial.print(",\"alarm\":");

  if (halamanTerkunci == HAL_ALARM_HC) {
    Serial.print("\"HIGH_CURRENT\"");
  }
  else if (halamanTerkunci == HAL_ALARM_OV) {
    Serial.print("\"OVER_VOLTAGE\"");
  }
  else if (halamanTerkunci == HAL_ALARM_VD) {
    Serial.print("\"VOLTAGE_DROP\"");
  }
  else {
    Serial.print("null");
  }

  Serial.println("}");
}
//void kirimStatusOtomatis() {
//  if (millis() - lastApiStatusTime >= API_STATUS_INTERVAL) {
//    lastApiStatusTime = millis();
//    sendStatusJson();
//  }
//}

// ============================================================================
//  SERIAL COMMAND PARSER
// ============================================================================

void processJsonCommand(String command) {
  command.trim();

  // =========================
  // SSR1
  // =========================
  if (command.indexOf("\"device\":\"ssr1\"") >= 0) {

    bool state;

    if (command.indexOf("\"state\":true") >= 0) {
      state = true;
    }
    else if (command.indexOf("\"state\":false") >= 0) {
      state = false;
    }
    else {
      Serial.println("{\"ok\":false,\"error\":\"invalid_state\"}");
      return;
    }

    statusSSR1 = state;
    digitalWrite(SSR1, state ? HIGH : LOW);

    Serial.print("{\"ok\":true,\"device\":\"ssr1\",\"state\":");
    Serial.print(state ? "true" : "false");
    Serial.println("}");
    return;
  }

  // =========================
  // SSR2
  // =========================
  if (command.indexOf("\"device\":\"ssr2\"") >= 0) {

    bool state;

    if (command.indexOf("\"state\":true") >= 0) {
      state = true;
    }
    else if (command.indexOf("\"state\":false") >= 0) {
      state = false;
    }
    else {
      Serial.println("{\"ok\":false,\"error\":\"invalid_state\"}");
      return;
    }

    statusSSR2 = state;
    digitalWrite(SSR2, state ? HIGH : LOW);

    Serial.print("{\"ok\":true,\"device\":\"ssr2\",\"state\":");
    Serial.print(state ? "true" : "false");
    Serial.println("}");
    return;
  }

  // =========================
  // RELAY
  // =========================
  if (command.indexOf("\"device\":\"relay\"") >= 0) {

    bool state;

    if (command.indexOf("\"state\":true") >= 0) {
      state = true;
    }
    else if (command.indexOf("\"state\":false") >= 0) {
      state = false;
    }
    else {
      Serial.println("{\"ok\":false,\"error\":\"invalid_state\"}");
      return;
    }

    stateRelay = state;

    // Relay kamu ACTIVE-LOW
    digitalWrite(PIN_RELAY, state ? LOW : HIGH);

    Serial.print("{\"ok\":true,\"device\":\"relay\",\"state\":");
    Serial.print(state ? "true" : "false");
    Serial.println("}");
    return;
  }

  Serial.println("{\"ok\":false,\"error\":\"unknown_device\"}");
}

void prosesSerialInput() {
  static String command = "";

  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      command.trim();

      if (command.length() > 0) {

  // =========================
  // JSON API COMMAND
  // =========================
  if (command.startsWith("{")) {
    processJsonCommand(command);
  }

  // =========================
  // COMMAND LAMA
  // =========================
  else if (command.equalsIgnoreCase("STATUS")) {
    sendStatusJson();
  }

  else if (command.equalsIgnoreCase("TRACKER ON")) {
    setTracker(true);
    Serial.println("{\"ok\":true,\"device\":\"tracker\",\"state\":true}");
    sendStatusJson();
  }

  // ... lanjutkan command lama kamu di bawah sini
        // ==========================================================
        // API COMMAND
        // ==========================================================

        if (command.equalsIgnoreCase("STATUS")) {
          sendStatusJson();
        }

        else if (command.equalsIgnoreCase("TRACKER ON")) {
          setTracker(true);
          Serial.println("{\"ok\":true,\"device\":\"tracker\",\"state\":true}");
          sendStatusJson();
        }

        else if (command.equalsIgnoreCase("TRACKER OFF")) {
          setTracker(false);
          Serial.println("{\"ok\":true,\"device\":\"tracker\",\"state\":false}");
          sendStatusJson();
        }

        else if (command.equalsIgnoreCase("HM30 ON")) {
          setHM30(true);
          Serial.println("{\"ok\":true,\"device\":\"hm30\",\"state\":true}");
          sendStatusJson();
        }

        else if (command.equalsIgnoreCase("HM30 OFF")) {
          setHM30(false);
          Serial.println("{\"ok\":true,\"device\":\"hm30\",\"state\":false}");
          sendStatusJson();
        }

        // ==========================================================
        // TEST COMMAND LAMA
        // ==========================================================

        else {
          String cmd = command;
          cmd.toUpperCase();

          if      (cmd == "HC") {
            triggerAlarm(HAL_ALARM_HC);
            Serial.println(">>> [SIM] HC Aktif!");
          }
          else if (cmd == "LC") {
            clearAlarm();
            Serial.println(">>> [SIM] HC Selesai.");
          }
          else if (cmd == "OV") {
            triggerAlarm(HAL_ALARM_OV);
            Serial.println(">>> [SIM] OV Aktif!");
          }
          else if (cmd == "NO") {
            clearAlarm();
            Serial.println(">>> [SIM] OV Selesai.");
          }
          else if (cmd == "VD") {
            triggerAlarm(HAL_ALARM_VD);
            Serial.println(">>> [SIM] VD Aktif!");
          }
          else if (cmd == "NV") {
            clearAlarm();
            Serial.println(">>> [SIM] VD Selesai.");
          }
          else if (cmd == "F") {
            hm30_active = true;
            statusToggle2 = true;
            hm30_state = 0;
            Serial.println("[TEST] Sekuens HM-30.");
          }
          else if (cmd == "1") {
            statusSSR1 = !statusSSR1;
            digitalWrite(SSR1, statusSSR1 ? HIGH : LOW);
            Serial.printf("[SSR1] -> %s\n", statusSSR1 ? "ON" : "OFF");
          }
          else if (cmd == "2") {
            statusSSR2 = !statusSSR2;
            digitalWrite(SSR2, statusSSR2 ? HIGH : LOW);
            Serial.printf("[SSR2] -> %s\n", statusSSR2 ? "ON" : "OFF");
          }
          else if (cmd == "ADC") {
            float c1, c2, v1, v2;
            readAllSensors(c1, c2, v1, v2);
            Serial.printf(
              "\nV1=%.2fV V2=%.2fV C1=%.2fA C2=%.2fA Daya=%.2fW\n",
              v1, v2, c1, c2,
              max(v1 * c1, 0.0f) + max(v2 * c2, 0.0f)
            );
          }
          else if (cmd == "PZEM") {
            float v = pzem.voltage();
            if (isnan(v)) {
              Serial.println("PZEM tidak terbaca.");
            } else {
              Serial.printf(
                "AC: %.2fV | %.3fA | %.2fW | %.3fkWh\n",
                v, pzem.current(), pzem.power(), pzem.energy()
              );
            }
          }
          else {
            Serial.println("{\"ok\":false,\"error\":\"unknown_command\"}");
          }
        }
      }

      command = "";
    }
    else {
      // Batasi buffer supaya command rusak tidak membuat String membengkak.
      if (command.length() < 100) {
        command += c;
      } else {
        command = "";
      }
    }
  }
}

// ============================================================================
//  SIMULASI ALARM
// ============================================================================

void triggerAlarm(uint16_t alarm) {
  RELAY_ON();
  stateRelay = true;
  halamanTerkunci = alarm;
  wakeUpScreen();
  gantiHalaman(alarm);
}

void clearAlarm() {
  RELAY_OFF();
  stateRelay = false;
  halamanTerkunci = 999;
  gantiHalaman(HAL_DASHBOARD);
}

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_OFF); btStop();   // Hemat daya

  preferences.begin("auth", false);
  KUNCI_SANDI = preferences.getString("password", DEFAULT_PASSWORD);

  // DWIN RS-485
  pinMode(RS485_DIR_PIN, OUTPUT);
  rs485Rx();
  dwin.setRxBufferSize(512);
  dwin.begin(DWIN_BAUD, SERIAL_8N1, DWIN_RX_PIN, DWIN_TX_PIN);

  // Pin Output — pinMode DULU, baru digitalWrite
  pinMode(PIN_MOSFET_1, OUTPUT); digitalWrite(PIN_MOSFET_1, HIGH);  // Normally ON
  pinMode(PIN_MOSFET_2, OUTPUT); digitalWrite(PIN_MOSFET_2, HIGH);  // Normally ON
  pinMode(SSR1,         OUTPUT); digitalWrite(SSR1, LOW);           // SSR1 OFF
  pinMode(SSR2,         OUTPUT); digitalWrite(SSR2, LOW);           // SSR2 OFF
  pinMode(PIN_OPTO,     OUTPUT); OPTO_OFF();                        // OFF
  pinMode(PIN_RELAY,    OUTPUT); RELAY_OFF();                       // OFF

  // ADC
  analogSetPinAttenuation(PIN_SENSE_CUR_1, ADC_11db);
  analogSetPinAttenuation(PIN_SENSE_CUR_2, ADC_11db);
  analogSetPinAttenuation(PIN_V1, ADC_11db);
  analogSetPinAttenuation(PIN_V2, ADC_11db);

  Serial.println("\n[SYSTEM] Booting Selesai. Menunggu DWIN...");
  Serial.println("[API]    STATUS | TRACKER ON | TRACKER OFF | HM30 ON | HM30 OFF");
  Serial.println("[TEST]   HC LC OV NO VD NV F ADC PZEM | 1=SSR1 2=SSR2");
}

// ============================================================================
//  LOOP
// ============================================================================
void loop() {
  bacaSerialDWIN();
  kirimHeartbeatPing();
  prosesTriggerAnimasiLanding();
  prosesPopupPassword();
  prosesPowerManagement();
  handleHM30Sequence();
  updateJamTextDWIN();
  if (isLayarOnline && isSystemActive) updateRealTimeData();

  // USB Serial API: terima command dari LattePanda
  // dan kirim status otomatis setiap 1.5 detik.
  prosesSerialInput();
  //  kirimStatusOtomatis();
}
