#!/usr/bin/env python3
# ============================================================
# Сверка двух кадров минимальной сцены: эталон с хоста и снимок с
# устройства.
#
#   ./tools/vkcheck/compare.py эталон.ppm снимок.jpg [--out разница.png]
#
# Снимок с телефона может быть шире кадра игры (чёрные поля по краям) и
# пережат в JPEG — поэтому сначала ищется полоса кадра, потом
# сравнение идёт по областям, а не по отдельным пикселям: точное
# совпадение байт после пережатия недостижимо и не нужно. Нужно
# другое — чтобы совпадали цвета поверхностей и положение примет.
# ============================================================
import sys
from PIL import Image, ImageFile, ImageChops
ImageFile.LOAD_TRUNCATED_IMAGES = True


def load(path):
    im = Image.open(path).convert('RGB')
    return im


def crop_letterbox(im):
    """Снимает чёрные поля по краям снимка телефона."""
    w, h = im.size
    px = im.load()
    def dark_column(x):
        return all(sum(px[x, y]) < 40 for y in range(0, h, max(1, h // 40)))
    x0 = 0
    while x0 < w // 4 and dark_column(x0):
        x0 += 1
    x1 = w - 1
    while x1 > 3 * w // 4 and dark_column(x1):
        x1 -= 1
    return im.crop((x0, 0, x1 + 1, h)) if (x0 or x1 != w - 1) else im


def region_stats(im, boxes):
    px = im.load()
    out = {}
    for name, (x0, y0, x1, y1) in boxes.items():
        n = 0
        r = g = b = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                c = px[x, y]
                r += c[0]; g += c[1]; b += c[2]; n += 1
        out[name] = (r / n, g / n, b / n) if n else (0, 0, 0)
    return out


# Доли кадра, а не пиксели: кадры могут отличаться разрешением.
# Области выбраны так, чтобы на снимке с устройства не попасть под
# кнопки и полоски интерфейса: он рисуется поверх мира и сравнению
# мешает.
REGIONS = {
    'небо':            (0.05, 0.03, 0.25, 0.10),
    'земля вблизи':    (0.36, 0.60, 0.58, 0.71),
    'земля вдали':     (0.06, 0.31, 0.19, 0.36),
    'лесенка':         (0.45, 0.31, 0.54, 0.44),
    'вода':            (0.380, 0.345, 0.425, 0.372),
    'крона дерева':    (0.615, 0.305, 0.685, 0.365),
    'ствол дерева':    (0.634, 0.415, 0.648, 0.448),
}


def main():
    if len(sys.argv) < 3:
        print(__doc__ or 'нужны два кадра')
        return 2
    a = load(sys.argv[1])
    b = crop_letterbox(load(sys.argv[2]))
    print(f'эталон {a.size}, снимок {b.size} (после снятия полей)')

    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)
        print(f'снимок приведён к {a.size}')

    w, h = a.size
    boxes = {k: (int(x0 * w), int(y0 * h), int(x1 * w), int(y1 * h))
             for k, (x0, y0, x1, y1) in REGIONS.items()}
    sa = region_stats(a, boxes)
    sb = region_stats(b, boxes)

    print(f'{"область":<16} {"эталон":>18} {"устройство":>18} {"расхождение":>12}')
    worst = 0.0
    for k in REGIONS:
        ca, cb = sa[k], sb[k]
        d = max(abs(ca[i] - cb[i]) for i in range(3))
        worst = max(worst, d)
        fa = f'({ca[0]:5.0f},{ca[1]:5.0f},{ca[2]:5.0f})'
        fb = f'({cb[0]:5.0f},{cb[1]:5.0f},{cb[2]:5.0f})'
        print(f'{k:<16} {fa:>18} {fb:>18} {d:>12.0f}')
    print(f'\nхудшее расхождение по каналу: {worst:.0f} из 255')

    out = None
    for i, arg in enumerate(sys.argv):
        if arg == '--out' and i + 1 < len(sys.argv):
            out = sys.argv[i + 1]
    if out:
        diff = ImageChops.difference(a, b)
        diff.point(lambda v: min(255, v * 4)).save(out)
        print(f'карта разницы (усилена вчетверо): {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
