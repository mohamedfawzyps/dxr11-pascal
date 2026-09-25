// The hit group arguments of an application's TraceRay calls. See the header.
#include "trace_args.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>

namespace targs {
namespace {

std::string GlobalAt(const std::string& l, size_t at) {
    if (at == std::string::npos) return {};
    if (at + 1 < l.size() && l[at + 1] == '"') {
        const size_t e = l.find('"', at + 2);
        return e == std::string::npos ? std::string() : l.substr(at, e + 1 - at);
    }
    size_t e = at + 1;
    while (e < l.size() && (isalnum((unsigned char)l[e]) || l[e] == '_' || l[e] == '.' || l[e] == '$'))
        ++e;
    return l.substr(at, e - at);
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(at, e - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        at = e + 1;
    }
    return lines;
}

std::vector<std::string> Fields(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '{' || c == '[') ++depth;
        if (c == ')' || c == '}' || c == ']') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    out.push_back(cur);
    for (auto& f : out) {
        const size_t a = f.find_first_not_of(' ');
        const size_t b = f.find_last_not_of(' ');
        f = a == std::string::npos ? std::string() : f.substr(a, b - a + 1);
    }
    return out;
}

struct Builder {
    Args* out;
    std::map<std::string, std::pair<UINT, UINT>> recs;   // resource global -> (space, lower)
    std::map<std::string, const std::string*> defs;     // this function's values
    std::map<std::string, int> memo;
    std::set<std::string> building;

    int Add(Node n) {
        out->nodes.push_back(std::move(n));
        return (int)out->nodes.size() - 1;
    }
    int Const(uint32_t v) {
        Node n;
        n.kind = Node::kConst;
        n.value = v;
        return Add(n);
    }
    int Any() { return Add(Node{}); }
    int Union(std::vector<int> kids) {
        Node n;
        n.kind = Node::kUnion;
        n.kids = std::move(kids);
        return Add(n);
    }

    // The resource global a handle comes from.
    std::string HandleGlobal(std::string h) {
        static const std::regex kOperand(R"RX(\(i32 \d+, %[^ ]+ (%[^,)]+))RX");
        std::smatch m;
        for (int k = 0; k < 6; ++k) {
            auto it = defs.find(h);
            if (it == defs.end()) return {};
            const std::string& d = *it->second;
            if (d.find("@dx.op.annotateHandle(") != std::string::npos ||
                d.find("@dx.op.createHandleForLib") != std::string::npos) {
                if (!std::regex_search(d, m, kOperand)) return {};
                h = m[1].str();
                continue;
            }
            if (d.find("= load ") != std::string::npos && d.find('@') != std::string::npos)
                return GlobalAt(d, d.find('@'));
            return {};
        }
        return {};
    }

    int Build(std::string v, int depth) {
        static const std::regex kNum(R"RX(^-?\d+$)RX");
        static const std::regex kBin(
            R"RX(= (add|sub|mul|shl|lshr|ashr|and|or|xor|udiv|urem)(?: nuw| nsw| exact)* i32 ([^,\s]+), ([^,\s]+))RX");
        static const std::regex kSelect(R"RX(= select i1 [^,]+, i32 ([^,\s]+), i32 ([^,\s]+))RX");
        static const std::regex kPhi(R"RX(= phi i32 (.*)$)RX");
        static const std::regex kIncoming(R"RX(\[ ([^,\s]+), %[^\]\s]+ \])RX");
        static const std::regex kBool(R"RX(= (zext|sext) i1 [^ ]+ to i32)RX");
        static const std::regex kCast(R"RX(= (?:zext|trunc) i\d+ ([^ ]+) to i32)RX");
        static const std::regex kExtract(R"RX(= extractvalue %dx\.types\.CBufRet\.i32 (%[^,]+), (\d+))RX");
        static const std::regex kCbLoad(
            R"RX(@dx\.op\.cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle (%[^,]+), i32 (\d+)\))RX");
        if (std::regex_match(v, kNum)) return Const((uint32_t)std::stoll(v));
        if (v == "true") return Const(1);
        if (v == "false") return Const(0);
        if (v.empty() || v[0] != '%' || depth > 24) return Any();
        auto mm = memo.find(v);
        if (mm != memo.end()) return mm->second;
        if (building.count(v)) return Any();   // a value that depends on itself: a loop
        auto it = defs.find(v);
        if (it == defs.end()) return Any();    // an argument
        const std::string& d = *it->second;
        building.insert(v);
        std::smatch m;
        int r = -1;
        if (std::regex_search(d, m, kBin)) {
            const std::string op = m[1].str(), a = m[2].str(), b = m[3].str();
            Node n;
            n.kind = Node::kBin;
            n.op = op;
            n.kids = { Build(a, depth + 1), Build(b, depth + 1) };
            r = Add(n);
        } else if (std::regex_search(d, m, kSelect)) {
            const std::string a = m[1].str(), b = m[2].str();
            r = Union({ Build(a, depth + 1), Build(b, depth + 1) });
        } else if (std::regex_search(d, m, kPhi)) {
            const std::string list = m[1].str();
            std::vector<int> kids;
            for (std::sregex_iterator p(list.begin(), list.end(), kIncoming), e; p != e; ++p)
                kids.push_back(Build((*p)[1].str(), depth + 1));
            r = kids.empty() ? Any() : Union(kids);
        } else if (std::regex_search(d, m, kBool)) {
            r = Union({ Const(0), Const(m[1].str() == "zext" ? 1u : 0xFFFFFFFFu) });
        } else if (std::regex_search(d, m, kCast)) {
            r = Build(m[1].str(), depth + 1);
        } else if (std::regex_search(d, m, kExtract)) {
            const std::string load = m[1].str();
            const UINT comp = (UINT)std::stoul(m[2].str());
            auto li = defs.find(load);
            std::smatch lm;
            if (li != defs.end() && std::regex_search(*li->second, lm, kCbLoad)) {
                const std::string h = lm[1].str();
                const UINT row = (UINT)std::stoul(lm[2].str());
                auto rec = recs.find(HandleGlobal(h));
                if (rec != recs.end()) {
                    Node n;
                    n.kind = Node::kLeaf;
                    n.leaf.space = rec->second.first;
                    n.leaf.reg = rec->second.second;
                    n.leaf.offset = row * 16 + comp * 4;
                    r = Add(n);
                }
            }
        }
        if (r < 0) r = Any();
        building.erase(v);
        memo[v] = r;
        return r;
    }
};

// A set of 32-bit values, or every value.
struct Set {
    bool any = false;
    std::vector<uint32_t> v;
};
const size_t kCap = 64;

Set Norm(std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    Set s;
    if (v.size() > kCap) s.any = true;
    else s.v = std::move(v);
    return s;
}

int Popcount(uint32_t x) {
    int n = 0;
    for (; x; x &= x - 1) ++n;
    return n;
}

Set Eval(const Args& a, int i, const LeafValue& value, uint32_t kind, int depth) {
    Set s;
    if (i < 0 || i >= (int)a.nodes.size() || depth > 64) { s.any = true; return s; }
    const Node& n = a.nodes[(size_t)i];
    switch (n.kind) {
    case Node::kConst: s.v = { n.value }; return s;
    case Node::kAny: s.any = true; return s;
    case Node::kLeaf: {
        uint32_t x = 0;
        if (value && value(n.leaf, kind, &x)) s.v = { x };
        else s.any = true;
        return s;
    }
    case Node::kBool: s.v = { 0, 1 }; return s;
    case Node::kUnion: {
        std::vector<uint32_t> all;
        for (int k : n.kids) {
            const Set c = Eval(a, k, value, kind, depth + 1);
            if (c.any) { s.any = true; return s; }
            all.insert(all.end(), c.v.begin(), c.v.end());
        }
        return Norm(std::move(all));
    }
    case Node::kBin: {
        if (n.kids.size() != 2) { s.any = true; return s; }
        const Set x = Eval(a, n.kids[0], value, kind, depth + 1);
        const Set y = Eval(a, n.kids[1], value, kind, depth + 1);
        const std::string& op = n.op;
        if (!x.any && !y.any) {
            std::vector<uint32_t> out;
            for (uint32_t p : x.v)
                for (uint32_t q : y.v) {
                    uint32_t r = 0;
                    if (op == "add") r = p + q;
                    else if (op == "sub") r = p - q;
                    else if (op == "mul") r = p * q;
                    else if (op == "and") r = p & q;
                    else if (op == "or") r = p | q;
                    else if (op == "xor") r = p ^ q;
                    else if (op == "shl") { if (q >= 32) { s.any = true; return s; } r = p << q; }
                    else if (op == "lshr") { if (q >= 32) { s.any = true; return s; } r = p >> q; }
                    else if (op == "ashr") {
                        if (q >= 32) { s.any = true; return s; }
                        r = (uint32_t)((int32_t)p >> q);
                    }
                    else if (op == "udiv") { if (!q) { s.any = true; return s; } r = p / q; }
                    else if (op == "urem") { if (!q) { s.any = true; return s; } r = p % q; }
                    else { s.any = true; return s; }
                    out.push_back(r);
                }
            return Norm(std::move(out));
        }
        if (x.any && y.any) { s.any = true; return s; }
        // One side unknown: what the known side still bounds.
        const Set& k = x.any ? y : x;
        const bool knownRight = x.any;
        if (op == "and") {
            std::vector<uint32_t> out;
            for (uint32_t c : k.v) {
                if (Popcount(c) > 6) { s.any = true; return s; }
                for (uint32_t sub = c;; sub = (sub - 1) & c) {   // every submask of c
                    out.push_back(sub);
                    if (!sub) break;
                }
            }
            return Norm(std::move(out));
        }
        if (op == "mul") {
            if (k.v.size() == 1 && k.v[0] == 0) { s.v = { 0 }; return s; }
            s.any = true;
            return s;
        }
        if (op == "urem" && knownRight) {
            uint32_t mx = 0;
            for (uint32_t d : k.v) {
                if (!d || d > kCap) { s.any = true; return s; }
                mx = (std::max)(mx, d);
            }
            std::vector<uint32_t> out;
            for (uint32_t r = 0; r < mx; ++r) out.push_back(r);
            return Norm(std::move(out));
        }
        if (op == "lshr" && knownRight) {
            uint32_t mn = 32;
            for (uint32_t q : k.v) mn = (std::min)(mn, q);
            if (mn < 26 || mn >= 32) { s.any = true; return s; }
            std::vector<uint32_t> out;
            for (uint32_t r = 0; r < (1u << (32 - mn)); ++r) out.push_back(r);
            return Norm(std::move(out));
        }
        s.any = true;
        return s;
    }
    }
    s.any = true;
    return s;
}

std::vector<UINT> Low4(const Set& s) {
    std::vector<UINT> out;
    if (s.any) {
        for (UINT i = 0; i < 16; ++i) out.push_back(i);
        return out;
    }
    for (uint32_t x : s.v) out.push_back(x & 15u);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

void Scan(const std::string& text, Args* out) {
    const auto lines = Lines(text);
    Builder b;
    b.out = out;
    // Resource records, and each entry function's shader kind (tag 8).
    static const std::regex kFields(R"RX(, !"[^"]*", i32 (-?\d+), i32 (-?\d+), i32 (-?\d+),)RX");
    static const std::regex kNode(R"RX(^!(\d+) = (?:distinct )?!\{(.*)\}\s*$)RX");
    static const std::regex kEp(R"RX(^!dx\.entryPoints = !\{(.*)\}\s*$)RX");
    std::map<int, std::string> md;
    std::string eps;
    std::smatch m;
    for (const auto& l : lines) {
        if (std::regex_match(l, m, kNode)) md[std::stoi(m[1].str())] = m[2].str();
        else if (std::regex_match(l, m, kEp)) eps = m[1].str();
        if (l.rfind("!", 0) == 0 && l.find('@') != std::string::npos && std::regex_search(l, m, kFields)) {
            const std::string g = GlobalAt(l, l.find('@'));
            if (!g.empty()) b.recs[g] = { (UINT)std::stol(m[1].str()), (UINT)std::stol(m[2].str()) };
        }
    }
    std::map<std::string, uint32_t> kinds;
    for (const auto& ref : Fields(eps)) {
        if (ref.size() < 2 || ref[0] != '!') continue;
        const auto f = Fields(md[std::atoi(ref.c_str() + 1)]);
        if (f.size() < 5 || f[0].find('@') == std::string::npos || f[4].empty() || f[4][0] != '!') continue;
        const std::string fn = GlobalAt(f[0], f[0].find('@'));
        const auto tags = Fields(md[std::atoi(f[4].c_str() + 1)]);
        for (size_t t = 0; t + 1 < tags.size(); t += 2)
            if (tags[t] == "i32 8" && tags[t + 1].rfind("i32 ", 0) == 0)
                kinds[fn] = (uint32_t)std::stoul(tags[t + 1].substr(4));
    }
    static const std::regex kTrace(
        R"RX(@dx\.op\.traceRay\.[^(]+\(i32 157, [^,]+, [^,]+, [^,]+, i32 ([^,]+), i32 ([^,]+),)RX");
    uint32_t kind = 0;
    std::vector<size_t> traces;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.rfind("define ", 0) == 0) {
            b.defs.clear();
            b.memo.clear();
            auto k = kinds.find(GlobalAt(l, l.find('@')));
            kind = k == kinds.end() ? 0 : k->second;
            traces.clear();
            continue;
        }
        const size_t eq = l.find(" = ");
        if (l.rfind("  %", 0) == 0 && eq != std::string::npos) b.defs[l.substr(2, eq - 2)] = &l;
        if (l.find("@dx.op.traceRay.") != std::string::npos) traces.push_back(i);
        if (l != "}") continue;
        // The function is read whole: a phi can name a value defined later.
        for (size_t t : traces) {
            if (!std::regex_search(lines[t], m, kTrace)) continue;
            const std::string r = m[1].str(), mm = m[2].str();
            Site s;
            s.kind = kind;
            s.r = b.Build(r, 0);
            s.m = b.Build(mm, 0);
            out->sites.push_back(s);
        }
        traces.clear();
    }
}

void Merge(const Args& from, Args* into) {
    const int base = (int)into->nodes.size();
    for (Node n : from.nodes) {
        for (int& k : n.kids) k += base;
        into->nodes.push_back(std::move(n));
    }
    for (Site s : from.sites) {
        s.r += base;
        s.m += base;
        into->sites.push_back(s);
    }
}

bool Literal(const Args& a) {
    for (const auto& s : a.sites)
        for (int i : { s.r, s.m })
            if (i < 0 || a.nodes[(size_t)i].kind != Node::kConst) return false;
    return true;
}

std::vector<std::pair<UINT, UINT>> Pairs(const Args& a, const LeafValue& value) {
    std::set<std::pair<UINT, UINT>> out;
    for (const auto& s : a.sites) {
        const auto rs = Low4(Eval(a, s.r, value, s.kind, 0));
        const auto ms = Low4(Eval(a, s.m, value, s.kind, 0));
        for (UINT r : rs)
            for (UINT mv : ms) out.insert({ r, mv });
    }
    return { out.begin(), out.end() };
}

bool NoRefine() {
    static const bool v = GetEnvironmentVariableA("DXR_TIER11_TARGS_NOREFINE", nullptr, 0) != 0;
    return v;
}

}  // namespace targs
