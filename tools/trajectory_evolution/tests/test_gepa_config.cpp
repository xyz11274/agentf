#include "../src/gepa_search.h"
#include <iostream>

int main(int argc, char** argv) {
  try {
    if(argc != 2) return 2;
    const auto config = trajectory::evolution::integration::load_gepa_config(argv[1]);
    const auto fingerprint = trajectory::evolution::integration::gepa_config_fingerprint(config);
    if(fingerprint.size() != 64) return 3;
    std::cout << "GEPA configuration validated\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
