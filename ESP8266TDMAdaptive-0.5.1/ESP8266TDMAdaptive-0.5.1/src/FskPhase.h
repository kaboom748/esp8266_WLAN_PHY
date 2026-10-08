#ifndef ESP8266_FSK_PHASE_H
#define ESP8266_FSK_PHASE_H
#include <stdint.h>

namespace FskPhase {
inline uint32_t magnitude(int32_t x){return x<0?0u-uint32_t(x):uint32_t(x);}
inline void correlate(int32_t ai,int32_t aq,int32_t bi,int32_t bq,int32_t& cross,int32_t& dot){
  uint32_t peak=magnitude(ai)|magnitude(aq)|magnitude(bi)|magnitude(bq);
  if(!peak){cross=dot=0;return;}
  int shift=19-__builtin_clz(peak);
  if(shift>0){ai>>=shift;aq>>=shift;bi>>=shift;bq>>=shift;}
  cross=ai*bq-aq*bi;dot=ai*bi+aq*bq;
  peak=magnitude(cross)|magnitude(dot);
  shift=peak?20-__builtin_clz(peak):0;
  if(shift>0){cross>>=shift;dot>>=shift;}
}
// One turn is 65536. The bounded approximation is sufficient for tone slicing.
inline int32_t angle(int32_t y,int32_t x){
  if(!x && !y)return 0;
  int32_t ay=magnitude(y),a;
  if(x>=0)a=8192-8192*(x-ay)/(x+ay);
  else a=24576-8192*(x+ay)/(ay-x);
  return y<0?-a:a;
}
}
#endif
