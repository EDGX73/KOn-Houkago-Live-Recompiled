"""Draws the project icon from a group picture of the band: the five girls as
flat-colour silhouettes (each in the colour the game writes her name with:
Ritsu orange, Mio blue, Azusa green, Yui red, Mugi pink) on a light
background with sakura petals, and "K-ON! RECOMPILED" large in the middle.

Inputs (not in the repository): assets/branding/base.jpg (the picture,
1920x1200) and work/icon/mask_isnet-general-use.png, its foreground mask made
with rembg's isnet-general-use model (work/venv-icon):
    from rembg import remove,new_session
    remove(Image.open('assets/branding/base.jpg'),session=new_session('isnet-general-use'),only_mask=True)
Writes assets/branding/icon_1024.png (square; Discord crops it to a circle)
and icon.ico (executable)."""
import math, random
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'assets/branding'
S=1024;K=2;W=S*K
# Each girl's three flat tones (shade, colour, light), left to right: Ritsu orange,
# Mio blue, Azusa green, Yui red, Mugi pink (the colours the game writes their names in).
COLOURS=[[(196,96,18),(255,165,61),(255,222,165)],[(28,70,176),(74,140,255),(188,216,255)],[(18,112,56),(61,190,107),(184,240,204)],
         [(178,28,62),(255,77,109),(255,192,204)],[(206,58,152),(255,128,206),(255,216,242)]]
# Borders between neighbouring girls (polylines, picture pixels, traced along
# their outlines from top to bottom): Ritsu|Mio, Mio|Azusa, Azusa|Yui, Yui|Mugi.
BORDERS=[[(468,190),(472,290),(525,318),(548,380),(548,440),(535,550),(512,690),(500,840),(490,1200)],
         [(775,190),(780,390),(765,520),(750,640),(745,790),(740,1200)],
         [(1056,190),(1062,520),(1075,670),(1040,750),(1010,830),(1030,1200)],
         [(1325,190),(1335,390),(1345,520),(1360,640),(1370,790),(1380,1200)]]
CROP=(250,190,1690,1010)  # region of the picture used (heads and upper bodies)
def silhouettes():
    mask=np.asarray(Image.open(ROOT/'work/icon/mask_isnet-general-use.png').convert('L'))>110
    # fill holes: everything not reachable from the border through background is foreground
    bg=Image.fromarray(np.where(mask,0,255).astype(np.uint8)).copy()  # writable for floodfill
    for seed in ((0,0),(1919,0),(0,1199),(1919,1199)):
        if bg.getpixel(seed)==255:ImageDraw.floodfill(bg,seed,128)
    filled=np.asarray(bg)!=128
    h,w=filled.shape;yy,xx=np.mgrid[0:h,0:w];label=np.zeros((h,w),np.uint8)
    for line in BORDERS:
        ys=np.array([p[1] for p in line],float);xs=np.array([p[0] for p in line],float)
        xb=np.interp(np.arange(h),ys,xs)[:,None];label+=(xx>xb).astype(np.uint8)
    return [Image.fromarray(((filled&(label==i))*255).astype(np.uint8)).filter(ImageFilter.MedianFilter(7)).crop(CROP) for i in range(5)]
def draw():
    random.seed(7)
    img=Image.new('RGBA',(W,W),(255,246,232,255))
    glow=Image.new('L',(W,W),0);ImageDraw.Draw(glow).ellipse([200*K,60*K,824*K,620*K],fill=200);img.paste((255,196,220,255),(0,0),glow.filter(ImageFilter.GaussianBlur(110*K)))
    d=ImageDraw.Draw(img)
    for _ in range(26):  # sakura petals
        x,y,r=random.uniform(60,964),random.uniform(40,560),random.uniform(10,22)
        d.ellipse([(x-r)*K,(y-r*0.6)*K,(x+r)*K,(y+r*0.6)*K],fill=(255,170,200,200))
    # silhouettes along the bottom: a white outline, then each girl in her colour
    # the band in flat colours: each girl in three flat tones of her colour
    # (from the picture's light and shade), so hair, faces and uniforms read
    pic=Image.open(ROOT/'assets/branding/base.jpg').convert('L').crop(CROP).filter(ImageFilter.MedianFilter(7))
    lum=np.asarray(pic).astype(float)
    tone=np.digitize(lum,[70,150])  # 0 dark (uniforms, lines), 1 mid (hair), 2 light (skin, shirts)
    layers=silhouettes();h,w=lum.shape;rgb=np.zeros((h,w,3),np.uint8)
    for m,c in zip(layers,COLOURS):
        shades=np.array(c,np.uint8)
        sel=np.asarray(m)>127;rgb[sel]=shades[tone[sel]]
    flat=Image.fromarray(rgb).filter(ImageFilter.ModeFilter(9))
    union=np.zeros((h,w),np.uint8)
    for m in layers:union=np.maximum(union,np.asarray(m))
    cut=Image.fromarray(union).filter(ImageFilter.MinFilter(3))
    size=(W,int((CROP[3]-CROP[1])*W/(CROP[2]-CROP[0])));top=36*K
    cutk=cut.resize(size,Image.LANCZOS);flatk=flat.resize(size,Image.NEAREST)
    img.paste((255,255,255,255),(0,top),cutk.filter(ImageFilter.MaxFilter(15)))   # white outline around the band
    img.paste(flatk,(0,top),cutk)
    # the lower half: a pink stage with petals, starting behind the title
    ground=top+size[1]-70*K
    d=ImageDraw.Draw(img)
    for y in range(ground,W):
        t=(y-ground)/max(1,W-ground);d.line([(0,y),(W,y)],fill=(255,int(112-40*t),int(168-30*t),255))
    d.rectangle([0,ground-6*K,W,ground],fill=(255,255,255,255))
    for _ in range(18):
        x,y,r=random.uniform(40,984),random.uniform(ground/K+40,1000),random.uniform(8,18)
        d.ellipse([(x-r)*K,(y-r*0.6)*K,(x+r)*K,(y+r*0.6)*K],fill=(255,214,230,220))
    # title, large, in the middle
    d=ImageDraw.Draw(img)
    title=ImageFont.truetype('GILLUBCD.TTF',290*K);t='K-ON!';tw=d.textlength(t,font=title);bb=d.textbbox((0,0),t,font=title);x=(W-tw)/2;y=W//2-bb[3]+40*K
    sh=Image.new('RGBA',(W,W));ImageDraw.Draw(sh).text((x+12*K,y+18*K),t,font=title,fill=(60,20,70,170),stroke_width=22*K,stroke_fill=(60,20,70,170));img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(6*K)))
    d=ImageDraw.Draw(img);d.text((x,y),t,font=title,fill=(255,255,255),stroke_width=22*K,stroke_fill=(232,40,120))
    sub=ImageFont.truetype('GILLUBCD.TTF',90*K);s='RECOMPILED';sw=d.textlength(s,font=sub)
    bx0,by0,bx1,by1=(W-sw)/2-46*K,W//2+60*K,(W+sw)/2+46*K,W//2+184*K
    d.rounded_rectangle([bx0+8*K,by0+12*K,bx1+8*K,by1+12*K],radius=60*K,fill=(60,20,70,150))
    d.rounded_rectangle([bx0,by0,bx1,by1],radius=60*K,fill=(255,206,64),outline=(255,255,255),width=10*K)
    d.text(((W-sw)/2,by0+10*K),s,font=sub,fill=(150,24,90))
    return img.resize((S,S),Image.LANCZOS)
img=draw();img.save(OUT/'icon_1024.png')
mask=Image.new('L',(S,S),0);ImageDraw.Draw(mask).ellipse([0,0,S-1,S-1],fill=255)
app=Image.new('RGBA',(S,S));app.paste(img,(0,0),mask)
app.save(OUT/'icon.ico',sizes=[(256,256),(128,128),(64,64),(48,48),(32,32),(16,16)])
print('icons in',OUT)
