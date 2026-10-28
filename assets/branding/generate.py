"""Original geometric mark, no external art/fonts; generated assets: CC0-1.0.
Requires Python 3 and Pillow. Run from any directory; outputs beside this script.
"""
from pathlib import Path
from PIL import Image, ImageDraw
import hashlib, json

root = Path(__file__).resolve().parent
svg = '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024"><rect width="1024" height="1024" rx="208" fill="#152338"/><path d="M256 288 L432 512 L256 736 M592 288 L768 512 L592 736" fill="none" stroke="#50dbc5" stroke-width="72" stroke-linejoin="round" stroke-linecap="round"/></svg>\n'
(root/'icon.svg').write_text(svg, encoding='utf-8')
im = Image.new('RGBA', (2048,2048))
d = ImageDraw.Draw(im)
d.rounded_rectangle((0,0,2047,2047), radius=416, fill='#152338')
for x in (256,592):
    points=[(x*2,576),((x+176)*2,1024),(x*2,1472)]
    d.line(points, fill='#50dbc5', width=144, joint='curve')
    for px,py in points:
        d.ellipse((px-72,py-72,px+72,py+72), fill='#50dbc5')
for size in (512,1024):
    im.resize((size,size), Image.Resampling.LANCZOS).save(root/f'icon_{size}.png')
im.resize((256,256), Image.Resampling.LANCZOS).save(root/'icon.ico',sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
manifest={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(root.iterdir()) if p.suffix in ('.svg','.png','.ico','.py')}
(root/'HASHES.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print(json.dumps(manifest,indent=2))
