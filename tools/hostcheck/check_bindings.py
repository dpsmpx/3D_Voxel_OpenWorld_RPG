#!/usr/bin/env python3
"""Каждый привязанный конвейер обязан привязать и свой набор дескрипторов.

Vulkan сохраняет привязку наборов между вызовами только при
совместимых layout'ах, а совместимость ломает любое расхождение — в
том числе push-константа, которой у одного конвейера есть, а у другого
нет. Молчаливая расплата за это — шейдер, читающий чужие данные:
небо строилось по матрицам, оставшимся от интерфейса прошлого кадра,
и заметить это на глаз почти невозможно.

Проверка лексическая: между привязкой конвейера и следующей такой же
(или концом функции) обязан найтись vkCmdBindDescriptorSets.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIRS = ["app/src/main/cpp/src/render", "app/src/main/cpp/src/ui"]

BIND_PIPE = re.compile(r"vkCmdBindPipeline\s*\(")
BIND_SETS = re.compile(r"vkCmdBindDescriptorSets\s*\(")


def main() -> int:
    errs = []
    checked = 0
    for d in DIRS:
        for path in sorted((ROOT / d).glob("*.cpp")):
            lines = path.read_text(encoding="utf-8").splitlines()
            for i, line in enumerate(lines):
                if not BIND_PIPE.search(line):
                    continue
                checked += 1
                # До следующей привязки конвейера или до конца функции
                # (закрывающая скобка в первой колонке).
                tail = []
                for t in lines[i + 1:]:
                    if BIND_PIPE.search(t) or t.startswith("}"):
                        break
                    tail.append(t)
                if not any(BIND_SETS.search(t) for t in tail):
                    errs.append(f"{path.name}:{i + 1}: конвейер привязан, "
                                f"а набор дескрипторов — нет")
    if errs:
        print("✗ Конвейер без своих дескрипторов:")
        for e in errs:
            print("    " + e)
        return 1
    print(f"✓ Дескрипторы: проверено привязок конвейера {checked}, "
          f"все привязывают свой набор")
    return 0


if __name__ == "__main__":
    sys.exit(main())
