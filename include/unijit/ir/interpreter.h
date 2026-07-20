#ifndef UNIJIT_IR_INTERPRETER_H
#define UNIJIT_IR_INTERPRETER_H

#include <cstddef>
#include <vector>

#include "unijit/ir/function.h"
#include "unijit/status.h"

namespace unijit::ir {

struct EvaluationResult final {
  Status status;
  Word value{0};

  bool ok() const noexcept { return status.ok(); }
};

class Interpreter final {
 public:
  static EvaluationResult evaluate(const Function& function, const Word* args,
                                   std::size_t arg_count);

  static EvaluationResult evaluate(const Function& function,
                                   const std::vector<Word>& args) {
    return evaluate(function, args.data(), args.size());
  }
};

}  // namespace unijit::ir

#endif  // UNIJIT_IR_INTERPRETER_H
