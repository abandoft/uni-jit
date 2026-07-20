#include "counted_loop_translator.h"

#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"

namespace unijit::frontend::quickjs {
namespace {

constexpr std::size_t kMaximumSourceBytes = 64U * 1024U;
constexpr std::size_t kMaximumParameters = 64;
constexpr std::size_t kMaximumLocals = 64;
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

struct Symbol final {
  std::string name;
  ir::Value value;
  bool loop_local{false};
};

class CountedLoopParser final {
 public:
  explicit CountedLoopParser(std::string_view source) : source_(source) {}

  TranslationResult translate() {
    if (source_.size() > kMaximumSourceBytes) {
      return fail(invalid("QuickJS function source exceeds the frontend limit"));
    }
    if (!consume_keyword("function")) {
      return fail(invalid("expected a conventional function expression"));
    }
    skip_space();
    if (peek() != '(') {
      std::string ignored_name;
      if (!parse_identifier(&ignored_name)) {
        return fail(status_);
      }
    }
    if (!consume('(') || !parse_parameters() || !consume(')')) {
      return fail(status_);
    }

    builder_ = std::make_unique<ir::ControlFlowBuilder>(
        std::vector<ir::ValueType>(parameter_names_.size(),
                                   ir::ValueType::kFloat64));
    for (std::size_t index = 0; index < parameter_names_.size(); ++index) {
      symbols_.push_back(
          Symbol{parameter_names_[index], builder_->parameter(index), false});
    }

    if (!consume('{')) {
      return fail(status_);
    }
    while (matches_keyword("let")) {
      if (!parse_local_declaration()) {
        return fail(status_);
      }
    }
    if (!consume_keyword("for") || !consume('(') ||
        !consume_keyword("let")) {
      return fail(invalid("expected a counted for loop after local state"));
    }
    const std::size_t safepoint_site = position_;
    std::string induction_name;
    if (!parse_identifier(&induction_name) || symbol(induction_name) != nullptr ||
        !consume('=')) {
      return fail(invalid("invalid counted-loop induction variable"));
    }
    const ir::Value initial_induction = parse_expression(0);
    if (!initial_induction.valid() || !consume(';') ||
        !add_loop_local(std::move(induction_name), initial_induction)) {
      return fail(status_);
    }
    const std::size_t induction_symbol = symbols_.size() - 1;

    const std::vector<ir::Value> initial_values = loop_values();
    const std::vector<ir::ValueType> loop_types(
        initial_values.size(), ir::ValueType::kFloat64);
    const bool has_continue = source_contains_keyword("continue");
    const ir::Block header = builder_->create_block(loop_types);
    const ir::Block body = builder_->create_block(0);
    const ir::Block update =
        has_continue ? builder_->create_block(loop_types) : ir::Block{};
    const ir::Block exit = builder_->create_block(loop_types);
    if (!header.valid() || !body.valid() ||
        (has_continue && !update.valid()) || !exit.valid() ||
        !builder_->jump(header, initial_values).ok() ||
        !builder_->set_insertion_block(header).ok()) {
      return fail(invalid("unable to create counted-loop control flow"));
    }
    continue_target_ = update;
    break_target_ = exit;
    install_loop_parameters(header);
    const std::vector<ir::Value> header_values = loop_values();

    const ir::Value condition = parse_condition();
    if (!condition.valid() || !consume(';') ||
        !parse_increment_clause(symbols_[induction_symbol].name) ||
        !consume(')') || !consume('{') ||
        !builder_->branch(condition, body, {}, exit, header_values).ok() ||
        !builder_->set_insertion_block(body).ok()) {
      return fail(status_.ok()
                      ? invalid("unable to construct counted-loop header")
                      : status_);
    }
    if (!builder_->safepoint(safepoint_site).valid() ||
        !parse_statement_block()) {
      return fail(status_.ok() ? invalid("unable to insert loop safepoint")
                               : status_);
    }

    if (has_continue) {
      if (!builder_->jump(update, loop_values()).ok() ||
          !builder_->set_insertion_block(update).ok()) {
        return fail(invalid("unable to enter counted-loop update"));
      }
      install_loop_parameters(update);
    }
    Symbol& induction = symbols_[induction_symbol];
    induction.value = builder_->float64_add(
        induction.value, induction_step_);
    if (!induction.value.valid() || !builder_->jump(header, loop_values()).ok()) {
      return fail(invalid("unable to close counted-loop backedge"));
    }

    if (!builder_->set_insertion_block(exit).ok()) {
      return fail(invalid("unable to select counted-loop exit"));
    }
    install_loop_parameters(exit);
    if (!consume_keyword("return")) {
      return fail(invalid("expected a return after the counted loop"));
    }
    const ir::Value result = parse_expression(0);
    skip_space();
    if (peek() == ';') {
      ++position_;
    }
    if (!result.valid() || !consume('}')) {
      return fail(status_);
    }
    skip_space();
    if (position_ != source_.size()) {
      return fail(invalid("unexpected source after the function body"));
    }
    const Status return_status = builder_->set_return(result);
    if (!return_status.ok()) {
      return fail(return_status);
    }

    jit::CompilationResult compilation =
        jit::Compiler::compile(std::move(*builder_).build());
    if (!compilation.ok()) {
      return {compilation.status, parameter_names_.size(), nullptr};
    }
    return {Status::ok_status(), parameter_names_.size(),
            std::move(compilation.function)};
  }

 private:
  TranslationResult fail(Status status) const {
    return {std::move(status), parameter_names_.size(), nullptr};
  }

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

  bool matches_keyword(std::string_view keyword) {
    skip_space();
    if (source_.substr(position_, keyword.size()) != keyword) {
      return false;
    }
    const std::size_t end = position_ + keyword.size();
    return end >= source_.size() || !is_identifier_continue(source_[end]);
  }

  bool source_contains_keyword(std::string_view keyword) const noexcept {
    std::size_t position = source_.find(keyword);
    while (position != std::string_view::npos) {
      const bool begins =
          position == 0 || !is_identifier_continue(source_[position - 1]);
      const std::size_t end = position + keyword.size();
      const bool ends =
          end >= source_.size() || !is_identifier_continue(source_[end]);
      if (begins && ends) {
        return true;
      }
      position = source_.find(keyword, position + 1);
    }
    return false;
  }

  bool consume(char expected) {
    skip_space();
    if (peek() != expected) {
      invalid("unexpected token in QuickJS counted loop");
      return false;
    }
    ++position_;
    return true;
  }

  bool consume_keyword(std::string_view keyword) {
    if (!matches_keyword(keyword)) {
      invalid("expected a QuickJS counted-loop keyword");
      return false;
    }
    position_ += keyword.size();
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
      for (const std::string& existing : parameter_names_) {
        if (existing == parameter) {
          invalid("duplicate parameters are not supported");
          return false;
        }
      }
      if (parameter_names_.size() >= kMaximumParameters) {
        invalid("QuickJS numeric function has too many parameters");
        return false;
      }
      parameter_names_.push_back(std::move(parameter));
      skip_space();
      if (peek() != ',') {
        return true;
      }
      ++position_;
    }
  }

  Symbol* symbol(std::string_view name) {
    for (Symbol& candidate : symbols_) {
      if (candidate.name == name) {
        return &candidate;
      }
    }
    return nullptr;
  }

  bool add_loop_local(std::string name, ir::Value value) {
    if (!value.valid()) {
      return false;
    }
    if (loop_symbol_indices_.size() >= kMaximumLocals) {
      invalid("QuickJS counted loop has too many local values");
      return false;
    }
    symbols_.push_back(Symbol{std::move(name), value, true});
    loop_symbol_indices_.push_back(symbols_.size() - 1);
    return true;
  }

  bool parse_local_declaration() {
    if (!consume_keyword("let")) {
      return false;
    }
    std::string name;
    if (!parse_identifier(&name) || symbol(name) != nullptr || !consume('=')) {
      invalid("invalid local declaration in QuickJS counted loop");
      return false;
    }
    const ir::Value initial = parse_expression(0);
    if (!initial.valid() || !consume(';')) {
      return false;
    }
    return add_loop_local(std::move(name), initial);
  }

  std::vector<ir::Value> loop_values() const {
    std::vector<ir::Value> result;
    result.reserve(loop_symbol_indices_.size());
    for (const std::size_t index : loop_symbol_indices_) {
      result.push_back(symbols_[index].value);
    }
    return result;
  }

  void install_loop_parameters(ir::Block block) {
    for (std::size_t index = 0; index < loop_symbol_indices_.size(); ++index) {
      symbols_[loop_symbol_indices_[index]].value =
          builder_->block_parameter(block, index);
    }
  }

  void restore_loop_values(const std::vector<ir::Value>& values) {
    for (std::size_t index = 0; index < loop_symbol_indices_.size(); ++index) {
      symbols_[loop_symbol_indices_[index]].value = values[index];
    }
  }

  bool parse_increment_clause(std::string_view induction_name) {
    skip_space();
    if (source_.substr(position_, 2) == "++" ||
        source_.substr(position_, 2) == "--") {
      const bool increments = source_[position_] == '+';
      position_ += 2;
      std::string name;
      if (!parse_identifier(&name) || name != induction_name) {
        invalid("counted-loop increment must update its induction variable");
        return false;
      }
      induction_step_ = builder_->float64_constant(increments ? 1.0 : -1.0);
      return true;
    }
    std::string name;
    if (!parse_identifier(&name) || name != induction_name) {
      invalid("counted-loop increment must update its induction variable");
      return false;
    }
    skip_space();
    if (source_.substr(position_, 2) == "++" ||
        source_.substr(position_, 2) == "--") {
      induction_step_ = builder_->float64_constant(
          source_[position_] == '+' ? 1.0 : -1.0);
      position_ += 2;
      return true;
    }
    if (source_.substr(position_, 2) != "+=" &&
        source_.substr(position_, 2) != "-=") {
      invalid("unsupported counted-loop induction update");
      return false;
    }
    const bool adds = source_[position_] == '+';
    position_ += 2;
    const ir::Value magnitude = parse_expression(0);
    if (!magnitude.valid()) {
      return false;
    }
    induction_step_ =
        adds ? magnitude
             : builder_->float64_subtract(builder_->float64_constant(0.0),
                                          magnitude);
    if (!induction_step_.valid()) {
      invalid("unable to lower counted-loop induction update");
      return false;
    }
    return true;
  }

  ir::Value parse_condition() {
    const ir::Value lhs = parse_expression(0);
    if (!lhs.valid()) {
      return {};
    }
    skip_space();
    const std::size_t operation_site = position_;
    std::string_view operation;
    if (source_.substr(position_, 2) == "<=") {
      operation = "<=";
      position_ += 2;
    } else if (source_.substr(position_, 2) == ">=") {
      operation = ">=";
      position_ += 2;
    } else if (peek() == '<' || peek() == '>') {
      operation = source_.substr(position_, 1);
      ++position_;
    } else {
      invalid("expected an ordered Float64 comparison");
      return {};
    }
    const ir::Value rhs = parse_expression(0);
    if (!rhs.valid()) {
      return {};
    }
    ir::Value result;
    if (operation == "<") {
      result = builder_->float64_less_than(lhs, rhs);
    } else if (operation == "<=") {
      result = builder_->float64_less_equal(lhs, rhs);
    } else if (operation == ">") {
      result = builder_->float64_less_than(rhs, lhs);
    } else {
      result = builder_->float64_less_equal(rhs, lhs);
    }
    if (!result.valid()) {
      status_ = {StatusCode::kInvalidArgument,
                 "unable to lower an ordered Float64 comparison",
                 operation_site};
    }
    return result;
  }

  bool parse_statement_block() {
    while (true) {
      skip_space();
      if (peek() == '}') {
        ++position_;
        return true;
      }
      if (peek() == '\0') {
        invalid("unterminated QuickJS counted-loop block");
        return false;
      }
      if (matches_keyword("if")) {
        if (!parse_if()) {
          return false;
        }
      } else if (!parse_assignment()) {
        return false;
      }
    }
  }

  bool parse_if() {
    if (!consume_keyword("if") || !consume('(')) {
      return false;
    }
    const ir::Value condition = parse_condition();
    if (!condition.valid() || !consume(')') || !consume('{')) {
      return false;
    }

    skip_space();
    const bool breaks = matches_keyword("break");
    const bool continues = !breaks && matches_keyword("continue");
    if (breaks || continues) {
      const std::vector<ir::Value> incoming = loop_values();
      const std::vector<ir::ValueType> types(
          incoming.size(), ir::ValueType::kFloat64);
      const ir::Block fallthrough = builder_->create_block(types);
      if (!fallthrough.valid() ||
          !consume_keyword(breaks ? "break" : "continue") || !consume(';') ||
          !consume('}')) {
        return false;
      }
      skip_space();
      if (matches_keyword("else")) {
        invalid("break/continue guards cannot have an else arm");
        return false;
      }
      const ir::Block transfer = breaks ? break_target_ : continue_target_;
      if (!transfer.valid() ||
          !builder_
               ->branch(condition, transfer, incoming, fallthrough, incoming)
               .ok() ||
          !builder_->set_insertion_block(fallthrough).ok()) {
        invalid("unable to construct break/continue guard");
        return false;
      }
      install_loop_parameters(fallthrough);
      return true;
    }

    const std::vector<ir::Value> incoming = loop_values();
    const std::vector<ir::ValueType> types(
        incoming.size(), ir::ValueType::kFloat64);
    const ir::Block true_block = builder_->create_block(0);
    const ir::Block false_block = builder_->create_block(0);
    const ir::Block merge = builder_->create_block(types);
    if (!true_block.valid() || !false_block.valid() || !merge.valid() ||
        !builder_->branch(condition, true_block, {}, false_block, {}).ok() ||
        !builder_->set_insertion_block(true_block).ok()) {
      invalid("unable to construct conditional control flow");
      return false;
    }

    restore_loop_values(incoming);
    if (!parse_statement_block()) {
      return false;
    }
    const std::vector<ir::Value> true_values = loop_values();
    if (!builder_->jump(merge, true_values).ok() ||
        !builder_->set_insertion_block(false_block).ok()) {
      invalid("unable to close the true conditional arm");
      return false;
    }

    restore_loop_values(incoming);
    skip_space();
    if (matches_keyword("else")) {
      if (!consume_keyword("else") || !consume('{') ||
          !parse_statement_block()) {
        return false;
      }
    }
    const std::vector<ir::Value> false_values = loop_values();
    if (!builder_->jump(merge, false_values).ok() ||
        !builder_->set_insertion_block(merge).ok()) {
      invalid("unable to close the false conditional arm");
      return false;
    }
    install_loop_parameters(merge);
    return true;
  }

  bool parse_assignment() {
    std::string name;
    if (!parse_identifier(&name)) {
      return false;
    }
    Symbol* destination = symbol(name);
    if (destination == nullptr || !destination->loop_local) {
      invalid("assignment target must be a counted-loop local");
      return false;
    }
    skip_space();
    char operation = '=';
    if (source_.substr(position_, 2) == "+=" ||
        source_.substr(position_, 2) == "-=" ||
        source_.substr(position_, 2) == "*=" ||
        source_.substr(position_, 2) == "/=") {
      operation = source_[position_];
      position_ += 2;
    } else if (peek() == '=') {
      ++position_;
    } else {
      invalid("expected an assignment operator");
      return false;
    }
    const ir::Value previous = destination->value;
    const ir::Value rhs = parse_expression(0);
    if (!rhs.valid() || !consume(';')) {
      return false;
    }
    if (operation == '=') {
      destination->value = rhs;
    } else if (operation == '+') {
      destination->value = builder_->float64_add(previous, rhs);
    } else if (operation == '-') {
      destination->value = builder_->float64_subtract(previous, rhs);
    } else if (operation == '*') {
      destination->value = builder_->float64_multiply(previous, rhs);
    } else {
      destination->value = builder_->float64_divide(previous, rhs);
    }
    if (!destination->value.valid()) {
      invalid("unable to lower a counted-loop assignment");
      return false;
    }
    return true;
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
      return operand.valid()
                 ? builder_->float64_subtract(builder_->float64_constant(0.0),
                                              operand)
                 : ir::Value{};
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
      return value.valid() && consume(')') ? value : ir::Value{};
    }
    if (is_identifier_start(peek())) {
      std::string name;
      if (!parse_identifier(&name)) {
        return {};
      }
      Symbol* resolved = symbol(name);
      if (resolved == nullptr) {
        invalid("numeric expression references an unknown identifier");
        return {};
      }
      return resolved->value;
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
      invalid("expected a local, number, or parenthesized expression");
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
  std::size_t position_{0};
  Status status_;
  std::vector<std::string> parameter_names_;
  std::vector<Symbol> symbols_;
  std::vector<std::size_t> loop_symbol_indices_;
  ir::Block continue_target_;
  ir::Block break_target_;
  ir::Value induction_step_;
  std::unique_ptr<ir::ControlFlowBuilder> builder_;
};

}  // namespace

bool looks_like_counted_loop(std::string_view source) noexcept {
  std::size_t position = source.find("for");
  while (position != std::string_view::npos) {
    const bool begins_keyword =
        position == 0 || !is_identifier_continue(source[position - 1]);
    const std::size_t end = position + 3;
    const bool ends_keyword =
        end == source.size() || !is_identifier_continue(source[end]);
    if (begins_keyword && ends_keyword) {
      return true;
    }
    position = source.find("for", position + 3);
  }
  return false;
}

TranslationResult translate_counted_loop(std::string_view source) {
  try {
    return CountedLoopParser(source).translate();
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate QuickJS counted-loop translation state"},
            0, nullptr};
  }
}

}  // namespace unijit::frontend::quickjs
