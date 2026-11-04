/**
 * BLE センサーノード (BTHome v2 送信) サンプル
 *
 * 使用ボード: Seeed XIAO ESP32C3 (他の ESP32 系でもピン設定を変えれば動く想定)
 * 使用ライブラリ: NimBLE-Arduino
 *
 * センサーの値を BTHome v2 形式でアドバタイズするだけ (接続は受け付けない)。
 * 受信側は src/ble_monitor.cpp。Home Assistant などの BTHome 対応ソフトでも受信できる。
 *
 * 送信する値 (接続されていないセンサーは送らない):
 *   - 温度・湿度 : AHT20 (I2C 0x38)。2秒毎に測定
 *                  ※ AHT20 が無ければ MPU6050 の温度 → ESP32-C3 の内蔵温度センサーの順で代用
 *                    (この2つはチップ自体の温度なので室温より高めに出る。湿度は送らない)
 *   - オンオフ   : ホールセンサー (A3144 / DRV5032 などデジタル出力品) を HALL_PIN に接続
 *   - 加速度     : MPU6050 (I2C) の 3 軸から求めた大きさ (m/s²)。静止時は約 9.8
 *   - 動き検知   : 送信間隔の間に加速度が MOTION_THRESHOLD 以上変化したら motion=1
 *
 * 配線例 (XIAO ESP32C3):
 *   ホールセンサー OUT -> D1 (GPIO3)  ※ VCC=3.3V, GND。オープンドレイン出力なら内部プルアップで OK
 *   AHT20 / MPU6050 (同じ I2C バスに並列接続) SDA -> D4 (GPIO6), SCL -> D5 (GPIO7), VCC=3.3V, GND
 *   ※ 市販の AHT20 モジュールの多くは I2C プルアップ抵抗付き
 *   ※ GPIO2 / GPIO8 / GPIO9 はストラッピングピンなのでセンサーには使わない方が安全
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <math.h>

#include "aht20.h"
#include "bthome.h"

// ================== 設定 ==================
// ノード名 (広告に入れる。空なら "BTH-xxxx" (MACアドレス末尾) を自動生成)
#ifndef NODE_NAME
#define NODE_NAME ""
#endif

#define HALL_PIN 3              // ホールセンサー入力 (-1 で無効)
#define HALL_ACTIVE_LOW true    // 磁石を検知すると LOW になるセンサーなら true
#define I2C_SDA_PIN 6
#define I2C_SCL_PIN 7
#define MPU6050_ADDR 0x68

const unsigned long ADVERTISE_PERIOD_MS = 2000;  // 定期送信の間隔
const unsigned long SAMPLE_PERIOD_MS = 20;       // 加速度のサンプリング間隔 (50Hz)
const unsigned long AHT20_PERIOD_MS = 2000;      // AHT20 の測定間隔 (頻繁に測ると自己発熱で温度が上がる)
const unsigned long AHT20_MAX_AGE_MS = 10000;    // これより古い AHT20 の値は送らない
const unsigned long MIN_EVENT_INTERVAL_MS = 100;   // イベント送信の最短間隔
const float MOTION_THRESHOLD = 0.6f;             // 動き検知のしきい値 (m/s²)
const float GRAVITY = 9.80665f;

// 広告間隔 (単位 0.625ms)。受信側のスキャン間隔 100ms より短めにして取りこぼしを減らす
const uint16_t ADV_INTERVAL_MIN = 80;   // 50ms
const uint16_t ADV_INTERVAL_MAX = 160;  // 100ms

// ================== MPU6050 ==================
bool mpuReady = false;

bool mpuWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool mpuRead(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool mpuBegin() {
  uint8_t who = 0;
  if (!mpuRead(0x75, &who, 1)) return false;  // WHO_AM_I
  Serial.printf("MPU6050 WHO_AM_I = 0x%02X\n", who);
  // スリープ解除 (PWR_MGMT_1 = 0)、加速度レンジ ±2g (ACCEL_CONFIG = 0)
  return mpuWrite(0x6B, 0x00) && mpuWrite(0x1C, 0x00);
}

// 加速度の大きさ (m/s²) を返す。失敗したら NAN
float mpuReadAccelMagnitude() {
  uint8_t b[6];
  if (!mpuRead(0x3B, b, sizeof(b))) return NAN;  // ACCEL_XOUT_H から 6 バイト
  const int16_t ax = (int16_t)((b[0] << 8) | b[1]);
  const int16_t ay = (int16_t)((b[2] << 8) | b[3]);
  const int16_t az = (int16_t)((b[4] << 8) | b[5]);
  const float scale = GRAVITY / 16384.0f;  // ±2g レンジ: 16384 LSB/g
  const float x = ax * scale, y = ay * scale, z = az * scale;
  return sqrtf(x * x + y * y + z * z);
}

float mpuReadTemperature() {
  uint8_t b[2];
  if (!mpuRead(0x41, b, sizeof(b))) return NAN;  // TEMP_OUT_H
  const int16_t raw = (int16_t)((b[0] << 8) | b[1]);
  return raw / 340.0f + 36.53f;  // データシート記載の換算式
}

// ================== AHT20 ==================
AHT20 aht;

void serviceAht20() {
  static unsigned long lastStart = 0;
  if (!aht.ready()) return;
  if (aht.update()) {
    Serial.printf("AHT20: %.2f C, %.1f %%\n", aht.temperature(), aht.humidity());
  }
  const unsigned long now = millis();
  if (now - lastStart >= AHT20_PERIOD_MS && aht.start()) {
    lastStart = now;
  }
}

// ================== センサー状態 ==================
float lastAccel = NAN;
float accelNow = NAN;
float peakAccelDelta = 0;  // 前回送信以降の、サンプル間の最大変化量
bool motionPending = false;
bool lastHall = false;

bool readHall() {
  if (HALL_PIN < 0) return false;
  const bool level = digitalRead(HALL_PIN) == HIGH;
  return HALL_ACTIVE_LOW ? !level : level;
}

void sampleAccel() {
  if (!mpuReady) return;
  const float a = mpuReadAccelMagnitude();
  if (isnan(a)) return;
  if (!isnan(lastAccel)) {
    peakAccelDelta = fmaxf(peakAccelDelta, fabsf(a - lastAccel));
  }
  lastAccel = a;
  accelNow = a;
}

// ================== BLE 広告 ==================
NimBLEAdvertising* advertising = nullptr;
std::string nodeName;
uint8_t packetId = 0;

void advertise(bool motion) {
  bthome::Encoder enc;
  enc.addPacketId(packetId++);  // 値が変わったことを受信側に知らせる (再送と区別)

  // object id の昇順で追加する (0x00 -> 0x02 -> 0x03 -> 0x0F -> 0x21 -> 0x51)
  float temp = NAN;
  float humidity = NAN;
  if (aht.ready()) {
    if (aht.hasFreshValue(AHT20_MAX_AGE_MS)) {  // 測定に失敗し続けているときは送らない
      temp = aht.temperature();
      humidity = aht.humidity();
    }
  } else {
    temp = mpuReady ? mpuReadTemperature() : temperatureRead();
  }
  if (!isnan(temp)) enc.addTemperature(temp);
  if (!isnan(humidity)) enc.addHumidity(humidity);
  if (HALL_PIN >= 0) enc.addGenericBoolean(lastHall);
  if (mpuReady) enc.addMotion(motion);
  if (mpuReady && !isnan(accelNow)) enc.addAcceleration(accelNow);

  // Flags は付けない: 接続不可の広告では省略してよい (Bluetooth Core 仕様) ので、
  // その 3 バイトを名前に回す (全項目送信時でも 9 文字まで入る)
  NimBLEAdvertisementData data;
  data.setServiceData(NimBLEUUID((uint16_t)bthome::SERVICE_UUID),
                      std::string(reinterpret_cast<const char*>(enc.data()), enc.length()));
  // 31 バイトに収まるよう名前を切り詰める (service data (4 + 長さ) + 名前 (2 + 長さ))
  const int nameRoom = 31 - (int)(4 + enc.length()) - 2;
  if (nameRoom > 0) data.setName(nodeName.substr(0, nameRoom));

  advertising->stop();
  advertising->setAdvertisementData(data);
  advertising->start();

  Serial.printf("adv #%u temp=%.2f hum=%.1f hall=%d acc=%.3f motion=%d (%u bytes)\n",
                (unsigned)(uint8_t)(packetId - 1), temp, humidity, lastHall, accelNow, motion,
                (unsigned)enc.length());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== BTHome Sensor Node ===");

  if (HALL_PIN >= 0) {
    pinMode(HALL_PIN, INPUT_PULLUP);
    lastHall = readHall();
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);  // AHT20・MPU6050 とも 400kHz 対応

  // AHT20 は電源投入後 40ms 以上待ってから通信する (起動直後の delay(500) で満たしている)
  if (aht.begin(Wire)) {
    Serial.println("AHT20 ready");
    aht.start();
  } else {
    Serial.println("AHT20 not found (fallback to MPU6050 / internal temperature)");
  }

  mpuReady = mpuBegin();
  Serial.println(mpuReady ? "MPU6050 ready" : "MPU6050 not found (acceleration disabled)");

  NimBLEDevice::init("");
  nodeName = NODE_NAME;
  if (nodeName.empty()) {
    const uint8_t* mac = NimBLEDevice::getAddress().getNative();
    char buf[12];
    snprintf(buf, sizeof(buf), "BTH-%02X%02X", mac[1], mac[0]);
    nodeName = buf;
  }
  Serial.printf("Node name: %s\n", nodeName.c_str());

  advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementType(BLE_GAP_CONN_MODE_NON);  // 接続不可 (ブロードキャストのみ)
  advertising->setScanResponse(false);
  advertising->setMinInterval(ADV_INTERVAL_MIN);
  advertising->setMaxInterval(ADV_INTERVAL_MAX);

  // AHT20 の最初の測定 (約80ms) を待ってから初回送信
  const unsigned long waitStart = millis();
  while (aht.ready() && !aht.hasFreshValue(AHT20_MAX_AGE_MS) && millis() - waitStart < 300) {
    serviceAht20();
    delay(5);
  }

  sampleAccel();
  advertise(false);
}

void loop() {
  static unsigned long lastSample = 0;
  static unsigned long lastAdvertise = millis();
  const unsigned long now = millis();

  if (now - lastSample >= SAMPLE_PERIOD_MS) {
    lastSample = now;
    sampleAccel();
    if (peakAccelDelta >= MOTION_THRESHOLD) motionPending = true;
  }

  serviceAht20();

  // ホールセンサーの変化、動き始めはすぐに送る (イベント送信)
  bool sendNow = now - lastAdvertise >= ADVERTISE_PERIOD_MS;
  const bool hall = readHall();
  if (HALL_PIN >= 0 && hall != lastHall) {
    lastHall = hall;
    sendNow = true;
  }
  static bool lastSentMotion = false;
  if (motionPending && !lastSentMotion) sendNow = true;

  // イベント送信が連続しすぎないように最短間隔を設ける (チャタリング対策も兼ねる)
  if (sendNow && now - lastAdvertise >= MIN_EVENT_INTERVAL_MS) {
    advertise(motionPending);
    lastSentMotion = motionPending;
    motionPending = false;
    peakAccelDelta = 0;
    lastAdvertise = now;
  }

  delay(5);
}
