/**
 * @file text_entry.cpp
 * @brief Интерфейс: ввод текста с экранной клавиатуры.
 */
#include "text_entry.h"
#include "font_data.h"

namespace ui {

namespace {

/// Длина кодировки символа в UTF-8. 0 — такой символ не поддержан.
///
/// Трёхбайтовых здесь нет намеренно: шрифт знает латиницу, кириллицу
/// и длинное тире, а тире с клавиатуры не набирают.
u32 utf8Len(u32 cp) {
    if (cp < 0x80)   return 1;
    if (cp < 0x800)  return 2;
    return 0;
}

/// Начало ли это байта символа (а не его продолжения).
bool isLead(char c) { return ((u8)c & 0xC0) != 0x80; }

} // namespace

bool TextEntry::insert(u32 codepoint) {
    // Рисовать нечем — значит и хранить незачем: имя мира, которое
    // покажется рядом пустых клеток, хуже безымянного.
    if (glyphIndex(codepoint) < 0) return false;

    const u32 n = utf8Len(codepoint);
    if (n == 0) return false;
    if (len_ + n > CAP) return false;

    if (n == 1) {
        buf_[len_++] = (char)codepoint;
    } else {
        buf_[len_++] = (char)(0xC0 | (codepoint >> 6));
        buf_[len_++] = (char)(0x80 | (codepoint & 0x3F));
    }
    buf_[len_] = '\0';
    return true;
}

bool TextEntry::backspace() {
    if (len_ == 0) return false;
    // Назад до начала символа: у кириллицы их два байта, и забой
    // «на байт» оставлял бы за собой половину буквы.
    u32 i = len_;
    do { --i; } while (i > 0 && !isLead(buf_[i]));
    len_ = i;
    buf_[len_] = '\0';
    return true;
}

void TextEntry::setText(const char* s) {
    clear();
    if (!s) return;
    usize n = 0;
    while (s[n] != '\0') ++n;
    usize i = 0;
    while (i < n) {
        const u32 cp = utf8Next(s, n, i);
        if (!insert(cp)) {
            // Не влезло — дальше и подавно не влезет.
            if (len_ + utf8Len(cp) > CAP) break;
        }
    }
}

u32 TextEntry::glyphs() const {
    u32 g = 0;
    for (u32 i = 0; i < len_; ++i) if (isLead(buf_[i])) ++g;
    return g;
}

// ============================================================
// Раскладки
// ============================================================
//
// Ряды взяты с обычной телефонной клавиатуры, чтобы искать буквы не
// пришлось: пальцы помнят ЙЦУКЕН и QWERTY, а алфавитный порядок
// помнит только тот, кто его составлял.
namespace {

const char* const LATIN[3] = {
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
};

const char* const CYRILLIC[3] = {
    "ЙЦУКЕНГШЩЗХ",
    "ФЫВАПРОЛДЖЭ",
    "ЯЧСМИТЬБЮЁ",
};

const char* const DIGITS[3] = {
    "1234567890",
    "-_.,!?",
    "()[]#$%&*+=/",
};

} // namespace

u32 keyRowCount(KeyPage p) {
    (void)p;
    return 3;
}

const char* keyRow(KeyPage p, u32 row) {
    if (row >= 3) return "";
    switch (p) {
        case KeyPage::Latin:    return LATIN[row];
        case KeyPage::Cyrillic: return CYRILLIC[row];
        case KeyPage::Digits:   return DIGITS[row];
        default:                return "";
    }
}

u32 keysInRow(KeyPage p, u32 row) {
    const char* keys = keyRow(p, row);
    usize n = 0;
    while (keys[n] != '\0') ++n;
    u32 count = 0;
    for (usize i = 0; i < n; ) { utf8Next(keys, n, i); ++count; }
    return count;
}

u32 keyAt(KeyPage p, u32 row, u32 col) {
    const char* keys = keyRow(p, row);
    usize n = 0;
    while (keys[n] != '\0') ++n;
    u32 k = 0;
    for (usize i = 0; i < n; ) {
        const u32 cp = utf8Next(keys, n, i);
        if (k == col) return cp;
        ++k;
    }
    return 0;
}

KeyboardLayout keyboardLayout(KeyPage p, Rect area, f32 gap, f32 minKeyH) {
    KeyboardLayout L;
    L.area = area;
    L.page = p;
    L.gap  = gap;
    // Рядов на один больше, чем букв: последний — служебный.
    const u32 rows = keyRowCount(p) + 1;
    f32 h = (area.h - gap * (f32)(rows - 1)) / (f32)rows;
    if (h < minKeyH) h = minKeyH;
    L.keyH = h;
    return L;
}

Rect KeyboardLayout::keyRect(u32 row, u32 col) const {
    const u32 count = keysInRow(page, row);
    if (count == 0 || col >= count) return Rect{0.f, 0.f, 0.f, 0.f};
    const f32 keyW = (area.w - gap * (f32)(count - 1)) / (f32)count;
    return { area.x + (f32)col * (keyW + gap),
             area.y + (f32)row * (keyH + gap), keyW, keyH };
}

Rect KeyboardLayout::pageRect() const {
    const f32 sideW = area.w * 0.22f;
    return { area.x, area.y + (f32)keyRowCount(page) * (keyH + gap),
             sideW, keyH };
}

Rect KeyboardLayout::spaceRect() const {
    const Rect pr = pageRect();
    const f32 spaceW = area.w - (pr.w + gap) * 2.f;
    return { pr.x + pr.w + gap, pr.y, spaceW, keyH };
}

Rect KeyboardLayout::backspaceRect() const {
    const Rect sp = spaceRect();
    return { sp.x + sp.w + gap, sp.y, pageRect().w, keyH };
}

const char* keyPageName(KeyPage p) {
    switch (p) {
        case KeyPage::Latin:    return "ABC";
        case KeyPage::Cyrillic: return "АБВ";
        case KeyPage::Digits:   return "123";
        default:                return "?";
    }
}

} // namespace ui
