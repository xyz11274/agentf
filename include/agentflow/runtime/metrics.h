// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Metrics Collection and Monitoring

#ifndef AGENTFLOW_RUNTIME_METRICS_H
#define AGENTFLOW_RUNTIME_METRICS_H

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <functional>
#include <memory>
#include <chrono>

namespace agentflow {
namespace runtime {

/**
 * @brief Metric types for different kinds of measurements
 */
enum class MetricType {
    COUNTER,   // Monotonically increasing counter (e.g., requests processed)
    GAUGE,     // Instantaneous value (e.g., current queue size)
    HISTOGRAM  // Distribution of values (e.g., latency)
};

/**
 * @brief Histogram buckets for measuring distributions
 */
struct HistogramBuckets {
    std::vector<double> boundaries;
    std::map<int, int64_t> counts;

    HistogramBuckets(const std::vector<double>& bounds)
        : boundaries(bounds) {
        for (size_t i = 0; i <= bounds.size(); ++i) {
            counts[static_cast<int>(i)] = 0;
        }
    }

    void record(double value) {
        for (size_t i = 0; i < boundaries.size(); ++i) {
            if (value <= boundaries[i]) {
                counts[static_cast<int>(i)]++;
                return;
            }
        }
        // Value exceeds all buckets
        counts[static_cast<int>(boundaries.size())]++;
    }

    double get_percentile(double p) const {
        int64_t total = 0;
        for (const auto& pair : counts) {
            total += pair.second;
        }

        if (total == 0) return 0.0;

        int64_t target = static_cast<int64_t>(total * p / 100.0);
        int64_t sum = 0;

        for (const auto& pair : counts) {
            sum += pair.second;
            if (sum >= target) {
                if (pair.first < static_cast<int>(boundaries.size())) {
                    return boundaries[pair.first];
                }
                return boundaries.empty() ? 0.0 : boundaries.back();
            }
        }

        return boundaries.empty() ? 0.0 : boundaries.back();
    }
};

/**
 * @brief Base metric class
 */
class Metric {
public:
    Metric(const std::string& name, const std::string& description,
           const std::map<std::string, std::string>& labels = {})
        : name_(name), description_(description), labels_(labels),
          created_at_(std::chrono::steady_clock::now()) {}

    virtual ~Metric() = default;

    const std::string& get_name() const { return name_; }
    const std::string& get_description() const { return description_; }
    const std::map<std::string, std::string>& get_labels() const { return labels_; }
    MetricType get_type() const { return type_; }
    std::chrono::steady_clock::time_point get_created_at() const { return created_at_; }

    virtual std::string to_string() const = 0;
    virtual void reset() = 0;

protected:
    std::string name_;
    std::string description_;
    std::map<std::string, std::string> labels_;
    MetricType type_;
    std::chrono::steady_clock::time_point created_at_;
    mutable std::mutex mutex_;
};

/**
 * @brief Counter metric - monotonically increasing value
 */
class CounterMetric : public Metric {
public:
    CounterMetric(const std::string& name, const std::string& description,
               const std::map<std::string, std::string>& labels = {})
        : Metric(name, description, labels) {
        type_ = MetricType::COUNTER;
    }

    void increment() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_++;
    }

    void increment_by(int64_t delta) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ += delta;
    }

    int64_t get_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    std::string to_string() const override {
        return name_ + "=" + std::to_string(get_value());
    }

    void reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = 0;
    }

private:
    int64_t value_ = 0;
};

/**
 * @brief Gauge metric - instantaneous value that can go up and down
 */
class GaugeMetric : public Metric {
public:
    GaugeMetric(const std::string& name, const std::string& description,
              const std::map<std::string, std::string>& labels = {})
        : Metric(name, description, labels) {
        type_ = MetricType::GAUGE;
    }

    void set(double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = value;
    }

    void increment() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_++;
    }

    void decrement() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_--;
    }

    void add(double delta) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ += delta;
    }

    double get_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    std::string to_string() const override {
        return name_ + "=" + std::to_string(get_value());
    }

    void reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = 0.0;
    }

private:
    double value_ = 0.0;
};

/**
 * @brief Histogram metric - distribution of values
 */
class HistogramMetric : public Metric {
public:
    HistogramMetric(const std::string& name, const std::string& description,
                 const std::vector<double>& buckets = {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000},
                 const std::map<std::string, std::string>& labels = {})
        : Metric(name, description, labels), histogram_(buckets) {
        type_ = MetricType::HISTOGRAM;
        default_buckets_ = buckets;
    }

    void record(double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        histogram_.record(value);
        count_++;
        sum_ += value;
    }

    double get_p50() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return histogram_.get_percentile(50.0);
    }

    double get_p90() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return histogram_.get_percentile(90.0);
    }

    double get_p95() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return histogram_.get_percentile(95.0);
    }

    double get_p99() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return histogram_.get_percentile(99.0);
    }

    double get_average() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ > 0 ? sum_ / count_ : 0.0;
    }

    int64_t get_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    double get_sum() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sum_;
    }

    std::string to_string() const override {
        return name_ + "[count=" + std::to_string(get_count()) +
               ", avg=" + std::to_string(get_average()) + "]";
    }

    void reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        histogram_ = HistogramBuckets(default_buckets_);
        count_ = 0;
        sum_ = 0.0;
    }

private:
    HistogramBuckets histogram_;
    std::vector<double> default_buckets_;
    int64_t count_ = 0;
    double sum_ = 0.0;
};

/**
 * @brief Metrics collector - aggregates and exports metrics
 */
class MetricsCollector {
public:
    MetricsCollector();
    ~MetricsCollector() = default;

    // Metric registration
    void register_metric(std::shared_ptr<Metric> metric);
    void unregister_metric(const std::string& name);

    // Metric access
    std::shared_ptr<Metric> get_metric(const std::string& name);
    std::vector<std::shared_ptr<Metric>> get_all_metrics();

    // Metric creation helpers
    std::shared_ptr<CounterMetric> create_counter(
        const std::string& name, const std::string& description,
        const std::map<std::string, std::string>& labels = {});

    std::shared_ptr<GaugeMetric> create_gauge(
        const std::string& name, const std::string& description,
        const std::map<std::string, std::string>& labels = {});

    std::shared_ptr<HistogramMetric> create_histogram(
        const std::string& name, const std::string& description,
        const std::vector<double>& buckets = {},
        const std::map<std::string, std::string>& labels = {});

    // Export functionality
    std::string export_prometheus() const;
    std::string export_json() const;
    std::string export_text() const;

    // Reset all metrics
    void reset_all();

private:
    std::map<std::string, std::shared_ptr<Metric>> metrics_;
    mutable std::mutex metrics_mutex_;
};

/**
 * @brief Factory for creating metrics collectors
 */
class MetricsCollectorFactory {
public:
    static std::shared_ptr<MetricsCollector> create();
    static std::shared_ptr<MetricsCollector> get_default();
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_METRICS_H
