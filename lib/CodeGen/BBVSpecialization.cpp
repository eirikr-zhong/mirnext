#include "mirnext/IR/Access.hpp"
#include "mirnext/CodeGen/Access.hpp"
/* BBV property propagation derived from MIR. Licensed under the MIT License. */
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include <algorithm>
#include <map>
#include <sstream>
namespace mirnext::codegen {
namespace {
using ir::Opcode;
Error invalid (std::string message) {
  return {ErrorCode::InvalidOperand, "bbv-specialize: " + std::move (message)};
}
struct LocationLess {
  bool operator() (BBVPropertyLocation a, BBVPropertyLocation b) const {
    if (detail::NodeAccess::storage (a).index () != detail::NodeAccess::storage (b).index ())
      return detail::NodeAccess::storage (a).index () < detail::NodeAccess::storage (b).index ();
    if (auto r = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&a)))
      return r->encoding ()
             < std::get<target::PhysicalRegister> (detail::NodeAccess::storage (b)).encoding ();
    return std::get<const MachineMemoryLocation *> (detail::NodeAccess::storage (a))
             ->creation_order ()
           < std::get<const MachineMemoryLocation *> (detail::NodeAccess::storage (b))
               ->creation_order ();
  }
};
std::optional<BBVPropertyLocation> location (const MachineOperand &operand) {
  if (auto r = detail::operand_access (operand).get<target::PhysicalRegister> ()) return *r;
  if (auto m = detail::operand_access (operand).get<MachineMemory> ();
      m && detail::NodeAccess::location (*(m)))
    return BBVPropertyLocation (*detail::NodeAccess::location (*(m)));
  return {};
}
bool is (const MachineInstruction &i, Opcode opcode) {
  return i.opcode () == MachineOpcode (opcode);
}
bool call (const MachineInstruction &i) {
  return is (i, Opcode::Call) || is (i, Opcode::Inline) || is (i, Opcode::JCall);
}
bool move (const MachineInstruction &i) {
  // Private vector saves were UNSPEC in MIR, outside its property-copy cases.
  return is (i, Opcode::Mov) || is (i, Opcode::FMov) || is (i, Opcode::DMov)
         || is (i, Opcode::LDMov);
}
bool aliases (const MachineMemory &a, const MachineMemory &b) {
  return (a.alias.empty () || b.alias.empty () || a.alias == b.alias)
         && (a.nonalias.empty () || b.nonalias.empty () || a.nonalias != b.nonalias);
}
Result<std::uint64_t> integer (const MachineOperand &operand) {
  if (auto n = detail::operand_access (operand).get<std::int64_t> ())
    return static_cast<std::uint64_t> (*n);
  if (auto n = detail::operand_access (operand).get<std::uint64_t> ()) return *n;
  return invalid ("property requires an integer constant");
}
}  // namespace
Result<BBVProperties> BBVProperties::create (const MachineFunction &function,
                                             std::span<const BBVProperty> properties) {
  BBVProperties result (function);
  result.entries_.assign (properties.begin (), properties.end ());
  // Validate before comparing typed locations, including null/foreign memory.
  for (auto &p : result.entries_) {
    if (auto r
        = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&p.location))) {
      if (!function.target ().register_info (*r).get () || p.memory)
        return invalid ("invalid register property");
    } else {
      auto m = std::get<const MachineMemoryLocation *> (detail::NodeAccess::storage (p.location));
      if (!m || &m->function () != &function || !p.memory
          || detail::NodeAccess::location (*(p.memory)) != m)
        return invalid ("invalid memory property owner or access");
      for (auto address : {p.memory->base, p.memory->index})
        if (address) {
          auto r = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&*address));
          if (!r || !function.target ().register_info (*r).get ())
            return invalid ("memory property requires an allocated address");
        }
    }
  }
  std::sort (result.entries_.begin (), result.entries_.end (),
             [] (auto &a, auto &b) { return LocationLess{}(a.location, b.location); });
  for (std::size_t n = 1; n < result.entries_.size (); ++n)
    if (result.entries_[n - 1].location == result.entries_[n].location)
      return invalid ("duplicate property location");
  return result;
}
OptionalRef<const BBVProperty> BBVProperties::find (BBVPropertyLocation location) const {
  // Public lookups may contain foreign or null identities; compare equality
  // without dereferencing them. Generation uses its ordered working map.
  for (auto &p : entries_)
    if (p.location == location) return p;
  return {};
}
Result<void> BBVProperties::verify () const {
  for (std::size_t n = 0; n < entries_.size (); ++n) {
    auto &p = entries_[n];
    if (auto r
        = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&p.location))) {
      if (!function_->target ().register_info (*r).get () || p.memory)
        return invalid ("invalid register property");
    } else {
      auto m = std::get<const MachineMemoryLocation *> (detail::NodeAccess::storage (p.location));
      if (!m || &m->function () != function_ || !p.memory
          || detail::NodeAccess::location (*(p.memory)) != m)
        return invalid ("invalid memory property owner or access");
      for (auto address : {p.memory->base, p.memory->index})
        if (address) {
          auto r = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&*address));
          if (!r || !function_->target ().register_info (*r).get ())
            return invalid ("memory property requires an allocated address");
        }
    }
    if (n && !LocationLess{}(entries_[n - 1].location, p.location))
      return invalid ("property locations are not unique and ordered");
  }
  return {};
}
namespace detail {
struct BBVState {
  std::shared_ptr<const char> identity = std::make_shared<const char> (0);
  BBVPreparation preparation;
  std::vector<std::unique_ptr<BBVVersion>> versions;
  std::unordered_map<const BBVBlock *, std::size_t> blocks;
  explicit BBVState (BBVPreparation p) : preparation (std::move (p)) {
    for (auto &b : preparation.blocks ()) blocks.emplace (&b, blocks.size ());
    versions.resize (blocks.size ());
  }
  Result<BBVVersionRequest> request (const BBVBlock &block, const BBVProperties &incoming,
                                     bool call_entry) {
    auto found = blocks.find (&block);
    if (found == blocks.end ()) return invalid ("foreign block");
    if (&incoming.function () != &preparation.function ()) return invalid ("foreign property set");
    auto valid = incoming.verify ();
    if (!(valid).has_value ()) return valid.error ();
    auto &version = versions[found->second];
    bool created = !version;
    if (created)
      version.reset (new BBVVersion (*this, BlockVersionId (identity, found->second), block,
                                     incoming, call_entry));
    return detail::NodeAccess::make_BBVVersionRequest (version.get (), created);
  }
  Result<const BBVSpecialization &> specialize (const BBVVersion &view) {
    if (view.owner_ != this) return invalid ("foreign version");
    auto &version = *versions.at (blocks.at (&view.block ()));
    if (version.specialized_) return *(version.specialized_.get ());
    std::map<BBVPropertyLocation, BBVProperty, LocationLess> properties;
    for (auto &p : version.incoming_.entries ()) properties.emplace (p.location, p);
    auto snapshot = [&] () {
      BBVProperties result (preparation.function ());
      result.entries_.reserve (properties.size ());
      for (auto &[identity, p] : properties) result.entries_.push_back (p);
      return result;
    };
    auto result = std::unique_ptr<BBVSpecialization> (new BBVSpecialization (snapshot ()));
    auto edge = [&] (BBVSuccessor::Kind kind, const MachineInstruction *i, std::size_t operand,
                     const BBVBlock &destination) -> Result<void> {
      auto target = request (destination, snapshot (), false);
      if (!(target).has_value ()) return target.error ();
      result->successors_.push_back (
        detail::NodeAccess::make_BBVVersionEdge (kind, i, operand,
                                                 detail::NodeAccess::version (((target)).value ()),
                                                 target.value ().created));
      return {};
    };
    auto label_edge = [&] (BBVSuccessor::Kind kind, const MachineInstruction &i,
                           std::size_t operand) -> Result<void> {
      auto label = detail::operand_access (i.operands ()[operand]).get<const MachineLabel *> ();
      if (!label) return invalid ("successor requires a typed label");
      auto block = preparation.block_for (**label);
      if (!(block).has_value ()) return block.error ();
      return edge (kind, &i, operand, (block).value ());
    };
    bool unconditional = false, indirect = false, switch_exit = false, exit = false;
    auto instructions = ir::detail::ReferenceAccess::pointers (version.block ().instructions ());
    for (auto i : instructions) {
      bool skip = false;
      if (i->opcode () == MachineOpcode (MachinePseudoOpcode::Use))
        skip = true;
      else if (is (*i, Opcode::PrSet)) {
        auto value = integer (i->operands ()[1]);
        if (!(value).has_value ()) return value.error ();
        if (auto destination = location (i->operands ()[0])) {
          if (!(value).value ())
            properties.erase (*destination);
          else {
            std::optional<MachineMemory> memory;
            if (auto m = detail::operand_access (i->operands ()[0]).get<MachineMemory> ())
              memory = *m;
            properties.insert_or_assign (*destination,
                                         BBVProperty{*destination,
                                                     static_cast<std::uint32_t> ((value).value ()),
                                                     std::move (memory)});
          }
        }
        skip = true;
      } else if (is (*i, Opcode::PrBeq) || is (*i, Opcode::PrBne)) {
        auto value = integer (i->operands ()[2]);
        if (!(value).has_value ()) return value.error ();
        std::uint32_t actual = 0;
        if (auto spot = location (i->operands ()[1]))
          if (auto p = properties.find (*spot); p != properties.end ()) actual = p->second.value;
        if (((value).value () == actual) == is (*i, Opcode::PrBeq)) {
          result->emissions_.push_back (detail::NodeAccess::make_BBVEmission (i, Opcode::Jmp));
          auto added = label_edge (BBVSuccessor::Kind::Branch, *i, 0);
          if (!(added).has_value ()) return added.error ();
          unconditional = true;
        }
        skip = true;
      } else if (move (*i)) {
        auto destination = location (i->operands ()[0]), source = location (i->operands ()[1]);
        if (!source) {
          if (destination) properties.erase (*destination);
        } else if (!destination) {
          for (auto p = properties.begin (); p != properties.end ();)
            if (detail::NodeAccess::storage (p->first).index () == 1)
              p = properties.erase (p);
            else
              ++p;
        } else if (auto found = properties.find (*source); found != properties.end ()) {
          auto attribute = found->second;
          attribute.location = *destination;
          attribute.memory.reset ();
          if (auto m = detail::operand_access (i->operands ()[0]).get<MachineMemory> ()) {
            // Sparse locations are identities, not offsets into a compact
            // attribute array. Visit the sparse map directly when invalidating aliases.
            for (auto p = properties.begin (); p != properties.end ();) {
              if (detail::NodeAccess::storage (p->first).index () == 1
                  && (!p->second.memory || aliases (*m, *p->second.memory)))
                p = properties.erase (p);
              else
                ++p;
            }
            attribute.memory = *m;
          }
          properties.insert_or_assign (*destination, std::move (attribute));
        }
        // A known register with no property deliberately leaves the old
        // destination property alone, matching MIR rather than a new policy.
      }
      if (!skip) {
        result->emissions_.push_back (detail::NodeAccess::make_BBVEmission (i, i->opcode ()));
        if (is (*i, Opcode::LAddr)) {
          auto added = label_edge (BBVSuccessor::Kind::LabelAddress, *i, 1);
          if (!(added).has_value ()) return added.error ();
        } else if (is (*i, Opcode::Switch)) {
          switch_exit = true;
          for (std::size_t n = 1; n < i->operands ().size (); ++n) {
            auto added = label_edge (BBVSuccessor::Kind::SwitchCase, *i, n);
            if (!(added).has_value ()) return added.error ();
          }
        } else if (is (*i, Opcode::JmpIndirect))
          indirect = true;
        else if (is (*i, Opcode::Jmp)) {
          auto added = label_edge (BBVSuccessor::Kind::Branch, *i, 0);
          if (!(added).has_value ()) return added.error ();
          unconditional = true;
        } else if (is (*i, Opcode::Ret) || is (*i, Opcode::JRet))
          exit = true;
        else {
          auto op = i->opcode ();
          if (auto code = std::get_if<Opcode> (&op);
              code && ir::instruction_descriptor (*code).get ()->flow == ir::ControlFlow::Branch) {
            auto added = label_edge (BBVSuccessor::Kind::Branch, *i, 0);
            if (!(added).has_value ()) return added.error ();
            unconditional = *code == Opcode::Jmp;
          }
        }
      }
    }
    // Calls stay inside a BBV block. The original rule drops memory properties
    // only at a terminal call (before reserving the fallthrough version).
    if (!instructions.empty () && call (*instructions.back ())) {
      for (auto p = properties.begin (); p != properties.end ();)
        if (detail::NodeAccess::storage (p->first).index () == 1)
          p = properties.erase (p);
        else
          ++p;
    }
    result->outgoing_ = snapshot ();
    if (!unconditional && !indirect && !switch_exit && !exit) {
      auto successors = version.block ().successors ();
      auto next = std::find_if (successors.begin (), successors.end (),
                                [] (auto &e) { return e.kind == BBVSuccessor::Kind::Fallthrough; });
      if (next == successors.end ()) return invalid ("fallthrough has no following block");
      auto added = edge (BBVSuccessor::Kind::Fallthrough, detail::NodeAccess::instruction (*(next)),
                         SIZE_MAX, *detail::NodeAccess::block (*(next)));
      if (!(added).has_value ()) return added.error ();
    }
    version.specialized_ = std::move (result);
    return *(version.specialized_.get ());
  }
};
}  // namespace detail
BBVProgram::BBVProgram (std::unique_ptr<detail::BBVState> state) : state_ (std::move (state)) {}
BBVProgram::BBVProgram (BBVProgram &&) noexcept = default;
BBVProgram &BBVProgram::operator= (BBVProgram &&) noexcept = default;
BBVProgram::~BBVProgram () = default;
Result<BBVProgram> BBVProgram::create (BBVPreparation preparation) {
  auto valid = preparation.verify ();
  if (!(valid).has_value ()) return valid.error ();
  return BBVProgram (std::make_unique<detail::BBVState> (std::move (preparation)));
}
const BBVPreparation &BBVProgram::preparation () const { return state_->preparation; }
Result<BBVVersionRequest> BBVProgram::request_version (const BBVBlock &block,
                                                       const BBVProperties &properties,
                                                       bool call_entry) {
  if (!state_) return invalid ("moved-from program");
  return state_->request (block, properties, call_entry);
}
Result<const BBVSpecialization &> BBVProgram::specialize (const BBVVersion &version) {
  if (!state_) return invalid ("moved-from program");
  return state_->specialize (version);
}
Result<void> BBVProgram::verify () const {
  if (!state_) return invalid ("moved-from program");
  auto valid = state_->preparation.verify ();
  if (!(valid).has_value ()) return valid;
  for (auto &v : state_->versions)
    if (v) {
      valid = v->incoming ().verify ();
      if (!(valid).has_value ()) return valid;
      if (auto plan = v->specialization ().get ()) {
        valid = plan->outgoing ().verify ();
        if (!(valid).has_value ()) return valid;
        for (auto &e : plan->emissions ()) {
          auto block = state_->preparation.block_for (*detail::NodeAccess::instruction (e));
          if (!(block).has_value () || &(block).value () != &v->block ())
            return invalid ("foreign emission");
        }
        for (auto &e : plan->successors ()) {
          auto block = state_->blocks.find (&detail::NodeAccess::version (e)->block ());
          if (block == state_->blocks.end ()
              || state_->versions[block->second].get () != detail::NodeAccess::version (e))
            return invalid ("foreign successor version");
        }
      }
    }
  return {};
}
std::string BBVProgram::dump () const {
  if (!state_) return "<moved-from BBV program>\n";
  std::ostringstream out;
  out << state_->preparation.dump ();
  for (auto &v : state_->versions)
    if (v) {
      out << "version bb" << v->block ().index () << (v->call_entry () ? " call" : " jump")
          << " in=" << v->incoming ().entries ().size () << '\n';
      if (auto p = v->specialization ().get ()) {
        for (auto &e : p->emissions ())
          out << "  #" << detail::NodeAccess::instruction (e)->creation_order () << ' '
              << opcode_name (e.opcode) << '\n';
        for (auto &e : p->successors ())
          out << "  -> bb" << detail::NodeAccess::version (e)->block ().index ()
              << (e.newly_created ? " new" : " reused") << '\n';
      }
    }
  return out.str ();
}
}  // namespace mirnext::codegen
