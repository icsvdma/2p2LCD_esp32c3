/**
 * AHT20 温湿度センサー (I2C, アドレス 0x38) ドライバー
 *
 * 測定には約 80ms かかるので、start() で測定を開始し、update() を loop から呼んで
 * 完了したら結果を読む「ノンブロッキング」方式にしている (加速度サンプリングを止めないため)。
 *
 * 手順 (AHT20 データシート / Adafruit_AHTX0・AHTxx ライブラリと同じ):
 *   1. 電源投入後 40ms 以上待つ
 *   2. ステータスを読み、bit3 (キャリブレーション済み) が 0 なら 0xBE 0x08 0x00 を送る
 *   3. 測定開始: 0xAC 0x33 0x00
 *   4. 80ms 以上待ってから 7 バイト読む [status][RH][RH][RH|T][T][T][CRC]
 *      status の bit7 (busy) が 1 ならまだ測定中
 *   5. CRC-8 (多項式 0x31, 初期値 0xFF) を先頭 6 バイトで計算し、7 バイト目と照合
 *   湿度 = RH(20bit) / 2^20 * 100 [%]、温度 = T(20bit) / 2^20 * 200 - 50 [℃]
 *
 * 計算部分 (crc8 / decode) は Arduino 非依存なので PC でもテストできる。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aht20 {

constexpr uint8_t I2C_ADDR = 0x38;
constexpr uint8_t STATUS_BUSY = 0x80;
constexpr uint8_t STATUS_CALIBRATED = 0x08;

inline uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

enum class DecodeResult { Ok, Busy, CrcError };

// 測定結果 7 バイトを温度・湿度に変換する
inline DecodeResult decode(const uint8_t raw[7], float& temperature, float& humidity) {
  if (raw[0] & STATUS_BUSY) return DecodeResult::Busy;
  if (crc8(raw, 6) != raw[6]) return DecodeResult::CrcError;

  const uint32_t rh = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | (raw[3] >> 4);
  const uint32_t t = ((uint32_t)(raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | raw[5];
  humidity = rh * 100.0f / 1048576.0f;
  temperature = t * 200.0f / 1048576.0f - 50.0f;
  return DecodeResult::Ok;
}

}  // namespace aht20

#ifdef ARDUINO
#include <Arduino.h>
#include <Wire.h>

class AHT20 {
public:
  // Wire.begin() 済みであること。センサーが応答しなければ false
  bool begin(TwoWire& wire = Wire) {
    _wire = &wire;
    _ready = false;

    uint8_t status = 0;
    if (!readBytes(&status, 1)) return false;
    if (!(status & aht20::STATUS_CALIBRATED)) {
      const uint8_t init[3] = {0xBE, 0x08, 0x00};
      if (!writeBytes(init, sizeof(init))) return false;
      delay(10);
      if (!readBytes(&status, 1) || !(status & aht20::STATUS_CALIBRATED)) return false;
    }
    _ready = true;
    return true;
  }

  bool ready() const { return _ready; }

  // 測定を開始する (既に測定中なら何もしない)
  bool start() {
    if (!_ready || _measuring) return false;
    const uint8_t cmd[3] = {0xAC, 0x33, 0x00};
    if (!writeBytes(cmd, sizeof(cmd))) return false;
    _measuring = true;
    _startMs = millis();
    return true;
  }

  // loop から呼ぶ。新しい測定値が得られたら true
  bool update() {
    if (!_measuring) return false;
    const unsigned long elapsed = millis() - _startMs;
    if (elapsed < MEASURE_MS) return false;

    uint8_t raw[7];
    if (!readBytes(raw, sizeof(raw))) {
      _measuring = false;
      _errors++;
      return false;
    }
    float t = 0;
    float h = 0;
    switch (aht20::decode(raw, t, h)) {
      case aht20::DecodeResult::Ok:
        _measuring = false;
        _temperature = t;
        _humidity = h;
        _hasValue = true;
        _lastValueMs = millis();
        return true;
      case aht20::DecodeResult::Busy:
        if (elapsed > TIMEOUT_MS) {  // 応答しないまま時間切れ
          _measuring = false;
          _errors++;
        }
        return false;  // まだ測定中 → 次の update() で読み直す
      case aht20::DecodeResult::CrcError:
      default:
        _measuring = false;
        _errors++;
        return false;
    }
  }

  // 有効な値があり、かつ古すぎないか
  bool hasFreshValue(unsigned long maxAgeMs) const {
    return _hasValue && millis() - _lastValueMs <= maxAgeMs;
  }
  float temperature() const { return _temperature; }
  float humidity() const { return _humidity; }
  uint32_t errors() const { return _errors; }

private:
  static constexpr unsigned long MEASURE_MS = 80;
  static constexpr unsigned long TIMEOUT_MS = 300;

  bool writeBytes(const uint8_t* data, size_t len) {
    _wire->beginTransmission(aht20::I2C_ADDR);
    _wire->write(data, len);
    return _wire->endTransmission() == 0;
  }
  bool readBytes(uint8_t* buf, size_t len) {
    if (_wire->requestFrom(aht20::I2C_ADDR, (uint8_t)len) != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = _wire->read();
    return true;
  }

  TwoWire* _wire = nullptr;
  bool _ready = false;
  bool _measuring = false;
  bool _hasValue = false;
  unsigned long _startMs = 0;
  unsigned long _lastValueMs = 0;
  float _temperature = 0;
  float _humidity = 0;
  uint32_t _errors = 0;
};
#endif  // ARDUINO
