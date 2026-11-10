#!/usr/bin/env python3
"""Writes corpus.hex: the JSON cases GraniteC's reader and writers are held to, one per line,
hex-encoded so any byte can be a case. serde.expected is what serde_json 1.0.154 (GraniteRust's)
makes of each; scripts/json-goldens.sh regenerates it."""
import random
import struct

cases = []
add = cases.append

# Values that read, and come back sorted and compact, or indented.
for text in [
    '{}', '[]', 'null', 'true', 'false', '0', '-0', '1', '-1', '1.0', '-1.5', '0.1',
    '"plain"', '""', '"a/b"', '"tab\\tnewline\\nreturn\\rquote\\"backslash\\\\"',
    '"\\b\\f"', '"\\u0000\\u001f\\u007f\\u0080"', '"\\u00e9\\u4e2d\\ud83d\\ude00"', '"é中😀"',
    '"\\/"', '{"b":1,"a":2,"c":{"z":[3,2,1],"y":null}}', '{"a":1,"a":2}',
    '[1,[2,[3,[]]],{}]', ' \t\r\n{ "k" : [ 1 , 2 ] } \n', '{"":0,"A":1,"a":2,"_":3,"~":4,"é":5}',
    '[{"nested":{"deeper":{"deepest":[true,false,null]}}}]',
    '18446744073709551615', '18446744073709551616', '9223372036854775807', '9223372036854775808',
    '-9223372036854775808', '-9223372036854775809', '123456789012345678901234567890',
    '1e0', '1E2', '1e+2', '1e-2', '2.5e10', '1e15', '1e16', '1e17', '1e21', '1e22', '1e-5', '1e-6', '1e-7',
    '0.000001', '0.00001', '123456789.123', '1.7976931348623157e308', '5e-324', '2.2250738585072014e-308',
    '4.9406564584124654e-324', '0.30000000000000004', '100', '100.0', '1234567890123456.0',
    '12345678901234567.0', '-0.0', '3.14159', '[1.5,2,"x",{"k":-0.25}]',
    '{"seconds":300,"maxTokens":3000}',
    '"\\u2028\\u2029"', '"\x7f"',
]:
    add(text.encode())

# 127 levels read; the 128th is serde's recursion limit.
add(b'[' * 127 + b']' * 127)
add(b'[' * 128 + b']' * 128)
add(b'{"a":' * 127 + b'1' + b'}' * 127)
add(b'{"a":' * 128 + b'1' + b'}' * 128)

# What does not read, and where serde says so.
for text in [
    b'', b' ', b'x', b'nul', b'nulx', b'tru', b'True', b'falsy', b'-', b'-x', b'01', b'1.', b'1.x', b'1e', b'1ex',
    b'1e+', b'+1', b'.5', b'1e400', b'-1e400', b'"open', b'"bad \\x escape"', b'"\\u12"', b'"\\u12x4"',
    b'"\\ud800"', b'"\\ud800x"', b'"\\ud800\\u0041"', b'"\\udc00"', b'"ctrl \x01 char"', b'"new\nline"',
    b'[', b'[1', b'[1,', b'[1,]', b'[1 2]', b'{', b'{"a"', b'{"a":', b'{"a":1', b'{"a":1,', b'{"a":1,}',
    b'{"a" 1}', b'{1:2}', b'{"a":1 "b":2}', b'{} x', b'[] []', b'1 2', b'"a" "b"', b'nullnull',
    b'\xef\xbb\xbf{}', b'"\xff"', b'"ok \xc3"', b'"\xe2\x82"', b'{"k":"v\xc3\x28"}', b'\n\n  x',
    b'{\n  "a": 1,\n  "b": ?\n}', b'[\n1,\n2,\n]', b'\x00', b'{"a":1}\x00', b'[1,2,3}', b'{"a":[}',
]:
    add(text)

# Doubles across the layout's branches: shortest round-trip digits, laid out as ryu does.
random.seed(1066)
for _ in range(300):
    bits = random.getrandbits(64)
    value = struct.unpack('<d', struct.pack('<Q', bits))[0]
    if value != value or value in (float('inf'), float('-inf')):
        continue
    add(repr(value).encode())
for exponent in range(-30, 31):
    for mantissa in ('1', '1.5', '9.999', '7.25', '3'):
        add(f'{mantissa}e{exponent}'.encode())
for _ in range(100):
    add(repr(random.uniform(-1e6, 1e6)).encode())
    add(repr(random.random() / 10 ** random.randint(0, 12)).encode())

with open(__file__.replace('make_corpus.py', 'corpus.hex'), 'w') as out:
    for case in cases:
        out.write(case.hex() + '\n')
print(f'{len(cases)} cases')
