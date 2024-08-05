#include <algorithm>
#include <map>
#include <algorithm>
#include <vector>
#include <cstdint> 
#include <time.h>
#include "sleep.hpp"
#include <iostream>

void pp_sleep(uint64_t ns, double err){
  uint64_t threshold = get_threshold(ns, err);
  if( ns == 0 ){
    return;
  }
  uint64_t start = get_time_ns();
  if(ns >  threshold){
    uint64_t sleep_duration = ns - threshold;
    struct timespec t;
    t.tv_sec = sleep_duration/ 1000000000;
    t.tv_nsec = sleep_duration% 1000000000;
    nanosleep (&t, NULL);
    uint64_t diff = get_time_ns() - start;
    if(diff > ns){
         return;
    }
    ns -= diff;
  }
  uint64_t new_start = get_time_ns();
  while(get_time_ns() - new_start < ns){
    BARRIER();
  }
}



template<typename K, typename V> std::vector<K> getKeys(const std::map<K, V>& myMap) {
    std::vector<K> keys;
    for (const auto& pair : myMap) {
        keys.push_back(pair.first);
    }
    return keys;
}

uint64_t get_threshold(uint64_t interval, uint64_t err){
  std::map<uint64_t, std::map<uint64_t, uint64_t>> interval_to_err_to_threshold = {
    {10, {{1020, 10.0}, {555410.0, 5.0}, {556980.0, 1.0},{562360.0, 3.0},{571610.0, 7.0}}}
  };
  std::map<uint64_t, std::map<uint64_t, uint64_t>> a = {
{ 10, {{ 960.0, 10.0 },{ 550800.0, 7.0 },{ 556930.0, 1.0 },{ 557270.0, 3.0 },{ 560520.0, 5.0 }
,}},
{ 15, {{ 686.667, 15.0 },{ 367846.667, 4.0 },{ 372733.333, 7.0 },{ 380853.333, 10.0 },{ 391333.333, 1.0 },}},
};
  std::vector<uint64_t> intervals = getKeys(interval_to_err_to_threshold);
  auto closest_interval = std::lower_bound(intervals.begin(), intervals.end() ,interval);
  if(closest_interval == intervals.end()){
    closest_interval -=1;
  }
  const uint64_t inter = *closest_interval;
  std::vector<uint64_t> errors = getKeys(interval_to_err_to_threshold.at(inter));
  auto closest_error = std::lower_bound(errors.begin(), errors.end(), err);
  if (closest_error == errors.end()){
    closest_error -= 1;
  }
  auto error = *closest_error;
  return interval_to_err_to_threshold.at(inter).at(error);

}

inline uint64_t get_time_ns (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}
