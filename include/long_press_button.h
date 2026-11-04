/**
 * 非ブロッキングのボタン入力 (短押し / 長押し判定)
 *
 * プルアップ入力で、押すと LOW になるボタンを想定。
 * loop() から毎回 update() を呼ぶこと。
 */
#pragma once

#include <Arduino.h>

class LongPressButton {
public:
  LongPressButton(int pin, unsigned long longPressMs) : _pin(pin), _longPressMs(longPressMs) {}

  void begin() { pinMode(_pin, INPUT_PULLUP); }

  // 状態を更新し、長押しが成立している間は true を返す
  bool update() {
    const bool pressed = digitalRead(_pin) == LOW;
    const unsigned long now = millis();
    if (pressed && !_pressed) {
      _pressStart = now;
      _longReported = false;
    }
    _justReleased = !pressed && _pressed;
    if (_justReleased) _releasedHeldMs = now - _pressStart;
    _pressed = pressed;
    return pressed && now - _pressStart >= _longPressMs;
  }

  bool isPressed() const { return _pressed; }
  bool justReleased() const { return _justReleased; }
  unsigned long heldMs() const { return _pressed ? millis() - _pressStart : 0; }

  // 長押し成立を 1 回の押下につき 1 度だけ true で返す (update() の後に呼ぶ)
  bool consumeLongPress() {
    if (_pressed && !_longReported && millis() - _pressStart >= _longPressMs) {
      _longReported = true;
      return true;
    }
    return false;
  }

  // 長押しにならずに離された (= 短押し)。update() の後に呼ぶ
  // 50ms 未満はチャタリングとして無視する
  bool wasShortPress() const {
    return _justReleased && !_longReported && _releasedHeldMs >= 50 && _releasedHeldMs < _longPressMs;
  }

private:
  int _pin;
  unsigned long _longPressMs;
  bool _pressed = false;
  bool _justReleased = false;
  bool _longReported = false;
  unsigned long _pressStart = 0;
  unsigned long _releasedHeldMs = 0;
};
