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

#include "agent/data_variant_utils.h"
#include "agent/common.h"
#include "utils/logger.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/ostreamwrapper.h"
#include "rapidjson/stringbuffer.h"
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


#ifdef AGENTFLOW_WITH_RAPIDJSON
namespace {
rapidjson::Value make_json_string(const std::string& value,
                                  rapidjson::Document::AllocatorType& allocator) {
  return rapidjson::Value(value.c_str(), value.length(), allocator);
}

rapidjson::Value file_descriptor_to_rapidjson(const FileDescriptor& descriptor,
                                              rapidjson::Document::AllocatorType& allocator) {
  rapidjson::Value object(rapidjson::kObjectType);
  object.AddMember("file_id", make_json_string(descriptor.file_id, allocator), allocator);
  object.AddMember("uri", make_json_string(descriptor.uri, allocator), allocator);
  object.AddMember("storage_backend", make_json_string(descriptor.storage_backend, allocator), allocator);
  object.AddMember("media_type", make_json_string(descriptor.media_type, allocator), allocator);
  object.AddMember("size_bytes", descriptor.size_bytes, allocator);
  object.AddMember("checksum", make_json_string(descriptor.checksum, allocator), allocator);
  object.AddMember("created_at", descriptor.created_at, allocator);
  object.AddMember("ttl_seconds", descriptor.ttl_seconds, allocator);
  object.AddMember("owner", make_json_string(descriptor.owner, allocator), allocator);
  if(descriptor.metadata_json.empty()) {
    object.AddMember("metadata", rapidjson::Value(rapidjson::kObjectType), allocator);
  } else {
    rapidjson::Document metadata_doc;
    metadata_doc.Parse(descriptor.metadata_json.c_str());
    if(metadata_doc.HasParseError()) {
      object.AddMember("metadata", make_json_string(descriptor.metadata_json, allocator), allocator);
    } else {
      rapidjson::Value metadata_value;
      metadata_value.CopyFrom(metadata_doc, allocator);
      object.AddMember("metadata", metadata_value, allocator);
    }
  }
  return object;
}

bool rapidjson_to_file_descriptor(const rapidjson::Value& value, FileDescriptor& descriptor) {
  if(!value.IsObject()) {
    return false;
  }
  FileDescriptor parsed;
  if(value.HasMember("file_id") && value["file_id"].IsString()) parsed.file_id = value["file_id"].GetString();
  if(value.HasMember("uri") && value["uri"].IsString()) parsed.uri = value["uri"].GetString();
  if(value.HasMember("storage_backend") && value["storage_backend"].IsString()) parsed.storage_backend = value["storage_backend"].GetString();
  if(value.HasMember("media_type") && value["media_type"].IsString()) parsed.media_type = value["media_type"].GetString();
  if(value.HasMember("size_bytes") && value["size_bytes"].IsInt64()) parsed.size_bytes = value["size_bytes"].GetInt64();
  if(value.HasMember("checksum") && value["checksum"].IsString()) parsed.checksum = value["checksum"].GetString();
  if(value.HasMember("created_at") && value["created_at"].IsInt64()) parsed.created_at = value["created_at"].GetInt64();
  if(value.HasMember("ttl_seconds") && value["ttl_seconds"].IsInt64()) parsed.ttl_seconds = value["ttl_seconds"].GetInt64();
  else if(value.HasMember("ttl") && value["ttl"].IsInt64()) parsed.ttl_seconds = value["ttl"].GetInt64();
  if(value.HasMember("owner") && value["owner"].IsString()) parsed.owner = value["owner"].GetString();
  if(value.HasMember("metadata")) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value["metadata"].Accept(writer);
    parsed.metadata_json = buffer.GetString();
  }
  if(!is_file_descriptor_value(parsed)) {
    return false;
  }
  descriptor = std::move(parsed);
  return true;
}

bool rapidjson_to_data_variant(const rapidjson::Value& value, DataVariant& output) {
  if(value.IsInt64()) { output = value.GetInt64(); return true; }
  if(value.IsDouble()) { output = value.GetDouble(); return true; }
  if(value.IsBool()) { output = value.GetBool(); return true; }
  if(value.IsString()) { output = std::string(value.GetString()); return true; }
  if(value.IsObject()) {
    FileDescriptor descriptor;
    if(rapidjson_to_file_descriptor(value, descriptor)) {
      output = std::move(descriptor);
      return true;
    }
  }
  if(value.IsArray()) {
    FileDescriptorList descriptors;
    for(auto it = value.Begin(); it != value.End(); ++it) {
      FileDescriptor descriptor;
      if(!rapidjson_to_file_descriptor(*it, descriptor)) {
        return false;
      }
      descriptors.push_back(std::move(descriptor));
    }
    output = std::move(descriptors);
    return true;
  }
  return false;
}

rapidjson::Value data_variant_to_rapidjson(const DataVariant& value,
                                           rapidjson::Document::AllocatorType& allocator) {
  return std::visit(
      [&allocator](const auto& arg) -> rapidjson::Value {
        using T = std::decay_t<decltype(arg)>;
        rapidjson::Value out;
        if constexpr(std::is_same_v<T, int64_t>) {
          out.SetInt64(arg);
          return out;
        } else if constexpr(std::is_same_v<T, double>) {
          out.SetDouble(arg);
          return out;
        } else if constexpr(std::is_same_v<T, bool>) {
          out.SetBool(arg);
          return out;
        } else if constexpr(std::is_same_v<T, std::string>) {
          return make_json_string(arg, allocator);
        } else if constexpr(std::is_same_v<T, FileDescriptor>) {
          return file_descriptor_to_rapidjson(arg, allocator);
        } else {
          out.SetArray();
          for(const auto& descriptor : arg) {
            out.PushBack(file_descriptor_to_rapidjson(descriptor, allocator), allocator);
          }
          return out;
        }
      },
      value);
}
}  // namespace
#endif

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

  if(doc.IsObject()) {
    for(auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
      DataVariant parsed;
      if(rapidjson_to_data_variant(it->value, parsed)) {
        inputs[it->name.GetString()] = std::move(parsed);
      }
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
    rapidjson::Value key_val(pair.first.c_str(), pair.first.length(), allocator);
    doc.AddMember(key_val, data_variant_to_rapidjson(pair.second, allocator), allocator);
  }

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
