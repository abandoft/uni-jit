#include "counted_loop_translator.h"

#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "unijit/ir/control_flow.h"

namespace unijit::frontend::pocketpy {
namespace {

constexpr std::size_t kMaximumSourceBytes = 64U * 1024U;
constexpr std::size_t kMaximumParameters = 64;
constexpr std::size_t kMaximumLocals = 64;
constexpr std::size_t kMaximumExpressionDepth = 128;

bool is_identifier_start(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') || value == '_';
}

bool is_identifier_continue(char value) noexcept {
  return is_identifier_start(value) || (value >= '0' && value <= '9');
}

std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && value.front() == ' ') {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

bool begins_keyword(std::string_view value,
                    std::string_view keyword) noexcept {
  return value.substr(0, keyword.size()) == keyword &&
         (value.size() == keyword.size() ||
          !is_identifier_continue(value[keyword.size()]));
}

struct Line final {
  std::size_t offset{0};
  std::size_t indent{0};
  std::string_view text;
};

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
      return fail(invalid_at(0,
                             "PocketPy function source exceeds the frontend limit"));
    }
    if (!split_lines() || lines_.empty() || !parse_header(lines_.front())) {
      return fail(status_);
    }

    builder_ = std::make_unique<ir::ControlFlowBuilder>(
        std::vector<ir::ValueType>(parameter_names_.size(),
                                   ir::ValueType::kFloat64));
    for (std::size_t index = 0; index < parameter_names_.size(); ++index) {
      symbols_.push_back(
          Symbol{parameter_names_[index], builder_->parameter(index), false});
    }

    line_index_ = 1;
    while (line_index_ < lines_.size() && lines_[line_index_].indent == 4 &&
           !begins_keyword(lines_[line_index_].text, "for")) {
      if (begins_keyword(lines_[line_index_].text, "return")) {
        return fail(invalid_line(lines_[line_index_],
                                 "expected a counted for loop before return"));
      }
      if (!parse_initial_assignment(lines_[line_index_])) {
        return fail(status_);
      }
      ++line_index_;
    }
    if (line_index_ >= lines_.size() || lines_[line_index_].indent != 4 ||
        !parse_for_header(lines_[line_index_])) {
      return fail(status_.ok()
                      ? invalid_at(source_.size(),
                                   "expected an indented counted for loop")
                      : status_);
    }
    const std::size_t safepoint_site = lines_[line_index_].offset;
    ++line_index_;

    const std::vector<ir::Value> initial_values = loop_values();
    const std::vector<ir::ValueType> loop_types(
        initial_values.size(), ir::ValueType::kFloat64);
    const ir::Block header = builder_->create_block(loop_types);
    const ir::Block body = builder_->create_block(0);
    const ir::Block update = builder_->create_block(loop_types);
    const ir::Block exit = builder_->create_block(loop_types);
    if (!header.valid() || !body.valid() || !update.valid() || !exit.valid() ||
        !builder_->jump(header, initial_values).ok() ||
        !builder_->set_insertion_block(header).ok()) {
      return fail(invalid_at(safepoint_site,
                             "unable to create counted-loop control flow"));
    }
    continue_target_ = update;
    break_target_ = exit;
    install_loop_parameters(header);
    const std::vector<ir::Value> header_values = loop_values();
    Symbol *induction = symbol(induction_name_);
    if (induction == nullptr) {
      return fail(invalid_at(safepoint_site,
                             "counted-loop induction state is unavailable"));
    }
    const ir::Value condition =
        builder_->float64_less_than(induction->value, count_value_);
    if (!condition.valid() ||
        !builder_->branch(condition, body, {}, exit, header_values).ok() ||
        !builder_->set_insertion_block(body).ok() ||
        !builder_->safepoint(safepoint_site).valid()) {
      return fail(invalid_at(safepoint_site,
                             "unable to construct counted-loop header"));
    }

    if (!parse_body(8)) {
      return fail(status_);
    }
    if (!builder_->jump(update, loop_values()).ok() ||
        !builder_->set_insertion_block(update).ok()) {
      return fail(invalid_at(safepoint_site,
                             "unable to enter counted-loop update"));
    }
    install_loop_parameters(update);
    induction = symbol(induction_name_);
    induction->value = builder_->float64_add(
        induction->value, builder_->float64_constant(1.0));
    if (!induction->value.valid() ||
        !builder_->jump(header, loop_values()).ok()) {
      return fail(invalid_at(safepoint_site,
                             "unable to close counted-loop backedge"));
    }

    if (line_index_ >= lines_.size() || lines_[line_index_].indent != 4 ||
        !begins_keyword(lines_[line_index_].text, "return") ||
        !builder_->set_insertion_block(exit).ok()) {
      return fail(invalid_at(
          line_index_ < lines_.size() ? lines_[line_index_].offset
                                     : source_.size(),
          "expected a function return after the counted loop"));
    }
    install_loop_parameters(exit);
    const Line &return_line = lines_[line_index_];
    std::string_view return_expression =
        trim(return_line.text.substr(std::string_view("return").size()));
    set_expression(return_expression,
                   return_line.offset + return_line.text.find(return_expression));
    const ir::Value result = parse_expression(0);
    if (!result.valid() || !expression_finished()) {
      return fail(status_.ok()
                      ? invalid_expression(
                            "unexpected token after the return expression")
                      : status_);
    }
    ++line_index_;
    if (line_index_ != lines_.size()) {
      return fail(invalid_line(lines_[line_index_],
                               "unexpected statement after function return"));
    }
    const Status return_status = builder_->set_return(result);
    if (!return_status.ok()) {
      return fail(return_status);
    }

    jit::CompilationResult compilation =
        jit::Compiler::compile(std::move(*builder_).build(),
                               deoptimization_table_);
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

  Status invalid_at(std::size_t location, const char *message) {
    status_ = {StatusCode::kInvalidArgument, message, location};
    return status_;
  }

  Status invalid_line(const Line &line, const char *message) {
    return invalid_at(line.offset, message);
  }

  Status invalid_expression(const char *message) {
    return invalid_at(expression_offset_ + expression_position_, message);
  }

  bool split_lines() {
    std::size_t position = 0;
    while (position < source_.size()) {
      const std::size_t end = source_.find('\n', position);
      const std::size_t line_end =
          end == std::string_view::npos ? source_.size() : end;
      std::string_view raw = source_.substr(position, line_end - position);
      if (raw.find('\t') != std::string_view::npos) {
        invalid_at(position, "tabs are not supported in compiled PocketPy loops");
        return false;
      }
      std::size_t indent = 0;
      while (indent < raw.size() && raw[indent] == ' ') {
        ++indent;
      }
      const std::string_view text = trim(raw.substr(indent));
      if (!text.empty()) {
        if ((indent % 4) != 0) {
          invalid_at(position, "compiled PocketPy loops require four-space indentation");
          return false;
        }
        lines_.push_back(Line{position + indent, indent, text});
      }
      if (end == std::string_view::npos) {
        break;
      }
      position = end + 1;
    }
    return true;
  }

  bool parse_header(const Line &line) {
    if (line.indent != 0 || !begins_keyword(line.text, "def")) {
      invalid_line(line, "expected a top-level Python function definition");
      return false;
    }
    std::string_view header = trim(line.text.substr(3));
    const std::size_t open = header.find('(');
    const std::size_t close = header.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos ||
        close <= open || trim(header.substr(close + 1)) != ":") {
      invalid_line(line, "invalid PocketPy counted-loop function header");
      return false;
    }
    const std::string_view name = trim(header.substr(0, open));
    if (!valid_identifier(name)) {
      invalid_line(line, "invalid PocketPy function name");
      return false;
    }
    std::string_view parameters = header.substr(open + 1, close - open - 1);
    while (!trim(parameters).empty()) {
      const std::size_t comma = parameters.find(',');
      const std::string_view parameter =
          trim(parameters.substr(0, comma));
      if (!valid_identifier(parameter)) {
        invalid_line(line, "invalid PocketPy counted-loop parameter");
        return false;
      }
      for (const std::string &existing : parameter_names_) {
        if (existing == parameter) {
          invalid_line(line, "duplicate parameters are not supported");
          return false;
        }
      }
      if (parameter_names_.size() >= kMaximumParameters) {
        invalid_line(line, "PocketPy numeric function has too many parameters");
        return false;
      }
      parameter_names_.emplace_back(parameter);
      if (comma == std::string_view::npos) {
        parameters = {};
      } else {
        parameters.remove_prefix(comma + 1);
      }
    }
    return true;
  }

  bool valid_identifier(std::string_view value) const noexcept {
    if (value.empty() || !is_identifier_start(value.front())) {
      return false;
    }
    for (const char character : value.substr(1)) {
      if (!is_identifier_continue(character)) {
        return false;
      }
    }
    return true;
  }

  Symbol *symbol(std::string_view name) {
    for (Symbol &candidate : symbols_) {
      if (candidate.name == name) {
        return &candidate;
      }
    }
    return nullptr;
  }

  bool add_loop_local(std::string name, ir::Value value,
                      std::size_t location) {
    if (!value.valid()) {
      return false;
    }
    if (loop_symbol_indices_.size() >= kMaximumLocals) {
      invalid_at(location, "PocketPy counted loop has too many local values");
      return false;
    }
    symbols_.push_back(Symbol{std::move(name), value, true});
    loop_symbol_indices_.push_back(symbols_.size() - 1);
    return true;
  }

  bool parse_initial_assignment(const Line &line) {
    const std::size_t equals = line.text.find('=');
    if (equals == std::string_view::npos ||
        (equals + 1 < line.text.size() && line.text[equals + 1] == '=')) {
      invalid_line(line, "expected a numeric local initializer");
      return false;
    }
    const std::string_view name = trim(line.text.substr(0, equals));
    if (!valid_identifier(name) || symbol(name) != nullptr) {
      invalid_line(line, "invalid or duplicate PocketPy loop local");
      return false;
    }
    const std::string_view expression = trim(line.text.substr(equals + 1));
    set_expression(expression, line.offset + line.text.find(expression));
    const ir::Value initial = parse_expression(0);
    if (!initial.valid() || !expression_finished()) {
      if (status_.ok()) {
        invalid_expression("unexpected token after local initializer");
      }
      return false;
    }
    return add_loop_local(std::string(name), initial, line.offset);
  }

  bool parse_for_header(const Line &line) {
    if (!begins_keyword(line.text, "for") || line.text.back() != ':') {
      invalid_line(line, "expected a PocketPy for-range loop");
      return false;
    }
    std::string_view header = trim(line.text.substr(3));
    header.remove_suffix(1);
    header = trim(header);
    const std::size_t in_site = header.find(" in ");
    if (in_site == std::string_view::npos) {
      invalid_line(line, "counted loop must use 'in range(...)'");
      return false;
    }
    const std::string_view induction = trim(header.substr(0, in_site));
    std::string_view range = trim(header.substr(in_site + 4));
    constexpr std::string_view kRangePrefix = "range(";
    if (!valid_identifier(induction) || symbol(induction) != nullptr ||
        range.substr(0, kRangePrefix.size()) != kRangePrefix ||
        range.size() <= kRangePrefix.size() || range.back() != ')') {
      invalid_line(line, "invalid PocketPy range-counted loop header");
      return false;
    }
    range.remove_prefix(kRangePrefix.size());
    range.remove_suffix(1);
    range = trim(range);
    set_expression(range, line.offset + line.text.find(range));
    count_value_ = parse_expression(0);
    if (!count_value_.valid() || !expression_finished()) {
      if (status_.ok()) {
        invalid_expression("unexpected token in range bound");
      }
      return false;
    }
    induction_name_.assign(induction);
    return add_loop_local(induction_name_, builder_->float64_constant(0.0),
                          line.offset);
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

  void restore_loop_values(const std::vector<ir::Value> &values) {
    for (std::size_t index = 0; index < loop_symbol_indices_.size(); ++index) {
      symbols_[loop_symbol_indices_[index]].value = values[index];
    }
  }

  bool parse_body(std::size_t indent) {
    bool parsed_statement = false;
    while (line_index_ < lines_.size()) {
      const Line &line = lines_[line_index_];
      if (line.indent < indent) {
        break;
      }
      if (line.indent > indent) {
        invalid_line(line, "unexpected indentation in compiled PocketPy loop");
        return false;
      }
      if (line.text == "else:") {
        break;
      }
      parsed_statement = true;
      if (begins_keyword(line.text, "if")) {
        if (!parse_if(indent)) {
          return false;
        }
      } else {
        if (!parse_assignment(line)) {
          return false;
        }
        ++line_index_;
      }
    }
    if (!parsed_statement) {
      return invalid_at(
                 line_index_ < lines_.size() ? lines_[line_index_].offset
                                            : source_.size(),
                 "compiled PocketPy blocks cannot be empty")
          .ok();
    }
    return true;
  }

  bool parse_if(std::size_t indent) {
    const Line condition_line = lines_[line_index_];
    if (condition_line.text.back() != ':') {
      invalid_line(condition_line, "compiled PocketPy if requires a colon");
      return false;
    }
    std::string_view condition_text =
        trim(condition_line.text.substr(std::string_view("if").size()));
    condition_text.remove_suffix(1);
    condition_text = trim(condition_text);
    set_expression(condition_text,
                   condition_line.offset + condition_line.text.find(condition_text));
    const ir::Value condition = parse_condition();
    if (!condition.valid() || !expression_finished()) {
      if (status_.ok()) {
        invalid_expression("unexpected token after if condition");
      }
      return false;
    }
    ++line_index_;

    if (line_index_ < lines_.size() &&
        lines_[line_index_].indent == indent + 4 &&
        (lines_[line_index_].text == "break" ||
         lines_[line_index_].text == "continue") &&
        (line_index_ + 1 == lines_.size() ||
         lines_[line_index_ + 1].indent <= indent)) {
      const bool breaks = lines_[line_index_].text == "break";
      const std::vector<ir::Value> incoming = loop_values();
      const std::vector<ir::ValueType> types(
          incoming.size(), ir::ValueType::kFloat64);
      const ir::Block fallthrough = builder_->create_block(types);
      ++line_index_;
      if (line_index_ < lines_.size() &&
          lines_[line_index_].indent == indent &&
          lines_[line_index_].text == "else:") {
        invalid_line(lines_[line_index_],
                     "break/continue guards cannot have an else arm");
        return false;
      }
      const ir::Block transfer = breaks ? break_target_ : continue_target_;
      if (!fallthrough.valid() || !transfer.valid() ||
          !builder_
               ->branch(condition, transfer, incoming, fallthrough, incoming)
               .ok() ||
          !builder_->set_insertion_block(fallthrough).ok()) {
        invalid_line(condition_line,
                     "unable to construct break/continue guard");
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
      invalid_line(condition_line, "unable to construct conditional control flow");
      return false;
    }

    restore_loop_values(incoming);
    if (!parse_body(indent + 4)) {
      return false;
    }
    const std::vector<ir::Value> true_values = loop_values();
    if (!builder_->jump(merge, true_values).ok() ||
        !builder_->set_insertion_block(false_block).ok()) {
      invalid_line(condition_line, "unable to close the true conditional arm");
      return false;
    }

    restore_loop_values(incoming);
    if (line_index_ < lines_.size() && lines_[line_index_].indent == indent &&
        lines_[line_index_].text == "else:") {
      ++line_index_;
      if (!parse_body(indent + 4)) {
        return false;
      }
    }
    const std::vector<ir::Value> false_values = loop_values();
    if (!builder_->jump(merge, false_values).ok() ||
        !builder_->set_insertion_block(merge).ok()) {
      invalid_line(condition_line, "unable to close the false conditional arm");
      return false;
    }
    install_loop_parameters(merge);
    return true;
  }

  bool parse_assignment(const Line &line) {
    std::size_t operation_site = std::string_view::npos;
    char operation = '=';
    std::size_t operation_size = 1;
    constexpr std::string_view kAugmented[] = {"+=", "-=", "*=", "/="};
    for (const std::string_view candidate : kAugmented) {
      operation_site = line.text.find(candidate);
      if (operation_site != std::string_view::npos) {
        operation = candidate.front();
        operation_size = candidate.size();
        break;
      }
    }
    if (operation_site == std::string_view::npos) {
      operation_site = line.text.find('=');
    }
    if (operation_site == std::string_view::npos) {
      invalid_line(line, "expected a numeric loop assignment");
      return false;
    }
    const std::string_view name = trim(line.text.substr(0, operation_site));
    Symbol *destination = symbol(name);
    if (!valid_identifier(name) || destination == nullptr ||
        !destination->loop_local || destination->name == induction_name_) {
      invalid_line(line, "assignment target must be a mutable loop local");
      return false;
    }
    const std::string_view expression =
        trim(line.text.substr(operation_site + operation_size));
    const ir::Value previous = destination->value;
    set_expression(expression, line.offset + line.text.find(expression));
    const ir::Value rhs = parse_expression(0);
    if (!rhs.valid() || !expression_finished()) {
      if (status_.ok()) {
        invalid_expression("unexpected token after loop assignment");
      }
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
      destination->value =
          checked_divide(previous, rhs, line.offset + operation_site);
    }
    if (!destination->value.valid()) {
      invalid_line(line, "unable to lower a counted-loop assignment");
      return false;
    }
    return true;
  }

  void set_expression(std::string_view expression, std::size_t offset) {
    expression_ = expression;
    expression_offset_ = offset;
    expression_position_ = 0;
  }

  char expression_peek() const noexcept {
    return expression_position_ < expression_.size()
               ? expression_[expression_position_]
               : '\0';
  }

  void skip_expression_space() noexcept {
    while (expression_position_ < expression_.size() &&
           expression_[expression_position_] == ' ') {
      ++expression_position_;
    }
  }

  bool expression_finished() noexcept {
    skip_expression_space();
    return expression_position_ == expression_.size();
  }

  ir::Value checked_divide(ir::Value lhs, ir::Value rhs, std::size_t site) {
    runtime::DeoptimizationRecord deoptimization;
    deoptimization.site = site;
    deoptimization.resume_offset = site;
    deoptimization.reason = runtime::DeoptimizationReason::kDivisionByZero;
    deoptimization.recovery.reserve(parameter_names_.size() + 1 +
                                    loop_symbol_indices_.size());
    for (std::size_t index = 0; index < parameter_names_.size(); ++index) {
      deoptimization.recovery.push_back(runtime::RecoveryOperation::argument(
          index, ir::ValueType::kFloat64, index));
    }
    deoptimization.recovery.push_back(runtime::RecoveryOperation::exit_value(
        parameter_names_.size(), ir::ValueType::kFloat64));
    for (std::size_t index = 0; index < loop_symbol_indices_.size(); ++index) {
      deoptimization.recovery.push_back(
          runtime::RecoveryOperation::captured_value(
              parameter_names_.size() + 1 + index,
              ir::ValueType::kFloat64,
              symbols_[loop_symbol_indices_[index]].value));
    }
    const Status metadata_status = deoptimization_table_.add(deoptimization);
    if (!metadata_status.ok()) {
      status_ = metadata_status;
      return {};
    }
    if (!builder_->guard_float64_nonzero(rhs, site).valid()) {
      invalid_at(site, "unable to create a checked loop division");
      return {};
    }
    return builder_->float64_divide(lhs, rhs);
  }

  ir::Value parse_condition() {
    const ir::Value lhs = parse_expression(0);
    if (!lhs.valid()) {
      return {};
    }
    skip_expression_space();
    std::string_view operation;
    if (expression_.substr(expression_position_, 2) == "<=") {
      operation = "<=";
      expression_position_ += 2;
    } else if (expression_.substr(expression_position_, 2) == ">=") {
      operation = ">=";
      expression_position_ += 2;
    } else if (expression_peek() == '<' || expression_peek() == '>') {
      operation = expression_.substr(expression_position_, 1);
      ++expression_position_;
    } else {
      invalid_expression("expected an ordered Float64 comparison");
      return {};
    }
    const ir::Value rhs = parse_expression(0);
    if (!rhs.valid()) {
      return {};
    }
    if (operation == "<") {
      return builder_->float64_less_than(lhs, rhs);
    }
    if (operation == "<=") {
      return builder_->float64_less_equal(lhs, rhs);
    }
    if (operation == ">") {
      return builder_->float64_less_than(rhs, lhs);
    }
    return builder_->float64_less_equal(rhs, lhs);
  }

  ir::Value parse_expression(std::size_t depth) {
    ir::Value value = parse_product(depth + 1);
    while (value.valid()) {
      skip_expression_space();
      const char operation = expression_peek();
      if (operation != '+' && operation != '-') {
        break;
      }
      ++expression_position_;
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
      skip_expression_space();
      const char operation = expression_peek();
      if (operation != '*' && operation != '/') {
        break;
      }
      const std::size_t operation_site =
          expression_offset_ + expression_position_;
      ++expression_position_;
      const ir::Value rhs = parse_unary(depth + 1);
      if (!rhs.valid()) {
        return {};
      }
      value = operation == '*'
                  ? builder_->float64_multiply(value, rhs)
                  : checked_divide(value, rhs, operation_site);
    }
    return value;
  }

  ir::Value parse_unary(std::size_t depth) {
    if (depth > kMaximumExpressionDepth) {
      invalid_expression("PocketPy numeric expression is nested too deeply");
      return {};
    }
    skip_expression_space();
    if (expression_peek() == '+') {
      ++expression_position_;
      return parse_unary(depth + 1);
    }
    if (expression_peek() == '-') {
      ++expression_position_;
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
      invalid_expression("PocketPy numeric expression is nested too deeply");
      return {};
    }
    skip_expression_space();
    if (expression_peek() == '(') {
      ++expression_position_;
      const ir::Value value = parse_expression(depth + 1);
      skip_expression_space();
      if (!value.valid() || expression_peek() != ')') {
        invalid_expression("expected a closing parenthesis");
        return {};
      }
      ++expression_position_;
      return value;
    }
    if (is_identifier_start(expression_peek())) {
      const std::size_t begin = expression_position_++;
      while (expression_position_ < expression_.size() &&
             is_identifier_continue(expression_[expression_position_])) {
        ++expression_position_;
      }
      const std::string_view name =
          expression_.substr(begin, expression_position_ - begin);
      Symbol *resolved = symbol(name);
      if (resolved == nullptr) {
        invalid_expression("numeric expression references an unknown identifier");
        return {};
      }
      return resolved->value;
    }
    return parse_number();
  }

  ir::Value parse_number() {
    skip_expression_space();
    const std::size_t begin = expression_position_;
    bool digits = false;
    while (expression_peek() >= '0' && expression_peek() <= '9') {
      digits = true;
      ++expression_position_;
    }
    if (expression_peek() == '.') {
      ++expression_position_;
      while (expression_peek() >= '0' && expression_peek() <= '9') {
        digits = true;
        ++expression_position_;
      }
    }
    if (!digits) {
      invalid_expression("expected a local, number, or parenthesized expression");
      return {};
    }
    if (expression_peek() == 'e' || expression_peek() == 'E') {
      ++expression_position_;
      if (expression_peek() == '+' || expression_peek() == '-') {
        ++expression_position_;
      }
      const std::size_t exponent_begin = expression_position_;
      while (expression_peek() >= '0' && expression_peek() <= '9') {
        ++expression_position_;
      }
      if (expression_position_ == exponent_begin) {
        invalid_expression("numeric exponent has no digits");
        return {};
      }
    }
    if (is_identifier_start(expression_peek())) {
      invalid_expression("numeric literal is followed by an identifier");
      return {};
    }
    const std::string token(
        expression_.substr(begin, expression_position_ - begin));
    char *end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end == nullptr || *end != '\0') {
      invalid_expression("unable to decode a numeric literal");
      return {};
    }
    return builder_->float64_constant(value);
  }

  std::string_view source_;
  std::vector<Line> lines_;
  std::size_t line_index_{0};
  Status status_;
  std::vector<std::string> parameter_names_;
  std::vector<Symbol> symbols_;
  std::vector<std::size_t> loop_symbol_indices_;
  std::string induction_name_;
  ir::Value count_value_;
  ir::Block continue_target_;
  ir::Block break_target_;
  std::unique_ptr<ir::ControlFlowBuilder> builder_;
  runtime::DeoptimizationTable deoptimization_table_;
  std::string_view expression_;
  std::size_t expression_offset_{0};
  std::size_t expression_position_{0};
};

} // namespace

bool looks_like_counted_loop(std::string_view source) noexcept {
  std::size_t line_begin = 0;
  while (line_begin < source.size()) {
    const std::size_t line_end = source.find('\n', line_begin);
    const std::size_t size =
        (line_end == std::string_view::npos ? source.size() : line_end) -
        line_begin;
    const std::string_view line = trim(source.substr(line_begin, size));
    if (begins_keyword(line, "for")) {
      return true;
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_begin = line_end + 1;
  }
  return false;
}

TranslationResult translate_counted_loop(std::string_view source) {
  try {
    return CountedLoopParser(source).translate();
  } catch (const std::bad_alloc &) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate PocketPy counted-loop translation state"},
            0,
            nullptr};
  }
}

} // namespace unijit::frontend::pocketpy
