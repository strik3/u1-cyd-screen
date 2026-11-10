#include "frame_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
struct FileSrc : ByteSource { FILE*f; int32_t read(uint8_t*b,int32_t n) override { int k=1+rand()%1500; if(k<n)n=k; return (int32_t)fread(b,1,n,f);} };
static std::vector<uint16_t> disp; static int DW=320, DH=240;
struct Ctx{ FrameDecoder*d; int rows; };
static void sink(int y,const uint16_t*p,int w,void*u){ Ctx*c=(Ctx*)u; c->rows++; for(int x=0;x<w;x++) disp[(c->d->geometry().dstY+y)*DW+c->d->geometry().dstX+x]=p[x]; }
int main(int argc,char**argv){
  const char*name=argv[1]; ScaleMode mode=(ScaleMode)atoi(argv[2]);
  FrameDecoder d; FrameGeometry g; d.setTarget(DW,DH,mode); int fails=0;
  for(int iter=0;iter<3;iter++){
    disp.assign(DW*DH,0xF81F);
    char fn[256]; snprintf(fn,256,"%s.png",name); FILE*f=fopen(fn,"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    FileSrc s; s.f=f; Ctx c{&d,0}; int rc=d.decode(&s, iter==2?-1:(int32_t)sz, sink,&c); fclose(f);
    g=d.geometry();
    printf("iter %d rc=%d (%s) src=%dx%d dst=%d,%d %dx%d rows=%d geomChanged=%d\n",iter,rc,pngErrorString(rc),d.geometry().srcW,d.geometry().srcH,d.geometry().dstX,d.geometry().dstY,d.geometry().dstW,d.geometry().dstH,c.rows,d.geometryChanged());
    if(rc||c.rows!=g.dstH) fails++;
    snprintf(fn,256,"%s.ref",name); FILE*r=fopen(fn,"rb"); std::vector<uint8_t> ref(g.dstW*g.dstH*3); fread(ref.data(),1,ref.size(),r); fclose(r);
    int maxe=0;
    for(int y=0;y<g.dstH;y++)for(int x=0;x<g.dstW;x++){ uint16_t p=disp[(g.dstY+y)*DW+g.dstX+x]; const uint8_t*q=&ref[(y*g.dstW+x)*3];
      int e[3]={abs((p>>11)-(q[0]>>3)),abs(((p>>5)&63)-(q[1]>>2)),abs((p&31)-(q[2]>>3))}; for(int k=0;k<3;k++) if(e[k]>maxe)maxe=e[k]; }
    printf("  max error vs reference: %d LSB\n",maxe); if(maxe>1) fails++;
  }
  // write PPM of display for viewing
  char fn[256]; snprintf(fn,256,"%s_out.ppm",name); FILE*o=fopen(fn,"wb"); fprintf(o,"P6 %d %d 255\n",DW,DH);
  for(auto p:disp){ uint8_t c[3]={(uint8_t)((p>>11)<<3),(uint8_t)(((p>>5)&63)<<2),(uint8_t)((p&31)<<3)}; fwrite(c,1,3,o);} fclose(o);
  // touch mapping checks
  int sx,sy; mapDisplayToSource(g,g.dstX,g.dstY,&sx,&sy,false); printf("  touch TL -> %d,%d\n",sx,sy);
  mapDisplayToSource(g,g.dstX+g.dstW-1,g.dstY+g.dstH-1,&sx,&sy,false); printf("  touch BR -> %d,%d\n",sx,sy);
  printf("  touch in letterbox accepted? %d\n", mapDisplayToSource(g,0,0,&sx,&sy,false) && g.dstY>0);
  printf(fails?"FAIL\n":"PASS\n"); return fails;
}
