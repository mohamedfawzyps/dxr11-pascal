// C++ port of phase5/rewriter/nvapi.py. Mirrors its structure on purpose, so
// the two can be held byte-identical; see nvapi_fold.h.
#include "nvapi_fold.h"

#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace rq {
namespace {

const char* kNoCluster = "-1";   // 0xFFFFFFFF as LLVM prints an i32

bool IsOp(const std::string& op, int* v) {
    if (op.empty()) return false;
    size_t i = op[0] == '-' ? 1 : 0;
    if (i >= op.size()) return false;
    for (size_t k = i; k < op.size(); ++k)
        if (op[k] < '0' || op[k] > '9') return false;
    *v = std::stoi(op);
    return true;
}

bool NameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.';
}

// `name` not followed by a name character, as the Python's
// re.escape(name) + r'(?![\w.])'.
bool Uses(const std::string& line, const std::string& name) {
    for (size_t p = line.find(name); p != std::string::npos; p = line.find(name, p + 1)) {
        size_t e = p + name.size();
        if (e >= line.size() || !NameChar(line[e])) return true;
    }
    return false;
}

std::string Substitute(const std::string& line, const std::string& name,
                       const std::string& with) {
    std::string r;
    size_t at = 0;
    for (size_t p = line.find(name); p != std::string::npos;) {
        size_t e = p + name.size();
        if (e >= line.size() || !NameChar(line[e])) {
            r += line.substr(at, p - at);
            r += with;
            at = e;
            p = line.find(name, e);
        } else {
            p = line.find(name, p + 1);
        }
    }
    r += line.substr(at);
    return r;
}

bool EndsBlock(const std::string& line) {
    size_t b = line.find_first_not_of(" \t");
    size_t e = line.find_last_not_of(" \t\r");
    std::string s = b == std::string::npos ? std::string() : line.substr(b, e - b + 1);
    auto starts = [&](const char* p) { return s.compare(0, std::strlen(p), p) == 0; };
    return starts("br ") || starts("ret") || starts("switch ") || starts("unreachable") ||
           (!s.empty() && s.back() == ':') || starts("; <label>:") || s == "}";
}

}  // namespace

bool FoldNvapi(const std::string& in, std::string* out, std::string* why) {
    static const std::regex kRecord(
        std::string(R"RX(^!\d+ = !\{i32 (\d+), )RX") +
        R"RX(%"class\.RWStructuredBuffer<NvShaderExtnStruct>"\* [^,]+, !"[^"]*", i32 (\d+), i32 (\d+), )RX");
    static const std::regex kDef(R"RX(^\s*(%[\w.]+) = )RX");
    static const std::regex kCounter(
        R"RX(@dx\.op\.bufferUpdateCounter\(i32 70, %dx\.types\.Handle (%[\w.]+), i8 1\))RX");
    static const std::regex kStore(
        R"RX(@dx\.op\.(?:rawBufferStore|bufferStore)\.\w+\(i32 (?:140|69), )RX"
        R"RX(%dx\.types\.Handle (%[\w.]+), i32 (%[\w.]+), i32 (\d+), i32 (%?[\w.-]+),)RX");
    static const std::regex kAnnotate(
        R"RX(@dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle (%[\w.]+),)RX");
    static const std::regex kHandleUse(R"RX(%dx\.types\.Handle (%[\w.]+))RX");
    static const std::regex kCallee(R"RX( @(dx\.op\.[\w.]+)\()RX");

    std::vector<std::string> lines;
    for (size_t s = 0;;) {
        size_t n = in.find('\n', s);
        if (n == std::string::npos) { lines.push_back(in.substr(s)); break; }
        lines.push_back(in.substr(s, n - s));
        s = n + 1;
    }

    std::set<std::tuple<int, int, int>> bindings;
    std::smatch m;
    for (const auto& ln : lines)
        if (std::regex_search(ln, m, kRecord))
            bindings.insert({std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3])});
    if (bindings.empty()) { *out = in; return true; }

    std::set<std::string> ext;
    for (const auto& ln : lines) {
        std::smatch d;
        if (!std::regex_search(ln, d, kDef)) continue;
        for (const auto& b : bindings) {
            int rid = std::get<0>(b), space = std::get<1>(b), lb = std::get<2>(b);
            std::string fromBinding =
                "@dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 " +
                std::to_string(lb) + ", i32 ";
            std::string spaceTail = ", i32 " + std::to_string(space) + ", i8 1 }";
            std::string legacy = "@dx.op.createHandle(i32 57, i8 1, i32 " + std::to_string(rid) + ", ";
            if ((ln.find(fromBinding) != std::string::npos &&
                 ln.find(spaceTail) != std::string::npos) ||
                ln.find(legacy) != std::string::npos)
                ext.insert(d[1]);
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& ln : lines) {
            std::smatch d, a;
            if (std::regex_search(ln, d, kDef) && std::regex_search(ln, a, kAnnotate) &&
                ext.count(a[1]) && !ext.count(d[1])) {
                ext.insert(d[1]);
                changed = true;
            }
        }
    }

    std::set<size_t> drop;
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<std::string> indices;
    for (size_t i = 0; i < lines.size();) {
        std::smatch c, d;
        bool isC = std::regex_search(lines[i], c, kCounter);
        bool isD = std::regex_search(lines[i], d, kDef);
        if (!(isC && isD && ext.count(c[1])) || drop.count(i)) { ++i; continue; }
        std::string index = d[1], op;
        bool haveOp = false;
        std::vector<size_t> stores;
        size_t second = 0;
        bool haveSecond = false;
        for (size_t j = i + 1; j < lines.size() && !EndsBlock(lines[j]); ++j) {
            std::smatch s;
            if (std::regex_search(lines[j], s, kStore) && ext.count(s[1]) && s[2] == index) {
                stores.push_back(j);
                if (s[3] == "0") { op = s[4]; haveOp = true; }
            }
            std::smatch c2, d2;
            if (std::regex_search(lines[j], c2, kCounter) &&
                std::regex_search(lines[j], d2, kDef) && ext.count(c2[1])) {
                second = j;
                haveSecond = true;
                break;
            }
        }
        int v = 0;
        if (!haveOp || !IsOp(op, &v)) {
            *why = "uses the NVAPI shader extension UAV in a way that is not a "
                   "recognisable call; the driver reads it as an intrinsic, and it "
                   "cannot be moved";
            return false;
        }
        if ((v != 94 && v != 95) || !haveSecond) {
            *why = "uses NVAPI shader extension op " + op + ", which has no DXR 1.0 "
                   "lowering here (only the RayQuery cluster ID calls, 94 and 95, "
                   "are folded)";
            return false;
        }
        drop.insert(i);
        drop.insert(second);
        for (size_t k : stores) drop.insert(k);
        std::smatch d2;
        std::regex_search(lines[second], d2, kDef);
        bool seen = false;
        for (auto& p : values)
            if (p.first == d2[1].str()) { p.second = kNoCluster; seen = true; }
        if (!seen) values.push_back({d2[1].str(), kNoCluster});
        indices.push_back(index);
        i = second + 1;
    }

    std::vector<std::string> o;
    for (size_t k = 0; k < lines.size(); ++k)
        if (!drop.count(k)) o.push_back(lines[k]);
    for (const auto& p : values)
        for (auto& ln : o) ln = Substitute(ln, p.first, p.second);
    for (const auto& idx : indices)
        for (const auto& ln : o)
            if (Uses(ln, idx)) {
                *why = "an NVAPI call index is used beyond the call";
                return false;
            }

    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = 0; k < o.size(); ++k) {
            std::smatch d, a;
            if (std::regex_search(o[k], d, kDef) && std::regex_search(o[k], a, kAnnotate) &&
                ext.count(d[1])) {
                std::string name = d[1];
                bool used = false;
                for (size_t n = 0; n < o.size() && !used; ++n)
                    if (n != k && Uses(o[n], name)) used = true;
                if (!used) {
                    o.erase(o.begin() + k);
                    changed = true;
                    break;
                }
            }
        }
    }
    for (const auto& ln : o) {
        std::smatch d;
        if (std::regex_search(ln, d, kDef) && ext.count(d[1])) continue;
        for (std::sregex_iterator it(ln.begin(), ln.end(), kHandleUse), end; it != end; ++it)
            if (ext.count((*it)[1])) {
                *why = "uses the NVAPI shader extension UAV outside a cluster ID "
                       "call, which cannot be moved";
                return false;
            }
    }

    std::set<std::string> callees;
    for (size_t k : drop) {
        std::smatch c;
        if (std::regex_search(lines[k], c, kCallee)) callees.insert(c[1]);
    }
    for (const auto& fn : callees) {
        std::string called = "@" + fn + "(";
        bool still = false;
        for (const auto& ln : o)
            if (ln.find(called) != std::string::npos && ln.compare(0, 8, "declare ") != 0)
                still = true;
        if (still) continue;
        for (size_t k = 0; k < o.size(); ++k) {
            if (o[k].compare(0, 8, "declare ") == 0 && o[k].find(called) != std::string::npos) {
                size_t first = (k && o[k - 1].compare(0, 17, "; Function Attrs:") == 0) ? k - 1 : k;
                o.erase(o.begin() + first, o.begin() + k + 1);
                break;
            }
        }
    }

    std::string r;
    for (size_t k = 0; k < o.size(); ++k) {
        if (k) r += '\n';
        r += o[k];
    }
    *out = r;
    return true;
}

}  // namespace rq
