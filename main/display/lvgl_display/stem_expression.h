#ifndef STEM_EXPRESSION_H
#define STEM_EXPRESSION_H

#include <string>
#include <vector>

// A small math expression compiled once and evaluated many times, for the graph plotter.
// Supports + - * / ^ (or **), unary minus, parentheses, implicit multiplication ("2x",
// "3sin(x)", "(x+1)(x-1)"), the variable x, the constants pi and e, and the functions
// sin cos tan asin acos atan sinh cosh tanh sqrt cbrt abs ln log log2 exp floor ceil sign.
class StemExpression {
public:
    // Returns an empty string on success, or a human-readable error.
    std::string Compile(const std::string& text);
    // Returns NaN when the expression is undefined at x.
    double Evaluate(double x) const;

private:
    enum class Op : unsigned char {
        kNumber, kVariable, kAdd, kSub, kMul, kDiv, kPow, kNeg, kFunction
    };
    struct Instruction {
        Op op;
        unsigned char function;
        double value;
    };

    std::vector<Instruction> code_;
    std::string text_;
    size_t pos_ = 0;
    std::string error_;

    void SkipSpaces();
    bool StartsPrimary();
    void ParseSum();
    void ParseProduct();
    void ParseUnary();
    void ParsePower();
    void ParsePrimary();
};

#endif  // STEM_EXPRESSION_H
