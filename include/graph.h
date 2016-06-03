/**
 * @author Ravi Gaddipati (rgaddip1@jhu.edu)
 * @date May 28, 2016
 *
 * Implementation of a directed graph. Each node stores a sequence and relevant
 * information. Graphs can be derived from other graphs with a filter, allowing
 * the extraction of population subsets.
 *
 * @file graph.h
 */

#ifndef VARGAS_GRAPH_H
#define VARGAS_GRAPH_H

#include <cstdio>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <bitset>
#include <thread>
#include "fasta.h"
#include "varfile.h"
#include "doctest/doctest.h"
#include "utils.h"
#include "dyn_bitset.h"

namespace vargas {

/**
 * Represents a Graph of the genome. The Graph is backed by a map of Graph::Nodes, and edges
 * are backed by a map of node ID's.
 */
class Graph {

 public:

  /**
   * When a normal population filter is not used, a flag can be used. REF includes
   * only reference alleles, MAXAF picks the allele with the highest frequency.
   * Both result in linear graphs.
   */
  enum Type { REF, MAXAF };

  /**
   * A population is represented with a dynamic bitset. This allows
   * for quick population overlap checks.
   */
  typedef dyn_bitset<32> Population;

  /**
   * Represents a node in the directed graphs. Sequences are stored numerically.
   * populations are stored as bitsets, where 1 indicates that indivudal posseses
   * the allele.
   */
  class Node {
   public:
    // Assign a unique ID to each node
    Node() : _id(_newID++) { }
    Node(int pos,
         const std::string &seq,
         const std::vector<bool> &pop,
         bool ref,
         float af) :
        _endPos(pos), _seq(seq_to_num(seq)), _individuals(pop), _ref(ref), _af(af), _id(_newID++) { }

    // Access functions
    ulong length() const { return _seq.size(); } // Length of sequence
    int end() const { return _endPos; } // Sequence end position in genome
    int belongs(uint ind) const {
      if (_ref) return -1; // True if a ref node
      return _individuals[ind];
    } // Check if a certain individual has this node
    const std::vector<uchar> &seq() const { return _seq; } // Sequence in numeric form
    std::string seq_str() const { return num_to_seq(_seq); }
    ulong pop_size() const { return _individuals.size(); } // How many individuals are represented in the node
    long id() const { return _id; } // Node ID
    bool is_ref() const { return _ref; } // True if part of the reference seq
    float freq() const { return _af; } // allele frequency. <0 if ref
    const std::vector<bool> &individuals() const { return _individuals; }

    static long _newID; // ID of the next instance to be created

    // Setup functions
    void setID(long id) {
      if (id >= _newID) {
        this->_id = id;
        _newID = ++id;
      }
    }
    void set_endpos(int pos) { this->_endPos = pos; }
    void set_population(const std::vector<bool> &pop) {
      _individuals = pop;
    }
    void set_seq(std::string seq) { _seq = seq_to_num(seq); }
    void set_seq(std::vector<uchar> &seq) { this->_seq = seq; }
    void set_as_ref() { _ref = true; }
    void set_not_ref() { _ref = false; }
    void set_af(float af) { _af = af; }

   private:
    int _endPos; // End position of the sequence
    std::vector<uchar> _seq;
    std::vector<bool> _individuals; // Each bit marks an individual, 1 if they have this node
    bool _ref = false; // Part of the reference sequence
    float _af = 1;
    long _id;

    std::vector<std::bitset<32>> _indivs;

  };

  typedef std::shared_ptr<Node> nodeptr;

  /**
   * Default constructor inits a new Graph, including a new node map.
   */
  Graph() : _IDMap(std::make_shared<std::unordered_map<long, nodeptr>>(std::unordered_map<long, nodeptr>())) { }

  /**
   * Create a Graph with another Graph and a population filter. The new Graph will only
   * contain nodes if any of the individuals in filter possess the node. The actual nodes
   * are shared_ptr's to the parent Graph, as to prevent duplication of Nodes.
   * @param g Graph to derive the new Graph from
   * @param filter population filter, only include nodes representative of this population
   */
  Graph(const Graph &g,
        const std::vector<bool> &filter);

  Graph(const Graph &g,
        Type t);

  /**
   * Builds the topographical sort of the Graph, used for Graph iteration.
   */
  void finalize();

  /**
   * Add a new node to the Graph. A new node is created so the original can be destroyed.
   * The first node added is set as the Graph root. Nodes must be added in topographical order.
   */
  long add_node(Node &n);

  /**
   * Create an edge linking two nodes. Previous and Next edges are added.
   * @param n1 Node one ID
   * @param n2 Node two ID
   */
  bool add_edge(long n1,
                long n2);

  /**
   * Sets the root of the Graph.
   * @param id ID of root node
   */
  void set_root(long id) {
    _root = id;
  }

  void set_desc(std::string description) { _desc = description; }

  // Return root node ID
  long root() const { return _root; }

  // const reference to maps
  const std::shared_ptr<std::unordered_map<long, nodeptr>> &node_map() const { return _IDMap; }
  const std::unordered_map<long, std::vector<long>> &next_map() const { return _next_map; }
  const std::unordered_map<long, std::vector<long>> &prev_map() const { return _prev_map; }
  const Node &node(long id) const { return *(*_IDMap)[id]; }

  std::string desc() const { return _desc; }

  // Export the Graph in DOT format.
  std::string to_DOT(std::string name = "g") const {
    std::stringstream dot;
    dot << "// Each node has the sequence, followed by end_pos,allele_freq\n";
    dot << "digraph " << name << " {\n";
    for (auto n : *_IDMap) {
      dot << n.second->id() << "[label=\"" << n.second->seq_str() << "\n" << n.second->end() << "," << n.second->freq()
          << "\"];\n";
    }
    for (auto &n : _next_map) {
      for (auto e : n.second) {
        dot << n.first << " -> " << e << ";\n";
      }
    }
    dot << "}\n";
    return dot.str();
  }

  void set_popsize(int popsize) { _pop_size = popsize; }
  int pop_size() const { return _pop_size; }

  /**
   * const forward iterator to traverse the Graph topologically.
   * All nodes are covered.
   */
  class TopologicalIter {

   public:
    TopologicalIter(const Graph &g) : _graph(g), _idx(0) { }
    TopologicalIter(const Graph &g, long index) : _graph(g), _idx(index) { }
    ~TopologicalIter() { }

    TopologicalIter &operator=(const TopologicalIter &other) {
      _idx = other._idx;
      return *this;
    }

    bool operator==(const TopologicalIter &other) const {
      // Check if comparing like-graphs (weak check)
      if (other._graph._toposort != _graph._toposort) return false;
      return _idx == other._idx;
    }

    bool operator!=(const TopologicalIter &other) const {
      return _idx != other._idx;
    }

    TopologicalIter &operator++() {
      if (_idx < _graph._toposort.size()) {
        _idx++;
      }
      return *this;
    }

    TopologicalIter &operator--() {
      if (_idx > 0) {
        _idx--;
      }
      return *this;
    }

    const Graph::Node &operator*() const { return _graph.node(_graph._toposort[_idx]); }

   private:
    const Graph &_graph;
    long _idx;
  };

  /**
   * Provides an iterator to a topological sorting of the Graph.
   */
  TopologicalIter begin() const {
    if (_toposort.size() == 0 && _IDMap->size() > 0) {
      throw std::logic_error("Graph must be finalized before iteration.");
    }
    return TopologicalIter(*this);
  }

  /**
   * Iterator to end of topological sorting.
   */
  TopologicalIter end() const {
    return TopologicalIter(*this, _toposort.size());
  }

 private:
  long _root = -1; // Root of the Graph
  // maps a node ID to a nodeptr. Any derived graphs use the same base node ID map.
  std::shared_ptr<std::unordered_map<long, nodeptr>> _IDMap;
  // maps a node ID to the vector of nodes it points to
  std::unordered_map<long, std::vector<long>> _next_map;
  // maps a node ID to a vector of node ID's that point to it
  std::unordered_map<long, std::vector<long>> _prev_map;
  std::vector<long> _toposort; // Sorted Graph
  std::vector<long> _add_order; // Order nodes were added
  // Description, used by the builder to store construction params
  std::string _desc;
  int _pop_size = 0;

  /**
   * Recursive depth first search to find dependencies. Used to topological sort.
   * @param n current node ID
   * @param unmarked set of unvisited nodes
   * @param temp set of visited but unadded nodes
   * @param perm set of completed nodes
   */
  void _visit(long n,
              std::set<long> &unmarked,
              std::set<long> &temp,
              std::set<long> &perm);

  /**
   * Given a subset of nodes from Graph g, rebuild all applicable edges in the new graph.
   * @param g underlying parent graph
   * @param includedNodes subset of g's nodes to include
   */
  void _build_derived_edges(const Graph &g,
                            const std::unordered_map<long, nodeptr> &includedNodes);

};

TEST_CASE ("Node class") {
  vargas::Graph::Node::_newID = 0;
  vargas::Graph::Node n1;
  vargas::Graph::Node n2;
      CHECK(n1.id() == 0);
      CHECK(n2.id() == 1);

      SUBCASE("Node ID change") {
    n1.setID(1);
        CHECK(n1.id() == 0);
    n1.setID(2);
        CHECK(n1.id() == 2);
  }

      SUBCASE("Set Node params") {
    n1.set_seq("ACGTN");
    std::vector<bool> a = {0, 0, 1};
    n1.set_population(a);
    n1.set_endpos(100);

        REQUIRE(n1.seq().size() == 5);

        CHECK(n1.seq()[0] == 0);
        CHECK(n1.seq()[1] == 1);
        CHECK(n1.seq()[2] == 2);
        CHECK(n1.seq()[3] == 3);
        CHECK(n1.seq()[4] == 4);
        CHECK(n1.end() == 100);
        CHECK(!n1.is_ref());
        CHECK(!n1.belongs(0));
        CHECK(!n1.belongs(1));
        CHECK(n1.belongs(2));

    // When ref node, belongs returns -1
    n1.set_as_ref();
        CHECK(n1.is_ref());
        CHECK(n1.belongs(0) == -1);
        CHECK(n1.belongs(1) == -1);
        CHECK(n1.belongs(2) == -1);
  }

}

TEST_CASE ("Graph class") {
  vargas::Graph::Node::_newID = 0;
  vargas::Graph g;

  /**   GGG
  *    /   \
  * AAA     TTT
  *    \   /
  *     CCC(ref)
  */

  {
    vargas::Graph::Node n;
    n.set_endpos(3);
    n.set_as_ref();
    std::vector<bool> a = {0, 1, 1};
    n.set_population(a);
    n.set_seq("AAA");
    g.add_node(n);
  }

  {
    vargas::Graph::Node n;
    n.set_endpos(6);
    n.set_as_ref();
    std::vector<bool> a = {0, 0, 1};
    n.set_population(a);
    n.set_af(0.4);
    n.set_seq("CCC");
    g.add_node(n);
  }

  {
    vargas::Graph::Node n;
    n.set_endpos(6);
    n.set_not_ref();
    std::vector<bool> a = {0, 1, 0};
    n.set_population(a);
    n.set_af(0.6);
    n.set_seq("GGG");
    g.add_node(n);
  }

  {
    vargas::Graph::Node n;
    n.set_endpos(9);
    n.set_as_ref();
    std::vector<bool> a = {0, 1, 1};
    n.set_population(a);
    n.set_seq("TTT");
    g.add_node(n);
  }

  g.add_edge(0, 1);
  g.add_edge(0, 2);
  g.add_edge(1, 3);
  g.add_edge(2, 3);

      CHECK_THROWS(g.begin());

  g.finalize();

      REQUIRE(g.node_map()->size() == 4);
      REQUIRE(g.prev_map().size() == 3);
      REQUIRE(g.next_map().size() == 3);

  // Check forward edges
      REQUIRE(g.next_map().at(0).size() == 2);
      REQUIRE(g.next_map().at(1).size() == 1);
      REQUIRE(g.next_map().at(2).size() == 1);
      REQUIRE(g.next_map().count(3) == 0);

  // Check prev edges
      REQUIRE(g.prev_map().count(0) == 0);
      REQUIRE(g.prev_map().at(1).size() == 1);
      REQUIRE(g.prev_map().at(2).size() == 1);
      REQUIRE(g.prev_map().at(3).size() == 2);

      SUBCASE("Proper Graph setup") {
        CHECK(num_to_seq(g.node(0).seq()) == "AAA");
        CHECK(num_to_seq(g.node(1).seq()) == "CCC");
        CHECK(num_to_seq(g.node(2).seq()) == "GGG");
        CHECK(num_to_seq(g.node(3).seq()) == "TTT");
  }

      SUBCASE("Topographical invalidation") {
    g.add_edge(1, 2);
        CHECK_THROWS(g.begin());
    g.finalize();
        CHECK_NOTHROW(g.begin());
  }

      SUBCASE("Graph iterator") {
    // Node visit order should be topological
    vargas::Graph::TopologicalIter i = g.begin();

        CHECK(num_to_seq((*i).seq()) == "AAA");
    ++i;

    // Order of these two don't matter
    bool mid = (num_to_seq((*i).seq()) == "CCC") || (num_to_seq((*i).seq()) == "GGG");
        CHECK(mid);
    ++i;
    mid = (num_to_seq((*i).seq()) == "CCC") || (num_to_seq((*i).seq()) == "GGG");
        CHECK(mid);
    ++i;

        CHECK(num_to_seq((*i).seq()) == "TTT");
    ++i;
        CHECK(i == g.end());
    ++i;
        CHECK(i == g.end());
  }

      SUBCASE("Derived Graph") {
    std::vector<bool> filter = {0, 0, 1};
    vargas::Graph g2(g, filter);

        CHECK(g2.node_map()->size() == 4);
        CHECK(&(*g.node_map()) == &(*g2.node_map())); // Underlying node map unchanged
        CHECK(g2.next_map().size() == 2);
        CHECK(g2.prev_map().size() == 2);

        CHECK(g2.next_map().at(0).size() == 1);
        CHECK(g2.next_map().at(1).size() == 1);
        CHECK(g2.next_map().count(2) == 0); // This node shouldn't be included
        CHECK(g2.next_map().count(3) == 0);
        CHECK(g2.prev_map().count(0) == 0);
        CHECK(g2.prev_map().at(1).size() == 1);
        CHECK(g2.prev_map().at(3).size() == 1);
  }

      SUBCASE("REF graph") {
    vargas::Graph g2(g, vargas::Graph::REF);
    vargas::Graph::TopologicalIter iter(g2);

        CHECK((*iter).seq_str() == "AAA");
    ++iter;
        CHECK((*iter).seq_str() == "CCC");
    ++iter;
        CHECK((*iter).seq_str() == "TTT");
    ++iter;
        CHECK(iter == g2.end());
  }

      SUBCASE("MAXAF graph") {
    vargas::Graph g2(g, vargas::Graph::MAXAF);
    vargas::Graph::TopologicalIter iter(g2);

        CHECK((*iter).seq_str() == "AAA");
    ++iter;
        CHECK((*iter).seq_str() == "GGG");
    ++iter;
        CHECK((*iter).seq_str() == "TTT");
    ++iter;
        CHECK(iter == g2.end());

  }

}

class GraphBuilder {

 public:
  GraphBuilder(std::string reffile,
               std::string vcffile) :
      _fa_file(reffile), _vf_file(vcffile) { }

  void open(std::string ref,
            std::string vcf) {
    _fa_file = ref;
    _vf_file = vcf;
  }

  void region(std::string region) {
    _vf.set_region(region);
  }

  void region(std::string chr,
              int min,
              int max) {
    _vf.set_region(chr, min, max);
  }

  /**
   * Use a certain percentage of individuals. Reference nodes are always included.
   * @param percent, 0 - 100
   */
  void ingroup(int percent);

  /**
   * Set maximum node length. If <= 0, length is unbounded.
   * @param max maximum node length.
   */
  void node_len(int max) { _max_node_len = max; }

  /**
   * Apply the various parameters and build the Graph.
   * @return pointer to Graph.
   */
  void build(Graph &g);

 protected:
  __attribute__((always_inline))
  inline void _build_edges(Graph &g, std::vector<int> &prev,
                    std::vector<int> &curr);

  __attribute__((always_inline))
  inline int _build_linear_ref(Graph &g, std::vector<int> &prev,
                               std::vector<int> &curr,
                               int pos,
                               int target);

 private:
  std::string _fa_file, _vf_file;
  VarFile _vf;
  FASTAFile _fa;
  Graph g;

  // Graph construction parameters
  int _ingroup = 100; // percent of individuals to use. Ref nodes always included
  int _max_node_len = 1000000;
};

}

TEST_CASE ("Graph Builder") {
  using std::endl;
  std::string tmpfa = "tmp_tc.fa";
  {
    std::ofstream fao(tmpfa);
    fao
        << ">x" << endl
        << "CAAATAAGGCTTGGAAATTTTCTGGAGTTCTATTATATTCCAACTCTCTGGTTCCTGGTGCTATGTGTAACTAGTAATGG" << endl
        << "TAATGGATATGTTGGGCTTTTTTCTTTGATTTATTTGAAGTGACGTTTGACAATCTATCACTAGGGGTAATGTGGGGAAA" << endl
        << "TGGAAAGAATACAAGATTTGGAGCCAGACAAATCTGGGTTCAAATCCTCACTTTGCCACATATTAGCCATGTGACTTTGA" << endl
        << "ACAAGTTAGTTAATCTCTCTGAACTTCAGTTTAATTATCTCTAATATGGAGATGATACTACTGACAGCAGAGGTTTGCTG" << endl
        << "TGAAGATTAAATTAGGTGATGCTTGTAAAGCTCAGGGAATAGTGCCTGGCATAGAGGAAAGCCTCTGACAACTGGTAGTT" << endl
        << "ACTGTTATTTACTATGAATCCTCACCTTCCTTGACTTCTTGAAACATTTGGCTATTGACCTCTTTCCTCCTTGAGGCTCT" << endl
        << "TCTGGCTTTTCATTGTCAACACAGTCAACGCTCAATACAAGGGACATTAGGATTGGCAGTAGCTCAGAGATCTCTCTGCT" << endl
        << ">y" << endl
        << "GGAGCCAGACAAATCTGGGTTCAAATCCTGGAGCCAGACAAATCTGGGTTCAAATCCTGGAGCCAGACAAATCTGGGTTC" << endl;
  }
  std::string tmpvcf = "tmp_tc.vcf";

  // Write temp VCF file
  {
    std::ofstream vcfo(tmpvcf);
    vcfo
        << "##fileformat=VCFv4.1" << endl
        << "##phasing=true" << endl
        << "##contig=<ID=x>" << endl
        << "##contig=<ID=y>" << endl
        << "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">" << endl
        << "##INFO=<ID=AF,Number=1,Type=Float,Description=\"Allele Freq\">" << endl
        << "##INFO=<ID=AC,Number=A,Type=Integer,Description=\"Alternate Allele count\">" << endl
        << "##INFO=<ID=NS,Number=1,Type=Integer,Description=\"Num samples at site\">" << endl
        << "##INFO=<ID=NA,Number=1,Type=Integer,Description=\"Num alt alleles\">" << endl
        << "##INFO=<ID=LEN,Number=A,Type=Integer,Description=\"Length of each alt\">" << endl
        << "##INFO=<ID=TYPE,Number=A,Type=String,Description=\"type of variant\">" << endl
        << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\ts1\ts2" << endl
        << "x\t9\t.\tG\tA,C,T\t99\t.\tAF=0.01,0.6,0.1;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t0|1\t2|3" << endl
        << "x\t10\t.\tC\t<CN2>,<CN0>\t99\t.\tAF=0.01,0.01;AC=2;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|1\t2|1" << endl
        << "x\t14\t.\tG\t<DUP>,<BLAH>\t99\t.\tAF=0.01,0.1;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|0\t1|1" << endl
        << "y\t34\t.\tTATA\t<CN2>,<CN0>\t99\t.\tAF=0.01,0.1;AC=2;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|1\t2|1" << endl
        << "y\t39\t.\tT\t<CN0>\t99\t.\tAF=0.01;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|0\t0|1" << endl;
  }

      SUBCASE("Basic Graph") {
    vargas::GraphBuilder gb(tmpfa, tmpvcf);
    gb.node_len(5);
    gb.ingroup(100);
    gb.region("x:0-15");

    vargas::Graph g;
    gb.build(g);

    auto giter = g.begin();

        CHECK((*giter).seq_str() == "CAAAT");
        CHECK((*giter).belongs(0) == -1); // its a ref
        CHECK((*giter).is_ref());

    ++giter;
        CHECK((*giter).seq_str() == "AAG");
        CHECK((*giter).belongs(0) == -1); // its a ref
        CHECK((*giter).is_ref());

    ++giter;
        CHECK((*giter).seq_str() == "G");

    ++giter;
        CHECK((*giter).seq_str() == "A");

    ++giter;
        CHECK((*giter).seq_str() == "C");

    ++giter;
        CHECK((*giter).seq_str() == "T");
        CHECK(!(*giter).is_ref());
        CHECK(!(*giter).belongs(0));
        CHECK(!(*giter).belongs(1));
        CHECK(!(*giter).belongs(2));
        CHECK((*giter).belongs(3));

  }

      SUBCASE("Deriving a Graph") {
    vargas::GraphBuilder gb(tmpfa, tmpvcf);
    gb.node_len(5);
    gb.ingroup(100);
    gb.region("x:0-15");

    vargas::Graph g;
    gb.build(g);

    std::vector<bool> filter = {0, 0, 0, 1};
    vargas::Graph g2(g, filter);

    vargas::Graph::TopologicalIter iter(g2);

        CHECK((*iter).seq_str() == "CAAAT");
    ++iter;
        CHECK((*iter).seq_str() == "AAG");
    ++iter;
        CHECK((*iter).seq_str() == "G");
    ++iter;
        CHECK((*iter).seq_str() == "T");
    ++iter;
        CHECK((*iter).seq_str() == "C");
    ++iter;
        CHECK((*iter).seq_str() == "CC");

  }


  remove(tmpfa.c_str());
  remove(tmpvcf.c_str());
  remove((tmpfa + ".fai").c_str());
}

#endif //VARGAS_GRAPH_H
