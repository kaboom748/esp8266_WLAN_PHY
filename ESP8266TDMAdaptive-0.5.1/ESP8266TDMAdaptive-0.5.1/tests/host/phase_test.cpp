#include <cassert>
#include <cmath>
#include <cstdio>
#include "FskPhase.h"
int main(){
  for(int deg=-179;deg<=179;deg++){
    for(int shift=0;shift<=16;shift+=4){
      double radians=deg*3.141592653589793/180;
      int32_t amplitude=10000<<shift,cross,dot;
      FskPhase::correlate(amplitude,0,int32_t(amplitude*cos(radians)),
        int32_t(amplitude*sin(radians)),cross,dot);
      double estimated=FskPhase::angle(cross,dot)*360.0/65536;
      assert(fabs(estimated-deg)<4.2);
    }
  }
  int32_t cross,dot;
  FskPhase::correlate(INT32_MIN,INT32_MAX,INT32_MAX,INT32_MIN,cross,dot);
  assert(cross || dot);
  assert(FskPhase::angle(0,0)==0);
  puts("PASS integer phase: quadrants, dynamic range, error below 4.2 degrees");
}
