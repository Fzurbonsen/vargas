/**
 * Ravi Gaddipati
 * Dec 23, 2016
 * rgaddip1@jhu.edu
 *
 * @brief
 * Main aligner interface
 *
 * @file
 */

#include "align_main.h"
#include "alignment.h"
#include "sam.h"
#include "gdef.h"
#include "doctest.h"
#include "sim.h"

#ifdef _OPENMP
#include <omp.h>
#endif


int align_main(int argc, char *argv[]) {
    std::string cl;
    {
        std::ostringstream ss;
        for (int i = 0; i < argc; ++i) ss << std::string(argv[i]) << " ";
        cl = ss.str();
    }

    // Load parameters
    // hisat similar params: match = 2, mismatch = 6, open = 5, extend = 3
    size_t match, mismatch, gopen, gext, threads, read_len, tolerance, chunk_size;
    std::string read_file, gdf_file, align_targets, out_file;
    bool align_targets_isfile = false, end_to_end = false;

    cxxopts::Options opts("vargas align", "Align reads to a graph.");
    try {
        opts.add_options()
        ("g,gdef", "<str> *Graph definition file.", cxxopts::value(gdf_file))
        ("r,reads", "<str> SAM reads file. (default: stdin)", cxxopts::value(read_file))
        ("a,align", "<str> Alignment targets/file of form \"RG:[ID][gd],target\"", cxxopts::value(align_targets))
        ("f,file", " -a specifies a file name.", cxxopts::value(align_targets_isfile))
        ("l,rlen", "<N> Maximum read length.", cxxopts::value(read_len)->default_value("50"))
        ("m,match", "<N> Match score.", cxxopts::value(match)->default_value("2"))
        ("n,mismatch", "<N> Mismatch penalty.", cxxopts::value(mismatch)->default_value("2"))
        ("o,gap_open", "<N> Gap opening penalty.", cxxopts::value(gopen)->default_value("3"))
        ("e,gap_extend", "<N> Gap extension penalty.", cxxopts::value(gext)->default_value("1"))
        ("x,endtoend", "Perform end to end alignment", cxxopts::value(end_to_end))
        ("c,tolerance", "<N> Correct if within readlen/N.",
         cxxopts::value(tolerance)->default_value(std::to_string(vargas::Aligner::default_tolerance())))
        ("u,chunk", "<N> Partition tasks into chunks with max size N.",
         cxxopts::value(chunk_size)->default_value("2048"))
        ("t,out", "<str> Output file. (default: stdout)", cxxopts::value(out_file))
        ("j,threads", "<N> Number of threads.", cxxopts::value(threads)->default_value("1"))
        ("h,help", "Display this message.");
        opts.parse(argc, argv);
    } catch (std::exception &e) { throw std::invalid_argument("Error parsing options: " + std::string(e.what())); }
    if (opts.count("h")) {
        align_help(opts);
        return 0;
    }
    if (!opts.count("g")) throw std::invalid_argument("Graph definition file required.");

    if (read_len * match > 255) {
        throw std::invalid_argument("Score matrix overflow with read length " + std::to_string(read_len) +
        " and match score " + std::to_string((int) match) + ".");
    }

    if (chunk_size < vargas::Aligner::read_capacity() ||
    chunk_size % vargas::Aligner::read_capacity() != 0) {
        std::cerr << "Warning: Chunk size is not a multiple of SIMD vector length: "
                  << vargas::Aligner::read_capacity() << std::endl;
    }

    #ifndef _OPENMP
    // Disable threads if no openMP.
    if (threads != 1) {
        std::cerr << "Warning: Threads specified without OpenMP Compilation." << std::endl;
    }
    threads = 1;
    #endif

    if (align_targets_isfile) {
        std::ifstream in(align_targets);
        if (!in.good()) throw std::invalid_argument("Invalid alignment targets file \"" + align_targets + "\".");
        std::stringstream ss;
        ss << in.rdbuf();
        align_targets = ss.str();
    }

    std::vector<std::string> alignment_pairs;
    if (align_targets.length() != 0) {
        std::replace(align_targets.begin(), align_targets.end(), '\n', ';');
        alignment_pairs = rg::split(align_targets, ';');
    }

    std::cerr << "Match=" << match
              << " Mismatch=" << mismatch
              << " GapOpen=" << gopen
              << " GapExtend=" << gext
              << " MaxReadLen=" << read_len
              << " CorrectnessTol=" << tolerance
              << "\nLoading reads... " << std::flush;

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::pair<std::string, std::vector<vargas::SAM::Record>>> task_list;
    vargas::SAM::Header reads_hdr;
    size_t total = 0;
    {
        // Maps a read group ID to a vector of reads
        std::unordered_map<std::string, std::vector<vargas::SAM::Record>> read_groups;
        {
            vargas::isam reads(read_file);
            reads_hdr = reads.header();
            std::string read_group;
            vargas::SAM::Record rec;
            do {
                rec = reads.record();
                if (rec.seq.length() > read_len) {
                    throw std::invalid_argument("Expected read of length <=" +
                    std::to_string(read_len) + ", got " + std::to_string(rec.seq.length()));
                }
                if (!rec.aux.get("RG", read_group)) {
                    read_group = UNGROUPED_READGROUP;
                    rec.aux.set("RG", UNGROUPED_READGROUP);
                    if (!reads_hdr.read_groups.count(UNGROUPED_READGROUP)) {
                        reads_hdr.add(vargas::SAM::Header::ReadGroup("@RG\tID:" + std::string(UNGROUPED_READGROUP)));
                    }
                }
                read_groups[read_group].push_back(rec);
            } while (reads.next());
        }

        if (alignment_pairs.size() == 0) {
            for (const auto &p : read_groups) {
                alignment_pairs.push_back("RG:ID:" + p.first + "\t" + vargas::GraphManager::GDEF_BASEGRAPH);
            }
        }

        // Maps target graph to read group ID's
        std::unordered_map<std::string, std::vector<std::string>> alignment_rg_map;
        {
            std::vector<std::string> pair;
            std::string tag, val, target_val;
            for (const std::string &p : alignment_pairs) {
                rg::split(p, pair);
                if (pair.size() != 2)
                    throw std::invalid_argument("Malformed alignment pair \"" + p + "\".");
                if (pair[0].at(2) != ':')
                    throw std::invalid_argument("Expected source format Read_group_tag:value in \"" + pair[0] + "\".");
                if (pair[0].substr(0, 2) != "RG")
                    throw std::invalid_argument("Expected a read group tag \'RG:xx:\', got \"" + pair[0] + "\"");

                tag = pair[0].substr(3, 2);
                target_val = pair[0].substr(6);

                for (const auto &rg_pair : reads_hdr.read_groups) {
                    if (tag == "ID") val = rg_pair.second.id;
                    else if (rg_pair.second.aux.get(tag, val));
                    else continue;
                    if (val == target_val) alignment_rg_map[pair[1]].push_back(rg_pair.first);
                }

            }
        }

        std::cerr << rg::chrono_duration(start_time) << " seconds." << std::endl;

        // graph label to vector of reads

        for (const auto &sub_rg_pair : alignment_rg_map) {
            for (const std::string &rgid : sub_rg_pair.second) {
                // If there is a header line that there are no reads associated with, skip
                if (read_groups.count(rgid) == 0) continue;

                const auto beg = std::begin(read_groups.at(rgid));
                const auto end = std::end(read_groups.at(rgid));
                const size_t nrecords = read_groups.at(rgid).size();
                const size_t n_chunks = (nrecords / chunk_size) + 1;
                total += read_groups.at(rgid).size();

                for (size_t i = 0; i < n_chunks; ++i) {
                    const auto safe_beg = beg + (i * chunk_size);
                    const auto safe_end = (i + 1) * chunk_size > nrecords ? end : safe_beg + chunk_size;
                    if (safe_beg != safe_end)
                        task_list.emplace_back(sub_rg_pair.first, std::vector<vargas::SAM::Record>(safe_beg, safe_end));
                }
            }
        }


        std::cerr << '\t' << read_groups.size() << " Read groups.\n"
                  << '\t' << alignment_rg_map.size() << " Subgraphs.\n"
                  << '\t' << task_list.size() << " Tasks.\n"
                  << '\t' << total << " Total alignments.\n";
    }

    std::cerr << "Loading graphs... " << std::flush;
    start_time = std::chrono::steady_clock::now();
    vargas::GraphManager gm(gdf_file);
    std::cerr << "(" << gm.base()->node_map()->size() << " nodes), ";
    std::cerr << rg::chrono_duration(start_time) << " seconds." << std::endl;
    std::cerr << "Estimated aligner memory usage: "
              << threads * vargas::Aligner::estimated_size(gm.node_len(), read_len) / 1000000 << "MB" << std::endl;


    {
        vargas::SAM::Header::Program pg;
        pg.command_line = cl;
        pg.name = "vargas_align";
        pg.id = "VA";
        pg.version = __DATE__;
        std::replace_if(pg.version.begin(), pg.version.end(), isspace, ' '); // rm tabs
        reads_hdr.add(pg);
    }

    const size_t num_tasks = task_list.size();
    if (num_tasks < threads) {
        std::cerr << "Warning: Number of threads is greater than number of tasks. Try decreasing --chunk.\n";
    }

    #ifdef _OPENMP
    if (threads) threads = threads > task_list.size() ? task_list.size() : threads;
    omp_set_num_threads(threads);
    #endif

    std::cerr << "Aligning with " << threads << " thread(s)..." << std::endl;
    start_time = std::chrono::steady_clock::now();
    auto start_cpu = std::clock();


    vargas::osam aligns_out(out_file, reads_hdr);
    std::vector<std::unique_ptr<vargas::AlignerBase>> aligners(threads);
    for (size_t k = 0; k < threads; ++k) {
        if (end_to_end) {
            aligners[k] = rg::make_unique<vargas::AlignerETE>(gm.node_len(), read_len, match, mismatch, gopen, gext);
        } else {
            aligners[k] = rg::make_unique<vargas::Aligner>(gm.node_len(), read_len, match, mismatch, gopen, gext);
        }
        aligners[k]->set_correctness_tolerance(tolerance);
    }

    #pragma omp parallel for
    for (size_t l = 0; l < num_tasks; ++l) {
        #ifdef _OPENMP
        const int tid = omp_get_thread_num();
        #else
        const int tid = 0;
        #endif
        const size_t num_reads = task_list.at(l).second.size();
        std::vector<std::string> read_seqs(num_reads);
        std::vector<size_t> targets(num_reads);
        for (size_t i = 0; i < num_reads; ++i) {
            const auto &r = task_list.at(l).second.at(i);
            read_seqs[i] = r.seq;
            targets[i] = r.pos + r.seq.length() - 1;
        }
        auto subgraph = gm.make_subgraph(task_list.at(l).first);
        const auto aligns = aligners[tid]->align(read_seqs, targets, subgraph->begin(), subgraph->end());
        for (size_t j = 0; j < task_list.at(l).second.size(); ++j) {
            vargas::SAM::Record &rec = task_list.at(l).second.at(j);
            rec.ref_name = task_list.at(l).first;
            rec.aux.set(ALIGN_SAM_MAX_POS_TAG, aligns.max_pos[j]);
            rec.aux.set(ALIGN_SAM_MAX_SCORE_TAG, aligns.max_score[j]);
            rec.aux.set(ALIGN_SAM_MAX_COUNT_TAG, aligns.max_count[j]);
            rec.aux.set(ALIGN_SAM_SUB_POS_TAG, aligns.sub_pos[j]);
            rec.aux.set(ALIGN_SAM_SUB_SCORE_TAG, aligns.sub_score[j]);
            rec.aux.set(ALIGN_SAM_SUB_COUNT_TAG, aligns.sub_count[j]);
            rec.aux.set(ALIGN_SAM_COR_FLAG_TAG, aligns.correctness_flag[j]);
            rec.aux.set(ALIGN_SAM_END_TO_END_TAG, end_to_end);
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    auto cput = (std::clock() - start_cpu) / (double) CLOCKS_PER_SEC;
    std::cerr << rg::chrono_duration(start_time, end_time) << " seconds, "
              << cput << " CPU seconds, "
              << cput / total << " CPU s/alignment.\n" << std::endl;

    for (size_t l = 0; l < num_tasks; ++l) {
        gm.destroy(task_list.at(l).first);
        for (size_t j = 0; j < task_list.at(l).second.size(); ++j) {
            aligns_out.add_record(task_list.at(l).second.at(j));
        }
    }

    return 0;
}

void align_help(const cxxopts::Options &opts) {
    using std::cerr;
    using std::endl;

    cerr << opts.help() << "\n" << endl;
    cerr << "Elements per vector: " << vargas::Aligner::read_capacity() << endl;
}

TEST_SUITE("System");

TEST_CASE ("Coordinate System Matches") {
    srand(1);
    vargas::Graph::Node::_newID = 0;
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
        << "x\t9\t.\tG\tA,CC,T\t99\t.\tAF=0.01,0.6,0.1;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t0|1\t2|3" << endl
        << "x\t10\t.\tC\t<CN7>,<CN0>\t99\t.\tAF=0.01,0.01;AC=2;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|1\t2|1" << endl
        << "x\t14\t.\tG\t<DUP>,<BLAH>\t99\t.\tAF=0.01,0.1;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|0\t1|1" << endl
        << "y\t34\t.\tTATA\t<CN2>,<CN0>\t99\t.\tAF=0.01,0.1;AC=2;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|1\t2|1" << endl
        << "y\t39\t.\tT\t<CN0>\t99\t.\tAF=0.01;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|0\t0|1" << endl;
    }

    vargas::GraphFactory gb(tmpfa);
    gb.open_vcf(tmpvcf);
    gb.node_len(5);
    gb.set_region("x:0-50");
    vargas::Graph g = gb.build();

    vargas::Sim::Profile prof;
    prof.len = 5;
    vargas::Sim sim(g, prof);

    vargas::Aligner aligner(g.max_node_len(), 5);
    auto reads = sim.get_batch(aligner.read_capacity());

    std::vector<std::string> seqs;
    std::vector<size_t> targets;
    for (auto &r : reads) {
        seqs.push_back(r.seq);
        targets.push_back(r.pos + r.seq.length() - 1);
    }

    auto results = aligner.align(seqs, targets, g.begin(), g.end());

    for (auto i : results.correctness_flag) CHECK ((int) i == 1);

    remove(tmpfa.c_str());
    remove(tmpvcf.c_str());
    remove((tmpfa + ".fai").c_str());
}
TEST_CASE ("Correctness flag") {
    srand(1);
    vargas::Graph::Node::_newID = 0;
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
        << "x\t9\t.\tG\tA,CC,T\t99\t.\tAF=0.01,0.6,0.1;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t0|1\t2|3" << endl
        << "x\t10\t.\tC\t<CN7>,<CN0>\t99\t.\tAF=0.01,0.01;AC=2;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|1\t2|1" << endl
        << "x\t14\t.\tG\t<DUP>,<BLAH>\t99\t.\tAF=0.01,0.1;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|0\t1|1" << endl
        << "x\t20\t.\tTTC\t<CN3>,<CN2>\t99\t.\tAF=0.01,0.01;AC=2;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|1\t2|1" << endl
        << "y\t34\t.\tTATA\t<CN2>,<CN0>\t99\t.\tAF=0.01,0.1;AC=2;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|1\t2|1" << endl
        << "y\t39\t.\tT\t<CN0>\t99\t.\tAF=0.01;AC=1;LEN=1;NA=1;NS=1;TYPE=snp\tGT\t1|0\t0|1" << endl;
    }

    std::string reads_file("tmp_rd.sam");
    {
        std::ofstream ro(reads_file);
        ro << "@HD\tVN:1.0\n*\t4\t*\t14\t255\t*\t*\t0\t0\tGAAATT\t*\n*\t4\t*\t17\t255\t*\t*\t0\t0\tATTTTC\t*";
    }

    vargas::GraphFactory gb(tmpfa);
    gb.open_vcf(tmpvcf);
    gb.set_region("x:0-100");
    vargas::Graph g = gb.build();

    vargas::Aligner aligner(g.max_node_len(), 6);
    vargas::isam reads(reads_file);

    std::vector<vargas::SAM::Record> records;
    std::vector<std::string> read_seq;
    std::vector<size_t> targets;
    do {
        records.push_back(reads.record());
        read_seq.push_back(reads.record().seq);
        targets.push_back(reads.record().pos + read_seq.back().length() - 1);
    } while (reads.next());

    auto res = aligner.align(read_seq, targets, g.begin(), g.end());

    vargas::osam align_out("tmp_aout.sam", reads.header());
    for (auto &r : records) align_out.add_record(r);

    remove(tmpfa.c_str());
    remove((tmpfa + ".fai").c_str());
    remove(tmpvcf.c_str());
    remove(reads_file.c_str());
    remove("tmp_aout.sam");
}

TEST_SUITE_END();