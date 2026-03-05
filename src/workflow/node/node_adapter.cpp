// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// MMNode Adapter - Standalone executable for node execution

#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <map>
#include <variant>

#include "workflow/node.h"
#include "agent/common.h"
#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/ostreamwrapper.h"
#include "rapidjson/writer.h"
#endif

using namespace agentflow;
using namespace agentflow::framework;

// Parse JSON config file and create MMNode config
bool parse_node_config(const std::string& config_file, MMNode::Config& config) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  std::ifstream file(config_file);
  if(!file.is_open()) {
    Logger::instance().error("Failed to open config file: " + config_file);
    return false;
  }

  rapidjson::IStreamWrapper isw(file);
  rapidjson::Document doc;
  doc.ParseStream(isw);

  if(doc.HasParseError()) {
    Logger::instance().error("JSON parse error in config file");
    return false;
  }

  // Parse basic node configuration
  if(doc.HasMember("id") && doc["id"].IsString()) {
    config.id = doc["id"].GetString();
  }

  if(doc.HasMember("name") && doc["name"].IsString()) {
    config.name = doc["name"].GetString();
  }

  if(doc.HasMember("type") && doc["type"].IsString()) {
    config.type = doc["type"].GetString();
  }

  if(doc.HasMember("priority") && doc["priority"].IsInt()) {
    config.priority = doc["priority"].GetInt();
  }

  // Parse execution mode
  if(doc.HasMember("execution_mode") && doc["execution_mode"].IsString()) {
    std::string mode = doc["execution_mode"].GetString();
    if(mode == "CPP_FUNCTION") {
      config.execution_mode = MMNode::ExecutionMode::CPP_FUNCTION;
    } else if(mode == "SHELL_SCRIPT") {
      config.execution_mode = MMNode::ExecutionMode::SHELL_SCRIPT;
    } else if(mode == "EXECUTABLE") {
      config.execution_mode = MMNode::ExecutionMode::EXECUTABLE;
    } else if(mode == "GO_SCRIPT") {
      config.execution_mode = MMNode::ExecutionMode::GO_SCRIPT;
    } else if(mode == "PYTHON_SCRIPT") {
      config.execution_mode = MMNode::ExecutionMode::PYTHON_SCRIPT;
    } else if(mode == "JAVA_JAR") {
      config.execution_mode = MMNode::ExecutionMode::JAVA_JAR;
    } else if(mode == "SO_LIBRARY") {
      config.execution_mode = MMNode::ExecutionMode::SO_LIBRARY;
    }
  }

  if(doc.HasMember("command") && doc["command"].IsString()) {
    config.command = doc["command"].GetString();
  }

  if(doc.HasMember("working_dir") && doc["working_dir"].IsString()) {
    config.working_dir = doc["working_dir"].GetString();
  }

  if(doc.HasMember("max_retries") && doc["max_retries"].IsInt()) {
    config.max_retries = doc["max_retries"].GetInt();
  }

  if(doc.HasMember("timeout_ms") && doc["timeout_ms"].IsInt()) {
    config.timeout_ms = doc["timeout_ms"].GetInt();
  }

  if(doc.HasMember("memory_limit_mb") && doc["memory_limit_mb"].IsInt()) {
    config.memory_limit_mb = doc["memory_limit_mb"].GetInt();
  }

  if(doc.HasMember("enable_hot_reload") && doc["enable_hot_reload"].IsBool()) {
    config.enable_hot_reload = doc["enable_hot_reload"].GetBool();
  }

  // Parse input and output parameters
  if(doc.HasMember("input_params") && doc["input_params"].IsArray()) {
    for(const auto& param : doc["input_params"].GetArray()) {
      if(param.IsString()) {
        config.input_params.push_back(param.GetString());
      }
    }
  }

  if(doc.HasMember("output_params") && doc["output_params"].IsArray()) {
    for(const auto& param : doc["output_params"].GetArray()) {
      if(param.IsString()) {
        config.output_params.push_back(param.GetString());
      }
    }
  }

  // Parse additional arguments
  if(doc.HasMember("args") && doc["args"].IsArray()) {
    for(const auto& arg : doc["args"].GetArray()) {
      if(arg.IsString()) {
        config.args.push_back(arg.GetString());
      }
    }
  }

  Logger::instance().info("MMNode config parsed: id=" + config.id + ", name=" + config.name);
  return true;
#else
  Logger::instance().warn("RapidJSON not available - using basic JSON parsing");
  // Stub implementation - would need alternative JSON parser
  return false;
#endif
}

// Parse JSON inputs file
DataMap parse_inputs(const std::string& inputs_file) {
  DataMap inputs;

#ifdef AGENTFLOW_WITH_RAPIDJSON
  std::ifstream file(inputs_file);
  if(!file.is_open()) {
    Logger::instance().warn("No inputs file provided or not found");
    return inputs;
  }

  rapidjson::IStreamWrapper isw(file);
  rapidjson::Document doc;
  doc.ParseStream(isw);

  if(doc.HasParseError()) {
    Logger::instance().error("JSON parse error in inputs file");
    return inputs;
  }

  // Parse inputs object
  if(doc.IsObject()) {
    for(auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
      std::string key = it->name.GetString();
      const auto& val = it->value;

      if(val.IsInt64()) {
        inputs[key] = val.GetInt64();
      } else if(val.IsDouble()) {
        inputs[key] = val.GetDouble();
      } else if(val.IsBool()) {
        inputs[key] = val.GetBool();
      } else if(val.IsString()) {
        inputs[key] = std::string(val.GetString());
      }
      // For now, we only support basic types
    }
  }

  Logger::instance().info("Inputs parsed: " + std::to_string(inputs.size()) + " entries");
#else
  Logger::instance().warn("RapidJSON not available - cannot parse inputs");
#endif

  return inputs;
}

// Write output results to JSON file
bool write_outputs(const std::string& output_file, const DataMap& outputs) {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& allocator = doc.GetAllocator();

  for(const auto& pair : outputs) {
    const std::string& key = pair.first;
    const auto& val = pair.second;

    // Create key as Value with allocator
    rapidjson::Value key_val(key.c_str(), key.length(), allocator);

    std::visit(
        [&doc, &key_val, &allocator](auto&& arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, int64_t>) {
            doc.AddMember(key_val, arg, allocator);
          } else if constexpr(std::is_same_v<T, double>) {
            doc.AddMember(key_val, arg, allocator);
          } else if constexpr(std::is_same_v<T, bool>) {
            doc.AddMember(key_val, arg, allocator);
          } else if constexpr(std::is_same_v<T, std::string>) {
            rapidjson::Value str_val(arg.c_str(), arg.length(), allocator);
            doc.AddMember(key_val, str_val, allocator);
          }
        },
        val);
  }

  // Write to file
  std::ofstream file(output_file);
  if(!file.is_open()) {
    Logger::instance().error("Failed to open output file: " + output_file);
    return false;
  }

  rapidjson::OStreamWrapper osw(file);
  rapidjson::Writer<rapidjson::OStreamWrapper> writer(osw);
  doc.Accept(writer);

  Logger::instance().info("Results written to: " + output_file);
  return true;
#else
  Logger::instance().warn("RapidJSON not available - cannot write outputs");
  return false;
#endif
}

// Print usage information
void print_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " [OPTIONS]\n"
            << "Options:\n"
            << "  --config <file>    MMNode config JSON file (required)\n"
            << "  --inputs <file>    MMNode inputs JSON file (optional)\n"
            << "  --output <file>    Output JSON file for results (default: output.json)\n"
            << "  --help             Show this help message\n";
}

// Main entry point
int main(int argc, char* argv[]) {
  // Initialize logger
  Logger::instance().info("MMNode Adapter initializing");

  // Parse command line arguments
  std::string config_file;
  std::string inputs_file;
  std::string output_file = "output.json";

  for(int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if(arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if(arg == "--config" && i + 1 < argc) {
      config_file = argv[++i];
    } else if(arg == "--inputs" && i + 1 < argc) {
      inputs_file = argv[++i];
    } else if(arg == "--output" && i + 1 < argc) {
      output_file = argv[++i];
    }
  }

  // Validate required arguments
  if(config_file.empty()) {
    std::cerr << "Error: --config argument is required\n";
    print_usage(argv[0]);
    return 1;
  }

  Logger::instance().info("MMNode Adapter started");
  Logger::instance().info("Config file: " + config_file);
  if(!inputs_file.empty()) {
    Logger::instance().info("Inputs file: " + inputs_file);
  }
  Logger::instance().info("Output file: " + output_file);

  // Parse node configuration
  MMNode::Config node_config;
  if(!parse_node_config(config_file, node_config)) {
    Logger::instance().error("Failed to parse node config");
    return 1;
  }

  // Parse inputs if provided
  DataMap inputs;
  if(!inputs_file.empty()) {
    inputs = parse_inputs(inputs_file);
  }

  // Create and initialize node
  MMNode node(node_config);
  if(!node.initialize()) {
    Logger::instance().error("Failed to initialize node");
    return 1;
  }

  Logger::instance().info("MMNode initialized successfully");

  // Set inputs
  node.set_inputs(inputs);

  // Execute node
  Logger::instance().info("Executing node...");
  if(!node.execute()) {
    Logger::instance().error("MMNode execution failed");
    DataMap error_output;
    error_output["error"] = std::string("MMNode execution failed");
    error_output["status"] = std::string("FAILED");
    write_outputs(output_file, error_output);
    return 1;
  }

  Logger::instance().info("MMNode execution completed successfully");

  // Get outputs
  const DataMap& outputs = node.get_outputs();
  DataMap result_output = outputs;
  result_output["status"] = std::string("SUCCESS");

  // Write results
  if(!write_outputs(output_file, result_output)) {
    Logger::instance().error("Failed to write output file");
    return 1;
  }

  // Cleanup
  if(!node.cleanup()) {
    Logger::instance().warn("MMNode cleanup completed with warnings");
  }

  Logger::instance().info("MMNode Adapter finished successfully");

  return 0;
}
