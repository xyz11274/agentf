#include "agentflow/trajectory/runtime_config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace trajectory = agentflow::trajectory;

void expect(bool condition, const std::string& message) {
  if(!condition) {
    throw std::runtime_error(message);
  }
}

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const std::string& label) {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path()
        / ("agentflow-runtime-config-" + label + '-' + std::to_string(nonce));
    if(!std::filesystem::create_directories(path_)) {
      throw std::runtime_error("cannot create temporary test directory");
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if(!input) {
    throw std::runtime_error("cannot read test file");
  }
  return std::string(
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>());
}

void write_text(
    const std::filesystem::path& path,
    const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if(!output) {
    throw std::runtime_error("cannot write test file");
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if(!output.flush()) {
    throw std::runtime_error("cannot flush test file");
  }
}

std::filesystem::path copy_configuration(
    const TemporaryDirectory& temporary) {
  const auto source =
      trajectory::discover_runtime_configuration().parent_path();
  const auto destination = temporary.path() / "config";
  std::filesystem::copy(
      source, destination, std::filesystem::copy_options::recursive);
  return destination / "runtime.json";
}

void test_default_configuration() {
  const auto runtime = trajectory::load_runtime_configuration(
      trajectory::discover_runtime_configuration());
  expect(runtime.config_path.is_absolute(), "config path must be absolute");
  expect(runtime.source_files.size() == 9U,
         "snapshot must track the config and eight prompts");
  expect(runtime.evaluator.prompts != nullptr,
         "evaluator prompts must be configured");
  expect(runtime.evolution.prompts == runtime.evaluator.prompts,
         "one run must share one immutable prompt snapshot");
  expect(!runtime.evaluator.prompts->episode_system.empty(),
         "episode system prompt must load from disk");
  expect(!runtime.evaluator.prompts->dialogue_user.empty(),
         "dialogue user prompt must load from disk");
  expect(runtime.model.api_key.empty(),
         "runtime configuration must never contain an API key value");
}

void test_prompt_hot_replacement_uses_next_snapshot() {
  TemporaryDirectory temporary("reload");
  const auto config = copy_configuration(temporary);
  const auto first = trajectory::load_runtime_configuration(config);
  const std::string original = first.evaluator.prompts->episode_system;
  const std::string marker = "\nHOT_RELOAD_TEST_MARKER\n";
  write_text(
      config.parent_path() / "prompts/episode-system.txt",
      original + marker);

  const auto second = trajectory::load_runtime_configuration(config);
  expect(first.evaluator.prompts->episode_system == original,
         "an active batch snapshot must not change");
  expect(second.evaluator.prompts->episode_system == original + marker,
         "the next load must observe an edit without recompilation");
}

void test_template_data_cannot_become_template_syntax() {
  const std::string rendered = trajectory::render_prompt_template(
      "injection-test",
      "before {{evidence_json}} after",
      {{"evidence_json", "evidence {{not_a_placeholder}} remains data"}},
      1024U);
  expect(rendered
             == "before evidence {{not_a_placeholder}} remains data after",
         "inserted evidence must not be reparsed as template syntax");
}

void test_missing_placeholder_is_rejected() {
  TemporaryDirectory temporary("placeholder");
  const auto config = copy_configuration(temporary);
  write_text(
      config.parent_path() / "prompts/episode-user.txt",
      "this template has no evidence placeholder\n");
  bool rejected = false;
  try {
    (void)trajectory::load_runtime_configuration(config);
  } catch(const trajectory::EvaluationError&) {
    rejected = true;
  }
  expect(rejected, "a missing required placeholder must reject the snapshot");
}

void test_plaintext_api_key_field_is_rejected() {
  TemporaryDirectory temporary("secret-field");
  const auto config = copy_configuration(temporary);
  std::string json = read_text(config);
  const std::string allowed = "\"api_key_env\": \"\"";
  const std::string forbidden = "\"api_key\": \"not-allowed\"";
  const std::size_t position = json.find(allowed);
  expect(position != std::string::npos, "test config must contain api_key_env");
  json.replace(position, allowed.size(), forbidden);
  write_text(config, json);

  bool rejected = false;
  try {
    (void)trajectory::load_runtime_configuration(config);
  } catch(const trajectory::EvaluationError&) {
    rejected = true;
  }
  expect(rejected, "a plaintext api_key field must be rejected");
}

}  // namespace

int main() {
  try {
    test_default_configuration();
    test_prompt_hot_replacement_uses_next_snapshot();
    test_template_data_cannot_become_template_syntax();
    test_missing_placeholder_is_rejected();
    test_plaintext_api_key_field_is_rejected();
    std::cout << "runtime configuration tests passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "runtime configuration test failed: " << error.what() << '\n';
    return 1;
  }
}
