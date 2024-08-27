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
std::map<uint64_t, std::map<uint64_t, uint64_t>> b = 
  {
{ 10, {{ 1090.0, 10.0 },{ 618060.0, 3.0 },{ 627470.0, 5.0 },{ 627950.0, 1.0 },{ 671360.0, 7.0 },}},
{ 15, {{ 673.333, 15.0 },{ 387460.0, 1.0 },{ 403086.667, 4.0 },{ 408926.667, 7.0 },{ 509746.667, 10.0 },}},
{ 100, {{ 98.0, 100.0 },{ 59471.0, 10.0 },{ 61207.0, 30.0 },{ 61587.0, 50.0 },{ 64235.0, 70.0 },}},
{ 150, {{ 66.0, 150.0 },{ 39157.333, 45.0 },{ 39373.333, 105.0 },{ 41424.0, 15.0 },{ 47416.0, 75.0 },}},
{ 1000, {{ 12.0, 1.0 },{ 6029.0, 500.0 },{ 6048.4, 700.0 },{ 6343.4, 300.0 },{ 6631.0, 100.0 },}},
{ 1500, {{ 5.933, 1.5 },{ 4060.533, 750.0 },{ 4077.467, 1.05 },{ 4326.8, 150.0 },{ 4403.867, 450.0 },}},
{ 10000, {{ 1.72, 10.0 },{ 553.11, 7.0 },{ 561.6, 5.0 },{ 571.08, 1.0 },{ 625.93, 3.0 },}},
{ 15000, {{ 3.0, 15.0 },{ 359.393, 10.5 },{ 368.24, 7.5 },{ 382.273, 1.5 },{ 413.98, 4.5 },}},
{ 30000, {{ 0.88, 30.0 },{ 142.803, 21.0 },{ 159.133, 15.0 },{ 184.217, 9.0 },{ 191.75, 3.0 },}},
{ 50000, {{ 0.952, 50.0 },{ 56.936, 35.0 },{ 87.67, 15.0 },{ 101.664, 25.0 },{ 109.576, 5.0 },}},
{ 70000, {{ 0.657, 70.0 },{ 20.113, 49.0 },{ 39.187, 35.0 },{ 55.316, 21.0 },{ 76.839, 7.0 },}},
{ 80000, {{ 0.281, 80.0 },{ 8.652, 56.0 },{ 29.223, 40.0 },{ 42.341, 24.0 },{ 64.831, 8.0 },}},
{ 90000, {{ 1.226, 90.0 },{ 8.47, 62.999 },{ 19.572, 45.0 },{ 33.941, 27.0 },{ 54.194, 9.0 },}},
{ 100000, {{ 0.191, 100.0 },{ 0.792, 70.0 },{ 10.283, 50.0 },{ 30.152, 30.0 },{ 47.804, 10.0 },}},
{ 150000, {{ 0.083, 150.0 },{ 0.148, 105.0 },{ 0.407, 75.0 },{ 8.577, 45.0 },{ 27.945, 15.0 },}},
{ 1000000, {{ 0.016, 500.0 },{ 0.021, 1.0 },{ 0.06, 700.0 },{ 0.111, 100.0 },}},
{ 1500000, {{ 0.014, 750.0 },{ 0.034, 150.0 },{ 0.048, 1.5 },{ 0.223, 1.05 },}},
{ 10000000, {{ 0.002, 10.0 },{ 23.632, 7.0 },{ 88.096, 5.0 },{ 152.078, 3.0 },{ 205.157, 1.0 },}},
{ 15000000, {{ 0.001, 15.0 },{ 27.57, 10.5 },{ 88.467, 7.5 },{ 147.835, 4.5 },{ 217.344, 1.5 },}},
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
