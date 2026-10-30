#include "stem_expression.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

struct Function {
    const char* name;
    double (*fn)(double);
};

double Sign(double v) { return v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0); }

const Function kFunctions[] = {
    {"asin", asin}, {"acos", acos}, {"atan", atan}, {"sinh", sinh},   {"cosh", cosh},
    {"tanh", tanh}, {"sqrt", sqrt}, {"cbrt", cbrt}, {"log2", log2},   {"floor", floor},
    {"ceil", ceil}, {"sign", Sign}, {"sin", sin},   {"cos", cos},     {"tan", tan},
    {"abs", fabs},  {"exp", exp},   {"log", log10}, {"ln", log},
};

constexpr size_t kMaxInstructions = 128;
constexpr int kMaxStack = 32;

}  // namespace

std::string StemExpression::Compile(const std::string& text) {
    code_.clear();
    error_.clear();
    text_ = text;
    pos_ = 0;
    ParseSum();
    SkipSpaces();
    if (error_.empty() && pos_ < text_.size()) {
        error_ = "Unexpected '" + text_.substr(pos_, 1) + "' at position " + std::to_string(pos_);
    }
    if (error_.empty() && code_.size() > kMaxInstructions) {
        error_ = "Expression is too long";
    }
    if (!error_.empty()) {
        code_.clear();
    }
    return error_;
}

double StemExpression::Evaluate(double x) const {
    double stack[kMaxStack];
    int top = 0;
    for (const auto& ins : code_) {
        switch (ins.op) {
            case Op::kNumber:
            case Op::kVariable:
                if (top >= kMaxStack) {
                    return NAN;
                }
                stack[top++] = ins.op == Op::kNumber ? ins.value : x;
                break;
            case Op::kNeg:
                stack[top - 1] = -stack[top - 1];
                break;
            case Op::kFunction:
                stack[top - 1] = kFunctions[ins.function].fn(stack[top - 1]);
                break;
            default: {
                double b = stack[--top];
                double& a = stack[top - 1];
                if (ins.op == Op::kAdd) {
                    a += b;
                } else if (ins.op == Op::kSub) {
                    a -= b;
                } else if (ins.op == Op::kMul) {
                    a *= b;
                } else if (ins.op == Op::kDiv) {
                    a = b == 0 ? NAN : a / b;
                } else {
                    a = pow(a, b);
                }
                break;
            }
        }
    }
    return top == 1 && std::isfinite(stack[0]) ? stack[0] : NAN;
}

void StemExpression::SkipSpaces() {
    while (pos_ < text_.size() && isspace(static_cast<unsigned char>(text_[pos_]))) {
        pos_++;
    }
}

bool StemExpression::StartsPrimary() {
    SkipSpaces();
    if (pos_ >= text_.size()) {
        return false;
    }
    char c = text_[pos_];
    return c == '(' || isalpha(static_cast<unsigned char>(c)) ||
           isdigit(static_cast<unsigned char>(c)) || c == '.';
}

void StemExpression::ParseSum() {
    ParseProduct();
    while (error_.empty()) {
        SkipSpaces();
        if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
            Op op = text_[pos_++] == '+' ? Op::kAdd : Op::kSub;
            ParseProduct();
            code_.push_back({op, 0, 0});
        } else {
            break;
        }
    }
}

void StemExpression::ParseProduct() {
    ParseUnary();
    while (error_.empty()) {
        SkipSpaces();
        if (pos_ < text_.size() && (text_[pos_] == '*' || text_[pos_] == '/') &&
            text_.compare(pos_, 2, "**") != 0) {
            Op op = text_[pos_++] == '*' ? Op::kMul : Op::kDiv;
            ParseUnary();
            code_.push_back({op, 0, 0});
        } else if (StartsPrimary()) {
            // Implicit multiplication: "2x", "3sin(x)", "(x+1)(x-1)"
            ParseUnary();
            code_.push_back({Op::kMul, 0, 0});
        } else {
            break;
        }
    }
}

void StemExpression::ParseUnary() {
    SkipSpaces();
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
        bool negate = text_[pos_++] == '-';
        ParseUnary();
        if (negate) {
            code_.push_back({Op::kNeg, 0, 0});
        }
        return;
    }
    ParsePower();
}

void StemExpression::ParsePower() {
    ParsePrimary();
    SkipSpaces();
    if (!error_.empty() || pos_ >= text_.size()) {
        return;
    }
    if (text_[pos_] == '^' || text_.compare(pos_, 2, "**") == 0) {
        pos_ += text_[pos_] == '^' ? 1 : 2;
        // Right associative, and binds tighter than unary minus on its left: -x^2 = -(x^2)
        ParseUnary();
        code_.push_back({Op::kPow, 0, 0});
    }
}

void StemExpression::ParsePrimary() {
    SkipSpaces();
    if (!error_.empty()) {
        return;
    }
    if (pos_ >= text_.size()) {
        error_ = "Unexpected end of expression";
        return;
    }
    char c = text_[pos_];
    if (c == '(') {
        pos_++;
        ParseSum();
        SkipSpaces();
        if (error_.empty() && (pos_ >= text_.size() || text_[pos_] != ')')) {
            error_ = "Missing ')'";
            return;
        }
        pos_++;
        return;
    }
    if (isdigit(static_cast<unsigned char>(c)) || c == '.') {
        char* end = nullptr;
        double value = strtod(text_.c_str() + pos_, &end);
        if (end == text_.c_str() + pos_) {
            error_ = "Bad number at position " + std::to_string(pos_);
            return;
        }
        pos_ = end - text_.c_str();
        code_.push_back({Op::kNumber, 0, value});
        return;
    }
    if (isalpha(static_cast<unsigned char>(c))) {
        for (size_t i = 0; i < sizeof(kFunctions) / sizeof(kFunctions[0]); i++) {
            size_t len = strlen(kFunctions[i].name);
            if (text_.compare(pos_, len, kFunctions[i].name) == 0) {
                size_t after = pos_ + len;
                while (after < text_.size() && isspace(static_cast<unsigned char>(text_[after]))) {
                    after++;
                }
                if (after < text_.size() && text_[after] == '(') {
                    pos_ = after + 1;
                    ParseSum();
                    SkipSpaces();
                    if (error_.empty() && (pos_ >= text_.size() || text_[pos_] != ')')) {
                        error_ = "Missing ')' after " + std::string(kFunctions[i].name);
                        return;
                    }
                    pos_++;
                    code_.push_back({Op::kFunction, static_cast<unsigned char>(i), 0});
                    return;
                }
            }
        }
        if (text_.compare(pos_, 2, "pi") == 0) {
            pos_ += 2;
            code_.push_back({Op::kNumber, 0, M_PI});
            return;
        }
        if (c == 'x' || c == 'X') {
            pos_++;
            code_.push_back({Op::kVariable, 0, 0});
            return;
        }
        if (c == 'e') {
            pos_++;
            code_.push_back({Op::kNumber, 0, M_E});
            return;
        }
        error_ = "Unknown name at position " + std::to_string(pos_) +
                 " (use x, pi, e, or functions like sin, sqrt, ln)";
        return;
    }
    error_ = "Unexpected '" + text_.substr(pos_, 1) + "' at position " + std::to_string(pos_);
}
