/**
 * Ravi Gaddipati
 * November 24, 2015
 * rgaddip1@jhu.edu
 *
 * Wrapper for a reads file that loads reads and meta information
 * into a Read structure.
 *
 * readfile.cpp
 */
#include "../include/readfile.h"

bool vargas::ReadFile::updateRead() {
  if (!readfile) throw std::invalid_argument("No readfile specified.");
  if (!std::getline(readfile, line)) return false;
  if (line.at(0) == '#') return updateRead();
  unsigned long delim = line.find('#');
  if (delim == std::string::npos) {
    read.read = line;
    read.readEnd = 0;
    read.indiv = -1;
    read.numSubErr = -1;
    read.numVarNodes = -1;
    read.numVarBases = -1;
  } else {
    read.read = line.substr(0, delim);
    split(line.substr(delim + 1), ',', splitMeta);
    if (splitMeta.size() != 5) {
      std::cerr << "Unexpected number of fields." << std::endl;
      read.readEnd = 0;
      read.indiv = -1;
      read.numSubErr = -1;
      read.numVarNodes = -1;
      read.numVarBases = -1;
      return false;
    }
    read.readEnd = uint32_t(atoi(splitMeta[0].c_str()));
    read.indiv = atoi(splitMeta[1].c_str());
    read.numSubErr = atoi(splitMeta[2].c_str());
    read.numVarNodes = atoi(splitMeta[3].c_str());
    read.numVarBases = atoi(splitMeta[4].c_str());
  }
  return true;
}