#include "agent_context_pack.h"
#include "context_internal.h"

namespace shin::context {
namespace {

struct PackOptions {
    fs::path input;
    bool stdin_input = false;
    bool require_current = false;
    bool strict = false;
    bool telemetry = false;
    PackProjection projection = PackProjection::full;
    int jobs = 0;
};

PackOptions parse_pack_options(const std::vector<std::string>& args) {
    PackOptions options;
    bool input_seen = false;
    bool jobs_seen = false;
    bool projection_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--input") {
            if (input_seen || index + 1 >= args.size()) {
                throw_usage_error(input_seen ? "duplicate-option" : "missing-option-value",
                                  input_seen ? "--input may be supplied only once."
                                             : "--input requires a file path or -.");
            }
            const std::string& value = args[++index];
            options.stdin_input = value == "-";
            if (!options.stdin_input) {
                options.input = fs::absolute(value);
            }
            input_seen = true;
        } else if (args[index] == "--require-current") {
            options.require_current = true;
        } else if (args[index] == "--strict") {
            options.strict = true;
        } else if (args[index] == "--telemetry") {
            options.telemetry = true;
        } else if (args[index] == "--projection") {
            if (projection_seen || index + 1 >= args.size()) {
                throw_usage_error(projection_seen ? "duplicate-option" : "missing-option-value",
                                  projection_seen ? "--projection may be supplied only once."
                                                  : "--projection requires full or compact.");
            }
            const std::string& value = args[++index];
            if (value == "full") {
                options.projection = PackProjection::full;
            } else if (value == "compact") {
                options.projection = PackProjection::compact;
            } else {
                throw_usage_error("invalid-option-value",
                                  "--projection requires full or compact.");
            }
            projection_seen = true;
        } else if (args[index] == "--jobs") {
            if (jobs_seen || index + 1 >= args.size()) {
                throw_usage_error(jobs_seen ? "duplicate-option" : "missing-option-value",
                                  jobs_seen ? "--jobs may be supplied only once."
                                            : "--jobs requires a value.");
            }
            options.jobs = parse_cli_integer(args[++index], "--jobs", 1);
            if (options.jobs > kMaxPackJobs) {
                throw_usage_error("invalid-option-value", "--jobs must not exceed 8.");
            }
            jobs_seen = true;
        } else {
            throw_usage_error(
                "unsupported-option",
                "pack only accepts --input, --jobs, --projection, --telemetry, "
                "--require-current, and --strict.");
        }
    }
    if (!input_seen) {
        throw_usage_error("missing-option", "pack requires --input FILE or --input -.");
    }
    return options;
}

}  // namespace

int run_pack(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const SteadyClock::time_point total_start = SteadyClock::now();
    const SteadyClock::time_point parse_start = SteadyClock::now();
    const PackOptions pack_options = parse_pack_options(args);
    const Json input = read_task_request_document(pack_options.input,
                                                  pack_options.stdin_input);
    const TaskRequest request = parse_task_request(input, options.profile);
    PackTelemetry telemetry;
    telemetry.request_parse_ms = elapsed_ms_since(parse_start);
    Json output;
    bool complete = false;
    {
        const SteadyClock::time_point session_start = SteadyClock::now();
        RetrievalSession session(options, pack_options.require_current);
        telemetry.session_open_ms = elapsed_ms_since(session_start);
        const AgentContextPack pack = execute_agent_context_pack(
            options, request, session, pack_options.jobs);
        telemetry.retrieval_ms = pack.retrieval_ms;
        telemetry.selection_ms = pack.selection_ms;
        complete = pack.completion.complete;
        const PackRenderOptions render_options = {
            pack_options.projection,
            pack_options.require_current,
            pack_options.strict
        };
        const SteadyClock::time_point projection_start = SteadyClock::now();
        output = render_agent_context_pack(options, render_options, request, session, pack);
        telemetry.projection_ms = elapsed_ms_since(projection_start);
    }
    const SteadyClock::time_point serialization_start = SteadyClock::now();
    std::string serialized = output.dump(2);
    telemetry.serialization_ms = elapsed_ms_since(serialization_start);
    telemetry.total_ms = elapsed_ms_since(total_start);
    telemetry.process_peak_rss_bytes = process_peak_rss_bytes();
    telemetry.projected_payload_bytes = serialized.size();
    if (pack_options.telemetry) {
        output["telemetry"] = pack_telemetry_json(telemetry);
        serialized = output.dump(2);
    }
    if (serialized.size() > kMaxPackOutputBytes) {
        throw CliError("context-pack-output-too-large", "resource",
                       "Context pack output exceeds the 16 MiB serialized limit.", 1, false,
                       "Reduce requirement count, record limits, or evidence byte budget.");
    }
    std::cout << serialized << '\n';
    return pack_options.strict && !complete ? 1 : 0;
}

}  // namespace shin::context
