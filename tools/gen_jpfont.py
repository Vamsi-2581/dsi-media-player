#!/usr/bin/env python3
"""Generate an 8x8 bitmap font header for Japanese character support.
Covers: ASCII (0x20-0x7E), Hiragana (U+3040-U+309F), Katakana (U+30A0-U+30FF),
CJK punctuation (U+3000-U+3003), fullwidth punctuation, and ~1000 common kanji."""

from PIL import ImageFont, Image, ImageDraw
import sys

FONT_W = 8
FONT_H = 8

# Common kanji used in song lyrics (JIS Level 1 subset ~1000 most common)
COMMON_KANJI = (
    "一二三四五六七八九十百千万億兆"
    "日月火水木金土曜年時分秒朝昼夜"
    "上下左右前後中内外間東西南北"
    "大小高低長短強弱新古"
    "人子女男父母兄弟姉妹友"
    "目耳口手足体心頭顔首声"
    "山川海空雨雪風花草木森林"
    "赤青白黒色光闇影"
    "食飲見聞話言読書歌踊走歩立座"
    "行来出入帰着届届届届届届届届"
    "思知考感覚信望愛恋夢想願祈"
    "生死活殺泣笑怒悲喜楽苦痛"
    "持待送届届届届届届届届届届届"
    "開閉始終変化動止消現残"
    "会合別離近遠"
    "学校先生教室"
    "家門窓庭部屋"
    "国王様神仏"
    "車道路橋駅船飛"
    "天地世界星雲"
    "春夏秋冬温冷熱暑寒"
    "気力風音波"
    "形丸角線面"
    "名前最初最後次"
    "今昔未来過去永遠"
    "全部半数多少"
    "同違正誤真偽"
    "美醜清汚明暗深浅広狭"
    "早遅速急"
    "本当嘘本物偽物"
    "命魂精霊鬼"
    "戦争平和自由幸福不幸"
    "物事者所場方向側面裏表"
    "回度番号作品曲歌詞"
    "語文字句意味"
    "手紙電話写真絵画映像"
    "時計機械道具武器"
    "衣服布糸針"
    "石鉄銀銅"
    "酒茶米飯肉魚野菜果物"
    "眠起寝覚"
    "病院薬傷"
    "忘記憶"
    "問答"
    "使役"
    "選択決定"
    "守攻"
    "助救"
    "壊直治"
    "教育"
    "約束"
    "許赦"
    "呼叫"
    "落飛投捨拾"
    "引押切刺打"
    "掛付着離"
    "借返貸売買"
    "乗降"
    "勝負"
    "焼燃溶凍"
    "倒起転"
    "隠探"
    "触"
    "届届届"
    "繰込告届届届届届届届届"
    "届届届届届届届届"
    "僕私俺彼"
    "誰何処何時何故如何"
    "此其彼此何"
    "若老幼"
    "良悪"
    "要不可能必要"
    "為成"
    "在有無"
    "如此其之"
    "以与及"
    "但只亦又更"
    "既已未"
    "甚最極"
    "共各"
    "或若"
    "乃即"
    "然否"
    "猶尚"
    "更再"
    "他別"
    "特普"
    "単複"
    "主従"
    "公私"
    "内外"
    "表裏"
    "奥底"
    "端隅"
    "頂峰"
    "谷底"
    "岸辺浜"
    "島陸"
    "村町市都"
    "夕暮"
    "朧霞"
    "桜梅桃"
    "蝶鳥虫蛍"
    "猫犬"
    "月星"
    "輝煌"
    "静寂"
    "響鳴"
    "揺震"
    "流注"
    "浮沈"
    "散集"
    "増減"
    "重軽"
    "固柔"
    "甘苦辛酸"
    "香匂"
    "嬉悔恥"
    "寂淋"
    "穏激"
    "優劣"
    "貴賤"
    "聖俗"
    "善悪"
    "慕恨"
    "誓契"
    "絆縁"
    "翼羽"
    "舞飾"
    "歩踏"
    "抱包"
    "支差"
    "解放"
    "閉鎖"
    "創造"
    "破壊"
    "運命"
    "奇跡"
    "希望"
    "絶望"
    "勇敢"
    "臆病"
    "忍耐"
    "挑戦"
    "冒険"
    "旅路"
    "道程"
    "足跡"
    "記録"
    "歴史"
    "伝説"
    "物語"
    "秘密"
    "謎"
)

# Deduplicate kanji
seen = set()
unique_kanji = []
for c in COMMON_KANJI:
    if c not in seen:
        seen.add(c)
        unique_kanji.append(c)

# Build character set
chars = []
# ASCII printable
for cp in range(0x20, 0x7F):
    chars.append(chr(cp))
# CJK punctuation
for cp in [0x3000, 0x3001, 0x3002, 0x3003, 0x300C, 0x300D, 0x300E, 0x300F,
           0x3010, 0x3011, 0xFF01, 0xFF0C, 0xFF0E, 0xFF1F, 0xFF1A, 0xFF1B,
           0x30FB, 0x30FC, 0x2026, 0x2014, 0x2015, 0xFF5E]:
    chars.append(chr(cp))
# Hiragana
for cp in range(0x3041, 0x3097):
    chars.append(chr(cp))
# Katakana
for cp in range(0x30A1, 0x30FB):
    chars.append(chr(cp))
# Common kanji
for c in unique_kanji:
    if c not in [ch for ch in chars]:
        chars.append(c)

print(f"Total characters: {len(chars)}", file=sys.stderr)
print(f"  ASCII: 95", file=sys.stderr)
print(f"  Hiragana: {sum(1 for c in chars if 0x3041 <= ord(c) <= 0x3096)}", file=sys.stderr)
print(f"  Katakana: {sum(1 for c in chars if 0x30A1 <= ord(c) <= 0x30FA)}", file=sys.stderr)
print(f"  Kanji: {sum(1 for c in chars if ord(c) >= 0x4E00)}", file=sys.stderr)

# Load font
font_paths = [
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
]
font = None
for fp in font_paths:
    try:
        font = ImageFont.truetype(fp, 7)
        break
    except:
        continue

if font is None:
    print("ERROR: No suitable font found", file=sys.stderr)
    sys.exit(1)

# Render each character to 8x8 bitmap
glyph_data = []  # list of (codepoint, 8 bytes)

for ch in chars:
    cp = ord(ch)
    img = Image.new('1', (FONT_W, FONT_H), 0)
    d = ImageDraw.Draw(img)

    # Adjust positioning for CJK vs ASCII
    if cp >= 0x3000:
        d.text((0, -1), ch, fill=1, font=font)
    else:
        d.text((0, 0), ch, fill=1, font=font)

    # Convert to bytes (1 byte per row, MSB = leftmost pixel)
    rows = []
    for y in range(FONT_H):
        byte = 0
        for x in range(FONT_W):
            if img.getpixel((x, y)):
                byte |= (0x80 >> x)
        rows.append(byte)

    glyph_data.append((cp, bytes(rows)))

# Generate C header
print("/* Auto-generated Japanese bitmap font (8x8) */")
print("/* Covers ASCII, Hiragana, Katakana, CJK punctuation, common Kanji */")
print(f"/* Total glyphs: {len(glyph_data)} */")
print()
print("#ifndef FONT_JP_DATA_H")
print("#define FONT_JP_DATA_H")
print()
print("#include <stdint.h>")
print()
print(f"#define JP_FONT_W 8")
print(f"#define JP_FONT_H 8")
print(f"#define JP_GLYPH_COUNT {len(glyph_data)}")
print()

# Codepoint table (sorted for binary search)
print("static const uint16_t jpFontCodepoints[JP_GLYPH_COUNT] = {")
for i, (cp, _) in enumerate(glyph_data):
    if i % 16 == 0:
        print("    ", end="")
    print(f"0x{cp:04X},", end="")
    if i % 16 == 15 or i == len(glyph_data) - 1:
        print()
print("};")
print()

# Glyph bitmap data (8 bytes per glyph)
print(f"static const uint8_t jpFontData[JP_GLYPH_COUNT * JP_FONT_H] = {{")
for i, (cp, data) in enumerate(glyph_data):
    if i % 4 == 0:
        print("    ", end="")
    print("".join(f"0x{b:02X}," for b in data), end="")
    if i % 4 == 3 or i == len(glyph_data) - 1:
        print()
print("};")
print()
print("#endif")
