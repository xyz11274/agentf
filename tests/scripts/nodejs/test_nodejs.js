#!/usr/bin/env node
/**
 * Test Node.js script for hot reload functionality
 * Version: 1.0
 */

const version = "1.0";

console.log("[NODEJS_SCRIPT] Version:", version);
console.log("[NODEJS_SCRIPT] Arguments:", process.argv.slice(2));
console.log("[NODEJS_SCRIPT] Node version:", process.version);

// Environment variables
console.log("[NODEJS_SCRIPT] Environment:");
console.log("[NODEJS_SCRIPT]   HOME:", process.env.HOME);
console.log("[NODEJS_SCRIPT]   USER:", process.env.USER);
console.log("[NODEJS_SCRIPT]   PATH:", process.env.PATH ? process.env.PATH.substring(0, 50) + "..." : "undefined");

// Output data
const timestamp = Math.floor(Date.now() / 1000);
const output = {
    result: "success",
    status: "ok",
    timestamp: timestamp,
    nodeVersion: process.version
};

console.log("[NODEJS_SCRIPT] Output:", JSON.stringify(output));

// Simulate some work
const start = Date.now();
while (Date.now() - start < 100) {
    // Wait 100ms
}

console.log("[NODEJS_SCRIPT] Execution complete");
