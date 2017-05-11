/**
 * @author Ravi Gaddipati
 * @date June 26, 2016
 * rgaddip1@jhu.edu
 *
 * @brief
 * Simulates random reads from a graph, returning reads that follow a specified Sim::Profile.
 *
 * @copyright
 * Distributed under the MIT Software License.
 * See accompanying LICENSE or https://opensource.org/licenses/MIT
 *
 * @file
 */

#ifndef VARGAS_SIM_H
#define VARGAS_SIM_H

// SAM tags used in sim output
#define SIM_SAM_READ_ORIG_TAG "ro" // Original unmutated read
#define SIM_SAM_INDIV_TAG "nd" // Sample of VCF file. -1 if common to all.
#define SIM_SAM_SUB_ERR_TAG  "se" // Number of sub errors
#define SIM_SAM_VAR_NODES_TAG "vd" // Number of variant nodes traversed
#define SIM_SAM_VAR_BASE_TAG "vb" // Number of variant bases traversed
#define SIM_SAM_INDEL_ERR_TAG "ni" // Number of indel errors
#define SIM_SAM_END_POS_TAG "ep" // Position of the last base in the seq
#define SIM_SAM_SRC_TAG "gd" // Origin subgraph label
#define SIM_SAM_USE_RATE_TAG "rt" // Errors were generated with rates rather than discrete numbers
#define SIM_SAM_POPULATION "po" // Which samples were included in the subgraph
#define SIM_SAM_GRAPH_TAG "ph" // graph file

// SAM CIGAR modification types
#define SIM_CIGAR_ALIGNED 'M'
#define SIM_CIGAR_INSERT 'I'
#define SIM_CIGAR_DEL 'D'

// Tags defining meta information in FASTA read names
#define READ_META_END "pos"
#define READ_META_MUT "sub"
#define READ_META_INDEL "ind"
#define READ_META_VARNODE "vnd"
#define READ_META_VARBASE "vbs"
#define READ_META_FASTA_DELIM ';'

#include <random>
#include <stdexcept>

#include "sam.h"
#include "graph.h"

namespace vargas {

  /**
   * @brief
   * Generate reads from a graph using a given profile. srand() should be called externally.
   * @details
   * Given a Graph, reads are generated by randomly picking a location in the graph, and extracting
   * a subsequence. Errors (either a fixed number, or at a specified rate) are introduced into the
   * read. The read is packed in a Read struct, containing the sequence and origin information.
   * Usage:\n
   * @code{.cpp}
   * #include "sim.h"
   *
   * Vargas::GraphBuilder gb("reference.fa", "variants.bcf");
   * gb.region("22:0-10,000,000");
   *
   * Vargas::Graph g = gb.build();
   *
   * Vargas::Sim::Profile prof;
   * prof.len = 100; // 100bp reads
   * prof.mut = 4; // 4 mutation errors
   * prof.indel = 0; // No indel errors
   * prof.var_nodes = 2; // Read passes through two variant nodes
   * prof.var_bases = -1; // Any number of variant bases
   *
   * Vargas::Sim sim(g);
   * sim.set_prof(prof);
   *
   * // Generate 1 read
   * sim.update_read();
   * Read read = sim.get_read();
   *
   * // Change profile
   * prof.var_nodes = 0;
   * sim.set_prof(prof);
   *
   * // Generate 100 reads with new profile
   * std::vector<Read> reads = sim.get_batch(100);
   *
   * @endcode
   */
  class Sim {

    public:

      /**
       * @brief
       * Sim generated read sequence and associated parameters.
       * @details
       * When a Profile indicates any possible value (-1), the Read
       * will reflect the actual number.
       */
      struct Read {
          Read() :
          read_orig(""), read(""), end_pos(-1), indiv(-1), sub_err(-1), var_nodes(-1), var_bases(-1),
          indel_err(-1) {}

          /**
           * @brief
           * Construct a read given a sequence.
           * @param r read sequence.
           */
          Read(std::string r) :
          read_orig(""), read(r), end_pos(-1), indiv(-1), sub_err(-1), var_nodes(-1), var_bases(-1),
          indel_err(-1) {}

          std::string read_orig; /**< unmutated read sequence */
          std::string read; /**< base sequence. */
          int32_t end_pos; /**< position of last base in seq. */
          int32_t indiv; /**< Individual the read was taken from. */
          int32_t sub_err; /**< Number of substitution errors introduced. */
          int32_t var_nodes; /**< Number of variant nodes the read traverses. */
          int32_t var_bases; /**< Number of bases that are in variant nodes. */
          int32_t indel_err; /**< Number of insertions and deletions introduced. */


          /**
           * @brief
           * Output two lines in FASTA format.
           * @details
           * Output two lines given the form: \n
           * > Meta information \n
           * read_sequence \n
           * @return two-line string
           */
          std::string to_fasta() const;

          /**
           * @brief
           * Convert the read to a single line CSV.
           * @details
           * Output form: \n
           * src,read_seq,end_pos,sub_err,indel_err,var_nodes,var_bases \n
           * @return single line string
           */
          std::string to_csv() const;

      };

      /**
       * @brief
       * Parameter list controlling the types of reads created. -1 indicates no preferred value.
       */
      struct Profile {
          unsigned int len = 50; /**< Nominal length of the read */
          bool rand = false; /**< Number of mutation errors, or rate */
          float mut = 0; /**< number of insertions/deletions, or rate */
          float indel = 0; /**< Introduce mutations and indels at a random rate */
          int var_nodes = -1; /**< Number of variant nodes */
          int var_bases = -1; /**< number of total variant bases */

          /**
           * @brief
           * Output the profile as a single line string.
           * @details
           * Format:\n
           * len=X;mut=X;indel=X;vnode=X;vbase=X;rand=X \n
           * where X is the respective parameter.
           */
          std::string to_string() const;
      };

      /**
       * @param g Graph to simulate from
       */
      Sim(const Graph &g) : _graph(g),
                            _nodes(*(_graph.node_map())),
                            _next(_graph.next_map()) { _init(); }

      /**
       * @param _graph Graph to simulate from
       * @param prof accept reads following this profile
       */
      Sim(const Graph &_graph,
          const Profile &prof) : _graph(_graph),
                                 _prof(prof),
                                 _nodes(*(_graph.node_map())),
                                 _next(_graph.next_map()) { _init(); }

      /**
       * @brief
       * Generate and store an updated read.
       * @return true if successful
       */
      bool update_read();

      /**
       * @brief
       * Get size reads. If more reads are not available, a undersized
       * batch is returned.
       * @param size nominal number of reads to get.
       */
      const std::vector<SAM::Record> &get_batch(unsigned size);

      /**
       * @brief
       * Get the stored batch of reads.
       * @return vector of Reads
       */
      const std::vector<SAM::Record> &batch() const {
          return _batch;
      }

      /**
       * @brief
       * Get the profile being used to generate reads (if use_prof)
       * @return Sim::Profile
       */
      const Profile &prof() const {
          return _prof;
      }

      /**
       * @brief
       * Crete reads following prof as a template
       * @param prof Read Profile
       */
      void set_prof(const Profile &prof) {
          if (prof.var_nodes == 0 && prof.var_bases > 0)
              throw std::invalid_argument("Invalid profile option: var_nodes = 0, var_bases > 0.");
          _prof = prof;
      }

      /**
       * @return the profile used to generate the reads
       */
      std::string get_header() const {
          return _prof.to_string();
      }

      /**
       * @return current profile being used to filter reads.
       */
      Profile get_profile() const {
          return _prof;
      }

      SAM::Record &get_read() {
          return _read;
      }

    private:
      const vargas::Graph &_graph;
      Profile _prof;
      const std::unordered_map<unsigned, vargas::Graph::Node> &_nodes;
      const std::unordered_map<unsigned, std::vector<unsigned >> &_next;


      /**
       * The last element represents the number of bases in the entire graph, with each element
       * representing a running total. Generating a random number and then finding the first index
       * greater than that gives a random node weighted to sequence length.
       */
      std::vector<unsigned> _node_ids;
      std::vector<uint64_t> _node_weights;

      std::vector<SAM::Record> _batch;
      SAM::Record _read;

      std::uniform_int_distribution<uint64_t> _node_weight_dist;
      std::mt19937 _rand_generator;

      // Abort trying to update the read after N tries
      const unsigned _abort_after = 1000000;

      /**
       * Creates a vector of keys of all outgoing edges. Allows for random node selection
       * This precludes the possibility of having reads begin in the last node of the graph.
       */
      void _init();

      unsigned _random_node_id() {
          return _node_ids[std::lower_bound(_node_weights.begin(), _node_weights.end(),
                                            _node_weight_dist(_rand_generator)) - _node_weights.begin()];
      }

      bool _update_read();

  };

  /**
   * @brief
   * Output the profile as a string. See Vargas::Sim::Profile::to_string
   * @param os Output stream
   * @param rp Profile to print
   * @return output stream
   */
  inline std::ostream &operator<<(std::ostream &os,
                                  const Sim::Profile &rp) {
      os << rp.to_string();
      return os;
  }

}

#endif //VARGAS_SIM_H
