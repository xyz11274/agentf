#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <string>

// Define the interface for hot reload test functions
extern "C" {

/**
 * Execute function - This is the default function that Node::execute_so_library calls
 * @param inputs Input data map (key-value pairs)
 * @param outputs Output data map (key-value pairs)
 */
void execute(const std::map<std::string, std::string>* inputs,
             std::map<std::string, std::string>*       outputs) {
  std::cout << "[SO_LIB] execute called" << std::endl;

  // Process inputs
  if(inputs && !inputs->empty()) {
    std::cout << "[SO_LIB] Inputs: " << inputs->size() << std::endl;
    for(const auto& pair : *inputs) {
      std::cout << "[SO_LIB]   " << pair.first << " = " << pair.second << std::endl;
    }
  }

  // Set outputs
  if(outputs) {
    (*outputs)["result"]    = "success";
    (*outputs)["version"]   = "1.0";
    (*outputs)["message"]   = "Hot reload test library executed successfully";
    (*outputs)["timestamp"] = std::to_string(std::time(nullptr));
  }

  std::cout << "[SO_LIB] Execution complete" << std::endl;
}

/**
 * Execute version 2 - Simulates a new version of the function
 */
void execute_v2(const std::map<std::string, std::string>* inputs,
                std::map<std::string, std::string>*       outputs) {
  std::cout << "[SO_LIB] execute_v2 called" << std::endl;

  // Process inputs
  if(inputs && !inputs->empty()) {
    std::cout << "[SO_LIB] Inputs: " << inputs->size() << std::endl;
    for(const auto& pair : *inputs) {
      std::cout << "[SO_LIB]   " << pair.first << " = " << pair.second << std::endl;
    }
  }

  // Set outputs with version 2 data
  if(outputs) {
    (*outputs)["result"]    = "success";
    (*outputs)["version"]   = "2.0";
    (*outputs)["message"]   = "Hot reload test library v2 executed successfully";
    (*outputs)["timestamp"] = std::to_string(std::time(nullptr));
    (*outputs)["upgraded"]  = "true";
  }

  std::cout << "[SO_LIB] Execution complete" << std::endl;
}

/**
 * Execute version 3 - Simulates a newer version
 */
void execute_v3(const std::map<std::string, std::string>* inputs,
                std::map<std::string, std::string>*       outputs) {
  std::cout << "[SO_LIB] execute_v3 called" << std::endl;

  // Process inputs
  if(inputs && !inputs->empty()) {
    std::cout << "[SO_LIB] Inputs: " << inputs->size() << std::endl;
    for(const auto& pair : *inputs) {
      std::cout << "[SO_LIB]   " << pair.first << " = " << pair.second << std::endl;
    }
  }

  // Set outputs with version 3 data
  if(outputs) {
    (*outputs)["result"]    = "success";
    (*outputs)["version"]   = "3.0";
    (*outputs)["message"]   = "Hot reload test library v3 executed successfully";
    (*outputs)["timestamp"] = std::to_string(std::time(nullptr));
    (*outputs)["upgraded"]  = "true";
    (*outputs)["features"]  = "new_features_added";
  }

  std::cout << "[SO_LIB] Execution complete" << std::endl;
}

/**
 * Add function - Simulates addition operation
 */
void add(const std::map<std::string, std::string>* inputs,
         std::map<std::string, std::string>*       outputs) {
  std::cout << "[SO_LIB] add called" << std::endl;

  int a = 0, b = 0;

  if(inputs) {
    auto it_a = inputs->find("a");
    if(it_a != inputs->end()) {
      a = std::stoi(it_a->second);
    }

    auto it_b = inputs->find("b");
    if(it_b != inputs->end()) {
      b = std::stoi(it_b->second);
    }
  }

  int result = a + b;

  if(outputs) {
    (*outputs)["result"]    = std::to_string(result);
    (*outputs)["a"]         = std::to_string(a);
    (*outputs)["b"]         = std::to_string(b);
    (*outputs)["operation"] = "add";
  }

  std::cout << "[SO_LIB] " << a << " + " << b << " = " << result << std::endl;
}

/**
 * Multiply function - Simulates multiplication operation
 */
void multiply(const std::map<std::string, std::string>* inputs,
              std::map<std::string, std::string>*       outputs) {
  std::cout << "[SO_LIB] multiply called" << std::endl;

  int a = 0, b = 0;

  if(inputs) {
    auto it_a = inputs->find("a");
    if(it_a != inputs->end()) {
      a = std::stoi(it_a->second);
    }

    auto it_b = inputs->find("b");
    if(it_b != inputs->end()) {
      b = std::stoi(it_b->second);
    }
  }

  int result = a * b;

  if(outputs) {
    (*outputs)["result"]    = std::to_string(result);
    (*outputs)["a"]         = std::to_string(a);
    (*outputs)["b"]         = std::to_string(b);
    (*outputs)["operation"] = "multiply";
  }

  std::cout << "[SO_LIB] " << a << " * " << b << " = " << result << std::endl;
}

/**
 * Get info function - Returns library information
 */
void get_info(const std::map<std::string, std::string>* inputs,
              std::map<std::string, std::string>*       outputs) {
  std::cout << "[SO_LIB] get_info called" << std::endl;

  (void)inputs;

  if(outputs) {
    (*outputs)["name"]        = "Hot Reload Test Library";
    (*outputs)["version"]     = "1.0";
    (*outputs)["author"]      = "AgentFlow";
    (*outputs)["description"] = "Test library for hot reload functionality";
    (*outputs)["build_date"]  = __DATE__;
    (*outputs)["build_time"]  = __TIME__;
  }
}

}  // extern "C"
