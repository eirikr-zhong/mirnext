/* Licensed under the MIT License. Installed explicit API contracts. */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/IR/SSARewriter.hpp"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
static_assert (BuilderSignature<std::int64_t (std::int64_t)>);
static_assert (BuilderSignature<void ()>);
static_assert (BuilderSignature<bool (float, double, long double, void *)>);
static_assert (!BuilderSignature<std::string ()>);
static_assert (!BuilderSignature<int (int &)>);
static_assert (!BuilderSignature<void (int, ...)>);
static_assert (!BuilderSignature<void (int (*) ())>);
static_assert (!std::is_constructible_v<Value, int>);
static_assert (!std::is_constructible_v<bool, Value>);
static_assert (!std::is_constructible_v<bool, Result<void>>);
static_assert (!std::is_constructible_v<bool, Result<int>>);
static_assert (!std::is_constructible_v<bool, OptionalRef<int>>);
static_assert (!std::is_constructible_v<Result<const int &>, int>);
static_assert (!std::is_constructible_v<OptionalRef<const int>, int>);
static_assert (std::is_constructible_v<Result<const int &>, int &>);
static_assert (
  std::is_same_v<decltype (std::declval<Result<const int &>> ().value ()), const int &>);
template <class T>
concept ExpressionOperators = requires (T a) {
  a + a;
  a < a;
};
template <class T>
concept PointerOperators = requires (T a) {
  *a;
  a.operator->();
};
static_assert (!ExpressionOperators<Value>);
static_assert (!ExpressionOperators<CallResult>);
static_assert (!PointerOperators<Result<int>>);
static_assert (!PointerOperators<Result<int &>>);
static_assert (!PointerOperators<OptionalRef<int>>);
static_assert (std::is_same_v<decltype (std::declval<MIRBuilder> ().finish ()), Result<void>>);
static_assert (std::is_same_v<decltype (std::declval<MIRBuilder> ().create_module ("")), Module *>);
static_assert (
  std::is_same_v<decltype (std::declval<MIRBuilder> ().create_function<void ()> (nullptr, "")),
                 Function *>);
static_assert (std::is_same_v<decltype (std::declval<MIRBuilder> ().create_add ({}, {})), Value>);
static_assert (std::is_same_v<decltype (std::declval<MIRBuilder> ().create_call (
                                static_cast<const Function *> (nullptr))),
                              CallResult>);
static_assert (std::is_same_v<decltype (std::declval<Operand> ().get<Register> ()),
                              OptionalRef<const Register>>);
static_assert (std::is_same_v<decltype (std::declval<const Function> ().arguments ().front ()),
                              const Register &>);
static_assert (std::is_same_v<decltype (std::declval<const Program> ().find_module ("")),
                              OptionalRef<const Module>>);
static_assert (!std::is_copy_constructible_v<Program>);
static_assert (std::is_move_constructible_v<Program>);
static_assert (
  std::is_same_v<decltype (serialization::parse_text (std::string_view{})), Result<Program>>);
static_assert (std::is_same_v<decltype (std::declval<analysis::FunctionAnalysisManager> ()
                                          .get<analysis::ControlFlowAnalysis> ()),
                              Result<const analysis::ControlFlowGraph &>>);
static_assert (std::is_same_v<decltype (std::declval<ExecutionSession> ().find_function ("m", "f")),
                              Result<FunctionHandle>>);
static_assert (std::is_same_v<decltype (std::declval<ExecutionSession> ().find_data ("m", "d")),
                              Result<execution::DataHandle>>);
int main () {}
