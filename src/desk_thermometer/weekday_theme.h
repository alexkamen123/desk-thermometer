#pragma once
#include "clock_status.h"
// Candidate rotation policy: Mon..Fri = 0..4, weekends keep Friday.
// Compute from date instead of trusting an independently set RTC weekday register.
inline unsigned weekdayForDate(const ClockStamp &stamp) {
  if (!stamp.valid || stamp.month<1 || stamp.month>12 || stamp.day<1 || stamp.day>31) return 0;
  static const unsigned offsets[]={0,3,2,5,0,3,5,1,4,6,2,4};
  unsigned year=stamp.year-(stamp.month<3 ? 1 : 0);
  unsigned weekday=(year+year/4-year/100+year/400+offsets[stamp.month-1]+stamp.day)%7;
  return weekday;
}

inline unsigned coverForDate(const ClockStamp &stamp) {
  if (!stamp.valid || stamp.month<1 || stamp.month>12 || stamp.day<1 || stamp.day>31) return 0;
  const unsigned weekday=weekdayForDate(stamp);
  return weekday==0 || weekday==6 ? 4 : weekday-1;
}
