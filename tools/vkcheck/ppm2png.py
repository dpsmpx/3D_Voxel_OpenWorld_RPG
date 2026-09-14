#!/usr/bin/env python3
"""PPM (P6) -> PNG без внешних пакетов.

Кадр vkcheck пишется в .ppm, потому что так его проще записать из C++.
Смотреть его человеку удобнее в .png, а тащить ради этого Pillow
оказалось дорого: бинарный модуль PIL.Image в системе может быть
собран под другую версию python, и тогда падал весь прогон проверки
графики — при том что кадр отрисован и проверен.
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    fields, i = [], 0
    while len(fields) < 4:                    # магия, ширина, высота, максимум
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":             # комментарий до конца строки
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    if fields[0] != b"P6":
        raise ValueError(f"ожидался P6, получен {fields[0]!r}")
    i += 1                                    # ровно один пробельный символ
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i:i + w * h * 3]


def write_png(path, w, h, rgb):
    # Каждой строке PNG предшествует байт фильтра; ноль — «без фильтра».
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        c = tag + body
        return (struct.pack(">I", len(body)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    if len(sys.argv) != 3:
        print("использование: ppm2png.py вход.ppm выход.png", file=sys.stderr)
        return 2
    w, h, rgb = read_ppm(sys.argv[1])
    if len(rgb) != w * h * 3:
        print(f"ppm2png: тело короче заголовка ({len(rgb)} из {w * h * 3})",
              file=sys.stderr)
        return 1
    write_png(sys.argv[2], w, h, rgb)
    print(f"vkcheck: {sys.argv[2]} ({w}x{h})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
