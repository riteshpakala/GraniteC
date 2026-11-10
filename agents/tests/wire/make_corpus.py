#!/usr/bin/env python3
"""Writes corpus.hex: the wire lines GraniteC's reader is held to, hex-encoded, one per line.
rust.expected is what GraniteRust's AgentWire::read makes of each; scripts/wire-goldens.sh
regenerates it."""
lines = r'''
{"hello":{"pid":7,"wire":"granite-agent/1"}}
{"hello":{"answer":"granite-answer/1","pid":4512,"wire":"granite-agent/1"}}
{"hello":{"wire":"granite-agent/1","later":true}}
{"hello":{"wire":"granite-agent/2"}}
{"hello":{"pid":7}}
{"hello":{"wire":5}}
{"hello":{"wire":"granite-agent/1","pid":-3}}
{"hello":{"wire":"granite-agent/1","pid":3000000000}}
{"hello":{"wire":"granite-agent/1","pid":1.5}}
{"hello":{"wire":"granite-agent/1","pid":null,"answer":null}}
{"hello":{"wire":"granite-agent/1","pid":"7"}}
{"hello":5}
{"hello":"granite-agent/1"}
{"hello":null,"note":"x"}
{"hello":{"pid":"x"}}
{"perform":{"word":"plate","input":{"sources":["Sources"]},"directory":"/p","ask":"make a graphic","model":true}}
{"perform":{"word":"x"}}
{"perform":{"word":"x","input":null,"model":false}}
{"perform":{"word":"x","input":1}}
{"perform":{"input":{},"model":true}}
{"perform":{"word":"x","input":{},"model":"yes"}}
{"perform":{"word":7,"input":{},"model":true}}
{"perform":{"word":"x","input":{},"model":true,"directory":3}}
{"note":"round 1: measured clean"}
{"note":5}
{"note":["a"]}
{"note":"a/b \"quoted\" é"}
{"model":{"id":1,"kind":"object","schema":{"type":"object"},"system":"s","user":"u","maxTokens":3000,"seconds":300.0}}
{"model":{"id":2,"kind":"text","system":"s","user":"u","maxTokens":10,"seconds":2.5}}
{"model":{"id":2,"kind":"image","system":"s","user":"u","maxTokens":10,"seconds":2}}
{"model":{"id":2,"kind":5,"system":"s","user":"u","maxTokens":10,"seconds":2}}
{"model":{"id":-1,"kind":"text","system":"s","user":"u","maxTokens":10,"seconds":2}}
{"model":{"id":1.5,"kind":"text","system":"s","user":"u","maxTokens":10,"seconds":2}}
{"model":{"id":1,"kind":"text","system":"s","user":"u","maxTokens":5000000000,"seconds":2}}
{"model":{"id":1,"kind":"text","system":"s","user":"u","maxTokens":10,"seconds":"2"}}
{"model":{"id":1,"kind":"text","system":"s","user":"u","maxTokens":10}}
{"model":{"id":1,"kind":"text","user":"u","maxTokens":10,"seconds":2}}
{"model":{"id":1,"kind":"text","system":"s","user":"u","maxTokens":10,"seconds":2,"schema":null}}
{"answer":{"id":1,"object":{"n":3}}}
{"answer":{"id":1,"text":"hi"}}
{"answer":{"id":1,"error":"the model is busy"}}
{"answer":{"id":1,"object":null}}
{"answer":{"object":{}}}
{"answer":{"id":"1"}}
{"output":{"format":"granite-answer/1","paths":[]}}
{"output":null}
{"output":[1,2]}
{"output":{"b":1,"a":{"d":2,"c":3}}}
{"failed":"told to fail","refused":{"kind":"call","reasons":["leave out `fail`"]}}
{"failed":"it broke"}
{"failed":"x","refused":{"kind":"weather","reasons":["rain"]}}
{"failed":"x","refused":7}
{"failed":"x","refused":{"kind":"call"}}
{"failed":"x","refused":{"kind":3,"reasons":[]}}
{"failed":"x","refused":{"kind":"input","reasons":["a",2]}}
{"failed":"2 problems","refused":{"kind":"answer","reasons":["one","two"]}}
{"failed":7}
{"failed":null,"cancel":{}}
{"cancel":{}}
{"cancel":null}
{"cancel":5}
{"entries":[{"kind":"package","word":"echo","summary":"says it back.","module":"Echo","type":"Echo.Echo","schema":{"object":[{"name":"text","required":true,"schema":"string"}]},"lanes":2}]}
{"entries":[]}
{"entries":[{"kind":"package","word":"echo"}]}
{"entries":[{"kind":"robot","word":"e","summary":"s","module":"M","type":"M.T"}]}
{"entries":{"kind":"package"}}
{"entries":[{"kind":"package","word":"e","summary":"s","module":"M","type":"M.T","lanes":-1}]}
{"weather":{}}
{}
[]
"hello"
7
null
{"hello":{"wire":"granite-agent/1"}} trailing
{"hello":
not json at all
{"note":"x","hello":{"wire":"granite-agent/1"}}
{"output":1,"failed":"x"}
{"failed":"x","cancel":{}}
'''.strip('\n').split('\n')
with open(__file__.replace('make_corpus.py', 'corpus.hex'), 'w') as out:
    for line in lines:
        out.write(line.encode().hex() + '\n')
print(f'{len(lines)} lines')
