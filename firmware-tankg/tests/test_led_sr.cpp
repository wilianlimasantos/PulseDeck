#include <stdio.h>
#include <vector>
#include "led_sr.h"
struct Sim : tg::Pa { uint32_t o=0x0; uint32_t dir=0xFFFF, pull=0xF; std::vector<int> bits; int latch=0; std::vector<int> ev;
 uint32_t out() override {return o;} void noPull(uint32_t m) override {pull&=~m; ev.push_back(1);} void dirOut(uint32_t m) override {dir&=~m; ev.push_back(3);} void delay() override {}
 void setOut(uint32_t v) override { if(!(dir&4)&&!(o&4)&&(v&4)) bits.push_back(!(v&8)); if(!(dir&1)&&!(o&1)&&(v&1)) latch++; if(v!=o) ev.push_back(2); o=v; } };
int main(){ Sim s; tg::LedSr l(s); uint32_t w=tg::LedSr::base()|tg::LedSr::led(0)|tg::LedSr::digit(5);
 l.send(w); uint32_t r=0; for(int i=0;i<(int)s.bits.size();i++) r=(r<<1)|s.bits[i];
 int ok=(s.bits.size()==29&&r==w&&s.latch==1&&(s.dir&0xF)==0&&s.pull==0&&s.ev[0]==1&&s.ev[1]==2&&s.ev[2]==3); l.send(w); ok&=(s.bits.size()==29);
 tg::LedSr l2(s); s.bits.clear(); l2.send(1u<<17); r=0; for(int b:s.bits) r=(r<<1)|b; ok&=(r==(1u<<17)); // LEDBIT-style single bit
 printf("%s word=%08x rx=%08x bits=%zu latch=%d\n",ok?"OK":"FAIL",w,r,s.bits.size(),s.latch); return !ok; }
