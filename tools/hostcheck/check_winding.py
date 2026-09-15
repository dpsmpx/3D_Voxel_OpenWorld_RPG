#!/usr/bin/env python3
"""Обход треугольников у статичной геометрии кубов.

Куб мобов, NPC, предметов и снарядов задан таблицами MOB_CUBE_V и
MOB_CUBE_I в mob_renderer.cpp. Ошибка в такой таблице не ловится ни
компилятором, ни тестами: половина граней просто просвечивает насквозь,
и заметно это только на устройстве. Здесь разбираем таблицы и требуем,
чтобы нормаль, посчитанная по обходу, смотрела наружу от центра куба.

Таблица одна на все четыре рендера. Раньше их было четыре дословных
копии, и проверка обходила каждую — но копии расходятся молча, а эта
проверка увидела бы только неверный обход, не расхождение. Поэтому
ниже ещё и требование: своих таблиц у остальных рендеров быть не
должно.

Соглашение всего проекта: против часовой стрелки при взгляде снаружи.
Оно же доезжает до координат кадра: p[1][1] *= -1 меняет знак NDC по Y,
а видовое преобразование Vulkan с положительной высотой переносит этот
знак в кадр — второго переворота нет. Поэтому конвейеры объявлены
VK_FRONT_FACE_COUNTER_CLOCKWISE; здесь стояло CLOCKWISE, и отсекались
ровно наружные грани (см. комментарий в vk/vk_pipeline.h).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDER = ROOT / "app/src/main/cpp/src/render"
# Где лежит единственная таблица куба.
CUBE_OWNER = "mob_renderer.cpp"
# Кто ею пользуется и потому обязан НЕ иметь своей.
CUBE_USERS = ["item_renderer.cpp", "npc_renderer.cpp",
              "projectile_renderer.cpp"]

NUM = re.compile(r"-?\d+\.?\d*")


def strip_comments(s: str) -> str:
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def block(text: str, decl: str):
    """Тело инициализатора после `... decl ... = {` до парной `}`."""
    m = re.search(re.escape(decl) + r"[^=]*=\s*\{", text)
    if not m:
        return None
    i = m.end()
    depth = 1
    start = i
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[start:i - 1]


def parse_verts(body: str):
    """Каждая вершина — group из трёх чисел во вложенных скобках."""
    verts = []
    for grp in re.findall(r"\{\{([^{}]*)\}\}", body):
        nums = [float(x) for x in NUM.findall(grp)]
        if len(nums) != 3:
            return None
        verts.append(tuple(nums))
    return verts


def parse_idx(body: str):
    return [int(x) for x in re.findall(r"\d+", body)]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def check(path: Path):
    text = strip_comments(path.read_text(encoding="utf-8"))
    vb = block(text, "MOB_CUBE_V")
    ib = block(text, "MOB_CUBE_I")
    if vb is None or ib is None:
        return [f"{path.name}: не нашёл таблиц MOB_CUBE_V/MOB_CUBE_I"]

    verts = parse_verts(vb)
    idx = parse_idx(ib)
    errs = []
    if not verts:
        return [f"{path.name}: MOB_CUBE_V не разобрался"]
    if len(idx) % 3:
        return [f"{path.name}: в MOB_CUBE_I {len(idx)} индексов, не кратно трём"]

    cx = sum(v[0] for v in verts) / len(verts)
    cy = sum(v[1] for v in verts) / len(verts)
    cz = sum(v[2] for v in verts) / len(verts)

    for t in range(len(idx) // 3):
        i0, i1, i2 = idx[3 * t:3 * t + 3]
        if max(i0, i1, i2) >= len(verts):
            errs.append(f"{path.name}: треугольник {t} ссылается на вершину "
                        f"{max(i0, i1, i2)}, а вершин {len(verts)}")
            continue
        a, b, c = verts[i0], verts[i1], verts[i2]
        e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = cross(e1, e2)
        # Из центра куба к середине треугольника.
        ox = (a[0] + b[0] + c[0]) / 3 - cx
        oy = (a[1] + b[1] + c[1]) / 3 - cy
        oz = (a[2] + b[2] + c[2]) / 3 - cz
        dot = n[0] * ox + n[1] * oy + n[2] * oz
        if abs(n[0]) + abs(n[1]) + abs(n[2]) < 1e-9:
            errs.append(f"{path.name}: треугольник {t} вырожден "
                        f"({i0},{i1},{i2})")
        elif dot <= 0:
            errs.append(f"{path.name}: треугольник {t} ({i0},{i1},{i2}) "
                        f"намотан внутрь — грань исчезнет при отсечении")
    return errs


def check_terrain():
    """Обход граней террейна: таблица WINDING против осей FACES.

    Здесь геометрии нет — есть две таблицы в разных файлах, и смысл
    имеет только их сочетание. Считаем, как это делает мешер, и
    требуем, чтобы нормаль по обходу совпала с направлением грани.
    """
    chunk = ROOT / "app/src/main/cpp/src/world/chunk.cpp"
    mesh = ROOT / "app/src/main/cpp/src/render/mesh_builder.cpp"
    if not chunk.exists() or not mesh.exists():
        return ["не нашёл chunk.cpp или mesh_builder.cpp"]

    fb = block(strip_comments(chunk.read_text(encoding="utf-8")), "FACES")
    wb = block(strip_comments(mesh.read_text(encoding="utf-8")), "WINDING")
    if fb is None or wb is None:
        return ["не нашёл таблиц FACES/WINDING"]

    faces = [[int(x) for x in re.findall(r"-?\d+", g)]
             for g in re.findall(r"\{([^{}]*)\}", fb)]
    wind = [[int(x) for x in re.findall(r"\d+", g)]
            for g in re.findall(r"\{([^{}]*)\}", wb)]
    if len(faces) != 6 or any(len(f) != 4 for f in faces):
        return [f"FACES разобрался как {faces}"]
    if len(wind) != 6 or any(len(w) != 4 for w in wind):
        return [f"WINDING разобрался как {wind}"]

    names = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]
    errs = []
    for f, (au, av, aw, sign) in enumerate(faces):
        org = [0.0, 0.0, 0.0]
        org[aw] = 1.0 if sign > 0 else 0.0
        du = [0.0] * 3; du[au] = 1.0
        dv = [0.0] * 3; dv[av] = 1.0
        corners = [org,
                   [org[i] + du[i] for i in range(3)],
                   [org[i] + du[i] + dv[i] for i in range(3)],
                   [org[i] + dv[i] for i in range(3)]]
        o = wind[f]
        for tri in ((o[0], o[1], o[2]), (o[0], o[2], o[3])):
            a, b, c = (corners[i] for i in tri)
            e1 = [b[i] - a[i] for i in range(3)]
            e2 = [c[i] - a[i] for i in range(3)]
            n = cross(e1, e2)
            want = [0.0] * 3; want[aw] = float(sign)
            if sum(n[i] * want[i] for i in range(3)) <= 0:
                errs.append(f"террейн, грань {names[f]}: обход {tri} "
                            f"смотрит внутрь")
    return errs


def check_no_copies():
    """Никто, кроме владельца, не заводит своей таблицы куба.

    Это не стилистика. Четыре копии одной таблицы — ровно тот случай,
    когда правку получает одна из них: так и вышло с форматом
    инстанса, где три рендера остались с прежними смещениями и стали
    читать чужие байты.
    """
    errs = []
    for name in CUBE_USERS:
        p = RENDER / name
        if not p.exists():
            errs.append(f"{name}: файла нет")
            continue
        text = strip_comments(p.read_text(encoding="utf-8"))
        for decl in ("CUBE_V[24]", "CUBE_I[36]"):
            if decl in text:
                errs.append(f"{name}: завёл свою таблицу {decl} — "
                            f"куб один, он в {CUBE_OWNER}")
    return errs


def main() -> int:
    all_errs = []
    checked = 0

    p = RENDER / CUBE_OWNER
    if not p.exists():
        all_errs.append(f"{CUBE_OWNER}: файла нет")
    else:
        checked += 1
        all_errs += check(p)

    all_errs += check_no_copies()
    checked += 1

    all_errs += check_terrain()
    checked += 1

    if all_errs:
        print("✗ Обход граней задан неверно:")
        for e in all_errs:
            print("    " + e)
        return 1
    print(f"✓ Обход граней: проверено таблиц {checked}, "
          f"все треугольники смотрят наружу")
    return 0


if __name__ == "__main__":
    sys.exit(main())
