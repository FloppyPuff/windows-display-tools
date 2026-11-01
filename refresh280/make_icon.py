"""refresh280 图标生成: 与 DisplayTuner 同族风格(蓝靛渐变圆角方块) + 大号刷新箭头 + 280"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

S = 2048
K = S / 1024.0


def P(v):
    return v * K


img = Image.new('RGBA', (S, S), (0, 0, 0, 0))

# 对角渐变(与 DisplayTuner 同色)
c1 = (74, 144, 226)
c2 = (43, 47, 158)
small = Image.new('RGB', (256, 256))
px = small.load()
for y in range(256):
    for x in range(256):
        t = (x + y) / 510.0
        px[x, y] = (
            round(c1[0] + (c2[0] - c1[0]) * t),
            round(c1[1] + (c2[1] - c1[1]) * t),
            round(c1[2] + (c2[2] - c1[2]) * t),
        )
bg = small.resize((S, S), Image.LANCZOS)

mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(P(228)), fill=255)
img.paste(bg, (0, 0), mask)

d = ImageDraw.Draw(img)
white = (255, 255, 255, 255)

cx, cy, r, w = P(512), P(512), P(252), int(P(56))
bb = [cx - r, cy - r, cx + r, cy + r]
d.arc(bb, 200, 340, fill=white, width=w)
d.arc(bb, 20, 160, fill=white, width=w)


def arrowhead(center_x, center_y, radius, angle_deg, width):
    th = math.radians(angle_deg)
    p = (center_x + radius * math.cos(th), center_y + radius * math.sin(th))
    t = (-math.sin(th), math.cos(th))
    n = (math.cos(th), math.sin(th))
    tip = (p[0] + t[0] * width * 1.3, p[1] + t[1] * width * 1.3)
    bc = (p[0] - t[0] * width * 0.3, p[1] - t[1] * width * 0.3)
    b1 = (bc[0] + n[0] * width * 1.2, bc[1] + n[1] * width * 1.2)
    b2 = (bc[0] - n[0] * width * 1.2, bc[1] - n[1] * width * 1.2)
    d.polygon([tip, b1, b2], fill=white)


arrowhead(cx, cy, r, 340, w)
arrowhead(cx, cy, r, 160, w)

# 中心 "280" 数字
font = None
for name in ('segoeuib.ttf', 'arialbd.ttf', 'msyhbd.ttc'):
    fp = os.path.join(r'C:\Windows\Fonts', name)
    if os.path.exists(fp):
        try:
            font = ImageFont.truetype(fp, int(P(178)))
            break
        except Exception:
            pass
if font is not None:
    text = '280'
    box = d.textbbox((0, 0), text, font=font)
    tw = box[2] - box[0]
    thh = box[3] - box[1]
    d.text((cx - tw / 2 - box[0], cy - thh / 2 - box[1]), text, font=font, fill=white)

base = r'C:\Users\fjh28\AppData\Roaming\com.qoder.app.stable\remote-control\sessions\10ad1010e8c3cd18e7b7154837b72fe6\5c2dbaf019f845cd8891035d\refresh-280hz'
img.save(os.path.join(base, 'app.ico'), format='ICO',
         sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (40, 40),
                (32, 32), (24, 24), (20, 20), (16, 16)])
img.resize((256, 256), Image.LANCZOS).save(r'C:\Users\fjh28\AppData\Local\Temp\icon280_preview.png')
print('icon generated')
