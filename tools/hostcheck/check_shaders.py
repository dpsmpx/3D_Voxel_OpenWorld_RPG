#!/usr/bin/env python3
"""
Сверка вершинных форматов: что объявлено в C++ (vk::VertexAttr)
против того, что реально требует шейдер (layout(location=...) in).

Раньше layout был зашит в GraphicsPipeline одним вариантом и
совпадал только с воксельным шейдером — UI, мобы и контур
рисовались бы мусором. Компилятор такое не ловит, тесты логики
тоже: ошибка видна только на устройстве. Поэтому проверка здесь.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CPP = os.path.join(ROOT, 'app', 'src', 'main', 'cpp')
SHADERS = os.path.join(CPP, 'shaders')

# Какой .cpp объявляет формат для какого вершинного шейдера.
PAIRS = [
    ('src/render/voxel_pipeline.cpp',      'voxel.vert'),
    ('src/ui/ui_renderer.cpp',            'ui.vert'),
    ('src/render/block_outline.cpp',      'outline.vert'),
    ('src/render/mob_renderer.cpp',       'mob.vert'),
    ('src/render/npc_renderer.cpp',       'mob.vert'),
    ('src/render/projectile_renderer.cpp','projectile.vert'),
    ('src/render/item_renderer.cpp',      'mob.vert'),
    ('src/render/voxel_model_renderer.cpp', 'voxmodel.vert'),
    ('src/render/flora_renderer.cpp',     'flora.vert'),
]

# Сколько компонентов несёт формат Vulkan.
FORMAT_COMPONENTS = {
    'VK_FORMAT_R32_SFLOAT': 1,
    'VK_FORMAT_R32G32_SFLOAT': 2,
    'VK_FORMAT_R32G32B32_SFLOAT': 3,
    'VK_FORMAT_R32G32B32A32_SFLOAT': 4,
    'VK_FORMAT_R8G8B8A8_UNORM': 4,
    'VK_FORMAT_R32_UINT': 1,
}
GLSL_COMPONENTS = {'float': 1, 'vec2': 2, 'vec3': 3, 'vec4': 4,
                   'uint': 1, 'uvec2': 2, 'uvec3': 3, 'uvec4': 4,
                   'int': 1, 'ivec2': 2, 'ivec3': 3, 'ivec4': 4}
# Целочисленному входу шейдера нужен целочисленный формат, и наоборот.
# Несовпадение здесь Vulkan не прощает: атрибут читается как мусор.
INT_FORMATS = {'VK_FORMAT_R32_UINT', 'VK_FORMAT_R32G32_UINT',
               'VK_FORMAT_R32G32B32_UINT', 'VK_FORMAT_R32G32B32A32_UINT',
               'VK_FORMAT_R32_SINT'}
INT_GLSL = {'uint', 'uvec2', 'uvec3', 'uvec4', 'int', 'ivec2', 'ivec3', 'ivec4'}

# Смещение вправе быть выражением: offsetof надёжнее числа,
# набранного руками, — именно ручные числа и разъезжались.
ATTR_RE = re.compile(
    r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(VK_FORMAT_\w+)\s*,\s*([^,}]+?)\s*\}')
IN_RE = re.compile(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(\w+)\s+(\w+)')


TABLE_RE = re.compile(
    r'static const vk::VertexAttr \w+\[\d+\]\s*=\s*\{(.*?)\n\};', re.S)
INCLUDE_RE = re.compile(r'#include\s+"([^"]+)"')


def _table(text):
    m = TABLE_RE.search(text)
    if not m:
        return None
    return {int(loc): fmt for loc, _b, fmt, _o in ATTR_RE.findall(m.group(1))}


def cpp_attrs(path):
    """Локации и форматы из таблицы VertexAttr.

    Таблица вправе лежать не в самом .cpp, а в его заголовке: четыре
    рендера MobInstance делят один формат, и дословные копии в каждом
    как раз и были ошибкой — правку получал один файл из четырёх.
    Поэтому если в .cpp таблицы нет, ищем её в заголовках, которые он
    включает (на один уровень — глубже прятать формат незачем).
    """
    full = os.path.join(CPP, path)
    text = open(full, encoding='utf-8').read()
    attrs = _table(text)
    if attrs is not None:
        return attrs

    # Заголовок с форматом вправе прийти не напрямую: npc_renderer.cpp
    # включает npc_renderer.h, а тот уже mob_renderer.h. Идём по цепочке
    # включений вширь, каждый файл разбирая один раз.
    seen = {os.path.normpath(full)}
    queue = [(full, text)]
    while queue:
        cur, src = queue.pop(0)
        base = os.path.dirname(cur)
        for inc in INCLUDE_RE.findall(src):
            hdr = os.path.normpath(os.path.join(base, inc))
            if hdr in seen or not hdr.startswith(CPP) or not os.path.exists(hdr):
                continue
            seen.add(hdr)
            htext = open(hdr, encoding='utf-8').read()
            attrs = _table(htext)
            if attrs is not None:
                return attrs
            queue.append((hdr, htext))
    return None


def shader_inputs(name):
    text = open(os.path.join(SHADERS, name), encoding='utf-8').read()
    return {int(loc): typ for loc, typ, _n in IN_RE.findall(text)}


def main():
    failed = 0
    checked = 0
    for cpp, vert in PAIRS:
        attrs = cpp_attrs(cpp)
        if attrs is None:
            print('  FAIL %-34s таблица VertexAttr не найдена' % cpp)
            failed += 1
            continue
        ins = shader_inputs(vert)

        missing = sorted(set(ins) - set(attrs))
        extra   = sorted(set(attrs) - set(ins))
        if missing:
            print('  FAIL %-22s -> %-16s шейдер ждёт location %s, а pipeline их не даёт'
                  % (os.path.basename(cpp), vert, missing))
            failed += 1
            continue
        if extra:
            print('  ПРЕД %-22s -> %-16s pipeline даёт лишние location %s'
                  % (os.path.basename(cpp), vert, extra))

        bad = []
        for loc, typ in sorted(ins.items()):
            want = GLSL_COMPONENTS.get(typ)
            got = FORMAT_COMPONENTS.get(attrs[loc])
            if want is None or got is None:
                continue
            if (typ in INT_GLSL) != (attrs[loc] in INT_FORMATS):
                bad.append('location %d: шейдер %s, pipeline %s — '
                           'целое против вещественного'
                           % (loc, typ, attrs[loc]))
                continue
            # R8G8B8A8_UNORM подаётся в vec4 — это нормально.
            if want != got:
                bad.append('location %d: шейдер %s(%d), pipeline %s(%d)'
                           % (loc, typ, want, attrs[loc], got))
        if bad:
            print('  FAIL %-22s -> %-16s' % (os.path.basename(cpp), vert))
            for b in bad:
                print('       ' + b)
            failed += 1
            continue

        print('  ok   %-22s -> %-16s %d атрибутов'
              % (os.path.basename(cpp), vert, len(ins)))
        checked += 1

    print('\n  вершинных форматов сверено: %d, расхождений: %d' % (checked, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
