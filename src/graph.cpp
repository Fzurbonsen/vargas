/**
 * @author Ravi Gaddipati (rgaddip1@jhu.edu)
 * @date November 20, 2015
 *
 * vargas::Graph is a DAG representation of a reference and its variants.
 * The class wraps a gssw_graph from gssw and provides a way to construct
 * graphs from a FASTA and a VCF file with various options.
 *
 * GSSW was originally written by Erik Garrison and was moderately modified.
 *
 * graph.cpp
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "../include/graph.h"


long vargas::Graph::Node::_newID = 0;


vargas::Graph::Graph(const vargas::Graph &g,
                     const std::vector<bool> &filter) {

  _IDMap = g._IDMap;
  _pop_size = g.pop_size();

//  std::unordered_set<long> indexes;
//  for (long i = 0; i < filter.size(); ++i) {
//    if (filter[i]) {
//      indexes.insert(i);
//    }
//  }

  std::vector<long> indexes; // indexes of the individuals that are included in the filter
  for (long i = 0; i < filter.size(); ++i) {
    if (filter[i]) {
      indexes.push_back(i);
    }
  }

//  // Add all nodes
//  std::unordered_map<long, nodeptr> includedNodes;
//  for (auto &n : *(g._IDMap)) {
//    auto &indivs = n.second->individuals();
//    for (int i = 0; i < indivs.size(); ++i) {
//      if (indivs[i] && indexes.count(i)) {
//        includedNodes[n.first] = n.second;
//        break;
//      }
//    }
//  }


  // Add all nodes
  std::unordered_map<long, nodeptr> includedNodes;
  for (auto &n : *(g._IDMap)) {
    for (long i : indexes) {
      if (n.second->belongs(i)) {
        includedNodes[n.first] = n.second;
        break;
      }
    }
  }

  _build_derived_edges(g, includedNodes);

  _add_order = g._add_order;
  for (int i = _add_order.size() - 1; i >= 0; --i) {
    if (includedNodes.count(_add_order[i]) == 0) _add_order.erase(_add_order.begin() + i);
  }

  // Use the same description but add the filter we used
  _desc = g.desc() + "\nfilter: ";
  for (auto b : filter) {
    _desc += b == true ? "1" : "0";
    _desc += ",";
  }

  finalize();
}


vargas::Graph::Graph(const Graph &g,
                     Type type) {
  _IDMap = g._IDMap;
  _pop_size = g.pop_size();
  std::unordered_map<long, nodeptr> includedNodes;

  if (type == REF) {
    // Add all nodes
    for (auto &n : *(g._IDMap)) {
      if (n.second->is_ref()) {
        includedNodes[n.first] = n.second;
      }
    }
    _desc = g.desc() + "\nfilter: REF";
  }

  else if (type == MAXAF) {
    long curr = g.root();
    long maxid, size;
    while (true) {
      includedNodes[curr] = (*g._IDMap).at(curr);
      if (g._next_map.count(curr) == 0) break; // end of graph
      maxid = g._next_map.at(curr).at(0);
      size = g._next_map.at(curr).size();
      for (long i = 1; i < size; ++i) {
        const long &id = g._next_map.at(curr).at(i);
        if ((*g._IDMap).at(id)->freq() > (*g._IDMap).at(maxid)->freq())
          maxid = id;
      }
      curr = maxid;
    }
    _desc = g.desc() + "\nfilter: MAXAF";
  }

  _build_derived_edges(g, includedNodes);

  _add_order = g._add_order;
  for (int i = _add_order.size() - 1; i >= 0; --i) {
    if (includedNodes.count(_add_order[i]) == 0) _add_order.erase(_add_order.begin() + i);
  }
  finalize();
}


void vargas::Graph::_build_derived_edges(const vargas::Graph &g,
                                         const std::unordered_map<long, nodeptr> &includedNodes) {
  // Add all edges for included nodes
  for (auto &n : includedNodes) {
    if (g._next_map.find(n.second->id()) == g._next_map.end()) continue;
    for (auto e : g._next_map.at(n.second->id())) {
      if (includedNodes.find(e) != includedNodes.end()) {
        add_edge(n.second->id(), e);
      }
    }
  }

  // Set the new root
  if (includedNodes.find(g.root()) == includedNodes.end()) {
    throw std::invalid_argument("Currently the root must be common to all graphs.");
  }
  _root = g.root();
}

//TODO Assumes insertion was in order
void vargas::Graph::finalize() {
  _toposort = _add_order;
  return;

  /**
   * For now the user must insert nodes in topographical order. Below method
   * is not suitable for long graphs.
   */

  _toposort.clear();
  std::set<long> unmarked, tempmarked, permmarked;
  for (auto &n : *_IDMap) {
    // Leave out the lone nodes
    if (_next_map.count(n.first) != 0 || _prev_map.count(n.first) != 0) unmarked.insert(n.first);
  }
  while (!unmarked.empty()) {
    _visit(*unmarked.begin(), unmarked, tempmarked, permmarked);
  }
  std::reverse(_toposort.begin(), _toposort.end());
}


long vargas::Graph::add_node(Node &n) {
  if (_IDMap->find(n.id()) != _IDMap->end()) return 0; // make sure node isn't duplicate
  if (_root < 0) _root = n.id(); // first node added is default root

  _IDMap->emplace(n.id(), std::make_shared<Node>(n));
  _add_order.push_back(n.id());
  return n.id();
}


bool vargas::Graph::add_edge(long n1,
                             long n2) {
  // Check if the nodes exist
  if (_IDMap->find(n1) == _IDMap->end() || _IDMap->find(n2) == _IDMap->end()) return false;

  // init if first edge to be added
  if (_next_map.find(n1) == _next_map.end()) {
    _next_map[n1] = std::vector<long>();
  }
  if (_prev_map.find(n2) == _prev_map.end()) {
    _prev_map[n2] = std::vector<long>();
  }
  _next_map[n1].push_back(n2);
  _prev_map[n2].push_back(n1);
  _toposort.clear(); // any ordering is invalidated
  return true;
}


void vargas::Graph::_visit(long n,
                           std::set<long> &unmarked,
                           std::set<long> &temp,
                           std::set<long> &perm) {
  if (temp.count(n) != 0) throw std::domain_error("Graph contains a cycle.");
  if (unmarked.count(n)) {
    unmarked.erase(n);
    temp.insert(n);
    if (_next_map.find(n) != _next_map.end()) {
      for (auto m : _next_map[n]) {
        _visit(m, unmarked, temp, perm);
      }
    }
    temp.erase(n);
    perm.insert(n);
    _toposort.push_back(n);
  }
}


void vargas::GraphBuilder::build(vargas::Graph &g) {
  g = vargas::Graph();
  _fa.open(_fa_file);
  _vf.open(_vf_file);
  if (!_fa.good()) throw std::invalid_argument("Invalid FASTA file: " + _fa.file());
  if (!_vf.good()) throw std::invalid_argument("Invalid B/VCF file: " + _vf.file());

  _vf.create_ingroup(_ingroup);

  // If no region is specified, the default is the first sequence in the FASTA file
  if (_vf.region_chr().length() == 0) {
    _vf.set_region(_fa.sequences()[0] + ":0-0");
  }

  int curr = _vf.region_lower(); // The Graph has been built up to this position, exclusive
  std::vector<int> prev_unconnected; // ID's of nodes at the end of the Graph left unconnected
  std::vector<int> curr_unconnected; // ID's of nodes added that are unconnected

  const auto sample_size = _vf.samples().size();
  g.set_popsize(sample_size);

  while (_vf.next()) {
    _vf.genotypes();
    auto &af = _vf.frequencies();

    curr = _build_linear_ref(g, prev_unconnected, curr_unconnected, curr, _vf.pos());

    // Add variant nodes
    // Positions of variant nodes are referenced to ref node
    curr += _vf.ref().length();

    // ref pos
    {
      Graph::Node n;
      n.set_endpos(curr - 1);
      n.set_seq(_vf.ref());
      n.set_as_ref();
      n.set_af(af[0]);
      n.set_population(std::vector<bool>(sample_size, true));
      curr_unconnected.push_back(g.add_node(n));
    }

    //alt nodes
    for (int i = 1; i < _vf.alleles().size(); ++i) {
      Graph::Node n;
      n.set_not_ref();
      const std::string &allele = _vf.alleles()[i];
      n.set_seq(allele);
      n.set_af(af[i]);
      n.set_population(_vf.allele_pop(allele));
      curr_unconnected.push_back(g.add_node(n));
    }

    _build_edges(g, prev_unconnected, curr_unconnected);

  }
  // Nodes after last variant
  _build_linear_ref(g, prev_unconnected, curr_unconnected, curr, _vf.region_upper());

  _fa.close();
  _vf.close();
  g.finalize();

  std::string desc = "REF: " + _fa.file();
  desc += "\nB/VCF: " + _vf.file();
  desc += "\nRegion: " + _vf.region_chr() + ":" + std::to_string(_vf.region_lower()) + "-"
      + std::to_string(_vf.region_upper());
  desc += "\nIngroup: " + _vf.ingroup_str();
}


void vargas::GraphBuilder::_build_edges(vargas::Graph &g,
                                        std::vector<int> &prev,
                                        std::vector<int> &curr) {
  for (int pID : prev) {
    for (int cID : curr) {
      g.add_edge(pID, cID);
    }
  }
  prev = curr;
  curr.clear();
}


int vargas::GraphBuilder::_build_linear_ref(Graph &g,
                                            std::vector<int> &prev,
                                            std::vector<int> &curr,
                                            int pos,
                                            int target) {

  if (target <= 0) target = _fa.seq_len(_vf.region_chr());
  while (pos < target) {
    Graph::Node n;
    n.set_as_ref();
    n.set_population(std::vector<bool>(_vf.samples().size(), true));

    if (pos + _max_node_len >= target) {
      n.set_seq(_fa.subseq(_vf.region_chr(), pos, target - 1));
      pos = target;
    }

    else {
      n.set_seq(_fa.subseq(_vf.region_chr(), pos, pos + _max_node_len - 1));
      pos += _max_node_len;
    }

    n.set_endpos(pos - 1);
    n.set_as_ref();
    curr.push_back(g.add_node(n));
    _build_edges(g, prev, curr);
  }
  return pos;
}


void vargas::GraphBuilder::ingroup(int percent) {
  if (percent < 0 || percent > 100) return;
  _ingroup = percent;
}
