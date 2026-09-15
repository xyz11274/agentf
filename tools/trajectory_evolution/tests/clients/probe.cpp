#include "../../clients/cpp/evolution.hpp"
#include <cstdlib>
#include <iostream>
int main() {
  if(te_initialize() != 0) return 2;
  trajectory_evolution_http::Client client(std::getenv("EVOLUTION_URL"), std::getenv("EVOLUTION_TEST_TOKEN"), 7000, 2097152, 0);
  for(std::string line; std::getline(std::cin, line);) {
    try { std::cout << client.invoke(line) << std::endl; }
    catch(const trajectory_evolution_http::Error& error) { std::cout << "{\"ok\":false,\"error\":\"" << error.what() << "\",\"status\":" << error.status << "}" << std::endl; }
  }
  te_cleanup(); return 0;
}
