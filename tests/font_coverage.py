"""Verify exact M5GFX efontJA12 glyph table, not just its font name."""
import re, ast, sys
from pathlib import Path
src=Path(sys.argv[1]).read_text()
block=src.split('const uint8_t lgfx_efont_ja_12[',1)[1].split('\n#endif',1)[0]
data=b''.join(ast.literal_eval('b'+m.group()) for m in re.finditer(r'"(?:\\.|[^"\\])*"',block))
def glyph(code):
 p=23
 if code<=255:
  if code>=97:p+=int.from_bytes(data[19:21],'big')
  elif code>=65:p+=int.from_bytes(data[17:19],'big')
  while data[p+1]:
   if data[p]==code:return p+2
   p+=data[p+1]
 else:
  p+=int.from_bytes(data[21:23],'big');lut=p
  while True:
   p+=int.from_bytes(data[lut:lut+2],'big');end=int.from_bytes(data[lut+2:lut+4],'big');lut+=4
   if end>=code:break
  while int.from_bytes(data[p:p+2],'big'):
   if int.from_bytes(data[p:p+2],'big')==code:return p+3
   p+=data[p+2]
 return None
chars='£€¬¦äöüÄÖÜßéèàçñÑáíóú№'+''.join(chr(c) for c in range(0x410,0x450))+'Ёё'
missing=[c for c in chars if glyph(ord(c)) is None]
assert not missing,missing
print(f'efontJA12: {len(data)} bytes; Latin Extended symbols and all 66 Russian letters present')
