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
        std::cout << "Running Node tests..." << std::endl;
        test_node();
        std::cout << "✓ Node tests passed" << std::endl << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "✗ Node tests failed: " << e.what() << std::endl;
        failed++;
    }
    
    try {
        std::cout << "Running Workflow tests..." << std::endl;
        test_workflow();
        std::cout << "✓ Workflow tests passed" << std::endl << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "✗ Workflow tests failed: " << e.what() << std::endl;
        failed++;
    }
    
    try {
        std::cout << "Running Agent tests..." << std::endl;
        test_agent();
        std::cout << "✓ Agent tests passed" << std::endl << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "✗ Agent tests failed: " << e.what() << std::endl;
        failed++;
    }
    
    try {
        std::cout << "Running DataBus tests..." << std::endl;
        test_databus();
        std::cout << "✓ DataBus tests passed" << std::endl << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "✗ DataBus tests failed: " << e.what() << std::endl;
        failed++;
    }
    
    std::cout << "===================" << std::endl;
    if (failed == 0) {
        std::cout << "All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << failed << " test suite(s) failed!" << std::endl;
        return 1;
    }
}
