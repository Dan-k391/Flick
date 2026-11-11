# Draws src/flick.ico (app + tray icon): a dark lightning bolt on a mint rounded square. Run once; the .ico is committed.
from PIL import Image, ImageDraw

S = 1024  # drawn large on a 64-unit grid, scaled down per size
k = S / 64
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((4 * k, 4 * k, 60 * k, 60 * k), radius=14 * k, fill=(0x3e, 0xe0, 0xa8, 255))
bolt = [(37, 6), (15, 36), (29, 36), (25, 58), (49, 26), (35, 26)]
d.polygon([(x * k, y * k) for x, y in bolt], fill=(0x0d, 0x2a, 0x22, 255))
img.save(r'D:\Flick\src\flick.ico', sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (256, 256)])
img.resize((256, 256), Image.LANCZOS).save(r'D:\Flick\build\icon-preview.png')
