#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "agent/package.h"
#include "workflow/dsl/workflow_parser.h"

namespace agentflow {
namespace tests {

namespace {
std::filesystem::path repo_root() {
  namespace fs = std::filesystem;
  const fs::path cwd = fs::current_path();
  const std::vector<fs::path> candidates = {cwd, cwd / "..", cwd / "../.."};
  for(const auto& c : candidates) {
    if(fs::exists(c / "publish/workflow/README.md")) {
      return fs::absolute(c);
    }
  }
  return fs::absolute(cwd);
}
}  // namespace

TEST(BuiltinWorkflowTest, BuiltinWorkflowAssetsExist) {
  namespace fs = std::filesystem;
  const fs::path base = repo_root() / "publish/workflow";
  EXPECT_TRUE(fs::exists(base / "memory/workflow.json"));
  EXPECT_TRUE(fs::exists(base / "skills/workflow.json"));
  EXPECT_TRUE(fs::exists(base / "tools/workflow.json"));
}

TEST(BuiltinWorkflowTest, PackageBuiltinWorkflowsAreResolvedAndParseable) {
  namespace fs = std::filesystem;
  const fs::path tmp = repo_root() / "build/test_builtin_pkg";
  fs::create_directories(tmp);

  const fs::path package_json = tmp / "package.json";
  std::ofstream  ofs(package_json);
  ofs << R"({
    "name": "builtin-test-agent",
    "version": "1.0.0",
    "entry_point": "builtin_test.so",
    "builtin_workflows": ["memory", "tools"],
    "coordinator_workflow": "tools/workflow.json"
  })";
  ofs.close();

  auto package = framework::AgentPackage::load_from_directory(tmp.string());
  ASSERT_NE(package, nullptr);
  const auto& wf_files = package->get_config().workflow_files;
  ASSERT_GE(wf_files.size(), 2u);
  EXPECT_EQ(package->get_config().coordinator_workflow, "tools/workflow.json");

  bool has_memory = false;
  bool has_tools = false;
  for(const auto& wf : wf_files) {
    if(wf.find("memory/workflow.json") != std::string::npos) {
      has_memory = true;
    }
    if(wf.find("tools/workflow.json") != std::string::npos) {
      has_tools = true;
    }
  }
  EXPECT_TRUE(has_memory);
  EXPECT_TRUE(has_tools);

  auto parser = dsl::WorkflowParserFactory::create();
  for(const auto& wf : wf_files) {
    if(wf.find("builtin-") != std::string::npos || wf.find("workflow.json") != std::string::npos) {
      auto result = parser->parse_workflow_file(wf);
      EXPECT_TRUE(result.is_valid()) << "Failed to parse workflow file: " << wf;
    }
  }

  fs::remove_all(tmp);
}

}  // namespace tests
}  // namespace agentflow
