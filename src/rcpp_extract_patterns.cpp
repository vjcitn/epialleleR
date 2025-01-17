#include <Rcpp.h>
#include <boost/container/flat_map.hpp>
// using namespace Rcpp;

// Scans trough reads and extracts methylation patterns from the reads that
// overlap target area. Clips overhangs if necessary.
// Return value: a data frame with the following columns:
// 1) pattern id (FNV hash)
// 2) just read the epialleleR::extractPatterns() manual...

// ctx_to_idx conversion is described in the rcpp_cx_report.cpp
// Here's the nt_to_idx conversion for the SEQ string:
// nt   bin      >>1&3  dec              idx
// -    00101101    10    2  not in ctx_map!
// A    01000001    00    0                3
// C    01000011    01    1                4
// G    01000111    11    3               12
// T    01010100    10    2               11
// N    01001110    11    3  not in ctx_map!

// [[Rcpp::plugins(cpp17)]]
// [[Rcpp::depends(BH)]]

// fast, vectorised
// [[Rcpp::export("rcpp_extract_patterns")]]
Rcpp::DataFrame rcpp_extract_patterns(Rcpp::DataFrame &df,                      // data frame with BAM data
                                      unsigned int target_rname,                // target chromosome
                                      unsigned int target_start,                // target start
                                      unsigned int target_end,                  // target end
                                      signed int min_overlap,                   // min overlap of reads and capture targets
                                      std::string &ctx,                         // context string for bases to include
                                      double min_ctx_freq,                      // minimum frequency of observed context at position
                                      bool clip,                                // clip the matched reads to target area
                                      unsigned int reverse_offset,              // decrease reverse strand coordinates by this value: 0 for CHH, 1 for CpG, 2 for CHG
                                      Rcpp::IntegerVector &hlght) {             // positions of bases to extract sequence info; NB: overlapping, unique and sorted!
  Rcpp::IntegerVector rname   = df["rname"];                                    // template rname
  Rcpp::IntegerVector strand  = df["strand"];                                   // template strand
  Rcpp::IntegerVector start   = df["start"];                                    // template start
  Rcpp::IntegerVector templid = df["templid"];                                  // template id, effectively holds indexes of corresponding std::string in std::vector
  
  Rcpp::XPtr<std::vector<std::string>> xm((SEXP)df.attr("xm_xptr"));            // merged refspaced template XMs, as a pointer to std::vector<std::string>
  Rcpp::XPtr<std::vector<std::string>> seq((SEXP)df.attr("seq_xptr"));          // merged refspaced template SEQ, as a pointer to std::vector<std::string>
  
// http://www.isthe.com/chongo/tech/comp/fnv/
#define fnv_add(hash, pointer, size) {     /* hash starts with offset_basis */ \
  for (unsigned int idx=0; idx<size; idx++) {        /* cycle through bytes */ \
    hash ^= *(pointer+idx);                       /* hash xor octet_of_data */ \
    hash *= 1099511628211u;                             /* hash * FNV_prime */ \
  }                                                                            \
}
#define ctx_to_idx(c) ((c+2)>>2) & 15
#define nt_to_idx(c) base_map[(c>>1) & 3]
  
  // consts, vars, typedefs
  const uint64_t offset_basis = 14695981039346656037u;                          // FNV-1a offset basis
  const unsigned int base_map[] = {3, 4, 11, 12};                               // see comments on base conversion at the top
  unsigned int npat = 0;                                                        // pattern counter
  typedef int T_key;                                                            // map key
  typedef std::vector<int> T_val;                                               // map value
  boost::container::flat_map<T_key, T_key> pos_map;                             // all positions to figure out valid ones
  boost::container::flat_map<T_key, T_key>::iterator pos_hint = pos_map.begin();// position map iterator
  std::map<T_key, T_val> pat_map;                                               // per-pattern methylation counts
  std::map<T_key, T_val> hlght_map;                                             // per-pattern sequence bases
  std::vector<int> pat_strand, pat_start, pat_end, pat_nbase;                   // pattern strands, starts, ends, number of bases within context
  std::vector<double> pat_beta;                                                 // pattern betas
  std::vector<std::string> pat_fnv;                                             // FNV-1a hashes of patterns
  
  pos_map.reserve(std::max((int)(target_end-target_start), 0xFFFF));
  
  // filling the context map
  unsigned int ctx_map [128] = {0};
  std::for_each(ctx.begin(), ctx.end(), [&ctx_map] (unsigned int const &c) {
    ctx_map[c] = 1;
  });
  ctx_map[(int)'A'] = 1; ctx_map[(int)'C'] = 1;                                 // make these bases valid for highlighting
  ctx_map[(int)'G'] = 1; ctx_map[(int)'T'] = 1;
  
  // // rnames are sorted, should've been taking an advantage of it...
  // Rcpp::IntegerVector::const_iterator lower = std::lower_bound(rname.begin(), rname.end(), target_rname);
  // Rcpp::IntegerVector::const_iterator upper = std::upper_bound(lower, rname.end(), target_rname);
  // for (unsigned int x=lower-rname.begin(); x<upper-rname.begin(); x++) {
  
  // first - find valid positions
  for (unsigned int x=0; x<rname.size(); x++) {   
    // checking for the interrupt
    if ((x & 0xFFFF) == 0) Rcpp::checkUserInterrupt();                          // check for interrupt
    
    if (rname[x]==(int)target_rname) {
      const unsigned int size_x = xm->at(templid[x]).size();                    // length of the current read
      const unsigned int start_x = start[x];                                    // start position of the current read
      const unsigned int end_x = start_x + size_x - 1;                          // end position of the current read
      const unsigned int over_start_x = std::max(start_x, target_start);        // start of overlapped area
      const unsigned int over_end_x = std::min(end_x, target_end);              // end of overlapped area
      const signed int overlap = over_end_x - over_start_x + 1;                 // overlap with target
      if (overlap>=min_overlap) {                                               // if overlaps the target
        const char* xm_x = xm->at(templid[x]).c_str();                          // xm->at(templid[x]) is a reference to a corresponding XM string
        const unsigned int offset_x = strand[x]==2 ? reverse_offset : 0;        // offset coordinates of reverse strand for symmetric methylation
        const unsigned int begin_i = clip ? (over_start_x - start_x) : 0;       // clip the XM?
        const unsigned int end_i = clip ? overlap : size_x;                     // clip the XM?
        for (unsigned int i=begin_i; i<end_i; i++) {                            // char by char - it's faster this way than using std::string in the cycle
          if (ctx_map[(int)xm_x[i]]) {                                          // if base is within context
            const unsigned int pos = start_x + i - offset_x;                    // position of the base
            pos_hint = pos_map.try_emplace(pos_hint, pos, 0);                   // check if this position is already included, emplace if not
            pos_hint->second++;                                                 // position++
          }
        }
        npat++;                                                                 // patterns++, to know how many
      }
    }
  }
  
  // fill pattern map
  for (auto it=pos_map.begin(); it!=pos_map.end(); it++) {
    if (((double)it->second/npat >= min_ctx_freq) &&                            // if position frequency in patterns is higher than the min
        (std::find(hlght.begin(), hlght.end(), it->first) == hlght.end()))      // and it's not the highlighted position
      pat_map.emplace(it->first, T_val (npat, NA_INTEGER));                     // add position to the pattern map
  }
  // fill highlight map
  for (unsigned int i=0; i<hlght.size(); i++) {
    hlght_map.emplace(hlght[i], T_val (npat, NA_INTEGER));                      // add position to the highlight map
  }
  
  npat = 0;
  for (unsigned int x=0; x<rname.size(); x++) {   
    // checking for the interrupt
    if ((x & 0xFFFF) == 0) Rcpp::checkUserInterrupt();                          // check for interrupt
    
    if (rname[x]==(int)target_rname) {
      const unsigned int size_x = xm->at(templid[x]).size();                    // length of the current read
      const unsigned int start_x = start[x];                                    // start position of the current read
      const unsigned int end_x = start_x + size_x - 1;                          // end position of the current read
      const unsigned int over_start_x = std::max(start_x, target_start);        // start of overlapped area
      const unsigned int over_end_x = std::min(end_x, target_end);              // end of overlapped area
      const signed int overlap = over_end_x - over_start_x + 1;                 // overlap with target
      if (overlap>=min_overlap) {                                               // if overlaps the target
        const char* xm_x = xm->at(templid[x]).c_str();                          // xm->at(templid[x]) is a reference to a corresponding XM string
        const unsigned int offset_x = strand[x]==2 ? reverse_offset : 0;        // offset coordinates of reverse strand for symmetric methylation
        const unsigned int begin_i = clip ? (over_start_x - start_x) : 0;       // clip the XM?
        const unsigned int end_i = clip ? overlap : size_x;                     // clip the XM?
        unsigned int meth = 0, total = 0;                                       // counters for methylated and total within context
        uint64_t fnv = offset_basis;                                            // FNV-1a hash of current pattern
        for (unsigned int i=begin_i; i<end_i; i++) {                            // char by char - it's faster this way than using std::string in the cycle
          if (ctx_map[(int)xm_x[i]]) {                                          // if base is within context
            const unsigned int pos = start_x + i - offset_x;                    // position of the base
            auto hint = pat_map.find(pos);                                      // find and check if this position is already included
            if (hint != pat_map.end()) {
              const unsigned int base = ctx_to_idx(xm_x[i]);                    // rcpp_cx_report for details
              hint->second[npat] = base;                                        // save base by position
              meth += !(base & 8);                                              // methylated + (0 for lowercase, 1 for uppercase)
              total++;                                                          // total++
              fnv_add(fnv, reinterpret_cast<const char*>(&pos), sizeof(pos));   // FNV-1a: add int position
              fnv_add(fnv, xm_x+i, sizeof(char));                               // FNV-1a: add char base
            }
          }
        }
        
        if (fnv != offset_basis) {                                              // only if nonempty, valid pattern
          // extract bases to highlight
          const char* seq_x = seq->at(templid[x]).c_str();                      // seq->at(templid[x]) is a reference to a corresponding SEQ string
          for (unsigned int i=0; i<hlght.size(); i++) {                         // for every position to highlight
            const unsigned int hlght_pos = hlght[i] - start_x;
            if (((hlght_pos >= begin_i) && (hlght_pos < end_i)) &&              // if position within pattern
                (ctx_map[(int)seq_x[hlght_pos]])) {                             // and is a valid (ACGT) base 
              const unsigned int base = nt_to_idx(seq_x[hlght_pos]);            // see comments on base conversion at the top
              auto hint = hlght_map.find(hlght[i]);                             // find this position
              hint->second[npat] = base;                                        // save base by position
              fnv_add(fnv, reinterpret_cast<char*>(&hlght[i]), sizeof(hlght[i])); // FNV-1a: add int position
              fnv_add(fnv, seq_x+hlght_pos, sizeof(char));                      // FNV-1a: add char base
            }
          }
          
          // save pattern info
          npat++;                                                               // patterns++
          pat_strand.push_back(strand[x]);                                      // push strand
          pat_start.push_back(start_x+begin_i);                                 // push start
          pat_end.push_back(start_x+end_i-1);                                   // push end
          pat_nbase.push_back(total);                                           // push total
          pat_beta.push_back((double)meth/total);                               // push beta
          char fnv_str[17] = {'0'};
          snprintf(fnv_str, 17, "%.16" PRIX64, fnv);
          pat_fnv.emplace_back(fnv_str, 16);                                    // push FNV-1a hash
        }
      }
    }
  }
  
  if (!npat) return Rcpp::DataFrame::create();                                  // return empty DataFrame if no patterns were found
  
  pat_map.merge(hlght_map);                                                     // merge pattern map and highlight map
  
  for (auto it=pat_map.begin(); it!=pat_map.end(); it++) {                      // we reserved more, cutting off NA values now
    it->second.resize(npat);
  }
  Rcpp::DataFrame res = Rcpp::wrap(pat_map);                                    // wrap pattern map into DataFrame
  
  Rcpp::CharacterVector contexts = Rcpp::CharacterVector::create(               // base contexts
    "NA1", "H", "A", "C", "NA5", "X", "Z", "NA8",
    "NA9", "h", "T", "G","NA13", "x", "z","NA16"
  );
  for (int i=0; i<res.length(); i++) {                                          // context factor for every column
    Rcpp::IntegerVector column = res[i];
    column.attr("class") = "factor";
    column.attr("levels") = contexts;
  }
  
  Rcpp::IntegerVector pat_rname(npat, (int)target_rname);                       // rname factor (size, value)
  pat_rname.attr("class") = rname.attr("class");
  pat_rname.attr("levels") = rname.attr("levels");
  
  res.push_front(pat_fnv, "pattern");
  res.push_front(pat_beta, "beta");
  res.push_front(pat_nbase, "nbase");
  res.push_front(pat_end, "end");
  res.push_front(pat_start, "start");
  res.push_front(pat_strand, "strand");
  res.push_front(pat_rname, "seqnames");
  
  Rcpp::IntegerVector col_strand = res["strand"];                               // strand factor
  col_strand.attr("class") = strand.attr("class");
  col_strand.attr("levels") = strand.attr("levels");
  
  return(res) ;
}



// test code in R
//

/*** R
bam <- preprocessBam(bam.file=system.file("extdata", "amplicon010meth.bam", package="epialleleR"))
z <- data.table::data.table(rcpp_extract_patterns(bam, 47, 43124861, 43125249, 1, "zZ", 0.1, TRUE, 0, as.integer(c())))
z[, c(lapply(.SD, unique), .N), by=pattern, .SDcols=grep("^X", colnames(z), value=TRUE)][order(-N)]
*/

// Sourcing:
// Rcpp::sourceCpp("rcpp_cx_report.cpp")

// #############################################################################
