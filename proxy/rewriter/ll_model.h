// A deliberately small model of a DXIL .ll module. C++ port of
// phase5/rewriter/dxil.py plus phase5/hand/llnorm.py.
//
// This is NOT a general LLVM parser and must not grow into one. It understands
// exactly what the RayQuery lowering needs and keeps everything else as
// verbatim text, so untouched lines survive byte for byte.
//
// The Python original stays in the tree as the reference implementation. The
// port is checked against it by requiring BYTE-IDENTICAL output on every case
// in tools/run_rewriter_test.ps1, which is a far stronger oracle than "it
// renders correctly".
#pragma once

#include <map>
#include <regex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llm {

// Split a call's argument list on top-level commas. Types like <2 x float> and
// %"class.Foo<Bar>" contain commas and brackets, so a plain split is wrong.
std::vector<std::string> SplitArgs(const std::string& text);

// "%dx.types.Handle %v2" -> "%v2"; "i32 178" -> "".
std::string OperandName(const std::string& arg);

// Drop a trailing ; comment, respecting quotes.
std::string StripComment(const std::string& body);

// MSVC's std::regex has no multiline mode, so every pattern that would have
// used one is applied per line instead. These are the seams for that.
std::vector<std::string> SplitLines(const std::string& s);
std::string JoinLines(const std::vector<std::string>& v);
// Index of the first line fully matching `re`, or -1.
int FindLine(const std::vector<std::string>& lines, const std::regex& re);
// Insert `block`, split into lines, after line `at`.
void InsertAfter(std::vector<std::string>& lines, int at, const std::string& block);

// Give every unnamed value and block an explicit name, so that inserting and
// deleting stops being positional:
//     %33 = call ...        ->  %v33 = call ...
//     ; <label>:44          ->  bb44:
//     br i1 %45, label %36  ->  br i1 %v45, label %bb36
std::string Normalize(const std::string& text);

struct Instr {
    std::string line;          // verbatim
    int index = -1;            // line number in the module
    std::string result;        // "%v33", or empty
    std::string body;          // text after "= ", or the whole line
    std::string callee;        // "dx.op.foo", or empty
    std::vector<std::string> args;
    bool isPhi = false;
    // [value, block] pairs of a phi.
    std::vector<std::pair<std::string, std::string>> phiIncoming;

    explicit Instr(const std::string& lineText, int lineIndex = -1);

    // The dx.op opcode immediate, or -1. Operand 0 of every dx.op call is the
    // opcode, and it is the ONLY reliable discriminator: DXC reuses one LLVM
    // function for several opcodes, so 184 (CommittedStatus) and 185
    // (CandidateType) are both dx.op.rayQuery_StateScalar.i32.
    int DxOp() const;

    // Operand names this instruction reads.
    std::vector<std::string> Uses() const;
};

struct Block {
    std::string label;         // "%bb44"; empty for the implicit entry block
    int index = -1;            // line of the label; -1 for the entry block
    std::vector<Instr> instrs;

    const Instr* Terminator() const;
    std::vector<std::string> Successors() const;
};

// header, latch, and every block in the loop body (including both).
struct Loop {
    std::string header;
    std::string latch;
    std::set<std::string> body;
};

struct Function {
    std::string signatureLine;
    std::string name;
    int index = -1;            // line of "define ..."
    int endIndex = -1;         // line of the closing "}"
    std::vector<Block> blocks;

    const Block* FindBlock(const std::string& label) const;
    std::vector<const Instr*> FindOp(int opcode) const;
    // (block label, instruction) for every dx.op call with this opcode.
    std::vector<std::pair<const Block*, const Instr*>> FindOpIn(int opcode) const;

    std::map<std::string, std::vector<std::string>> Preds() const;
    std::map<std::string, std::set<std::string>> Dominators() const;

    // Every natural loop. A back edge is latch -> header where header
    // dominates latch. This is how the rotated `while (q.Proceed())` is found:
    // its guard sits in the preheader and only the latch carries the real
    // test, so there is no single loop condition to match textually.
    std::vector<Loop> NaturalLoops() const;
};

struct Module {
    std::string text;
    std::vector<std::string> lines;
    std::vector<Function> functions;

    explicit Module(const std::string& moduleText);
    const Function* FindFunction(const std::string& name) const;
};

// Rebuild module text with `edits` applied: a line index maps to replacement
// text, or to nullopt to delete that line. Anything not mentioned is emitted
// byte for byte, which is the point.
std::string Render(const Module& m,
                   const std::map<int, std::optional<std::string>>& edits);

}  // namespace llm
