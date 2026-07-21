#include "source_translator.h"

#include "counted_loop_translator.h"

#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "unijit/ir/function.h"

namespace unijit::frontend::quickjs {
namespace {

constexpr std::size_t kMaximumSourceBytes = 64U * 1024U;
constexpr std::size_t kMaximumParameters = 64;
constexpr std::size_t kMaximumExpressionDepth = 128;

bool is_space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

bool is_identifier_start(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') || value == '_' || value == '$';
}

bool is_identifier_continue(char value) noexcept {
  return is_identifier_start(value) || (value >= '0' && value <= '9');
}

class Parser final {
 public:
  Parser(std::string_view source, jit::OptimizationLevel optimization_level)
      : source_(source), optimization_level_(optimization_level) {}

  TranslationResult translate() {
    if (source_.size() > kMaximumSourceBytes) {
      return {invalid("QuickJS function source exceeds the frontend limit"),
              0, nullptr};
    }
    if (!consume_keyword("function")) {
      return {invalid("expected a conventional function expression"), 0,
              nullptr};
    }

    skip_space();
    if (peek() != '(') {
      std::string ignored_name;
      if (!parse_identifier(&ignored_name)) {
        return {status_, 0, nullptr};
      }
    }
    if (!consume('(') || !parse_parameters() || !consume(')')) {
      return {status_, 0, nullptr};
    }

    builder_ = std::make_unique<ir::FunctionBuilder>(std::vector<ir::ValueType>(
        parameters_.size(), ir::ValueType::kFloat64));
    if (!consume('{') || !consume_keyword("return")) {
      return {status_, 0, nullptr};
    }
    const ir::Value result = parse_return_expression();
    if (!result.valid()) {
      return {status_, 0, nullptr};
    }
    skip_space();
    if (peek() == ';') {
      ++position_;
    }
    if (!consume('}')) {
      return {status_, 0, nullptr};
    }
    skip_space();
    if (position_ != source_.size()) {
      return {invalid("unexpected source after the function body"), 0,
              nullptr};
    }
    const Status return_status = builder_->set_return(result);
    if (!return_status.ok()) {
      return {return_status, 0, nullptr};
    }

    const std::size_t parameter_count = parameters_.size();
    jit::CompilationResult compilation = jit::Compiler::compile(
        std::move(*builder_).build(),
        jit::CompilationOptions{optimization_level_});
    if (!compilation.ok()) {
      return {compilation.status, parameter_count, nullptr};
    }
    return {Status::ok_status(), parameter_count,
            std::move(compilation.function), result_kind_};
  }

 private:
  Status invalid(const char* message) {
    status_ = {StatusCode::kInvalidArgument, message, position_};
    return status_;
  }

  char peek() const noexcept {
    return position_ < source_.size() ? source_[position_] : '\0';
  }

  void skip_space() noexcept {
    while (position_ < source_.size() && is_space(source_[position_])) {
      ++position_;
    }
  }

  bool consume(char expected) {
    skip_space();
    if (peek() != expected) {
      invalid("unexpected token in QuickJS numeric function");
      return false;
    }
    ++position_;
    return true;
  }

  bool consume_keyword(std::string_view keyword) {
    skip_space();
    if (source_.substr(position_, keyword.size()) != keyword) {
      invalid("expected a QuickJS function keyword");
      return false;
    }
    const std::size_t end = position_ + keyword.size();
    if (end < source_.size() && is_identifier_continue(source_[end])) {
      invalid("keyword is followed by an identifier character");
      return false;
    }
    position_ = end;
    return true;
  }

  bool parse_identifier(std::string* result) {
    skip_space();
    if (!is_identifier_start(peek())) {
      invalid("expected an ASCII identifier");
      return false;
    }
    const std::size_t begin = position_++;
    while (position_ < source_.size() &&
           is_identifier_continue(source_[position_])) {
      ++position_;
    }
    result->assign(source_.substr(begin, position_ - begin));
    return true;
  }

  bool parse_parameters() {
    skip_space();
    if (peek() == ')') {
      return true;
    }
    while (true) {
      std::string parameter;
      if (!parse_identifier(&parameter)) {
        return false;
      }
      for (const std::string& existing : parameters_) {
        if (existing == parameter) {
          invalid("duplicate parameters are not supported");
          return false;
        }
      }
      if (parameters_.size() >= kMaximumParameters) {
        invalid("QuickJS numeric function has too many parameters");
        return false;
      }
      parameters_.push_back(std::move(parameter));
      skip_space();
      if (peek() != ',') {
        return true;
      }
      ++position_;
    }
  }

  ir::Value parse_return_expression() {
    const ir::Value lhs = parse_expression(0);
    if (!lhs.valid()) {
      return {};
    }
    skip_space();
    std::string_view operation;
    if (source_.substr(position_, 3) == "===") {
      operation = "===";
      position_ += 3;
    } else if (source_.substr(position_, 3) == "!==") {
      operation = "!==";
      position_ += 3;
    } else if (source_.substr(position_, 2) == "==" ||
               source_.substr(position_, 2) == "!=") {
      operation = source_.substr(position_, 2);
      position_ += 2;
    } else if (source_.substr(position_, 2) == "<=") {
      operation = "<=";
      position_ += 2;
    } else if (source_.substr(position_, 2) == ">=") {
      operation = ">=";
      position_ += 2;
    } else if (peek() == '<' || peek() == '>') {
      operation = source_.substr(position_, 1);
      ++position_;
    } else {
      return lhs;
    }
    const ir::Value rhs = parse_expression(0);
    if (!rhs.valid()) {
      return {};
    }
    result_kind_ = ResultKind::kBoolean;
    if (operation == "<") {
      return builder_->float64_less_than(lhs, rhs);
    }
    if (operation == "<=") {
      return builder_->float64_less_equal(lhs, rhs);
    }
    if (operation == ">") {
      return builder_->float64_less_than(rhs, lhs);
    }
    if (operation == ">=") {
      return builder_->float64_less_equal(rhs, lhs);
    }
    if (operation == "==" || operation == "===") {
      return builder_->float64_equal(lhs, rhs);
    }
    return builder_->float64_not_equal(lhs, rhs);
  }

  ir::Value parse_expression(std::size_t depth) {
    ir::Value value = parse_product(depth + 1);
    while (value.valid()) {
      skip_space();
      const char operation = peek();
      if (operation != '+' && operation != '-') {
        break;
      }
      ++position_;
      const ir::Value rhs = parse_product(depth + 1);
      if (!rhs.valid()) {
        return {};
      }
      value = operation == '+' ? builder_->float64_add(value, rhs)
                               : builder_->float64_subtract(value, rhs);
    }
    return value;
  }

  ir::Value parse_product(std::size_t depth) {
    ir::Value value = parse_unary(depth + 1);
    while (value.valid()) {
      skip_space();
      const char operation = peek();
      if (operation != '*' && operation != '/') {
        break;
      }
      ++position_;
      const ir::Value rhs = parse_unary(depth + 1);
      if (!rhs.valid()) {
        return {};
      }
      value = operation == '*' ? builder_->float64_multiply(value, rhs)
                               : builder_->float64_divide(value, rhs);
    }
    return value;
  }

  ir::Value parse_unary(std::size_t depth) {
    if (depth > kMaximumExpressionDepth) {
      invalid("QuickJS numeric expression is nested too deeply");
      return {};
    }
    skip_space();
    if (peek() == '+') {
      ++position_;
      return parse_unary(depth + 1);
    }
    if (peek() == '-') {
      ++position_;
      const ir::Value operand = parse_unary(depth + 1);
      if (!operand.valid()) {
        return {};
      }
      return builder_->float64_subtract(builder_->float64_constant(0.0),
                                        operand);
    }
    return parse_primary(depth + 1);
  }

  ir::Value parse_primary(std::size_t depth) {
    if (depth > kMaximumExpressionDepth) {
      invalid("QuickJS numeric expression is nested too deeply");
      return {};
    }
    skip_space();
    if (peek() == '(') {
      ++position_;
      const ir::Value value = parse_expression(depth + 1);
      if (!value.valid() || !consume(')')) {
        return {};
      }
      return value;
    }
    if (is_identifier_start(peek())) {
      std::string identifier;
      if (!parse_identifier(&identifier)) {
        return {};
      }
      for (std::size_t index = 0; index < parameters_.size(); ++index) {
        if (parameters_[index] == identifier) {
          return builder_->parameter(index);
        }
      }
      invalid("numeric expression references a non-parameter identifier");
      return {};
    }
    return parse_number();
  }

  ir::Value parse_number() {
    skip_space();
    const std::size_t begin = position_;
    bool digits = false;
    while (peek() >= '0' && peek() <= '9') {
      digits = true;
      ++position_;
    }
    if (peek() == '.') {
      ++position_;
      while (peek() >= '0' && peek() <= '9') {
        digits = true;
        ++position_;
      }
    }
    if (!digits) {
      invalid("expected a parameter, number, or parenthesized expression");
      return {};
    }
    if (peek() == 'e' || peek() == 'E') {
      ++position_;
      if (peek() == '+' || peek() == '-') {
        ++position_;
      }
      const std::size_t exponent_begin = position_;
      while (peek() >= '0' && peek() <= '9') {
        ++position_;
      }
      if (position_ == exponent_begin) {
        invalid("numeric exponent has no digits");
        return {};
      }
    }
    if (is_identifier_start(peek())) {
      invalid("numeric literal is followed by an identifier");
      return {};
    }

    const std::string token(source_.substr(begin, position_ - begin));
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end == nullptr || *end != '\0') {
      invalid("unable to decode a numeric literal");
      return {};
    }
    return builder_->float64_constant(value);
  }

  std::string_view source_;
  jit::OptimizationLevel optimization_level_;
  std::size_t position_{0};
  Status status_;
  std::vector<std::string> parameters_;
  std::unique_ptr<ir::FunctionBuilder> builder_;
  ResultKind result_kind_{ResultKind::kFloat64};
};

}  // namespace

TranslationResult translate_numeric_function(
    std::string_view source, jit::OptimizationLevel optimization_level) {
  try {
    if (looks_like_counted_loop(source)) {
      return translate_counted_loop(source, optimization_level);
    }
    return Parser(source, optimization_level).translate();
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate QuickJS translation state"},
            0, nullptr};
  }
}

bool supports_tiered_translation(std::string_view source) noexcept {
  (void)source;
  return true;
}

}  // namespace unijit::frontend::quickjs
