#pragma once
#include <string>
inline std::string jsonString(const char *value) {
  std::string out="\"";
  const char *hex="0123456789abcdef";
  for(const unsigned char *p=(const unsigned char*)value;*p;++p) {
    if(*p=='"' || *p=='\\') {out+='\\';out+=char(*p);}
    else if(*p<32) {out+="\\u00";out+=hex[*p>>4];out+=hex[*p&15];}
    else out+=char(*p);
  }
  return out+'"';
}
