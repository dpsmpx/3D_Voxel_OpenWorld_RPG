/**
 * @file save_format.h
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#pragma once
#include "../core/types.h"
#include <vector>
#include <string>
#include <cstring>

namespace save {

// ============================================================
// Формат файла:
//   [Header]  — SAVE_HEADER_SIZE байт, фиксированно
//     magic u32, version u32, profile u32, slot u32,
//     seed u64, timestamp u64, playtime u32,
//     originalSize u32, compressedSize u32, checksum u32
//   [Body]    — zlib-сжатый блок сериализованных данных
//
// Все многобайтные числа пишутся Little-Endian.
// ============================================================

constexpr u32 SAVE_MAGIC   = 0x47525856;   // "VXRG"

/// Размер заголовка. Считается из списка полей, а не записан числом.
///
/// Здесь стояло 44 — и в комментарии, и в загрузчике, — тогда как
/// поля дают 48. Писатель складывал поля и писал 48 байт, читатель
/// верил комментарию и читал 44: контрольная сумма, лежащая в
/// последних четырёх байтах, до него не доезжала и оставалась нулём,
/// а тело он начинал читать на четыре байта раньше начала. Любая
/// загрузка кончалась «CRC mismatch (expected=0)».
///
/// Считать из полей, а не писать числом, — единственный способ,
/// которым эти два места не могут разойтись снова.
constexpr u32 SAVE_HEADER_SIZE =
    sizeof(u32)   // magic
  + sizeof(u32)   // version
  + sizeof(u32)   // profile
  + sizeof(u32)   // slot
  + sizeof(u64)   // seed
  + sizeof(u64)   // timestamp
  + sizeof(u32)   // playtime
  + sizeof(u32)   // originalSize
  + sizeof(u32)   // compressedSize
  + sizeof(u32);  // checksum
static_assert(SAVE_HEADER_SIZE == 48, "изменили состав заголовка — поднимите SAVE_VERSION");
/// Версия формата сейва.
///
/// История:
///   1 — первоначальный формат
///   2 — добавлено время суток в начало тела; слотов инвентаря
///       стало 42 вместо 41 (второй слот аксессуара)
///   3 — список убитых NPC пишется постоянными ключами по 8 байт
///       (был u32 из другого набора полей) и наконец применяется
///       при загрузке
///   4 — точка возвращения игрока (тронутый колодец)
///   5 — список вскрытых тайников. Без него клад наполнялся бы
///       заново при каждой загрузке: докопался, вышел, загрузился —
///       и копай то же место снова.
///   6 — глава сюжетной цепочки. Без неё загрузившийся начинал бы
///       цепочку заново, и первая глава вела бы его в деревню, из
///       которой он вышел десять глав назад.
///   7 — состав файла не менялся, а мир — менялся. В генерацию
///       пришли хребты: у того же зерна теперь другой рельеф. Сейв
///       хранит ИЗМЕНЁННЫЕ блоки, а не весь мир, и старый файл лёг
///       бы поверх новой земли: дом повис бы в воздухе, погреб
///       оказался бы в скале. Честный отказ лучше такой загрузки,
///       поэтому версия поднята, хотя формат прежний.
///
/// Загрузчик отвергает файлы другой версии: лучше честно сказать
/// «не поддерживается», чем прочитать данные со сдвигом.
constexpr u32 SAVE_VERSION = 7;

/// ByteWriter — аккумулирует байты, пишет всё LE.
/// Имена методов с префиксом write*, чтобы не затенять
/// псевдонимы типов u8/u32/f32 из core/types.h.
/// Varint используется для экономии на больших массивах
/// (block mods, координаты).
class ByteWriter {
public:
    void writeU8(u8 v)   { buf_.push_back(v); }
    void writeU16(u16 v) {
        buf_.push_back((u8)v);
        buf_.push_back((u8)(v >> 8));
    }
    void writeU32(u32 v) {
        buf_.push_back((u8)v);
        buf_.push_back((u8)(v >> 8));
        buf_.push_back((u8)(v >> 16));
        buf_.push_back((u8)(v >> 24));
    }
    void writeU64(u64 v) { writeU32((u32)v); writeU32((u32)(v >> 32)); }
    void writeI32(i32 v) { writeU32((u32)v); }
    void writeI64(i64 v) { writeU64((u64)v); }

    void writeF32(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        writeU32(bits);
    }
    void writeF64(f64 v) {
        u64 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        writeU64(bits);
    }

    /// Varint: 7-bit groups, LSB-first, high bit = continue.
    void varU32(u32 v) {
        while (v >= 0x80) {
            buf_.push_back((u8)((v & 0x7F) | 0x80));
            v >>= 7;
        }
        buf_.push_back((u8)v);
    }
    void varU64(u64 v) {
        while (v >= 0x80) {
            buf_.push_back((u8)((v & 0x7F) | 0x80));
            v >>= 7;
        }
        buf_.push_back((u8)v);
    }
    void varI32(i32 v) {
        u32 zz = ((u32)v << 1) ^ (u32)(v >> 31);
        varU32(zz);
    }

    void str(const std::string& s) {
        varU32((u32)s.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
    }
    void cstr(const char* s) {
        if (!s) { varU32(0); return; }
        usize n = std::strlen(s);
        varU32((u32)n);
        buf_.insert(buf_.end(), s, s + n);
    }
    void raw(const void* p, usize n) {
        const u8* b = (const u8*)p;
        buf_.insert(buf_.end(), b, b + n);
    }

    std::vector<u8>&       data()       { return buf_; }
    const std::vector<u8>& data() const { return buf_; }
    usize size()    const { return buf_.size(); }
    bool  empty()   const { return buf_.empty(); }
    void  clear()         { buf_.clear(); }
    void  reserve(usize n){ buf_.reserve(n); }

private:
    std::vector<u8> buf_;
};

/// ByteReader — читает из буфера с курсором.
/// Любая неудачная операция выставляет error_ и блокирует
/// дальнейшие чтения (проверяйте ok()).
class ByteReader {
public:
    ByteReader(const u8* data, usize size)
        : data_(data), size_(size) {}

    explicit ByteReader(const std::vector<u8>& buf)
        : data_(buf.data()), size_(buf.size()) {}

    bool  ok() const        { return !error_; }
    usize pos() const       { return pos_; }
    usize remaining() const { return size_ - pos_; }

    bool u8v(u8& out) {
        if (pos_ + 1 > size_) { error_ = true; return false; }
        out = data_[pos_++];
        return true;
    }
    bool u16v(u16& out) {
        if (pos_ + 2 > size_) { error_ = true; return false; }
        out = (u16)data_[pos_] | ((u16)data_[pos_ + 1] << 8);
        pos_ += 2;
        return true;
    }
    bool u32v(u32& out) {
        if (pos_ + 4 > size_) { error_ = true; return false; }
        out = (u32)data_[pos_]
            | ((u32)data_[pos_ + 1] << 8)
            | ((u32)data_[pos_ + 2] << 16)
            | ((u32)data_[pos_ + 3] << 24);
        pos_ += 4;
        return true;
    }
    bool u64v(u64& out) {
        u32 lo = 0, hi = 0;
        if (!u32v(lo)) return false;
        if (!u32v(hi)) return false;
        out = (u64)lo | ((u64)hi << 32);
        return true;
    }
    bool i32v(i32& out) {
        u32 v;
        if (!u32v(v)) return false;
        out = (i32)v;
        return true;
    }
    bool i64v(i64& out) {
        u64 v;
        if (!u64v(v)) return false;
        out = (i64)v;
        return true;
    }
    bool f32v(f32& out) {
        u32 bits;
        if (!u32v(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
    bool f64v(f64& out) {
        u64 bits;
        if (!u64v(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool varU32v(u32& out) {
        out = 0;
        int shift = 0;
        for (int i = 0; i < 5; ++i) {
            if (pos_ >= size_) { error_ = true; return false; }
            u8 b = data_[pos_++];
            out |= (u32)(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        error_ = true;
        return false;
    }
    bool varU64v(u64& out) {
        out = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            if (pos_ >= size_) { error_ = true; return false; }
            u8 b = data_[pos_++];
            out |= (u64)(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        error_ = true;
        return false;
    }
    bool varI32v(i32& out) {
        u32 zz;
        if (!varU32v(zz)) return false;
        out = (i32)((zz >> 1) ^ (~(zz & 1) + 1));
        return true;
    }

    bool strv(std::string& out) {
        u32 n = 0;
        if (!varU32v(n)) return false;
        if (pos_ + n > size_) { error_ = true; return false; }
        out.assign((const char*)(data_ + pos_), n);
        pos_ += n;
        return true;
    }
    bool rawv(void* dst, usize n) {
        if (pos_ + n > size_) { error_ = true; return false; }
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    bool skip(usize n) {
        if (pos_ + n > size_) { error_ = true; return false; }
        pos_ += n;
        return true;
    }

private:
    const u8* data_  = nullptr;
    usize     size_  = 0;
    usize     pos_   = 0;
    bool      error_ = false;
};

} // namespace save
