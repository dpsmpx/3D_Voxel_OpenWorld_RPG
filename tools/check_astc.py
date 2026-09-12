#!/usr/bin/env python3
"""Проверяет, что .astc-атлас пригоден к загрузке игрой.

Битый или неожиданный файл игра молча отвергает и строит атлас
процедурно — снаружи это выглядит как «ASTC не заработал», без причины.
Разбор здесь тот же, что в render/astc.cpp.
"""
import sys

MAGIC = '13aba15c'


def main(path):
    with open(path, 'rb') as f:
        d = f.read()

    if len(d) < 16:
        print('  ✗ файл короче заголовка: %d байт' % len(d))
        return 1
    if d[:4].hex() != MAGIC:
        print('  ✗ чужая сигнатура: %s, ожидалась %s' % (d[:4].hex(), MAGIC))
        return 1

    bx, by, bz = d[4], d[5], d[6]
    if (bx, by, bz) != (4, 4, 1):
        print('  ✗ блок %dx%dx%d, игра ждёт 4x4x1' % (bx, by, bz))
        return 1

    w = int.from_bytes(d[7:10], 'little')
    h = int.from_bytes(d[10:13], 'little')
    z = int.from_bytes(d[13:16], 'little')
    if z != 1:
        print('  ✗ объёмная текстура (глубина %d) не поддерживается' % z)
        return 1
    if w == 0 or h == 0:
        print('  ✗ нулевой размер: %dx%d' % (w, h))
        return 1

    blocks = ((w + bx - 1) // bx) * ((h + by - 1) // by)
    expect = blocks * 16
    actual = len(d) - 16
    if actual != expect:
        print('  ✗ данных %d байт, по размеру должно быть %d' % (actual, expect))
        return 1

    print('  ✓ ASTC %dx%d, блок %dx%d, блоков %d, %d КБ'
          % (w, h, bx, by, blocks, len(d) // 1024))
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('использование: check_astc.py <файл.astc>')
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
