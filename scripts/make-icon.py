# Draws src/flick.ico (app + tray icon): a blue rounded square with a white eye. Run once; the .ico is committed.
from PIL import Image, ImageChops, ImageDraw

S = 1024  # drawn large, scaled down per size
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((40, 40, S - 40, S - 40), radius=220, fill=(0, 103, 192, 255))
# eye: the overlap of two big circles, one above and one below the centre
r, cx, cy, off = 480, S / 2, S / 2, 290
a, b = Image.new('L', (S, S), 0), Image.new('L', (S, S), 0)
ImageDraw.Draw(a).ellipse((cx - r, cy - off - r, cx + r, cy - off + r), fill=255)
ImageDraw.Draw(b).ellipse((cx - r, cy + off - r, cx + r, cy + off + r), fill=255)
eye = ImageChops.multiply(a, b)
img.paste((255, 255, 255, 255), (0, 0), eye)
d.ellipse((cx - 150, cy - 150, cx + 150, cy + 150), fill=(0, 103, 192, 255))
d.ellipse((cx - 70, cy - 70, cx + 70, cy + 70), fill=(255, 255, 255, 255))
img.save(r'D:\Flick\src\flick.ico', sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (256, 256)])
img.resize((256, 256), Image.LANCZOS).save(r'D:\Flick\build\icon-preview.png')
