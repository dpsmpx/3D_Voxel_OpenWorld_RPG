#!/usr/bin/env python3
"""
Сверяет таблицы строк с перечислением StrKey.

Таблиц две — английская и русская, — обе по сотне с лишним строк, обе
ведутся руками, и порядок в них обязан совпадать с порядком ключей в
enum. Единственная защита, которая была, — static_assert на РАЗМЕР:
он ловит забытую строку, но не ловит строку, вставленную не туда.

А вставленная не туда строка сдвигает всё, что идёт следом: у кнопок
и заголовков оказываются чужие подписи, причём в одном языке, а в
другом нет. Компилятору это безразлично, тесты логики этого не видят,
и замечает такое только человек, открывший нужный экран на нужном
языке.

Помогают комментарии /* Ключ */ перед каждой строкой: они уже есть в
обеих таблицах. Здесь мы проверяем, что они идут ровно в порядке enum.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HDR = ROOT / "app/src/main/cpp/src/config/localization.h"
SRC = ROOT / "app/src/main/cpp/src/config/localization.cpp"


def enum_keys(text):
    m = re.search(r"enum class StrKey\s*(?::\s*\w+)?\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return None
    body = re.sub(r"//[^\n]*", "", m.group(1))
    keys = [re.sub(r"=.*", "", k).strip() for k in body.split(",")]
    return [k for k in keys if k and k != "Count"]


def table_labels(text, name):
    m = re.search(re.escape(name) + r"\[STR_KEY_COUNT\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return None
    return re.findall(r"/\*\s*([A-Za-z_0-9]+)\s*\*/", m.group(1))


def main():
    hdr, src = HDR.read_text(encoding="utf-8"), SRC.read_text(encoding="utf-8")

    keys = enum_keys(hdr)
    if keys is None:
        print("✗ не найдено перечисление StrKey")
        return 1

    failed = False
    for name in ("EN", "RU"):
        labels = table_labels(src, name)
        if labels is None:
            print(f"✗ не найдена таблица {name}[STR_KEY_COUNT]")
            failed = True
            continue

        if len(labels) != len(keys):
            print(f"✗ {name}: помечено строк {len(labels)}, ключей {len(keys)}")
            print("    у каждой строки должен быть комментарий /* Ключ */")
            failed = True
            continue

        for i, (want, got) in enumerate(zip(keys, labels)):
            if want != got:
                print(f"✗ {name}: на позиции {i} ожидался {want}, а стоит {got}")
                print("    строка вставлена не на своё место — всё, что ниже,")
                print("    получит чужие подписи")
                failed = True
                break
        else:
            print(f"  ok   {name}: {len(labels)} строк в порядке enum")

    if failed:
        return 1
    print(f"  таблиц сверено: 2, ключей: {len(keys)}, расхождений нет")
    return 0


if __name__ == "__main__":
    sys.exit(main())
