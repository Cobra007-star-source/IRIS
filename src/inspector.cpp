// =============================================================================
// inspector.cpp
// Iterative Tarjan SCC on $ref graph; cycles force slow path (Phase 4 skeleton)
// =============================================================================
#include "iris/inspector.hpp"

#include <algorithm>
#include <stack>
#include <sstream>

namespace iris {

namespace {

struct TarjanState {
    std::vector<std::int32_t> index;
    std::vector<std::int32_t> lowlink;
    std::vector<bool>         on_stack;
    std::vector<std::uint32_t> stack;
    std::int32_t              counter = 0;
    bool                      has_cycle = false;
};

void tarjan_strongconnect(const std::vector<std::vector<std::uint32_t>>& g,
                          TarjanState& st, std::uint32_t v) {
    // iterative version: (v, child_index)
    struct Frame { std::uint32_t v; std::size_t i; };
    std::stack<Frame> work;

    st.index[v]    = st.counter;
    st.lowlink[v]  = st.counter;
    ++st.counter;
    st.stack.push_back(v);
    st.on_stack[v] = true;
    work.push({v, 0});

    while (!work.empty()) {
        auto& [u, i] = work.top();
        if (i < g[u].size()) {
            std::uint32_t w = g[u][i];
            ++i;
            if (st.index[w] == -1) {
                st.index[w]   = st.counter;
                st.lowlink[w] = st.counter;
                ++st.counter;
                st.stack.push_back(w);
                st.on_stack[w] = true;
                work.push({w, 0});
            } else if (st.on_stack[w]) {
                st.lowlink[u] = std::min(st.lowlink[u], st.index[w]);
            }
        } else {
            if (st.lowlink[u] == st.index[u]) {
                std::size_t members = 0;
                std::uint32_t w;
                do {
                    w = st.stack.back();
                    st.stack.pop_back();
                    st.on_stack[w] = false;
                    ++members;
                } while (w != u);
                if (members > 1) st.has_cycle = true;
            }
            work.pop();
            if (!work.empty()) {
                auto& parent = work.top();
                st.lowlink[parent.v] = std::min(st.lowlink[parent.v], st.lowlink[u]);
            }
        }
    }
}

}  // namespace

InspectionResult inspect(const SchemaInspectionInput& input) noexcept {
    InspectionResult r;
    const std::size_t n = input.ref_graph.size();

    TarjanState st;
    st.index.assign(n, -1);
    st.lowlink.assign(n, -1);
    st.on_stack.assign(n, false);

    // self-loop detected immediately
    for (std::size_t i = 0; i < n; ++i) {
        for (auto t : input.ref_graph[i]) {
            if (t == static_cast<std::uint32_t>(i)) {
                r.verdict = InspectionVerdict::kForceSlowPath;
                r.reason  = "self-loop in $ref graph";
                return r;
            }
        }
    }

    for (std::uint32_t i = 0; i < n; ++i) {
        if (st.index[i] == -1) tarjan_strongconnect(input.ref_graph, st, i);
    }

    if (st.has_cycle) {
        r.verdict = InspectionVerdict::kForceSlowPath;
        r.reason  = "non-trivial SCC detected ($ref cycle)";
        return r;
    }
    if (input.has_unevaluated_properties) {
        r.verdict = InspectionVerdict::kForceSlowPath;
        r.reason  = "unevaluatedProperties present";
        return r;
    }
    if (input.max_depth > 64) {
        r.verdict = InspectionVerdict::kForceSlowPath;
        std::ostringstream oss;
        oss << "nesting depth " << input.max_depth << " > 64";
        r.reason = oss.str();
        return r;
    }
    return r;
}

}  // namespace iris
