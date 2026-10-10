# Crysis Coop: the console's font. The game's console draws its text byte
# by byte (a byte = the Unicode character of that number), so Cyrillic is
# written to it in Windows-1251 and this font has the Cyrillic letters at
# U+0080..U+00FF where that code page has them. Made from DejaVu Sans Mono
# (renamed, as the Bitstream Vera license asks of changed fonts).
import sys
from fontTools.ttLib import TTFont

src, dst = sys.argv[1], sys.argv[2]
font = TTFont(src)
best = font.getBestCmap()
for table in font['cmap'].tables:
    if not table.isUnicode():
        continue
    for b in range(0x80, 0x100):
        try:
            ch = bytes([b]).decode('cp1251')
        except UnicodeDecodeError:
            continue
        glyph = best.get(ord(ch))
        if glyph:
            table.cmap[b] = glyph
family = 'Crysis Coop Console'
for rec in font['name'].names:
    if rec.nameID in (1, 16):
        rec.string = family
    elif rec.nameID in (3, 4):
        rec.string = family
    elif rec.nameID == 6:
        rec.string = 'CrysisCoopConsole'
font.save(dst)
print('saved', dst)
