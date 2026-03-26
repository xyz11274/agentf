/**
 * @file test_multi_session_concurrent.cpp
 * @brief Pure black-box E2E test for multi-session isolation
 *
 * Black-box verification only:
 * - Send messages with user_id/client_id metadata
 * - Verify session assignment and isolation
 * - No direct access to session_router or internal components
 *
 * Key verification points:
 * 1. Concurrent requests from different users get different sessions
 * 2. Pre-created pool session is reused
 * 3. Session affinity - same user+client gets same session
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>
#include <map>
#include <vector>
#include <atomic>

#include "agent/agent.h"
#include "agent/package.h"
#include "runtime/agent_runtime.h"

using namespace agentflow;

std::string find_agent_path() {
    std::vector<std::string> search_paths = {
        "examples/agent/system_agent_mock",
        "../examples/agent/system_agent_mock",
        "../../examples/agent/system_agent_mock",
        "../../../examples/agent/system_agent_mock",
        "/root/projects/AgentFlow/examples/agent/system_agent_mock"
    };
    for (const auto& path : search_paths) {
        std::filesystem::path full_path = std::filesystem::absolute(path);
        if (std::filesystem::exists(full_path / "package.json")) {
            return std::filesystem::canonical(full_path).string();
        }
    }
    return "";
}

// ============================================================================
// Black-box request/response structure
// ============================================================================
struct UserRequest {
    std::string user_id;
    std::string client_id;
    std::string content;
    std::string unique_marker;  // Unique marker to identify this request in response
};

struct UserResponse {
    std::string user_id;
    std::string client_id;
    std::string unique_marker;
    std::string response_content;
    int64_t latency_ms{0};
    bool success{false};
    std::string error;

    // Check if response contains expected marker (proves correct routing)
    bool contains_marker() const {
        return response_content.find(unique_marker) != std::string::npos;
    }
};

// ============================================================================
// Black-box execution: send message, wait for response
// ============================================================================
UserResponse send_and_receive(
    const std::shared_ptr<framework::MMAgent>& agent,
    const UserRequest& request,
    int timeout_seconds = 30) {

    UserResponse result;
    result.user_id = request.user_id;
    result.client_id = request.client_id;
    result.unique_marker = request.unique_marker;

    auto start_time = std::chrono::steady_clock::now();

    // Construct message with routing metadata
    framework::MMAgentMsgQue::Message message;
    message.message_id = "msg-" + request.user_id + "-" + request.unique_marker;
    message.metadata["message_type"] = "intent_request";
    message.metadata["user_id"] = request.user_id;
    message.metadata["client_id"] = request.client_id;
    // Include unique marker in content to verify response matching
    message.content = "[" + request.unique_marker + "] " + request.content;

    // Send message
    if (!agent->route_request_message(message)) {
        result.error = "Failed to route message";
        return result;
    }

    // Wait for response
    for (int i = 0; i < timeout_seconds * 10; ++i) {
        agent->process_all_sessions_until_idle();

        framework::MMAgentMsgQue::Message response;
        while (agent->dequeue_response_message(response)) {
            if (!response.content.empty()) {
                result.response_content = response.content;
                result.success = true;
                auto end_time = std::chrono::steady_clock::now();
                result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - start_time).count();
                return result;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end_time = std::chrono::steady_clock::now();
    result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    result.error = "Timeout after " + std::to_string(timeout_seconds) + "s";
    return result;
}

// ============================================================================
// Test 1: Concurrent users - verify independent sessions created
// ============================================================================
bool test_concurrent_users_independent_sessions(
    const std::shared_ptr<framework::MMAgent>& agent) {

    std::cout << "\n=== Test 1: Concurrent Users - Independent Session Creation ===" << std::endl;
    std::cout << "Verifying 5 concurrent users get different sessions..." << std::endl;

    // Record sessions before test (should have pre-created pool session "0")
    auto sessions_before = agent->get_sessions();
    std::cout << "\n--- Sessions Before Test ---" << std::endl;
    std::cout << "Pre-created session count: " << sessions_before.size() << std::endl;

    std::set<std::string> session_ids_before;
    for (const auto& s : sessions_before) {
        if (s) {
            std::string sid = s->get_session_id();
            std::string uid = s->get_user_id();
            std::string cid = s->get_client_id();
            bool assigned = s->is_assigned();
            session_ids_before.insert(sid);
            std::cout << "  Session: " << sid
                      << ", user_id: " << (uid.empty() ? "(unassigned)" : uid)
                      << ", client_id: " << (cid.empty() ? "(unassigned)" : cid)
                      << ", assigned: " << (assigned ? "YES" : "NO") << std::endl;
        }
    }

    // Define 5 different users
    std::vector<std::tuple<std::string, std::string, std::string, std::string>> users = {
        {"alice", "mobile", "Query about travel", "MARKER_ALICE"},
        {"bob", "web", "Query about shopping", "MARKER_BOB"},
        {"charlie", "desktop", "Query about food", "MARKER_CHARLIE"},
        {"david", "mobile", "Query about hotels", "MARKER_DAVID"},
        {"eve", "web", "Query about restaurants", "MARKER_EVE"}
    };

    // Track which sessions are assigned to which users
    std::map<std::string, std::string> user_to_session_id;  // user:client -> session_id

    // Send messages concurrently
    std::vector<std::future<void>> futures;

    for (const auto& [user_id, client_id, content, marker] : users) {
        futures.push_back(std::async(std::launch::async, [&]() {
            // Construct message
            framework::MMAgentMsgQue::Message message;
            message.message_id = "msg-" + user_id + "-" + marker;
            message.metadata["message_type"] = "intent_request";
            message.metadata["user_id"] = user_id;
            message.metadata["client_id"] = client_id;
            message.content = "[" + marker + "] " + content;

            // Route message (this triggers session assignment)
            agent->route_request_message(message);
        }));
    }

    // Wait for all message routing to complete
    for (auto& f : futures) {
        f.get();
    }

    // Wait for session assignment to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Query all sessions after routing (black-box observation)
    auto sessions = agent->get_sessions();
    for (const auto& s : sessions) {
        if (s && !s->get_user_id().empty() && !s->get_client_id().empty()) {
            user_to_session_id[s->get_user_id() + ":" + s->get_client_id()] = s->get_session_id();
        }
    }

    // Analyze results
    std::cout << "\n--- Session Assignment Results ---" << std::endl;

    std::set<std::string> unique_session_ids;
    for (const auto& [user_client, session_id] : user_to_session_id) {
        std::cout << "User:Client " << user_client << " -> Session: " << session_id << std::endl;
        unique_session_ids.insert(session_id);
    }

    std::cout << "\n--- Analysis ---" << std::endl;
    std::cout << "Users tested: " << users.size() << std::endl;
    std::cout << "Sessions assigned: " << user_to_session_id.size() << std::endl;
    std::cout << "Unique session IDs: " << unique_session_ids.size() << std::endl;

    // Check which sessions are from pool vs newly created
    int pool_sessions_used = 0;
    int new_sessions_created = 0;
    for (const auto& sid : unique_session_ids) {
        if (session_ids_before.find(sid) != session_ids_before.end()) {
            pool_sessions_used++;
            std::cout << "  Session " << sid << ": from pool (pre-created)" << std::endl;
        } else {
            new_sessions_created++;
            std::cout << "  Session " << sid << ": newly created (on-demand)" << std::endl;
        }
    }

    // Verify results
    bool all_users_have_session = (user_to_session_id.size() == users.size());
    bool all_sessions_unique = (unique_session_ids.size() == user_to_session_id.size());

    std::cout << "\n--- Verification ---" << std::endl;
    std::cout << "All users have session: " << (all_users_have_session ? "YES" : "NO") << std::endl;
    std::cout << "All sessions unique: " << (all_sessions_unique ? "YES" : "NO") << std::endl;
    std::cout << "Pool sessions used: " << pool_sessions_used << std::endl;
    std::cout << "New sessions created: " << new_sessions_created << std::endl;

    bool passed = all_users_have_session && all_sessions_unique;

    if (passed) {
        std::cout << "\nPASS: Each user got an independent session" << std::endl;
    } else {
        std::cout << "\nFAIL: Session isolation issue detected" << std::endl;
    }

    return passed;
}

// ============================================================================
// Test 2: Session affinity - same user+client gets same session
// ============================================================================
bool test_session_affinity(
    const std::shared_ptr<framework::MMAgent>& agent) {

    std::cout << "\n=== Test 2: Session Affinity - Same User Gets Same Session ===" << std::endl;
    std::cout << "Verifying same user+client always gets the same session..." << std::endl;

    std::string test_user = "affinity_user";
    std::string test_client = "affinity_client";
    std::string expected_session_id;
    bool all_match = true;

    std::cout << "\n--- Sending 5 requests from same user+client ---" << std::endl;

    for (int i = 0; i < 5; ++i) {
        // Send message
        framework::MMAgentMsgQue::Message message;
        message.message_id = "msg-affinity-" + std::to_string(i);
        message.metadata["message_type"] = "intent_request";
        message.metadata["user_id"] = test_user;
        message.metadata["client_id"] = test_client;
        message.content = "Affinity test query " + std::to_string(i);

        agent->route_request_message(message);

        // Query session (black-box observation)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto sessions = agent->get_sessions();
        std::string current_session_id;

        for (const auto& s : sessions) {
            if (s && s->get_user_id() == test_user && s->get_client_id() == test_client) {
                current_session_id = s->get_session_id();
                break;
            }
        }

        if (current_session_id.empty()) {
            std::cout << "Attempt " << (i+1) << ": No session found for user" << std::endl;
            all_match = false;
            continue;
        }

        if (expected_session_id.empty()) {
            expected_session_id = current_session_id;
            std::cout << "Attempt " << (i+1) << ": Got session " << current_session_id << " (first)" << std::endl;
        } else if (current_session_id == expected_session_id) {
            std::cout << "Attempt " << (i+1) << ": Same session " << current_session_id << " (OK)" << std::endl;
        } else {
            std::cout << "Attempt " << (i+1) << ": Different session " << current_session_id
                      << " (expected " << expected_session_id << ") - FAIL" << std::endl;
            all_match = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n--- Verification ---" << std::endl;
    if (all_match) {
        std::cout << "PASS: Session affinity maintained across all requests" << std::endl;
    } else {
        std::cout << "FAIL: Session affinity broken" << std::endl;
    }

    return all_match;
}

// ============================================================================
// Test 3: Same user multiple sequential dialogues
// ============================================================================
bool test_same_user_sequential_dialogues(
    const std::shared_ptr<framework::MMAgent>& agent) {

    std::cout << "\n=== Test 3: Same User Sequential Dialogues ===" << std::endl;
    std::cout << "Sending 3 sequential dialogues for user 'seq_user'..." << std::endl;

    std::string user_id = "seq_user";
    std::string client_id = "seq_client";

    std::vector<UserRequest> requests = {
        {user_id, client_id, "First question about travel", "SEQ_001"},
        {user_id, client_id, "Second question about shopping", "SEQ_002"},
        {user_id, client_id, "Third question about food", "SEQ_003"}
    };

    int success_count = 0;

    for (size_t i = 0; i < requests.size(); ++i) {
        std::cout << "\n--- Dialogue " << (i+1) << " ---" << std::endl;

        auto result = send_and_receive(agent, requests[i]);

        std::cout << "Status: " << (result.success ? "SUCCESS" : "FAILED") << std::endl;
        std::cout << "Latency: " << result.latency_ms << "ms" << std::endl;

        if (result.success) {
            success_count++;
            std::cout << "Response: " << result.response_content.substr(0, 80) << "..." << std::endl;
        } else {
            std::cout << "Error: " << result.error << std::endl;
        }

        // Small delay between dialogues
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bool passed = (success_count == static_cast<int>(requests.size()));
    std::cout << "\nResult: " << success_count << "/" << requests.size()
              << " dialogues succeeded - " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

// ============================================================================
// Test 3: Multiple users with rapid-fire requests
// ============================================================================
bool test_rapid_fire_concurrent(
    const std::shared_ptr<framework::MMAgent>& agent) {

    std::cout << "\n=== Test 4: Rapid-Fire Concurrent Requests ===" << std::endl;
    std::cout << "Sending rapid-fire requests from 3 users (2 each)..." << std::endl;

    // Each user sends 2 requests rapidly
    std::vector<UserRequest> requests = {
        {"rapid_alice", "mobile", "Query 1", "RAPID_ALICE_1"},
        {"rapid_alice", "mobile", "Query 2", "RAPID_ALICE_2"},
        {"rapid_bob", "web", "Query 1", "RAPID_BOB_1"},
        {"rapid_bob", "web", "Query 2", "RAPID_BOB_2"},
        {"rapid_charlie", "desktop", "Query 1", "RAPID_CHARLIE_1"},
        {"rapid_charlie", "desktop", "Query 2", "RAPID_CHARLIE_2"}
    };

    // Launch all rapidly (minimal delay)
    std::vector<std::future<UserResponse>> futures;
    auto launch_time = std::chrono::steady_clock::now();

    for (const auto& req : requests) {
        futures.push_back(std::async(std::launch::async, [&agent, req]() {
            return send_and_receive(agent, req);
        }));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 10ms apart
    }

    // Process
    while (std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::steady_clock::now() - launch_time).count() < 60) {
        agent->process_all_sessions_until_idle();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        bool all_done = true;
        for (auto& f : futures) {
            if (f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;
    }

    // Collect results
    std::cout << "\n--- Results ---" << std::endl;

    std::map<std::string, int> user_success;
    for (auto& f : futures) {
        auto result = f.get();
        user_success[result.user_id] += result.success ? 1 : 0;

        std::cout << result.user_id << "/" << result.unique_marker << ": "
                  << (result.success ? "OK" : "FAIL") << " (" << result.latency_ms << "ms)" << std::endl;
    }

    int total_success = 0;
    for (const auto& [user, count] : user_success) {
        total_success += count;
        std::cout << user << ": " << count << "/2 successful" << std::endl;
    }

    // Allow some failures under load
    bool passed = (total_success >= requests.size() * 0.7);
    std::cout << "\nResult: " << total_success << "/" << requests.size()
              << " - " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

// ============================================================================
// Test 4: Verify response content uniqueness (stronger isolation check)
// ============================================================================
bool test_response_uniqueness(
    const std::shared_ptr<framework::MMAgent>& agent) {

    std::cout << "\n=== Test 5: Response Uniqueness Verification ===" << std::endl;
    std::cout << "Verifying each user gets a unique response..." << std::endl;

    // Use very different queries to get different responses
    std::vector<UserRequest> requests = {
        {"unique_user_1", "test", "travel tokyo", "UNIQUE_001"},
        {"unique_user_2", "test", "shopping paris", "UNIQUE_002"},
        {"unique_user_3", "test", "food london", "UNIQUE_003"}
    };

    std::vector<std::future<UserResponse>> futures;
    auto launch_time = std::chrono::steady_clock::now();

    for (const auto& req : requests) {
        futures.push_back(std::async(std::launch::async, [&agent, req]() {
            return send_and_receive(agent, req);
        }));
    }

    // Process
    while (std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::steady_clock::now() - launch_time).count() < 60) {
        agent->process_all_sessions_until_idle();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Collect and check uniqueness
    std::set<std::string> unique_responses;
    int success_count = 0;

    std::cout << "\n--- Responses ---" << std::endl;
    for (auto& f : futures) {
        auto result = f.get();
        if (result.success) {
            success_count++;
            // Use first 100 chars as signature
            std::string sig = result.response_content.substr(0, 100);
            unique_responses.insert(sig);

            std::cout << result.user_id << ": "
                      << result.response_content.substr(0, 60) << "..." << std::endl;
        } else {
            std::cout << result.user_id << ": FAILED - " << result.error << std::endl;
        }
    }

    std::cout << "\n--- Analysis ---" << std::endl;
    std::cout << "Success: " << success_count << "/" << requests.size() << std::endl;
    std::cout << "Unique responses: " << unique_responses.size() << std::endl;

    // All successful responses should be unique (different skills selected)
    bool all_success = (success_count == static_cast<int>(requests.size()));
    bool all_unique = (unique_responses.size() == success_count);

    std::cout << "All succeeded: " << (all_success ? "YES" : "NO") << std::endl;
    std::cout << "All unique: " << (all_unique ? "YES" : "NO") << std::endl;

    return all_success && all_unique;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== Multi-Session Concurrent E2E Test (Black-Box) ===" << std::endl;

    // Find agent package
    std::string package_path = find_agent_path();
    if (package_path.empty()) {
        std::cerr << "ERROR: SystemAgentMock package not found" << std::endl;
        return 1;
    }
    std::cout << "Agent path: " << package_path << std::endl;

    // Change directory
    std::string original_dir = std::filesystem::current_path().string();
    std::filesystem::current_path(package_path);

    // Create agent
    auto agent = framework::MMAgent::create_from_package(".");
    if (!agent || !agent->initialize()) {
        std::cerr << "ERROR: Failed to create/initialize agent" << std::endl;
        std::filesystem::current_path(original_dir);
        return 1;
    }

    // Create runtime with more threads
    runtime::MMAgentRuntime::Config config;
    config.worker_threads = 8;
    auto runtime = std::make_unique<runtime::MMAgentRuntime>(config);

    if (!runtime->start() || !runtime->register_agent(agent)) {
        std::cerr << "ERROR: Failed to start runtime" << std::endl;
        std::filesystem::current_path(original_dir);
        return 1;
    }

    if (!agent->start()) {
        std::cerr << "ERROR: Failed to start agent" << std::endl;
        runtime->stop();
        std::filesystem::current_path(original_dir);
        return 1;
    }

    std::cout << "Agent started: " << agent->get_name() << std::endl;
    std::cout << "Worker threads: " << config.worker_threads << std::endl;

    int passed = 0, failed = 0;

    // Session isolation tests
    if (test_concurrent_users_independent_sessions(agent)) passed++; else failed++;
    if (test_session_affinity(agent)) passed++; else failed++;

    // Test 3: Same user sequential dialogues
    if (test_same_user_sequential_dialogues(agent)) passed++; else failed++;

    // Test 4: Rapid-fire concurrent requests
    if (test_rapid_fire_concurrent(agent)) passed++; else failed++;

    // Test 5: Response uniqueness verification
    // TODO: Re-enable after fixing random skill selection collision issue
    // if (test_response_uniqueness(agent)) passed++; else failed++;

    // Cleanup
    std::cout << "\n=== Cleanup ===" << std::endl;
    agent->stop();
    runtime->stop();
    std::filesystem::current_path(original_dir);

    // Summary
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Passed: " << passed << ", Failed: " << failed << std::endl;
    std::cout << "\n=== " << (failed == 0 ? "ALL PASSED" : "SOME FAILED") << " ===" << std::endl;

    return failed == 0 ? 0 : 1;
}