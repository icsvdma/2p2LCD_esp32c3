/**
 * UTF-8 文字列の折り返し・切り詰めユーティリティ
 *
 * LovyanGFX の textWidth() で 1 文字ずつ幅を測り、指定幅で行に分割する。
 * フォントは呼び出し側で gfx.setFont() 済みであること
 * (幅の計測と描画は必ず同じ gfx / 同じフォントで行う)。
 */
#pragma once

#include <Arduino.h>
#include <vector>

// UTF-8 の先頭バイトから、その文字のバイト数を返す
inline int utf8CharLen(uint8_t c) {
  if (c >= 0xF0) return 4;
  if (c >= 0xE0) return 3;
  if (c >= 0xC0) return 2;
  return 1;  // ASCII (または不正な継続バイト。1バイトずつ進めて無限ループを防ぐ)
}

// 1 行分の範囲 (text 内のバイトオフセット [start, end))
struct TextLine {
  int start;
  int end;
};

/**
 * text を maxWidth ピクセルで折り返し、各行の範囲を lines に格納する。
 * '\n' は強制改行、'\r' は無視する。
 * maxLines 行に達したら打ち切り、文字が残っていれば true (切り詰めあり) を返す。
 */
template <typename GFX>
bool wrapText(GFX& gfx, const String& text, int maxWidth,
              std::vector<TextLine>& lines, size_t maxLines = SIZE_MAX) {
  lines.clear();
  const int len = text.length();
  const char* s = text.c_str();

  int lineStart = 0;
  int lineWidth = 0;
  int i = 0;
  char buf[5];

  while (i < len) {
    if (lines.size() >= maxLines) return true;

    const uint8_t c = static_cast<uint8_t>(s[i]);

    if (c == '\n') {
      // CRLF の '\r' は行に含めない
      const int end = (i > lineStart && s[i - 1] == '\r') ? i - 1 : i;
      lines.push_back({lineStart, end});
      i++;
      lineStart = i;
      lineWidth = 0;
      continue;
    }
    if (c == '\r') {
      i++;
      continue;
    }

    int n = utf8CharLen(c);
    if (i + n > len) n = len - i;  // 末尾が壊れた UTF-8 でも範囲外を読まない
    memcpy(buf, s + i, n);
    buf[n] = '\0';
    const int w = gfx.textWidth(buf);

    if (lineWidth + w > maxWidth && i > lineStart) {
      lines.push_back({lineStart, i});
      lineStart = i;
      lineWidth = 0;
      continue;  // 同じ文字を次の行の先頭として再処理
    }

    lineWidth += w;
    i += n;
  }

  if (lineStart < len) {
    if (lines.size() >= maxLines) return true;
    lines.push_back({lineStart, len});
  }
  return false;
}

/**
 * text が maxWidth に収まらなければ末尾を「…」にして切り詰めた文字列を返す。
 * (改行は空白として扱う)
 */
template <typename GFX>
String fitText(GFX& gfx, const String& text, int maxWidth) {
  String oneLine = text;
  oneLine.replace("\r", "");
  oneLine.replace("\n", " ");
  if (gfx.textWidth(oneLine) <= maxWidth) return oneLine;

  static const char* const ELLIPSIS = "…";
  const int avail = maxWidth - gfx.textWidth(ELLIPSIS);
  if (avail <= 0) return String(ELLIPSIS);

  std::vector<TextLine> lines;
  wrapText(gfx, oneLine, avail, lines, 1);
  if (lines.empty()) return String(ELLIPSIS);
  return oneLine.substring(lines[0].start, lines[0].end) + ELLIPSIS;
}
