#include <iostream>

// Declare test functions
void test_node();
void test_workflow();
void test_agent();
void test_databus();

int main(int argc, char* argv[]) {
  std::cout << "AgentFlow Test Suite" << std::endl;
  std::cout << "===================" << std::endl << std::endl;

  int failed = 0;

  try {
    std::cout << "Running MMNode tests..." << std::endl;
    test_node();
    std::cout << "✓ MMNode tests passed" << std::endl << std::endl;
  } catch(const std::exception& e) {
    std::cerr << "✗ MMNode tests failed: " << e.what() << std::endl;
    failed++;
  }

  try {
    std::cout << "Running MMWorkflow tests..." << std::endl;
    test_workflow();
    std::cout << "✓ MMWorkflow tests passed" << std::endl << std::endl;
  } catch(const std::exception& e) {
    std::cerr << "✗ MMWorkflow tests failed: " << e.what() << std::endl;
    failed++;
  }

  try {
    std::cout << "Running MMAgent tests..." << std::endl;
    test_agent();
    std::cout << "✓ MMAgent tests passed" << std::endl << std::endl;
  } catch(const std::exception& e) {
    std::cerr << "✗ MMAgent tests failed: " << e.what() << std::endl;
    failed++;
  }

  try {
    std::cout << "Running MMDataBus tests..." << std::endl;
    test_databus();
    std::cout << "✓ MMDataBus tests passed" << std::endl << std::endl;
  } catch(const std::exception& e) {
    std::cerr << "✗ MMDataBus tests failed: " << e.what() << std::endl;
    failed++;
  }

  std::cout << "===================" << std::endl;
  if(failed == 0) {
    std::cout << "All tests passed!" << std::endl;
    return 0;
  } else {
    std::cout << failed << " test suite(s) failed!" << std::endl;
    return 1;
  }
}
