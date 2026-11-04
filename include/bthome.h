/**
 * BTHome v2 (https://bthome.io/format/) の最小実装
 *
 * - 送信側 (センサーノード) 用の Encoder と、受信側 (モニター) 用の parse() を提供
 * - Arduino に依存しない純粋な C++ なので、PC 上でもユニットテストできる
 * - 暗号化 (AES-CCM) ペイロードには未対応 (parse() は Encrypted を返す)
 *
 * BTHome v2 のアドバタイズ形式:
 *   AD type 0x16 (Service Data - 16bit UUID), UUID = 0xFCD2 (リトルエンディアンで D2 FC)
 *   [device info 1byte][object id][data]...[object id][data]
 *     device info: bit0=暗号化, bit2=トリガー送信 (不定期), bit5-7=バージョン (2)
 *   object は object id の昇順に並べる。値はリトルエンディアン。
 *
 * object id ごとのデータ長は公式パーサー (Bluetooth-Devices/bthome-ble の const.py) から生成した。
 * 未知の id が来たら以降は解析できない (長さが分からない) ので、そこで打ち切る。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace bthome {

constexpr uint16_t SERVICE_UUID = 0xFCD2;
constexpr uint8_t VERSION = 2;

constexpr uint8_t DEVICE_INFO_ENCRYPTED = 1 << 0;
constexpr uint8_t DEVICE_INFO_TRIGGER_BASED = 1 << 2;

// 本実装で扱う object id
namespace id {
constexpr uint8_t PACKET_ID = 0x00;          // uint8
constexpr uint8_t BATTERY = 0x01;            // uint8  %
constexpr uint8_t TEMPERATURE_001 = 0x02;    // sint16 x0.01 ℃
constexpr uint8_t HUMIDITY_001 = 0x03;       // uint16 x0.01 %
constexpr uint8_t VOLTAGE = 0x0C;            // uint16 x0.001 V
constexpr uint8_t GENERIC_BOOLEAN = 0x0F;    // uint8  0/1
constexpr uint8_t DOOR = 0x1A;               // uint8  0=閉 1=開
constexpr uint8_t MOTION = 0x21;             // uint8  0/1
constexpr uint8_t WINDOW = 0x2D;             // uint8  0=閉 1=開
constexpr uint8_t HUMIDITY_1 = 0x2E;         // uint8  %
constexpr uint8_t COMMAND = 0x3B;            // [len(下位5bit)][opcode][args...]
constexpr uint8_t TEMPERATURE_01 = 0x45;     // sint16 x0.1 ℃
constexpr uint8_t ACCELERATION = 0x51;       // uint16 x0.001 m/s² (大きさ)
constexpr uint8_t TEXT = 0x53;               // [len][bytes...]
constexpr uint8_t RAW = 0x54;                // [len][bytes...]
}  // namespace id

// object id -> 固定データ長 (byte)。0 = 未知、VAR = 長さバイト付き可変長
constexpr uint8_t VAR = 0xFF;

inline uint8_t objectLength(uint8_t objectId) {
  // 0x00-0x2F
  static const uint8_t T00[0x30] = {
      1, 1, 2, 2, 3, 3, 2, 2, 2, 1, 3, 3, 2, 2, 2, 1,  // 0x00
      1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x10
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x20
  };
  // 0x3A-0x65 (0x30-0x39 は未定義)
  static const uint8_t T3A[0x66 - 0x3A] = {
      1, VAR, 2, 2, 4, 2,                              // 0x3A-0x3F
      2, 2, 3, 2, 2, 2, 1, 2, 2, 2, 2, 3, 4, 4, 4, 4,  // 0x40-0x4F
      4, 2, 2, VAR, VAR, 4, 2, 1, 1, 1, 2, 4, 4, 2, 2, 2,  // 0x50-0x5F
      1, 2, 4, 4, 1, 1,                                // 0x60-0x65
  };
  if (objectId < 0x30) return T00[objectId];
  if (objectId >= 0x3A && objectId <= 0x65) return T3A[objectId - 0x3A];
  switch (objectId) {
    case 0xF0: return 2;  // device type id
    case 0xF1: return 4;  // firmware version (4byte)
    case 0xF2: return 3;  // firmware version (3byte)
    default: return 0;
  }
}

// 受信した値 (存在するものだけ has* が true)
struct Reading {
  uint8_t version = 0;
  bool triggerBased = false;

  bool hasPacketId = false;
  uint8_t packetId = 0;

  bool hasBattery = false;
  uint8_t battery = 0;  // %

  bool hasTemperature = false;
  float temperature = 0;  // ℃

  bool hasHumidity = false;
  float humidity = 0;  // %

  bool hasVoltage = false;
  float voltage = 0;  // V

  // 開閉/オンオフ系 (ホールセンサー想定)。0x0F / 0x1A / 0x2D のうち最初に見つかったもの
  bool hasBinary = false;
  bool binary = false;
  uint8_t binaryId = 0;

  bool hasMotion = false;
  bool motion = false;

  bool hasAcceleration = false;
  float acceleration = 0;  // m/s² (大きさ)
};

enum class ParseResult {
  Ok,
  NotBTHomeV2,  // バージョンが 2 ではない
  Encrypted,    // 暗号化ペイロード (未対応)
  Malformed,    // 長さ不足など
};

inline uint16_t readU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline int16_t readS16(const uint8_t* p) { return (int16_t)readU16(p); }

/**
 * Service Data (UUID 0xFCD2) の中身 (UUID の後ろ、device info から) を解析する。
 * 途中で未知の object id や長さ不足があっても、そこまでに読めた値は out に残る。
 */
inline ParseResult parse(const uint8_t* data, size_t len, Reading& out) {
  out = Reading();
  if (len < 1) return ParseResult::Malformed;

  const uint8_t info = data[0];
  out.version = (info >> 5) & 0x07;
  out.triggerBased = (info & DEVICE_INFO_TRIGGER_BASED) != 0;
  if (out.version != VERSION) return ParseResult::NotBTHomeV2;
  if (info & DEVICE_INFO_ENCRYPTED) return ParseResult::Encrypted;

  size_t pos = 1;
  while (pos < len) {
    const uint8_t objId = data[pos];
    const uint8_t fixedLen = objectLength(objId);
    if (fixedLen == 0) return ParseResult::Ok;  // 未知の id: 以降は長さが分からないので打ち切り

    size_t dataStart = pos + 1;
    size_t dataLen = fixedLen;
    if (fixedLen == VAR) {
      if (pos + 2 > len) return ParseResult::Malformed;
      const uint8_t lenByte = data[pos + 1];
      dataStart = pos + 2;
      // command は「下位5bit = 引数長」で、opcode 1byte を含める
      dataLen = (objId == id::COMMAND) ? 1 + (lenByte & 0x1F) : lenByte;
    }
    if (dataStart + dataLen > len) return ParseResult::Malformed;

    const uint8_t* v = data + dataStart;
    switch (objId) {
      case id::PACKET_ID:
        out.hasPacketId = true;
        out.packetId = v[0];
        break;
      case id::BATTERY:
        out.hasBattery = true;
        out.battery = v[0];
        break;
      case id::TEMPERATURE_001:
        out.hasTemperature = true;
        out.temperature = readS16(v) * 0.01f;
        break;
      case id::TEMPERATURE_01:
        if (!out.hasTemperature) {
          out.hasTemperature = true;
          out.temperature = readS16(v) * 0.1f;
        }
        break;
      case id::HUMIDITY_001:
        out.hasHumidity = true;
        out.humidity = readU16(v) * 0.01f;
        break;
      case id::HUMIDITY_1:
        if (!out.hasHumidity) {
          out.hasHumidity = true;
          out.humidity = v[0];
        }
        break;
      case id::VOLTAGE:
        out.hasVoltage = true;
        out.voltage = readU16(v) * 0.001f;
        break;
      case id::GENERIC_BOOLEAN:
      case id::DOOR:
      case id::WINDOW:
        if (!out.hasBinary) {
          out.hasBinary = true;
          out.binary = v[0] != 0;
          out.binaryId = objId;
        }
        break;
      case id::MOTION:
        out.hasMotion = true;
        out.motion = v[0] != 0;
        break;
      case id::ACCELERATION:
        out.hasAcceleration = true;
        out.acceleration = readU16(v) * 0.001f;
        break;
      default:
        break;  // 長さは分かるが表示しない object は読み飛ばす
    }
    pos = dataStart + dataLen;
  }
  return ParseResult::Ok;
}

/**
 * 送信用ペイロード (device info + objects) を組み立てる。
 * object は id の昇順で add すること (BTHome の仕様)。
 * 生成したバイト列を Service Data (UUID 0xFCD2) として広告する。
 */
class Encoder {
public:
  static constexpr size_t CAPACITY = 24;  // 31byte の広告パケットに収まる程度

  explicit Encoder(bool triggerBased = false) {
    _buf[0] = (uint8_t)((VERSION << 5) | (triggerBased ? DEVICE_INFO_TRIGGER_BASED : 0));
    _len = 1;
  }

  bool addPacketId(uint8_t v) { return addU8(id::PACKET_ID, v); }
  bool addBattery(uint8_t percent) { return addU8(id::BATTERY, percent > 100 ? 100 : percent); }
  bool addTemperature(float celsius) { return addS16(id::TEMPERATURE_001, celsius * 100.0f); }
  bool addHumidity(float percent) { return addU16(id::HUMIDITY_001, percent * 100.0f); }
  bool addVoltage(float volts) { return addU16(id::VOLTAGE, volts * 1000.0f); }
  bool addGenericBoolean(bool on) { return addU8(id::GENERIC_BOOLEAN, on ? 1 : 0); }
  bool addMotion(bool detected) { return addU8(id::MOTION, detected ? 1 : 0); }
  bool addAcceleration(float mps2) { return addU16(id::ACCELERATION, mps2 * 1000.0f); }

  const uint8_t* data() const { return _buf; }
  size_t length() const { return _len; }
  bool ok() const { return !_overflow; }

private:
  bool reserve(uint8_t objId, size_t n) {
    if (_len + 1 + n > CAPACITY) {
      _overflow = true;
      return false;
    }
    _buf[_len++] = objId;
    return true;
  }
  bool addU8(uint8_t objId, uint8_t v) {
    if (!reserve(objId, 1)) return false;
    _buf[_len++] = v;
    return true;
  }
  bool addU16(uint8_t objId, float scaled) {
    if (!reserve(objId, 2)) return false;
    const float r = scaled + 0.5f;
    const uint16_t v = r <= 0 ? 0 : (r >= 65535.0f ? 65535 : (uint16_t)r);
    _buf[_len++] = v & 0xFF;
    _buf[_len++] = v >> 8;
    return true;
  }
  bool addS16(uint8_t objId, float scaled) {
    if (!reserve(objId, 2)) return false;
    const float r = scaled >= 0 ? scaled + 0.5f : scaled - 0.5f;
    const int16_t v = r <= -32768.0f ? -32768 : (r >= 32767.0f ? 32767 : (int16_t)r);
    _buf[_len++] = (uint16_t)v & 0xFF;
    _buf[_len++] = (uint16_t)v >> 8;
    return true;
  }

  uint8_t _buf[CAPACITY];
  size_t _len = 0;
  bool _overflow = false;
};

}  // namespace bthome
