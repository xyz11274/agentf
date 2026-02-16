#!/usr/bin/env ruby
# Test Ruby script for hot reload functionality
# Version: 1.0

require 'json'
require 'time'

version = "1.0"

puts "[RUBY_SCRIPT] Version: #{version}"
puts "[RUBY_SCRIPT] Arguments: #{ARGV.join(', ')}"
puts "[RUBY_SCRIPT] Ruby version: #{RUBY_VERSION}"

# Environment variables
puts "[RUBY_SCRIPT] Environment:"
puts "[RUBY_SCRIPT]   HOME=#{ENV['HOME']}"
puts "[RUBY_SCRIPT]   USER=#{ENV['USER']}"
puts "[RUBY_SCRIPT]   PATH=#{ENV['PATH'] ? ENV['PATH'][0..50]}... : 'undefined'}"

# Output data
timestamp = Time.now.to_i
output = {
  result: "success",
  status: "ok",
  timestamp: timestamp,
  ruby_version: RUBY_VERSION
}

puts "[RUBY_SCRIPT] Output: #{output.to_json}"

# Simulate some work
start = Time.now
while (Time.now - start) < 0.1
  # Wait 100ms
end

puts "[RUBY_SCRIPT] Execution complete"
