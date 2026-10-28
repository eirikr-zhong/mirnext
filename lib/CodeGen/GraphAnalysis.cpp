#include "mirnext/CodeGen/Access.hpp"
/* Licensed under the MIT License. MIR traversal and loop-tree algorithms.
   Reference algorithms Copyright (C) 2018-2024 Vladimir Makarov. */
#include "mirnext/CodeGen/Analysis.hpp"
#include <algorithm>
#include <numeric>

namespace mirnext::codegen {
Result<MachineGraphAnalysis> MachineGraphAnalysis::build (const MachineFunction &function) {
  auto cfg = MachineControlFlow::build (function);
  if (!(cfg).has_value ()) return cfg.error ();
  return analyze (std::move ((cfg).value ()));
}
Result<MachineGraphAnalysis> MachineGraphAnalysis::build (const MachineFunction &function,
                                                          const MachineControlFlow &cfg) {
  auto valid = cfg.verify (function);
  if (!(valid).has_value ()) return valid.error ();
  return analyze (cfg);
}
MachineGraphAnalysis MachineGraphAnalysis::analyze (MachineControlFlow cfg) {
  MachineGraphAnalysis result;
  result.cfg_ = std::move (cfg);
  auto &g = result.cfg_;
  const auto count = detail::NodeAccess::blocks (g).size ();
  result.preorder_.resize (count);
  result.rpost_.resize (count);
  result.back_.resize (g.edges.size ());
  struct Visit {
    std::size_t block, next = 0;
  };
  std::vector<Visit> stack{{0}};
  std::size_t pre = 1, rpost = count;
  result.preorder_[0] = pre++;
  while (!stack.empty ()) {
    auto &v = stack.back ();
    if (v.next == g.outgoing[v.block].size ()) {
      result.rpost_[v.block] = rpost--;
      stack.pop_back ();
      continue;
    }
    auto e = g.outgoing[v.block][v.next++], dst = g.edges[e].destination;
    if (!result.preorder_[dst]) {
      result.preorder_[dst] = pre++;
      stack.push_back ({dst});
    } else if (!result.rpost_[dst])
      result.back_[e] = true;
  }
  // Disconnected blocks deliberately retain zero traversal numbers.
  auto &nodes = result.loops_;
  for (std::size_t b = 0; b < count; ++b) nodes.push_back ({b, b});
  // A separate disjoint-set index compresses membership queries without
  // changing the observable loop-tree parent/child links.
  std::vector<std::size_t> representatives (count);
  std::iota (representatives.begin (), representatives.end (), 0);
  auto top = [&] (std::size_t b) {
    auto root = b;
    while (representatives[root] != root) root = representatives[root];
    while (representatives[b] != b) {
      auto next = representatives[b];
      representatives[b] = root;
      b = next;
    }
    return root;
  };
  std::vector<std::size_t> entries;
  for (std::size_t b = 0; b < count; ++b)
    for (auto e : g.incoming[b])
      if (result.back_[e]) {
        entries.push_back (b);
        break;
      }
  std::stable_sort (entries.begin (), entries.end (),
                    [&] (auto a, auto b) { return result.rpost_[a] > result.rpost_[b]; });
  // Epoch marks avoid allocating a tree/set for each loop candidate.
  std::vector<std::size_t> seen (count, 0);
  std::size_t epoch = 0;
  for (auto entry : entries) {
    ++epoch;
    std::vector<std::size_t> loop, queue;
    auto collect = [&] (std::size_t b) {
      auto n = top (b);
      if (seen[n] == epoch) return;
      seen[n] = epoch;
      if (!result.preorder_[b] && !result.rpost_[b]) return;
      loop.push_back (n);
      queue.push_back (n);
    };
    for (auto e : g.incoming[entry])
      if (result.back_[e] && g.edges[e].source != entry) collect (g.edges[e].source);
    bool irreducible = false;
    while (!queue.empty ()) {
      auto n = queue.back ();
      queue.pop_back ();
      auto b = nodes[n].block == no_machine_node ? nodes[nodes[n].entry].block : nodes[n].block;
      if (b == 0) {
        irreducible = true;
        break;
      }
      for (auto e : g.incoming[b])
        if (g.edges[e].source != entry) collect (g.edges[e].source);
    }
    if (irreducible) continue;
    loop.push_back (entry);
    auto parent = nodes.size ();
    nodes.push_back ({});
    nodes[parent].entry = entry;
    seen.push_back (0);
    representatives.push_back (parent);
    while (!loop.empty ()) {
      auto n = loop.back ();
      loop.pop_back ();
      nodes[parent].children.push_back (n);
      nodes[n].parent = parent;
      representatives[n] = parent;
    }
    result.has_loops_ = true;
  }
  result.root_ = nodes.size ();
  nodes.push_back ({});
  representatives.push_back (result.root_);
  for (std::size_t b = 0; b < count; ++b) {
    auto n = top (b);
    if (n != result.root_) {
      nodes[result.root_].children.push_back (n);
      nodes[n].parent = result.root_;
      representatives[n] = result.root_;
    }
  }
  // Parents are always created after their children. Propagate depths once,
  // avoiding an ancestry walk for every block in deeply nested loop trees.
  std::vector<unsigned> depths (nodes.size ());
  result.depth_.resize (count);
  for (auto n = nodes.size (); n-- > 0;) {
    if (nodes[n].parent != no_machine_node)
      depths[n] = depths[nodes[n].parent] + (nodes[n].parent != result.root_);
    if (nodes[n].block != no_machine_node) result.depth_[nodes[n].block] = depths[n];
  }
  return result;
}
}  // namespace mirnext::codegen
