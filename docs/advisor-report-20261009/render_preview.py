#!/usr/bin/env python3
"""Render the PDF to page previews and a labelled contact sheet."""
from pathlib import Path
import fitz
from PIL import Image, ImageDraw

ROOT=Path(__file__).resolve().parent
PDF=ROOT/'AgentFlow_自进化系统_导师汇报_20261009.pdf'
OUT=ROOT/'preview'; OUT.mkdir(exist_ok=True)
doc=fitz.open(PDF)
thumbs=[]
for i,page in enumerate(doc):
    page.get_pixmap(matrix=fitz.Matrix(1.4,1.4)).save(str(OUT/f'slide-{i+1:02}.png'))
    px=page.get_pixmap(matrix=fitz.Matrix(.48,.48))
    im=Image.frombytes('RGB',[px.width,px.height],px.samples)
    t=Image.new('RGB',(im.width,im.height+24),'white');t.paste(im,(0,24))
    ImageDraw.Draw(t).text((8,5),f'{i+1:02d}',fill='#142B45');thumbs.append(t)
cw,ch=thumbs[0].size
contact=Image.new('RGB',(cw*4,ch*((len(thumbs)+3)//4)),'#dbe3eb')
for i,t in enumerate(thumbs):contact.paste(t,((i%4)*cw,(i//4)*ch))
contact.save(OUT/'contact-sheet.png')
print(f'Rendered {len(doc)} pages and contact sheet.')
