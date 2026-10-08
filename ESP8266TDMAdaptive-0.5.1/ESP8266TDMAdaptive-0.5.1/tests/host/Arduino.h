#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>
extern uint32_t fakeMs, tickMs;
extern long randomResult;
inline uint32_t millis(){return fakeMs;}
inline uint32_t micros(){return fakeMs*1000u;}
inline void yield(){fakeMs+=tickMs;}
inline void delay(uint32_t ms){fakeMs+=ms;}
inline long random(long n){return n?randomResult%n:0;}
#define HEX 16
struct NullSerial{
 std::string output;
 size_t maxWrite=4096;
 size_t write(const uint8_t* data,size_t n){n=std::min(n,maxWrite);output.append(reinterpret_cast<const char*>(data),n);return n;}
 void flush(){}
 template<class T> void print(T){}
 template<class T> void print(T,int){}
 template<class T> void println(T){}
 void println(){}
};
extern NullSerial Serial;
