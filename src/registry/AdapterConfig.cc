// registry/AdapterConfig.cc — implementation of the shared adapter config
// decode and test-selection policy. See AdapterConfig.h (CLI-160 / CLI-161).

#include "registry/AdapterConfig.h"

#include <google/protobuf/any.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>

#include <algorithm>
#include <memory>
#include <set>

namespace acadia::client {

namespace {

// CLI-161: destructive test catalogue. Curated from the HAL test inventory:
// these tests can reboot the board, arm the watchdog, suspend/transition
// power state, or write to block devices. They are excluded from implicit
// "run everything" selections; an explicit test_names list from the server
// always overrides the gate.
const std::set<std::string>& destructive_tests() {
    static const std::set<std::string> kDestructive = {
        // watchdog arming / reset generation (can reboot the device)
        "testWatchdogTimerFunctionality",
        "testWatchdogTimeoutConfiguration",
        "testWatchdogResetGeneration",
        "testWatchdogInterruptGeneration",
        "testWatchdogReset",
        "testPoweronReset",
        "testHardwareResetButton",
        "testSoftwareReset",
        "testThermalReset",
        "testMcuWatchdog",
        // power state transitions (can suspend / brown-out the system)
        "testPowerStateTransitions",
        "testSleepStates",
        "testPowerLossProtection",
        // storage / memory writes (overwrite block devices, wear flash)
        "testSequentialWritePerformance",
        "testRandomWriteIops",
        "testNvmeReadwritePerformance",
        "testSequentialReadwritePerformance",
        "testRandomReadwritePerformance",
        "testPcieWriteBandwidth",
        // MCU firmware operations (can brick the co-processor)
        "testMcuFirmwareOperation",
        "testMcuFailoverMechanism",
    };
    return kDestructive;
}

} // namespace

bool is_destructive_test(const std::string& test_name) {
    return destructive_tests().count(test_name) != 0;
}

bool decode_adapter_config(const std::string& module_id,
                           const std::vector<uint8_t>& config_bytes,
                           AdapterRunConfig& out) {
    out = AdapterRunConfig{};
    if (config_bytes.empty()) return true;  // no config: module defaults

    google::protobuf::Any any;
    if (!any.ParseFromArray(config_bytes.data(),
                            static_cast<int>(config_bytes.size()))) {
        return false;
    }
    if (any.type_url().empty()) return true;  // empty Any: defaults

    const std::string kPrefix = "type.googleapis.com/";
    std::string type_name = any.type_url();
    if (type_name.rfind(kPrefix, 0) == 0) type_name.erase(0, kPrefix.size());

    // The expected type for this module is hal.modules.<module_id>.Config;
    // require the type URL to agree with the adapter's module id so one
    // module's config can never be interpreted by another module.
    const std::string expected = "hal.modules." + module_id + ".Config";
    if (type_name != expected) return false;

    const google::protobuf::Descriptor* desc =
        google::protobuf::DescriptorPool::generated_pool()
            ->FindMessageTypeByName(type_name);
    if (!desc) return false;  // module proto not compiled into this binary

    google::protobuf::DynamicMessageFactory factory;
    const google::protobuf::Message* prototype = factory.GetPrototype(desc);
    if (!prototype) return false;
    std::unique_ptr<google::protobuf::Message> msg(prototype->New());
    if (!msg || !any.UnpackTo(msg.get())) return false;

    const google::protobuf::Reflection* refl = msg->GetReflection();

    // repeated string test_names
    if (const auto* f = desc->FindFieldByName("test_names");
        f && f->is_repeated() &&
        f->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
        const int n = refl->FieldSize(*msg, f);
        out.test_names.reserve(static_cast<size_t>(n));
        std::string scratch;
        for (int i = 0; i < n; ++i)
            out.test_names.push_back(
                refl->GetRepeatedStringReference(*msg, f, i, &scratch));
    }

    // hal.common.CommonConfig common
    if (const auto* cf = desc->FindFieldByName("common");
        cf && cf->message_type() && refl->HasField(*msg, cf)) {
        const google::protobuf::Message& common = refl->GetMessage(*msg, cf);
        const google::protobuf::Descriptor* cd   = common.GetDescriptor();
        const google::protobuf::Reflection* cref = common.GetReflection();
        auto get_i32 = [&](const char* name, int& target) {
            if (const auto* f = cd->FindFieldByName(name);
                f && f->type() == google::protobuf::FieldDescriptor::TYPE_INT32 &&
                cref->HasField(common, f))
                target = cref->GetInt32(common, f);
        };
        get_i32("interval_ms", out.interval_ms);
        get_i32("poll_interval_ms", out.poll_interval_ms);
        if (const auto* f = cd->FindFieldByName("duration_ms");
            f && f->type() == google::protobuf::FieldDescriptor::TYPE_INT64 &&
            cref->HasField(common, f))
            out.duration_ms = cref->GetInt64(common, f);
        if (const auto* f = cd->FindFieldByName("run_forever");
            f && f->type() == google::protobuf::FieldDescriptor::TYPE_BOOL &&
            cref->HasField(common, f))
            out.run_forever = cref->GetBool(common, f);
    }

    out.config_present = true;
    return true;
}

std::vector<std::string> select_adapter_tests(std::vector<std::string> available_tests,
                                              const AdapterRunConfig& cfg) {
    // Explicit server-provided selection always wins (CLI-161): the server
    // chose these tests deliberately, including any destructive ones.
    if (!cfg.test_names.empty()) return cfg.test_names;

    // Implicit selection: never run destructive tests in periodic loops.
    std::vector<std::string> out;
    out.reserve(available_tests.size());
    for (auto& name : available_tests)
        if (!is_destructive_test(name)) out.push_back(std::move(name));
    return out;
}

void apply_adapter_config(TestBase& hal, const AdapterRunConfig& cfg) {
    ModuleRunConfig mc;
    mc.test_names      = cfg.test_names;
    mc.interval_ms     = cfg.interval_ms;
    mc.duration_ms     = cfg.duration_ms;
    mc.run_forever     = cfg.run_forever;
    mc.poll_interval_ms = cfg.poll_interval_ms;
    hal.configure(mc);
}

hal::TestResult make_config_error_result(const std::string& module_id,
                                         const std::string& message) {
    hal::TestResult r;
    r.module_id = module_id;
    r.test_id   = module_id;
    r.name      = "configure";
    r.output.status        = hal::Status::ERROR;
    r.output.severity      = hal::Severity::ERROR;
    r.output.error_message = message;
    return r;
}

} // namespace acadia::client