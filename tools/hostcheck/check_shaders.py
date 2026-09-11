#!/usr/bin/env python3
"""
Сверка вершинных форматов: что объявлено в C++ (vk::VertexAttr)
против того, что реально требует шейдер (layout(location=...) in).

Раньше layout был зашит в GraphicsPipeline одним вариантом и
совпадал только с воксельным шейдером — UI, мобы, трава и контур
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
    ('src/render/render_system.cpp',      'voxel.vert'),
    ('src/ui/ui_renderer.cpp',            'ui.vert'),
    ('src/render/block_outline.cpp',      'outline.vert'),
    ('src/render/instanced_renderer.cpp', 'grass.vert'),
    ('src/render/mob_renderer.cpp',       'mob.vert'),
    ('src/render/npc_renderer.cpp',       'mob.vert'),
    ('src/render/projectile_renderer.cpp','projectile.vert'),
    ('src/render/item_renderer.cpp',      'projectile.vert'),
]

# Сколько компонентов несёт формат Vulkan.
FORMAT_COMPONENTS = {
    'VK_FORMAT_R32_SFLOAT': 1,
    'VK_FORMAT_R32G32_SFLOAT': 2,
    'VK_FORMAT_R32G32B32_SFLOAT': 3,
    'VK_FORMAT_R32G32B32A32_SFLOAT': 4,
    'VK_FORMAT_R8G8B8A8_UNORM': 4,
}
GLSL_COMPONENTS = {'float': 1, 'vec2': 2, 'vec3': 3, 'vec4': 4}

ATTR_RE = re.compile(
    r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(VK_FORMAT_\w+)\s*,\s*(\d+)\s*\}')
IN_RE = re.compile(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(\w+)\s+(\w+)')


def cpp_attrs(path):
    """Локации и форматы из таблицы kAttrs/kVoxelAttrs."""
    text = open(os.path.join(CPP, path), encoding='utf-8').read()
    m = re.search(r'static const vk::VertexAttr k\w*Attrs\[\d+\]\s*=\s*\{(.*?)\n\};',
                  text, re.S)
    if not m:
        return None
    return {int(loc): fmt for loc, _b, fmt, _o in ATTR_RE.findall(m.group(1))}


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
