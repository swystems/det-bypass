#include "sleep.hpp"
#include <iostream>

int main(){
  auto th =  get_threshold(15, 556000); 
  std::cout<< th <<std::endl;
  pp_sleep(15, 550000.0);
  return 0;
}
