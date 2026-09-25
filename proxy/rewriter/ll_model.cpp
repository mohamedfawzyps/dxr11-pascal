#include "ll_model.h"

#include <algorithm>
#include <regex>

namespace llm {
namespace {

const std::regex kResult(R"(^\s*(%[\w.$-]+)\s*=\s*(.*)$)");
// Metadata attachments after the arguments, `, !dx.precise !20`, are part of
// a call too; see the Python.
const std::regex kCall(R"(\bcall\s+.*?@([\w.$"<>-]+|"[^"]+")\s*\((.*)\)\s*(?:#\d+)?(?:\s*,\s*![\w.$-]+\s+!\d+)*\s*$)");
const std::regex kBrCond(R"(^\s*br\s+i1\s+(\S+),\s*label\s+(%[\w.$-]+),\s*label\s+(%[\w.$-]+))");
const std::regex kBrUncond(R"(^\s*br\s+label\s+(%[\w.$-]+))");
const std::regex kPhiIn(R"(\[\s*([^,\]]+),\s*(%[\w.$-]+)\s*\])");
const std::regex kOperandTail(R"((%[\w.$-]+|%"[^"]+")\s*$)");
const std::regex kOpcodeImm(R"(^i32\s+(\d+)$)");
const std::regex kDefineName(R"(@([\w.$-]+|"[^"]+")\s*\()");
const std::regex kBlockLabel(R"(^([\w.$-]+):)");

}  // namespace

std::vector<std::string> SplitLines(const std::string& s) {
    // A trailing carriage return is DROPPED. Without that, a CRLF .ll parses
    // wrongly and SILENTLY: the body-end test is a line equality against "}",
    // which "}\r" fails, so the parser never leaves the function body and the
    // eventual complaint names something unrelated. The Python tolerates CRLF
    // by accident, because its patterns end in `\s*$` and \r is whitespace,
    // so the two implementations disagreed on an input neither test covered
    // and the disagreement did not look like one.
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    out.push_back(cur);
    return out;
}

std::string JoinLines(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out.push_back('\n');
        out += v[i];
    }
    return out;
}

namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::string Unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

}  // namespace

std::vector<std::string> SplitArgs(const std::string& text) {
    std::vector<std::string> args;
    int depth = 0;
    bool quote = false;
    std::string cur;
    for (char ch : text) {
        if (quote) {
            cur.push_back(ch);
            if (ch == '"') quote = false;
            continue;
        }
        if (ch == '"') { quote = true; cur.push_back(ch); }
        else if (ch == '(' || ch == '[' || ch == '{' || ch == '<') { ++depth; cur.push_back(ch); }
        else if (ch == ')' || ch == ']' || ch == '}' || ch == '>') { --depth; cur.push_back(ch); }
        else if (ch == ',' && depth == 0) { args.push_back(Trim(cur)); cur.clear(); }
        else cur.push_back(ch);
    }
    if (!Trim(cur).empty()) args.push_back(Trim(cur));
    return args;
}

std::string OperandName(const std::string& arg) {
    std::smatch m;
    const std::string t = Trim(arg);
    if (std::regex_search(t, m, kOperandTail)) return m[1].str();
    return "";
}

std::string StripComment(const std::string& body) {
    bool quote = false;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '"') quote = !quote;
        else if (body[i] == ';' && !quote) return Trim(body.substr(0, i));
    }
    return Trim(body);
}

// --- normalize -------------------------------------------------------------

std::string Normalize(const std::string& text) {
    static const std::regex kLabelPred(R"(^; <label>:(\d+)(\s*); preds = (.*)$)");
    static const std::regex kLabelOnly(R"(^; <label>:(\d+)\s*$)");
    static const std::regex kNumInPreds(R"(%(\d+))");
    static const std::regex kLabelRef(R"(label %(\d+))");
    static const std::regex kPhiBlock(R"(, %(\d+) \])");
    static const std::regex kAnyNum(R"(%(\d+)\b)");

    std::vector<std::string> out;
    bool inBody = false;
    // The ENTRY block is the awkward one. DXC gives it no label, because
    // nothing can branch to it, so there is nothing here to rename. But a phi
    // may still name it as a PREDECESSOR: an `if` with no `else` straight out
    // of the entry block produces `phi [ 0.0, %0 ], [ 0.5, %9 ]`. That %0
    // becomes %bb0, nothing defines bb0, and the assembler says "use of
    // undefined value '%bb0'". A label is added when, and only when, the
    // function turns out to reference it, so a module that never needed one is
    // byte-for-byte unchanged.
    size_t bodyStart = 0;
    bool sawEntryLabel = false, refsEntry = false;
    for (const std::string& raw : SplitLines(text)) {
        std::string line = raw;
        if (line.rfind("define ", 0) == 0) {
            inBody = true;
            bodyStart = out.size();
            sawEntryLabel = false;
            refsEntry = false;
            out.push_back(line);
            continue;
        }
        if (inBody && line == "}") {
            inBody = false;
            if (refsEntry && !sawEntryLabel)
                out.insert(out.begin() + bodyStart + 1, "bb0:");
            out.push_back(line);
            continue;
        }
        if (!inBody) { out.push_back(line); continue; }

        // Already normalised (a library the shim rewrote, disassembled
        // again): its entry label is kept, never added a second time.
        if (line.rfind("bb0:", 0) == 0) sawEntryLabel = true;
        std::smatch m;
        if (std::regex_match(line, m, kLabelPred)) {
            if (m[1].str() == "0") sawEntryLabel = true;
            const std::string preds = std::regex_replace(m[3].str(), kNumInPreds, "%bb$1");
            out.push_back("bb" + m[1].str() + ":" + m[2].str() + "; preds = " + preds);
            continue;
        }
        if (std::regex_match(line, m, kLabelOnly)) {
            if (m[1].str() == "0") sawEntryLabel = true;
            out.push_back("bb" + m[1].str() + ":");
            continue;
        }

        line = std::regex_replace(line, kLabelRef, "label %bb$1");
        if (line.find(" = phi ") != std::string::npos)
            line = std::regex_replace(line, kPhiBlock, ", %bb$1 ]");
        line = std::regex_replace(line, kAnyNum, "%v$1");
        static const std::regex kEntryRef(R"(%bb0\b)");
        if (std::regex_search(line, kEntryRef)) refsEntry = true;
        out.push_back(line);
    }
    return JoinLines(out);
}

// --- Instr -----------------------------------------------------------------

Instr::Instr(const std::string& lineText, int lineIndex)
    : line(lineText), index(lineIndex) {
    body = Trim(lineText);
    std::smatch m;
    if (std::regex_match(line, m, kResult)) {
        result = m[1].str();
        body = Trim(m[2].str());
    }
    const std::string cut = StripComment(body);
    if (std::regex_search(cut, m, kCall)) {
        callee = Unquote(Trim(m[1].str()));
        args = SplitArgs(m[2].str());
    }
    isPhi = cut.rfind("phi ", 0) == 0;
    if (isPhi) {
        auto begin = std::sregex_iterator(cut.begin(), cut.end(), kPhiIn);
        for (auto it = begin; it != std::sregex_iterator(); ++it)
            phiIncoming.emplace_back((*it)[1].str(), (*it)[2].str());
    }
}

int Instr::DxOp() const {
    if (callee.empty() || callee.rfind("dx.op.", 0) != 0 || args.empty()) return -1;
    std::smatch m;
    const std::string a0 = Trim(args[0]);
    if (std::regex_match(a0, m, kOpcodeImm)) return std::stoi(m[1].str());
    return -1;
}

std::vector<std::string> Instr::Uses() const {
    // Phi incoming VALUES count. They were missed at first, and a phi is
    // exactly how a value escapes a loop, so the loop isolation check was
    // blind to the one shape it exists to catch.
    std::vector<std::string> out;
    for (const auto& a : args) {
        std::string n = OperandName(a);
        if (!n.empty()) out.push_back(n);
    }
    for (const auto& in : phiIncoming) {
        std::string n = OperandName(in.first);
        if (!n.empty()) out.push_back(n);
    }
    return out;
}

// --- Block -----------------------------------------------------------------

const Instr* Block::Terminator() const {
    return instrs.empty() ? nullptr : &instrs.back();
}

std::vector<std::string> Block::Successors() const {
    const Instr* t = Terminator();
    if (!t) return {};
    std::smatch m;
    if (std::regex_search(t->line, m, kBrCond)) return { m[2].str(), m[3].str() };
    if (std::regex_search(t->line, m, kBrUncond)) return { m[1].str() };
    return {};
}

// --- Function --------------------------------------------------------------

const Block* Function::FindBlock(const std::string& label) const {
    for (const auto& b : blocks) if (b.label == label) return &b;
    return nullptr;
}

std::vector<const Instr*> Function::FindOp(int opcode) const {
    std::vector<const Instr*> out;
    for (const auto& b : blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == opcode) out.push_back(&i);
    return out;
}

std::vector<std::pair<const Block*, const Instr*>> Function::FindOpIn(int opcode) const {
    std::vector<std::pair<const Block*, const Instr*>> out;
    for (const auto& b : blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == opcode) out.emplace_back(&b, &i);
    return out;
}

std::map<std::string, std::vector<std::string>> Function::Preds() const {
    std::map<std::string, std::vector<std::string>> p;
    for (const auto& b : blocks) p[b.label] = {};
    for (const auto& b : blocks)
        for (const auto& s : b.Successors())
            if (p.count(s)) p[s].push_back(b.label);
    return p;
}

std::map<std::string, std::set<std::string>> Function::Dominators() const {
    std::vector<std::string> labels;
    for (const auto& b : blocks) labels.push_back(b.label);
    if (labels.empty()) return {};
    const std::string entry = labels.front();
    const auto preds = Preds();

    std::set<std::string> all(labels.begin(), labels.end());
    std::map<std::string, std::set<std::string>> dom;
    for (const auto& l : labels) dom[l] = all;
    dom[entry] = { entry };

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 1; i < labels.size(); ++i) {
            const std::string& l = labels[i];
            std::set<std::string> next;
            bool first = true;
            for (const auto& p : preds.at(l)) {
                auto it = dom.find(p);
                if (it == dom.end()) continue;
                if (first) { next = it->second; first = false; }
                else {
                    std::set<std::string> tmp;
                    std::set_intersection(next.begin(), next.end(),
                                          it->second.begin(), it->second.end(),
                                          std::inserter(tmp, tmp.begin()));
                    next = std::move(tmp);
                }
            }
            next.insert(l);
            if (next != dom[l]) { dom[l] = std::move(next); changed = true; }
        }
    }
    return dom;
}

std::vector<Loop> Function::NaturalLoops() const {
    const auto dom = Dominators();
    const auto preds = Preds();
    std::vector<Loop> loops;
    for (const auto& b : blocks) {
        for (const auto& succ : b.Successors()) {
            auto d = dom.find(b.label);
            if (d == dom.end() || !d->second.count(succ)) continue;
            Loop lp;
            lp.header = succ;
            lp.latch = b.label;
            lp.body = { succ, b.label };
            std::vector<std::string> stack{ b.label };
            while (!stack.empty()) {
                const std::string cur = stack.back();
                stack.pop_back();
                auto pit = preds.find(cur);
                if (pit == preds.end()) continue;
                for (const auto& p : pit->second)
                    if (!lp.body.count(p)) { lp.body.insert(p); stack.push_back(p); }
            }
            loops.push_back(std::move(lp));
        }
    }
    return loops;
}

// --- Module ----------------------------------------------------------------

Module::Module(const std::string& moduleText) : text(moduleText) {
    lines = SplitLines(text);
    Function* fn = nullptr;
    Block* blk = nullptr;
    for (int n = 0; n < static_cast<int>(lines.size()); ++n) {
        const std::string& line = lines[n];
        if (line.rfind("define ", 0) == 0) {
            Function f;
            f.signatureLine = line;
            f.index = n;
            std::smatch m;
            f.name = std::regex_search(line, m, kDefineName) ? Unquote(m[1].str()) : "?";
            f.blocks.emplace_back();            // implicit entry block
            functions.push_back(std::move(f));
            fn = &functions.back();
            blk = &fn->blocks.back();
            continue;
        }
        if (!fn) continue;
        if (line == "}") { fn->endIndex = n; fn = nullptr; blk = nullptr; continue; }
        std::smatch m;
        if (std::regex_search(line, m, kBlockLabel) && m.position(0) == 0) {
            // A labelled FIRST block IS the entry block, not a second one. The
            // normaliser adds `bb0:` when a phi names the entry as a
            // predecessor, and without this that would leave an empty implicit
            // block standing in front of it.
            if (fn->blocks.size() == 1 && fn->blocks[0].label.empty() &&
                fn->blocks[0].instrs.empty()) {
                fn->blocks.pop_back();
            }
            Block b;
            b.label = "%" + m[1].str();
            b.index = n;
            fn->blocks.push_back(std::move(b));
            blk = &fn->blocks.back();
            continue;
        }
        const std::string t = Trim(line);
        if (!t.empty() && t[0] != ';' && blk) blk->instrs.emplace_back(line, n);
    }
}

const Function* Module::FindFunction(const std::string& name) const {
    for (const auto& f : functions) if (f.name == name) return &f;
    return nullptr;
}

std::string Render(const Module& m,
                   const std::map<int, std::optional<std::string>>& edits) {
    std::vector<std::string> out;
    for (int n = 0; n < static_cast<int>(m.lines.size()); ++n) {
        auto it = edits.find(n);
        if (it != edits.end()) {
            if (it->second.has_value()) out.push_back(*it->second);
        } else {
            out.push_back(m.lines[n]);
        }
    }
    return JoinLines(out);
}

int FindLine(const std::vector<std::string>& lines, const std::regex& re) {
    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
        if (std::regex_match(lines[i], re)) return i;
    return -1;
}

void InsertAfter(std::vector<std::string>& lines, int at, const std::string& block) {
    auto add = SplitLines(block);
    lines.insert(lines.begin() + (at + 1), add.begin(), add.end());
}

}  // namespace llm
