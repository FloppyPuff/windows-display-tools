"""DisplayTuner 图标生成: Fluent 风格圆角方块 + 显示器 + 刷新箭头, 输出多尺寸 ICO"""
import math
from PIL import Image, ImageDraw

S = 2048          # 超采样绘制尺寸
K = S / 1024.0    # 以 1024 为设计基准缩放


def P(v):
    return v * K


img = Image.new('RGBA', (S, S), (0, 0, 0, 0))

# ---- 1. 对角渐变(亮蓝 -> 深靛) ----
c1 = (74, 144, 226)    # #4A90E2
c2 = (43, 47, 158)     # #2B2F9E
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

# ---- 2. 圆角遮罩 ----
mask = Image.new('L', (S, S), 0)
md = ImageDraw.Draw(mask)
md.rounded_rectangle([0, 0, S - 1, S - 1], radius=int(P(228)), fill=255)
img.paste(bg, (0, 0), mask)

d = ImageDraw.Draw(img)
white = (255, 255, 255, 255)

# ---- 3. 显示器: 屏幕边框 + 支架 ----
stroke = int(P(34))
d.rounded_rectangle([P(286), P(318), P(738), P(614)], radius=int(P(30)),
                    outline=white, width=stroke)
d.rectangle([P(480), P(614), P(544), P(662)], fill=white)          # 立柱
d.rounded_rectangle([P(402), P(662), P(622), P(696)], radius=int(P(16)), fill=white)  # 底座

# ---- 4. 屏幕内的圆形刷新箭头(两段弧 + 箭头) ----
cx, cy, r, w = P(512), P(462), P(84), int(P(38))
bb = [cx - r, cy - r, cx + r, cy + r]

d.arc(bb, 200, 340, fill=white, width=w)   # 上半弧(从左上顺时针到右上)
d.arc(bb, 20, 160, fill=white, width=w)    # 下半弧


def arrowhead(center_x, center_y, radius, angle_deg, width):
    th = math.radians(angle_deg)
    px_ = center_x + radius * math.cos(th)
    py_ = center_y + radius * math.sin(th)
    tx, ty = -math.sin(th), math.cos(th)          # 顺时针切线方向
    nx, ny = math.cos(th), math.sin(th)           # 径向
    tip = (px_ + tx * width * 1.25, py_ + ty * width * 1.25)
    bc = (px_ - tx * width * 0.35, py_ - ty * width * 0.35)
    b1 = (bc[0] + nx * width * 1.15, bc[1] + ny * width * 1.15)
    b2 = (bc[0] - nx * width * 1.15, bc[1] - ny * width * 1.15)
    d.polygon([tip, b1, b2], fill=white)


arrowhead(cx, cy, r, 340, w)   # 上半弧的箭头(指向右下)
arrowhead(cx, cy, r, 160, w)   # 下半弧的箭头(指向左上)

# ---- 5. 输出 ----
img.save(r'C:\Users\fjh28\AppData\Roaming\com.qoder.app.stable\remote-control\sessions\10ad1010e8c3cd18e7b7154837b72fe6\5c2dbaf019f845cd8891035d\display-tuner\app.ico',
         format='ICO',
         sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (40, 40),
                (32, 32), (24, 24), (20, 20), (16, 16)])
img.resize((256, 256), Image.LANCZOS).save(
    r'C:\Users\fjh28\AppData\Local\Temp\icon_preview.png')
print('icon generated')
