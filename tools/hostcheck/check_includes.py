#!/usr/bin/env python3
"""Проверяет, что файл сам подключает заголовки, которыми пользуется.

Стандартные библиотеки тянут заголовки друг через друга по-разному, и
набор транзитивных включений меняется между версиями. Из-за этого
`std::clamp` без `#include <algorithm>` спокойно собирался на одной
машине и валил сборку на другой — поймать это компиляцией нельзя, если
под рукой нет той самой версии библиотеки.

Проверка лексическая: находит обращения к std:: и требует, чтобы в
этом же файле был подключён соответствующий заголовок. Транзитивные
включения намеренно не засчитываются — на них и нельзя полагаться.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_android_api import strip_noise          # noqa: E402

PROJ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(PROJ, 'app', 'src', 'main', 'cpp', 'src')

# Символ -> заголовки, любой из которых подходит.
NEEDS = {}


def need(header, *names, also=()):
    for n in names:
        NEEDS[n] = (header,) + tuple(also)


need('<algorithm>', 'min', 'max', 'clamp', 'sort', 'stable_sort', 'find',
     'find_if', 'fill', 'count', 'count_if', 'lower_bound', 'upper_bound',
     'reverse', 'transform', 'minmax', 'remove_if', 'any_of', 'all_of',
     'none_of', 'iter_swap', 'max_element', 'min_element', 'rotate',
     'unique', 'copy', 'shuffle', 'nth_element', 'partition')
need('<numeric>', 'accumulate', 'iota', 'reduce')
need('<vector>', 'vector')
need('<array>', 'array')
need('<string>', 'string', 'to_string', 'stoi', 'stof', 'stod', 'getline')
need('<string_view>', 'string_view')
need('<unordered_map>', 'unordered_map')
need('<unordered_set>', 'unordered_set')
need('<map>', 'map', 'multimap')
need('<set>', 'set', 'multiset')
need('<deque>', 'deque')
need('<queue>', 'queue', 'priority_queue')
need('<memory>', 'shared_ptr', 'unique_ptr', 'weak_ptr', 'make_unique',
     'make_shared', 'addressof', 'enable_shared_from_this', 'default_delete')
need('<memory_resource>', 'pmr')
need('<functional>', 'function', 'bind', 'ref', 'cref', 'invoke', 'hash')
need('<atomic>', 'atomic', 'atomic_flag', 'memory_order', 'memory_order_acquire',
     'memory_order_release', 'memory_order_relaxed', 'memory_order_acq_rel',
     'memory_order_seq_cst', 'atomic_thread_fence')
need('<mutex>', 'mutex', 'recursive_mutex', 'lock_guard', 'unique_lock',
     'scoped_lock', 'once_flag', 'call_once', 'lock')
need('<shared_mutex>', 'shared_mutex', 'shared_lock')
need('<condition_variable>', 'condition_variable', 'condition_variable_any')
need('<thread>', 'thread', 'this_thread', 'jthread')
need('<chrono>', 'chrono')
need('<random>', 'mt19937', 'mt19937_64', 'random_device', 'minstd_rand',
     'uniform_int_distribution', 'uniform_real_distribution',
     'normal_distribution')
need('<optional>', 'optional', 'nullopt', 'nullopt_t')
need('<tuple>', 'tuple', 'make_tuple', 'tie', 'apply')
need('<utility>', 'move', 'forward', 'pair', 'make_pair', 'exchange', 'swap',
     also=('<algorithm>',))
need('<limits>', 'numeric_limits')
need('<type_traits>', 'is_same', 'is_same_v', 'enable_if', 'enable_if_t',
     'decay', 'decay_t', 'remove_reference', 'remove_reference_t',
     'conditional', 'conditional_t', 'underlying_type', 'underlying_type_t',
     'is_trivially_copyable', 'is_trivially_copyable_v', 'declval')
need('<bit>', 'bit_cast', 'popcount', 'countr_zero', 'countl_zero',
     'has_single_bit', 'bit_width')
need('<new>', 'nothrow', 'bad_alloc', 'align_val_t', 'launder')
need('<initializer_list>', 'initializer_list')
need('<cmath>', 'floor', 'ceil', 'round', 'trunc', 'sqrt', 'cbrt', 'pow',
     'exp', 'exp2', 'log', 'log2', 'log10', 'sin', 'cos', 'tan', 'asin',
     'acos', 'atan', 'atan2', 'sinh', 'cosh', 'tanh', 'fabs', 'fmod',
     'hypot', 'isnan', 'isinf', 'isfinite', 'copysign', 'lround', 'nearbyint')
need('<cmath>', 'abs', also=('<cstdlib>',))
need('<cstdio>', 'printf', 'fprintf', 'snprintf', 'vsnprintf', 'sprintf',
     'fopen', 'fclose', 'fread', 'fwrite', 'fseek', 'ftell', 'fflush',
     'fputs', 'fgets', 'FILE', 'perror', 'rename')
need('<cstdio>', 'remove', also=('<algorithm>',))
need('<cstring>', 'memcpy', 'memset', 'memmove', 'memcmp', 'strcmp',
     'strncmp', 'strlen', 'strncpy', 'strchr', 'strstr', 'strcpy')
need('<cstdlib>', 'malloc', 'calloc', 'realloc', 'free', 'abort', 'exit',
     'strtol', 'strtoul', 'strtof', 'strtod', 'qsort', 'rand', 'srand',
     'getenv', 'atoi')
need('<cstdint>', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'int8_t',
     'int16_t', 'int32_t', 'int64_t', 'uintptr_t', 'intptr_t', 'size_t',
     also=('<cstddef>',))
need('<cstddef>', 'ptrdiff_t', 'byte', 'nullptr_t', 'max_align_t')

SYMBOL = None


def collect(path):
    with open(path, encoding='utf-8') as f:
        raw = f.read()
    code = strip_noise(raw)

    included = set()
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith('#include') and '<' in line:
            included.add('<' + line.split('<', 1)[1].split('>', 1)[0] + '>')

    used = {}
    i = 0
    while True:
        i = code.find('std::', i)
        if i < 0:
            break
        j = i + 5
        k = j
        while k < len(code) and (code[k].isalnum() or code[k] == '_'):
            k += 1
        name = code[j:k]
        i = k
        if name and name not in used:
            used[name] = code.count('\n', 0, j) + 1
    return included, used


def main():
    problems = []
    files = 0
    for root, _dirs, names in os.walk(SRC):
        for name in sorted(names):
            if not name.endswith(('.cpp', '.h', '.hpp')):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, PROJ)
            files += 1
            included, used = collect(path)
            for sym, line_no in sorted(used.items(), key=lambda kv: kv[1]):
                wanted = NEEDS.get(sym)
                if not wanted:
                    continue
                if not included.intersection(wanted):
                    problems.append('%s:%d: std::%s — нет #include %s'
                                    % (rel, line_no, sym, ' или '.join(wanted)))

    if problems:
        print('  ✗ файлы пользуются std:: без своего #include:')
        for p in problems:
            print('      ' + p)
        print('      Транзитивные включения не в счёт: их набор меняется')
        print('      между версиями стандартной библиотеки.')
        return 1

    print('  файлов проверено: %d, недостающих #include нет' % files)
    return 0


if __name__ == '__main__':
    sys.exit(main())
