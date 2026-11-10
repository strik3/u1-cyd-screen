# Replicates fb-http's encode_png_zlib (unfiltered rows, zlib level 3) and writes a
# reference area-averaged downscale for comparison.
import struct, zlib, sys, random
def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
def png(w,h,px,alpha=False):
    ch=4 if alpha else 3
    rows=b''.join(b'\x00'+bytes(px[y*w*ch:(y+1)*w*ch]) for y in range(h))
    ihdr=struct.pack('>IIBBBBB',w,h,8,6 if alpha else 2,0,0,0)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',ihdr)+chunk(b'IDAT',zlib.compress(rows,3))+chunk(b'IEND',b'')
def make(w,h,alpha):
    random.seed(1); px=[]
    for y in range(h):
        for x in range(w):
            if (x//7+y//5)%3==0: c=(255,255,255)
            elif 100<x<200 and 50<y<120: c=(random.randrange(256),random.randrange(256),random.randrange(256))
            else: c=(x*255//w, y*255//h, (x+y)%256)
            px+=c+((200,) if alpha else ())
    return px
def ref(w,h,px,dw,dh,ch):
    out=[]
    for dy in range(dh):
        y0,y1=dy*h/dh,(dy+1)*h/dh
        for dx in range(dw):
            x0,x1=dx*w/dw,(dx+1)*w/dw; acc=[0,0,0]; tot=0
            for y in range(int(y0),min(h,int(y1)+1)):
                wy=min(y+1,y1)-max(y,y0)
                if wy<=0: continue
                for x in range(int(x0),min(w,int(x1)+1)):
                    wx=min(x+1,x1)-max(x,x0)
                    if wx<=0: continue
                    i=(y*w+x)*ch
                    for k in range(3): acc[k]+=px[i+k]*wx*wy
                    tot+=wx*wy
            out+= [round(a/tot) for a in acc]
    return out
w,h,dw,dh,alpha,name=int(sys.argv[1]),int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),sys.argv[5]=='1',sys.argv[6]
px=make(w,h,alpha); open(name+'.png','wb').write(png(w,h,px,alpha))
r=ref(w,h,px,dw,dh,4 if alpha else 3); open(name+'.ref','wb').write(bytes(r))
print(name, len(open(name+'.png','rb').read()),'bytes')
