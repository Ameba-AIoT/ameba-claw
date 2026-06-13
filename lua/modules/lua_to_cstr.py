#!/usr/bin/env python3
"""Convert a Lua source file to a C string literal header.

Usage: lua_to_cstr.py <input.lua> <output.h> <var_name>
"""
import sys


def main():
    input_path = sys.argv[1]
    output_path = sys.argv[2]
    var_name = sys.argv[3]

    with open(input_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    basename = input_path.replace('\\', '/').rsplit('/', 1)[-1]

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write('/* Auto-generated from {} -- do not edit */\n'.format(basename))
        f.write('static const char {}[] =\n'.format(var_name))
        for line in lines:
            stripped = line.rstrip('\r\n')
            escaped = stripped.replace('\\', '\\\\').replace('"', '\\"')
            f.write('    "{}\\n"\n'.format(escaped))
        f.write(';\n')


if __name__ == '__main__':
    main()
