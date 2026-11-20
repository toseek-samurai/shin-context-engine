#pragma once

struct DisconnectRequest {
    std::uint64_t ticket;
    std::string project, root, name;
};

struct DisconnectResult {
    DisconnectRequest request;
    bool disconnected;
    std::string detail;
};

class ProjectDisconnection {
public:
    std::optional<DisconnectRequest> begin(const std::string& project, const std::string& root, const std::string& name) {
        if (pending_ || project.empty() || root.empty()) return {};
        pending_ = DisconnectRequest{++ticket_, project, root, name};
        result_.reset();
        return pending_;
    }
    const DisconnectResult* complete(std::uint64_t ticket, bool disconnected, std::string detail) {
        if (!pending_ || ticket != pending_->ticket) return nullptr;
        result_ = DisconnectResult{std::move(*pending_), disconnected, std::move(detail)};
        pending_.reset();
        return &*result_;
    }
    bool busy() const { return pending_.has_value(); }
    bool pending_for(const std::string& project, const std::string& root) const {
        return pending_ && pending_->project == project && pending_->root == root;
    }
    const DisconnectResult* result() const { return result_ ? &*result_ : nullptr; }
    const DisconnectResult* result_for(const std::string& project, const std::string& root) const {
        const auto* reply = result();
        return reply && reply->request.project == project && reply->request.root == root ? reply : nullptr;
    }
private:
    std::uint64_t ticket_ = 0;
    std::optional<DisconnectRequest> pending_;
    std::optional<DisconnectResult> result_;
};

ProjectDisconnection project_disconnection;
void platform_disconnect(const DisconnectRequest& request);

std::string disconnect_result_text(const DisconnectResult& result, bool name_project) {
    std::string message = shin::ui::tr(result.disconnected ? "Project disconnected" : "Could not disconnect the project");
    if (name_project && !result.request.name.empty()) message += " · " + result.request.name;
    if (!result.detail.empty()) message += "\n" + result.detail;
    return message;
}
