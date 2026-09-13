#!/usr/bin/env python3
"""
Ищет функции, объявленные в заголовках проекта и не вызванные ниоткуда.

Зачем. Три раза подряд находилось одно и то же: система написана
целиком, работает правильно и никем не вызывается. Ассортимент
торговца, который никому не выдавался. Уведомления о прогрессе
квестов, из-за чего семь квестов из десяти нельзя было выполнить.
Десяток звуковых событий, из-за чего бой шёл молча. Каждый раз это
находилось случайно — при чтении соседнего кода.

Такую дыру не видит ни компилятор (функция определена и слинкована),
ни тесты (их на неё не писали, раз её никто не звал), ни глаз
(в каждом отдельном файле всё выглядит правильно).

Это не сторож сборки, а инструмент для разбора: часть находок —
законная поверхность API, часть вызывается по адресу, а не по имени.
Отсюда и отдельный запуск, а не место в run.sh: список надо читать
глазами, а не пропускать мимо.

    ./tools/hostcheck/find_unused.py
"""
import re
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "app/src/main/cpp/src"

# Объявление функции в заголовке: возвращаемый тип, имя, скобки, ';'
DECL = re.compile(
    r"^\s*(?!#)(?:[A-Za-z_][\w:<>,\s\*&]*?\s[\*&]?\s*)"
    r"([a-z][A-Za-z0-9_]*)\s*\([^;{]*\)\s*(?:const\s*)?;\s*$",
    re.M,
)
NOT_FUNCTIONS = {"if", "for", "while", "switch", "return", "sizeof", "operator", "else"}


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def main():
    files = sorted(SRC.rglob("*.h")) + sorted(SRC.rglob("*.cpp"))
    texts = {p: strip_comments(p.read_text(encoding="utf-8", errors="ignore")) for p in files}

    declared = {}
    for p in files:
        if p.suffix != ".h":
            continue
        for m in DECL.finditer(texts[p]):
            name = m.group(1)
            if name not in NOT_FUNCTIONS:
                declared.setdefault(name, p)

    unused = []
    for name, hdr in declared.items():
        pattern = re.compile(r"(?<![\w])(?:\w+::)?" + re.escape(name) + r"\s*\(")
        # объявление в заголовке и определение в .cpp вызовами не считаются
        as_decl = re.compile(r"^[A-Za-z_][\w:<>,\s\*&]*\s[\*&]?" + re.escape(name) + r"\s*\(")
        as_def = re.compile(r"^[A-Za-z_][\w:<>,\s\*&]*\s[\*&]?(?:\w+::)+" + re.escape(name) + r"\s*\(")

        calls = 0
        for text in texts.values():
            for line in text.split("\n"):
                if not pattern.search(line):
                    continue
                stripped = line.strip()
                if stripped.endswith(";") and as_decl.match(stripped):
                    continue
                if as_def.match(stripped):
                    continue
                calls += 1
        if calls == 0:
            unused.append((name, str(hdr.relative_to(ROOT))))

    unused.sort()
    print(f"объявлений в заголовках: {len(declared)}")
    print(f"ни одного вызова: {len(unused)}\n")
    for name, hdr in unused:
        print(f"  {name:<32} {hdr}")
    print(
        "\nЧитать глазами. Функция, взятая по адресу (&Класс::метод), сюда\n"
        "попадает тоже — как и то, что законно составляет поверхность API."
    )
    return 0


if __name__ == "__main__":
    # Вывод длинный, его естественно смотреть через head или less.
    # Без этого закрытая труба роняет скрипт трассировкой.
    import signal
    try:
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
