// GTPro Query Engine, Compressor, and Indexer.
//
// For license and copyright information, please see
// https://github.com/zjshi/gt-pro2.0/blob/master/LICENSE
//
// C++11 code formatted with
//
//     clang-format -style="{BasedOnStyle: llvm, ColumnLimit: 128}"

#if __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 22)
#define _MAP_POPULATE_empty
#endif
#define __STDC_FORMAT_MACROS
#endif

#ifdef _MAP_POPULATE_empty
#define MMAP_FLAGS (MAP_PRIVATE | MAP_POPULATE)
#else
#define MMAP_FLAGS (MAP_PRIVATE)
#endif

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h> // for PRId64
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <libgen.h>
#include <limits>
#include <mutex>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std;

// each 8 MB chunk will run in its own thread
// 12 MB fastq ~ 26,000 reads ~ 0.5 cpu seconds
constexpr auto SEGMENT_SIZE = 12 * 1024 * 1024;

// allocate 15% more chunks than query threads
// that way query threads would never have to wait for I/O
// 15% is a guess
constexpr auto READ_AHEAD_RATIO = 1.15;

// The DB k-mers are 31-mers.
constexpr auto K = 31;

// 2 bits to encode each ACTG letter
constexpr auto BITS_PER_BASE = 2;

// number of bits to encode entire K-mer
constexpr auto K2 = BITS_PER_BASE * K;

constexpr uint64_t LSB = 1;
constexpr auto BASE_MASK = (LSB << BITS_PER_BASE) - LSB;
constexpr auto FULL_KMER = (LSB << K2) - LSB;

// Choose appropriately sized integer types to represent offsets into
// the database.
using LmerRange = uint64_t;
constexpr auto START_BITS = 48;
constexpr auto LEN_BITS = 64 - START_BITS;
constexpr auto MAX_START = (LSB << START_BITS) - LSB;
constexpr auto MAX_LEN = (LSB << LEN_BITS) - LSB;

// element 0, 1:  61-bp nucleotide sequence centered on SNP, for a more detailed map please see
// the comment "note on the binary representation of nucleotide sequences" below.
// element 2:  SNP coordinates consisting of species ID, major/minor allele bit, and genomic position;
// new -- the 8 MSBs are now a virtual snp id (vid) to help represent conflicting kmers

constexpr auto SNP_VID_BITS = 8;
constexpr auto SNP_MAX_VID = (LSB << SNP_VID_BITS) - LSB;
constexpr auto SNP_REAL_ID_BITS = 64 - SNP_VID_BITS;
constexpr auto SNP_MAX_REAL_ID = (LSB << SNP_REAL_ID_BITS) - LSB;

// This param is only useful for perf testing.  The setting below, not
// to exceed 64 TB of RAM, is equivalent to infinity in 2019.
constexpr auto MAX_MMAP_GB = 64 * 1024;
constexpr auto MAX_END = MAX_MMAP_GB * (LSB << 30) / 8;

// Ordered by preference, i.e. the best performing ones are at the top.
const char *compressors[][3] = {
    {".lz4", "lz4", "untested"}, {".bz2", "lbzip2", "untested"}, {".bz2", "bzip2", "untested"},
    {".gz", "pigz", "untested"}, {".gz", "gzip", "untested"},
};

extern int errno;

long chrono_time() {
  using namespace chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct Command {
  bool success;
  int exit_code;
  string output;
  string cmd;
  Command(const string& cmd) : cmd(cmd), exit_code(-1), success(false) {}
  void run(const bool quiet=true) {
    assert(errno == 0);
    char *line_str = NULL;
    size_t line_cap = 0;
    if (!(quiet)) {
      cerr << chrono_time() << ":  [Info] Command: " << cmd << endl;
    }
    FILE *f = popen(cmd.c_str(), "r");
    if (errno == 0 && f) {
      const auto chars_read = getline(&line_str, &line_cap, f);
      if (chars_read > 0 && line_str[chars_read - 1] == '\n') {
        line_str[chars_read - 1] = 0;
      }
      output = string(line_str);
      if (!(quiet)) {
        cerr << chrono_time() << ":  [Info] Output: " << output << endl;
      }
      if (errno == 0) {
        exit_code = pclose(f);
        if (errno == 0 && exit_code == 0) {
          success = true;
        }
      }
    }
    if (line_str) {
      free(line_str);
    }
  }
};

double system_ram_mac(const double default_result) {
  double result = default_result;
  Command cmd("sysctl hw.memsize | grep '^hw.memsize:' | awk '{print $2}'");
  cmd.run();
  if (cmd.success) {
    result = atof(cmd.output.c_str()) / (1ULL << 30);
  }
  return result;
}

double system_ram_linux(const double default_result) {
  double result = default_result;
  Command cmd("grep '^MemTotal:' /proc/meminfo  | awk '{print $2}'");
  cmd.run();
  if (cmd.success) {
    result = atof(cmd.output.c_str()) / (1ULL << 20);
  }
  return result;
}

// Return system RAM in GB
double system_ram(const bool quiet=false, const double default_result=0.0) {
  Command cmd("uname");
  cmd.run();
  if (cmd.success) {
    if (cmd.output == "Darwin") {
      return system_ram_mac(default_result);
    }
    if (cmd.output == "Linux") {
      return system_ram_linux(default_result);
    }
    if (!(quiet)) {
      cerr << chrono_time() << ":  [ERROR]  Unsupported system: " << cmd.output << endl;
    }
  }
  return default_result;
}

// Look up optimal -l and -m in the table of experimental results for the reference DB.
// That is, based on the test result matrix, choose values that are expected to
// perform best for the amount of RAM that would be left on the current
// system after loading the current DB in filesystem cache.
bool choose_optimal_l_and_m(int& l, int& m, double db_size, bool explicit_l_and_m) {
  bool success = true;
  const auto ram_gb = system_ram(explicit_l_and_m);
  if (ram_gb < 1.0) {
    cerr << chrono_time() << ":  [ERROR]  Failed to determine system RAM size." << endl;
    success = false;
  } else {
    db_size /= (1ULL << 30); // convert to gigs
    const auto reference_db_size = 13.0; // our perf testing was done for a 13GB ref DB
    const auto refeq_size = ram_gb - db_size + reference_db_size;
    if (refeq_size > 57) {
      l = 32;
      m = 36;
    } else if (refeq_size > 41) {
      l = 31;
      m = 36;
    } else if (refeq_size > 32) {
      l = 30;
      m = 36;
    } else if (refeq_size > 27) {
      l = 30;
      m = 35;
    } else if (refeq_size > 23) {
      l = 29;
      m = 35;
    } else if (refeq_size > 21) {
      l = 29;
      m = 34;
    } else if (refeq_size > 20) {
      l = 29;
      m = 33;
    } else if (refeq_size > 18) {
      l = 28;
      m = 33;
    } else if (refeq_size > 17) {
      l = 28;
      m = 31;
    } else {
      success = false;
    }
  }
  return success;
}

int test_compressor(const char *compressor) {
  // Return "true" iff the specified compressor is available on the system and
  // successfully compresses and decompresses our test string.
  // BASH-compatible shell required.
  assert(errno == 0);
  const size_t MAX_CMD_LEN = 4095;
  char *line_str = NULL;
  size_t line_cap = 0;
  const char *test_str = "hello there from gt pro testing testing 1 2 3";
  const size_t test_str_len = strlen(test_str);
  const char *cmd_pattern = "(echo '%s' | %s -c | %s -dc | cut -c1-1000 | head -1) 2>/dev/null";
  const size_t cmd_pattern_len = strlen(cmd_pattern);
  const size_t compressor_len = strlen(compressor);
  const size_t cmd_len = cmd_pattern_len + test_str_len + 2 * compressor_len;
  if (cmd_len > MAX_CMD_LEN) {
    const size_t max_compressor_len =
        MAX_CMD_LEN > (cmd_pattern_len + test_str_len) ? (MAX_CMD_LEN - cmd_pattern_len - test_str_len) / 2 : 0;
    fprintf(stderr, "Compressor command length %lu exceeds supported max length %lu.\n", compressor_len, max_compressor_len);
    return false;
  }
  char test_cmd[cmd_len + 1];
  sprintf(test_cmd, cmd_pattern, test_str, compressor, compressor);
  assert(errno == 0);
  FILE *f = popen(test_cmd, "r");
  if (errno == 0 && f) {
    const auto chars_read = getline(&line_str, &line_cap, f);
    if (errno == 0) {
      const int exit_code = pclose(f);
      if (errno == 0 && exit_code == 0) {
        size_t line_len = strlen(line_str);
        assert(chars_read == -1 || line_len == chars_read);
        if (line_len > 0 && line_str[line_len - 1] == '\n') {
          --line_len;
          line_str[line_len] = 0;
        }
        if ((line_len == test_str_len) && strcmp(test_str, line_str) == 0) {
          return true;
        }
      }
    }
  }
  if (line_str) {
    free(line_str);
  }
  errno = 0;
  return false;
}

int decompressor(const char *inpath) {
  // Return the decompressor that can handle inpath's extension,
  // or -1 if inpath does not require a decomprssor.  If multiple
  // decompressors are available for the inpath extension, pick
  // the best one that's installed on the system.
  const auto pl = strlen(inpath);
  int i = 0;
  int required = -1;
  for (auto &comp : compressors) {
    // comp[0] is the extension, e.g. ".gz"
    // comp[1] is the corresponding compressor program, e.g. "gzip"
    // comp[2] indicates if the decompressor is present and working on this system
    const auto clen = strlen(comp[0]);
    if (pl > clen && 0 == strcasecmp(inpath + pl - clen, comp[0])) {
      if (0 == strcmp(comp[2], "untested")) {
        comp[2] = test_compressor(comp[1]) ? "tested_and_works" : "tested_and_does_not_work";
      }
      if (required == -1 && 0 == strcmp(comp[2], "tested_and_works")) { // remember just the first one as it's preferred
        required = i;
		return required;
      }
    }
    ++i;
  }
  return required;
}

string chopext(string path, int decomp_idx) {
  // First chop off any compression extension.
  if (decomp_idx != -1) {
    auto &comp = compressors[decomp_idx];
    path = path.substr(0, path.size() - strlen(comp[0]));
  }
  // Next chop off whatever other extension there is, probably .fq or .fastq
  auto last_dot = path.find_last_of(".");
  if (last_dot == string::npos) {
    return path;
  }
  return path.substr(0, last_dot);
}

FILE *popen_compression_filter(const char *compressor, const char *path, const char *direction = "decompress") {
  // Retur a pipe open for reading from gzip -dc or equivalent.
  assert(errno == 0);
  FILE *f = NULL;
  const size_t MAX_CMD_LEN = 4095;
  const char *cmd_pattern = NULL;
  const char *flags = NULL;
  if (0 == strcmp(direction, "decompress")) {
    cmd_pattern = "%s -dc %s";
    flags = "r";
  } else if (0 == strcmp(direction, "compress")) {
    cmd_pattern = "%s -c > %s";
    flags = "w";
  } else {
    assert(false && "Unsupported direction.");
    return NULL; // just in case asserts are disabled
  }
  const size_t cmd_pattern_len = strlen(cmd_pattern);
  const size_t compressor_len = strlen(compressor);
  const size_t path_len = strlen(path);
  const size_t cmd_len = cmd_pattern_len + compressor_len + path_len - 4;
  if (cmd_len > MAX_CMD_LEN) {
    const size_t max_path_len =
        (MAX_CMD_LEN + 4) > (cmd_pattern_len + compressor_len) ? (MAX_CMD_LEN + 4 - cmd_pattern_len - compressor_len) : 0;
    fprintf(stderr, "ERROR: Path length %lu exceeds supported max %lu.\n", path_len, max_path_len);
  } else {
    char cmd[cmd_len + 1];
    sprintf(cmd, cmd_pattern, compressor, path);
    assert(errno == 0);
    f = popen(cmd, flags);
    if (errno) {
      f = NULL;
    }
    if (f && ferror(f)) {
      f = NULL;
    }
  }
  return f;
}

FILE *popen_compressor(const char *compressor, const char *out_path) {
  return popen_compression_filter(compressor, out_path, "compress");
}

FILE *popen_decompressor(const char *compressor, const char *in_path) {
  return popen_compression_filter(compressor, in_path, "decompress");
}

bool file_exists(const char *filename) {
  struct stat st;
  auto status = stat(filename, &st);
  errno = 0;
  return (status != -1);
}

size_t get_fsize(const char *filename) {
  struct stat st;
  if (stat(filename, &st) == -1) {
    // Probably file not found.
    errno = 0;
    return 0;
  }
  return st.st_size;
}

struct CodeDict {
  vector<uint8_t> code_dict;
  uint8_t *data;
  CodeDict() {
    constexpr auto CHAR_LIMIT = 1 << (sizeof(char) * 8);
    for (uint64_t c = 0; c < CHAR_LIMIT; ++c) {
      // This helps us detect non-nucleotide characters on encoding.
      code_dict.push_back(0xff);
    }
    code_dict['A'] = code_dict['a'] = 0;
    code_dict['C'] = code_dict['c'] = 1;
    code_dict['G'] = code_dict['g'] = 2;
    code_dict['T'] = code_dict['t'] = 3;
    data = code_dict.data();
  }
};
const CodeDict code_dict;

// see the comment "note on the binary representation of nucleotide sequences" below.
template <class int_type, int len> bool seq_encode(int_type *result, const char *buf) {
  result[0] = 0; // forward
  result[1] = 0; // rc
  // This loop may be unrolled by the compiler because len is a compile-time constant.
  for (int_type bitpos = 0; bitpos < BITS_PER_BASE * len; bitpos += BITS_PER_BASE) {
    const uint8_t b_code = code_dict.data[*buf++];
    if (b_code & 0xfc) {
      return true;
    }
    result[0] |= (((int_type)b_code) << bitpos);
    result[1] <<= BITS_PER_BASE;
    result[1] |= (b_code ^ 3);
  }
  return false; // success
}

uint64_t reverse_complement(uint64_t dna) {
  // The bitwise complement represents the ACTG nucleotide complement -- see seq_encode above.
  // Possibly not the fastest way to RC a kmer, but only runs during database construction.
  dna ^= FULL_KMER;
  uint64_t rc = dna & BASE_MASK;
  for (int k = 1; k < K; ++k) {
    dna >>= BITS_PER_BASE;
    rc <<= BITS_PER_BASE;
    rc |= (dna & BASE_MASK);
  }
  return rc;
}

static bool ends_with(const std::string &str, const std::string &suffix) {
  return str.size() >= suffix.size() && 0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix);
}

// GNU memrchr isn't available on Mac OS X
template <typename ElementType>
const ElementType *find_last(const ElementType *first, const ElementType *last, const ElementType needle) {
  while (first < last) {
    --last;
    if (*last == needle) {
      return last;
    }
  }
  return NULL;
}

int64_t kmer_lookup_chunk(vector<uint64_t> *kmer_matches, const LmerRange *const lmer_index, const uint64_t *const mmer_bloom,
                          const uint32_t *const kmers_index, const uint64_t *const snps, const char *const window,
                          const int bytes_in_chunk, const int M2, const int M3, const string &in_path, const long s_start) {

  const uint64_t MAX_BLOOM = (LSB << M3) - LSB;

  // Reads that contain wildcard characters ('N' or 'n') are split into
  // tokens at those wildcard characters.  Each token is processed as
  // though it were a separate read.
  constexpr int MAX_TOKEN_LENGTH = 500;
  constexpr int MIN_TOKEN_LENGTH = 31;

  // This ranges from 0 to the length of the longest read (could exceed MAX_TOKEN_LENGTH).
  int token_length = 0;
  char c = '\0';

  int n_lines = 0;

  char seq_buf[MAX_TOKEN_LENGTH];
  unordered_map<uint64_t, int> footprint;

  for (int i = 0; i < bytes_in_chunk; ++i) {

    // c is the character preceding window[i]
    if (c == '\n') {
      ++n_lines;
      // The first character on the read header must be @
      if (n_lines % 4 == 0 && window[i] != '@') {
        return -i; // here i >= 1
      }
      // Similar rule from the fastq spec.
      if (n_lines % 4 == 2 && window[i] != '+') {
        return -i;
      }
    }

    // Invariant:  The number of new line characters consumed before window[i]
    // is the value of n_lines.

    c = window[i];

    // In FASTQ format, every 4 lines define a read.  The first line is the
    // read header.  The next line is the read sequence.  We only care about
    // the read sequence, where n_lines % 4 == 1.
    if (n_lines % 4 != 1) {
      // The current line does *not* contain a read sequence.
      // Next character, please.
      continue;
    }

    // The current line contains a read sequence.  Split it into tokens at wildcard 'N'
    // characters.  Buffer current token in seq_buf[0...MAX_READ_LENGTH-1].
    const bool at_token_end = (c == '\n') || (c == 'N') || (c == 'n');
    if (!(at_token_end)) {
      // Invariant:  The current token length is token_length.
      // Only the first MAX_TOKEN_LENGTH charaters of the token are retained.
      if (token_length < MAX_TOKEN_LENGTH) {
        seq_buf[token_length] = c;
      }
      ++token_length;
      // next character, please
      continue;
    }

    // is token length within acceptable bounds?   if not, token will be dropped silently
    if (MIN_TOKEN_LENGTH <= token_length && token_length <= MAX_TOKEN_LENGTH) {

      // yes, process token
      for (int j = 0; j <= token_length - K; ++j) {

        // This kmer_tuple encodes the forward and reverse kmers at seq_buf[j...]
        uint64_t kmer_tuple[2];
        const auto malformed_dna = seq_encode<uint64_t, K>(kmer_tuple, seq_buf + j);
        if (malformed_dna) {
          return -(i - token_length);
        }
        const uint64_t kmer = min(kmer_tuple[0], kmer_tuple[1]);

        if ((mmer_bloom[(kmer & MAX_BLOOM) / 64] >> (kmer % 64)) & 1) {

          const uint32_t lmer = kmer >> M2;
          const auto range = lmer_index[lmer];
          const auto start = range >> LEN_BITS;
          const auto end = min(MAX_END, start + (range & MAX_LEN));
          for (uint64_t z = start; z < end; ++z) {
            const auto kmi = kmers_index[z];
            const auto offset = kmi & 0x1f;
            const auto snp_id = kmi >> 5;
            const auto snp_repr = snps + 3 * snp_id;
            const auto low_bits = snp_repr[0] >> (62 - (offset * BITS_PER_BASE));
            const auto high_bits = (snp_repr[1] << (offset * BITS_PER_BASE)) & FULL_KMER;
            const auto db_kmer = high_bits | low_bits;
            // The assert below is true but might be a bit slow due to reverse_complement.
            // TODO: instead of calling reverse_complement() consider using kmer_rc
            // assert(lmer == (db_kmer >> M2) || lmer == (reverse_complement(db_kmer) >> M2));
            if (kmer_tuple[0] == db_kmer || kmer_tuple[1] == db_kmer) {
              // The set of kmers that cover the SNP within the given read won't conflict
              // with each other, but may still belong to different virtual SNPs.  We need
              // to identify the real SNP they all belong to, and increment the real SNP's
              // counter just once for the read.
              const auto snp = snp_repr[2] & SNP_MAX_REAL_ID;
              if (footprint.find(snp) == footprint.end()) {
                kmer_matches->push_back(snp);
                footprint.insert({snp, 1});
              }
            }
          }
        }
      }
    }

    // clear footprint for every read instead of every token
    if (c == '\n') {
      footprint.clear();
    }

    // next token, please
    token_length = 0;
  }

  if (token_length != 0) {
    // Truncated read sequence at end of chunk.  Malformed FASTQ.
    return -bytes_in_chunk;
  }

  return (n_lines + 3) / 4;
}

const char *last_read(const char *window, const uint64_t bytes_in_window) {
  // Return a pointer to the initial '@' character of the last read header
  // within the given window.  Return NULL if no such read (for example,
  // if the file does not conform to FASTQ format).

  // HOW?
  //
  // In FASTQ format, a line that starts with '@' is either a read header
  // (also called sequence identifier), or a quality string.
  //
  // Examining only the local neighborhood of an arbitrary line L that
  // starts with '@', we may quickly determine whether L contains a read header
  // or a quality string by looking at the first character of line L - 2.
  //
  // If the first character of line L - 2 is '+', then line L is a read header.
  // Otherwise, line L is a quality string and line L - 3 is a read header.
  //
  // This is not merely an heuristic.  It follows from the fastq format definition.

  // First find where in the window the last few lines begin.
  // Let line_start[3] point to the first character on the last line within the
  // window, line_start[2] same for the previous line, etc.  If fewer than 4 lines
  // (malformed fastq), let all remaining line_start's be NULL.
  // Then compute L such that line_start[L] points to the '@' character of the
  // last read header in the window.

  // no read is under 4 bytes in FASTQ;  checking early helps avoid corner cases below
  if (bytes_in_window <= 4) {
    return NULL;
  }

  // Below we use "bytes_in_window - 1" not just "bytes_in_window" because we
  // are interested in the character that comes after the newline.
  const char *newline = find_last(window, window + bytes_in_window - 1, '\n');
  int L = -1;
  const char *line_start[4];
  for (int l = 3; l >= 0; --l) {
    if (newline == NULL) {
      line_start[l] = NULL;
      continue;
    }
    line_start[l] = newline + 1;
    if (L == -1 && *(line_start[l]) == '@') {
      L = l;
    }
    newline = find_last(window, newline, '\n');
  }
  if (L >= 2) {
    if (line_start[L - 2] != NULL && *(line_start[L - 2]) != '+') {
      L -= 3;
    }
  }
  if (L >= 0 && line_start[L] != NULL && *(line_start[L]) == '@') {
    return line_start[L];
  }
  return NULL;
}

struct ReadersContext {
  // Scan this many files in parallel.  More is better for serial decompressors like gzip.
  // Rule of thumb is one for every 4-6 physical cpu cores.
  // TODO:  Choose default automatically, and expose on command line.
  const int MAX_PARALLEL_READERS;
  int readers;
  mutex mtx;
  condition_variable cv;
  ReadersContext() : readers(0), MAX_PARALLEL_READERS(max(2, min(12, int(thread::hardware_concurrency()) / 6))) {};
  void acquire_reader() {
    unique_lock<mutex> lk(mtx);
    bool acquired = false;
    do {
      cv.wait(lk, [&] {
        if (readers < MAX_PARALLEL_READERS) {
          ++readers;
          acquired = true;
        }
        return acquired;
      });
    } while (!(acquired));
  };
  void acquire_all() {
    for (int i = 0; i < MAX_PARALLEL_READERS; ++i) {
      acquire_reader();
    }
  };
  void release_reader() {
    unique_lock<mutex> lk(mtx);
    assert(readers > 0);
    --readers;
    if (readers == MAX_PARALLEL_READERS - 1) {
      lk.unlock();
      cv.notify_one();
    }
  };
};

// Creating an "auto" instance of this class ensures a reader will
// be released on block exit, even if the exit is early.
struct ReadersContextRelease {
  ReadersContext &ctx;
  ReadersContextRelease(ReadersContext &rc) : ctx(rc) {}
  ~ReadersContextRelease() { ctx.release_reader(); }
};

struct SegmentContext {
  // There are two references to each buffer segment (two tokens).  When a segment is
  // acquired, both tokens are held by scan_input.  After run_queries is enqueued for
  // the segment, one token is held by run_queries until it completes, and the other
  // token is held by scan_input until it copies any unprocessed leftover bytes at
  // the end of the segment to the beginning of the next segment.  When both tokens are
  // released, the segment becomes available to be acquired again.
  constexpr static int TOKENS_PER_SEGMENT = 2;
  mutex mtx;
  condition_variable cv;
  vector<int> tokens;
  vector<char> buffer;
  int n_segments;
  char *buffer_addr;
  SegmentContext(const int n_threads) : n_segments(max(n_threads + 1, int(n_threads *READ_AHEAD_RATIO))) {
    tokens.resize(n_segments);
    buffer.resize(SEGMENT_SIZE * n_segments);
    buffer_addr = buffer.data();
  };
  int acquire_segment(int *n_tokens) {
    int segment_idx = -1;
    do {
      unique_lock<mutex> lk(mtx);
      cv.wait(lk, [&] {
        for (segment_idx = 0; segment_idx < n_segments; ++segment_idx) {
          if (tokens[segment_idx] == 0) {
            tokens[segment_idx] = TOKENS_PER_SEGMENT;
            if (n_tokens) {
              *n_tokens = tokens[segment_idx];
            }
            return true;
          }
        }
        return false;
      });
    } while (segment_idx == -1);
    return segment_idx;
  };
  void release_segment(const int segment_idx, const int n_tokens) {
    // cerr << "Releasing " << n_tokens << " tokens for segment " << segment_idx << endl;
    if (segment_idx < 0) {
      assert(n_tokens == 0);
      return;
    }
    if (n_tokens <= 0) {
      assert(segment_idx < 0);
      return;
    }
    unique_lock<mutex> lk(mtx);
    assert(tokens[segment_idx] >= n_tokens);
    tokens[segment_idx] -= n_tokens;
    if (tokens[segment_idx] == 0) {
      lk.unlock();
      cv.notify_one();
    }
  };
};

struct Segment {
  int idx;
  int tokens;
  char *start_addr;
  const char *end_addr;
  uint64_t size;
  uint64_t bytes_leftover;
  SegmentContext &ctx;
  void assert_invariant() {
    if ((end_addr - start_addr) != (size - bytes_leftover)) {
      cerr << "Segment invariant broken: " << end_addr << " " << bytes_leftover << " " << start_addr << " " << size << endl;
      assert((end_addr + bytes_leftover) == (start_addr + size) && "Segment invariant broken.");
    }
  };
  void reset() {
    idx = -1;
    tokens = 0;
    start_addr = NULL;
    end_addr = NULL;
    bytes_leftover = 0;
    size = 0;
    assert_invariant();
  };
  Segment(SegmentContext &sc) : ctx(sc) { reset(); };
  ~Segment() {
    ctx.release_segment(idx, tokens);
    reset();
  };
  void advance(FILE *input_file, const int channel) {
    assert_invariant();
    Segment old(*this);
    // cerr << "Waiting to acquire segment for channel " << channel << endl;
    idx = ctx.acquire_segment(&tokens);
    // cerr << "Acquired segment " << idx << " for channel " << channel << endl;
    assert(idx != old.idx);
    start_addr = ctx.buffer_addr + idx * SEGMENT_SIZE;
    if (old.bytes_leftover) {
      memmove(start_addr, old.end_addr, old.bytes_leftover);
    }
    const auto bytes_read = fread(start_addr + old.bytes_leftover, 1, SEGMENT_SIZE - old.bytes_leftover, input_file);
    size = bytes_read + old.bytes_leftover;
    end_addr = start_addr + size;
    bytes_leftover = 0;
    assert_invariant();
  };
};

struct Result {
  int channel;
  const string in_path;
  string o_name;
  int n_input_chunks, n_processed_chunks;
  bool finished_reading;
  bool done_with_output;
  vector<uint64_t> *p_kmer_matches;
  uint64_t chars_read;
  bool error;
  bool io_error;
  bool output_error;
  bool missing_decompressor;
  uint64_t error_pos;
  uint64_t n_reads;
  FILE *input_file;
  bool popened;
  int decomp_idx;
  string out_path;
  string err_path;
  bool skip;
  mutex *p_print_lock;
  string full_inpath;
  Result(int channel, const char *in_path, const char *oooname, const string &dbbase, const bool force, mutex *p_print_lock,
         const string &c_prefix)
      : channel(channel), in_path(in_path), n_input_chunks(0), n_processed_chunks(0), finished_reading(false),
        done_with_output(false), o_name(oooname == NULL ? "" : oooname), p_kmer_matches(new vector<uint64_t>()), chars_read(0),
        error(false), output_error(false), missing_decompressor(false), error_pos(1ULL << 48), io_error(false), n_reads(0),
        input_file(NULL), popened(false), skip(false), p_print_lock(p_print_lock) {
    decomp_idx = decompressor(in_path);
    const char *compext = "";
    if (decomp_idx != -1) {
      // compress output with same compressor as input
      compext = compressors[decomp_idx][0];
    }
    if (o_name.empty()) {
      out_path = "/dev/stdout";
      err_path = "/dev/stderr";
    }
    if (c_prefix.empty()) {
      full_inpath = in_path;
    } else if (in_path[0] == '/') {
      cerr << chrono_time() << ": [WARNING] Ignoring specified -C prefix for non-relative input path: " << in_path << endl;
      full_inpath = in_path;
    } else {
      full_inpath = c_prefix + "/" + in_path;
    }
    // Create a unique tag 'inbase' from the input path, to include in the
    // output prefix where indicated by %{in}.  For instance, if the input
    // path is /foo/bar123/r1.fastq.lz4, then inbase would be foo_bar123_r1.
    // The -C prefix, if any, is intentionally not included.
    const bool special = (0 == strncmp(out_path.c_str(), "/dev/", 5));
    if (!(special)) {
      // First, chop off compressor and format extensions.
      string inbase = chopext(in_path, decomp_idx);
      // Chop off all initial '.' and '/' characters.  Those would otherwise
      // turn into leading underscores later.  Could use regex_replace,
      // but this also works.
      int nsi = 0;
      while (nsi < inbase.size() && (inbase[nsi] == '/' || inbase[nsi] == '.')) {
        ++nsi;
      }
      if (nsi) {
        inbase = inbase.substr(nsi, inbase.size() - nsi);
      }
      // Replace all '/' and '.' with '_'
      inbase = regex_replace(inbase, regex("\\."), "_");
      inbase = regex_replace(inbase, regex("/"), "_");
      // Replace %{in} with inbase in output prefix.
      o_name = regex_replace(o_name, regex("%\\{in\\}"), inbase);
      // Replace %{n} with channel in output prefix.
      o_name = regex_replace(o_name, regex("%\\{n\\}"), to_string(channel));
      // Replace %{db} with dbbase in output prefix.
      o_name = regex_replace(o_name, regex("%\\{db\\}"), dbbase);
      // Output will be compressed with the same format as input.
      out_path = o_name + ".tsv" + compext;
      err_path = o_name + ".err";
    }
    auto output_exists = !(special) && file_exists(out_path.c_str());
    auto error_exists = !(special) && file_exists(err_path.c_str());
    if (force || error_exists || !(output_exists)) {
      if (output_exists && !(error_exists)) {
        assert(force);
        cerr << chrono_time() << ":  [Info] Forcing recompute for input: " << in_path << endl;
      }
      if (output_exists && error_exists) {
        cerr << chrono_time() << ":  [Info] Redoing input due to the presence of an error file: " << in_path << endl;
      }
      remove_output();
      recreate_error();
    } else {
      cerr << chrono_time() << ":  [Info] Skipping input due to pre-existing result; use -f to recompute: " << in_path << endl;
      skip = true;
    }
  }
  ~Result() {
    if (p_kmer_matches) {
      delete p_kmer_matches;
      p_kmer_matches = NULL;
    }
    close_input();
  }
  void open_input() {
    assert(errno == 0);
    if (decomp_idx == -1) { // no decompression required
      popened = false;
      input_file = fopen(in_path.c_str(), "r");
    } else {
      popened = true;
      auto &decomp = compressors[decomp_idx];
      if (0 == strcmp(decomp[2], "tested_and_does_not_work")) {
        cerr << chrono_time() << ":  "
             << "[ERROR] Required decompressor " << decomp[1] << " is unavailable: " << in_path << endl;
        missing_decompressor = true;
        note_io_error();
      } else {
        assert(0 == strcmp(decomp[2], "tested_and_works"));
        input_file = popen_decompressor(decomp[1], full_inpath.c_str());
      }
    }
    if (errno || input_file == NULL || ferror(input_file)) {
      note_io_error();
    }
  }
  void close_input() {
    errno = 0;
    if (input_file) {
      if (popened) {
        pclose(input_file);
      } else {
        fclose(input_file);
      }
      input_file = NULL;
      if (errno) {
        note_io_error();
      }
    }
    finished_reading = true;
  }
  void note_io_error(bool quiet = false) {
    error_pos = min(error_pos, chars_read);
    if (!(error) && !(quiet)) {
      cerr << chrono_time() << ":  "
           << "[ERROR] Failed to read past position " << error_pos << " in presumed FASTQ file " << full_inpath << endl;
    }
    io_error = true;
    error = true;
    if (errno) {
      perror(in_path.c_str());
      errno = 0;
    }
  }
  void data_format_error(uint64_t pos = 1ULL << 48) {
    error_pos = min(error_pos, min(pos, chars_read));
    error = true;
  }
  void write_error_info() {
    assert(error || output_error);
    ofstream fh(err_path, ofstream::out | ofstream::binary);
    if (output_error) {
      fh << "[ERROR] Failed to write to output file." << endl;
    } else if (missing_decompressor) {
      fh << "[ERROR] Decompressor " << compressors[decomp_idx][1] << " is unavailable for input " << in_path << endl;
    } else if (io_error) {
      // I/O errors are reported on stderr in realtime.  Note them in the .err file.
      fh << "[ERROR] Failed to read past position " << error_pos << " in presumed FASTQ file " << in_path << endl;
    } else {
      fh << "[ERROR] Failed to parse somewhere past position " << error_pos << " in presumed FASTQ file " << in_path << endl;
      unique_lock<mutex> lk(*p_print_lock);
      cerr << chrono_time() << ":  "
           << "[ERROR] Failed to parse somewhere past position " << error_pos << " in presumed FASTQ file " << in_path << endl;
    }
    fh.close();
    delete p_kmer_matches;
    p_kmer_matches = NULL;
    remove_output();
  }
  void remove_file(const string &path, const string placeholder_text = "") {
    if (0 == strncmp(path.c_str(), "/dev/", 5)) { // do not delete /dev/std{out, err}, /dev/null, etc.
      return;
    }
    string rm_cmd = string("/bin/rm -f '" + path + "'");
    errno = 0;
    const auto rm_exit = system(rm_cmd.c_str()); // if this fails, well, such is life
    if (errno || rm_exit) {
      perror(rm_cmd.c_str());
      errno = 0;
      unique_lock<mutex> lk(*p_print_lock);
      cerr << "[ERROR]:  Failed to remove pre-existing file " << path << ";  will not process input " << in_path << endl;
      note_io_error(true); // quiet
    }
    if (!(placeholder_text.empty())) {
      ofstream fh(err_path, ofstream::out | ofstream::binary);
      fh << placeholder_text;
      fh.close();
    }
  }
  void remove_output() { remove_file(out_path); }
  void remove_error() { remove_file(err_path); }
  void recreate_error() {
    remove_file(
        err_path,
        "This placeholder .err file will be removed when execution succeeds for input " + in_path +
            ". If it's still hanging around after the program is done, with this uninformative error message, then the program "
            "must have been aborted in the middle of a computation, without a chance to record a more helpful error message.  "
            "If that's the case, don't trust any result files that may have been produced for this input.");
  }
  void write_output() {
    {
      unique_lock<mutex> lk(*p_print_lock);
      cerr << chrono_time() << ":  "
           << "[Done] searching is completed for the " << n_reads << " reads input from " << in_path << endl;
    }
    if (error) {
      write_error_info();
      return;
    }
    FILE *out_file;
    auto check_output_error = [&](int code_line) -> bool {
      if (out_file == NULL || ferror(out_file)) {
        {
          unique_lock<mutex> lk(*p_print_lock);
          cerr << chrono_time() << ":  "
               << "[ERROR] Error writing output " << out_path << " cl " << code_line << endl;
          if (errno) {
            perror(out_path.c_str());
            errno = 0;
          }
          output_error = true;
        }
        write_error_info();
        return true;
      }
      return false;
    };
    if (decomp_idx != -1) {
      out_file = popen_compressor(compressors[decomp_idx][1], out_path.c_str());
    } else {
      out_file = fopen(out_path.c_str(), "w");
    }
    if (check_output_error(__LINE__)) {
      return;
    }
    if (p_kmer_matches->size() == 0) {
      unique_lock<mutex> lk(*p_print_lock);
      cerr << chrono_time() << ":  "
           << "[WARNING] found zero hits for the " << n_reads << " reads input from " << in_path << endl;
    } else {
      // For each kmer output how many times it occurs in kmer_matches.
      sort(p_kmer_matches->begin(), p_kmer_matches->end());
      const uint64_t end = p_kmer_matches->size();
      uint64_t i = 0;
      uint64_t n_snps = 0;
      uint64_t n_hits = 0;
      while (i != end) {
        uint64_t j = i + 1;
        while (j != end && (*p_kmer_matches)[i] == (*p_kmer_matches)[j]) {
          ++j;
        }
        ++n_snps;
        n_hits += (j - i);
        fprintf(out_file, "%" PRId64 "\t%" PRId64 "\n", (*p_kmer_matches)[i], (j - i));
        if (check_output_error(__LINE__)) {
          return;
        }
        i = j;
      }
      {
        unique_lock<mutex> lk(*p_print_lock);
        cerr << chrono_time() << ":  "
             << "[Stats] " << n_snps << " snps, " << n_reads << " reads, " << int((((double)n_hits) / n_snps) * 100) / 100.0
             << " hits/snp, for " << in_path << endl;
      }
    }
    if (decomp_idx == -1) {
      fclose(out_file);
    } else {
      pclose(out_file);
    }
    if (check_output_error(__LINE__)) {
      return;
    }
    delete p_kmer_matches;
    p_kmer_matches = NULL;
    remove_error();
  }
  bool pending_output() {
    if (done_with_output || !(finished_reading)) {
      return false;
    }
    {
      unique_lock<mutex> lk(mtx);
      return n_input_chunks == n_processed_chunks;
    }
  }
  void merge_kmer_matches(vector<uint64_t> &kmt, const int64_t n_reads_chunk) {
    unique_lock<mutex> lk(mtx);
    p_kmer_matches->insert(p_kmer_matches->end(), kmt.begin(), kmt.end());
    ++n_processed_chunks;
    if (n_reads_chunk >= 0) {
      n_reads += n_reads_chunk;
    }
  }

private:
  mutex mtx;
};

bool kmer_lookup(LmerRange *lmer_index, uint64_t *mmer_bloom, uint32_t *kmers_index, uint64_t *snps, int n_inputs,
                 const char **input_paths, char *o_name, const int M2, const int M3, const int n_threads, const string &dbbase,
                 const bool force, const string &c_prefix) {

  auto s_start = chrono_time();
  const char *stdin = "/dev/stdin";

  if (n_inputs == 0 || input_paths == NULL || o_name == NULL) {
    cerr << chrono_time() << ":  [Info] Will input reads from stdin and output snps to stdout." << endl;
    cerr << chrono_time() << ":  [Info] Output will appear only after stdin reaches EOF." << endl;
    n_inputs = 1;
    input_paths = &stdin;
    o_name = NULL;
  }

  uint64_t total_reads = 0;
  mutex print_lock;

  vector<Result *> results;
  for (int i = 0; i < n_inputs; ++i) {
    // This deletes any pre-existing output file and emits out.i.err if
    // the required decompressor for input_paths[i] is not installed.
    results.push_back(new Result(i, input_paths[i], o_name, dbbase, force, &print_lock, c_prefix));
  }

  SegmentContext sc(n_threads);

  using QueryTask = tuple<int, int, uint64_t, uint64_t>;
  queue<QueryTask> query_tasks;
  mutex queue_mtx;
  condition_variable queue_cv;
  int running_threads = 0;
  bool all_inputs_scanned = false;

  auto enqueue_query_task = [&](QueryTask qt) {
    unique_lock<mutex> lk(queue_mtx);
    query_tasks.push(qt);
    if (query_tasks.size() == 1) {
      lk.unlock();
      queue_cv.notify_one();
    }
  };

  auto query_task_func = [&](QueryTask qt) {
    int channel;
    int segment_idx;
    uint64_t segment_size;
    uint64_t offset_in_file;
    tie(channel, segment_idx, segment_size, offset_in_file) = qt;
    int64_t n_reads;
    {
      vector<uint64_t> kmt;
      n_reads = kmer_lookup_chunk(&kmt, lmer_index, mmer_bloom, kmers_index, snps, sc.buffer_addr + SEGMENT_SIZE * segment_idx,
                                  segment_size, M2, M3, results[channel]->in_path, s_start);
      sc.release_segment(segment_idx, 1);
      if (n_reads < 0) {
        // if negative, n_reads isn't actually a count of reads;  it's a count of chars before the error
        results[channel]->data_format_error(offset_in_file - n_reads);
      }
      results[channel]->merge_kmer_matches(kmt, n_reads);
    }
    {
      unique_lock<mutex> lk(queue_mtx);
      --running_threads;
      if (n_reads >= 0) {
        total_reads += n_reads;
      }
      if (running_threads == 0 || running_threads == n_threads - 1) {
        lk.unlock();
        queue_cv.notify_one();
      }
    }
  };

  int closed_outputs = 0;

  // this function will output result for an input file
  auto write_output_func = [&](const int result_idx) {
    auto &r = *results[result_idx];
    r.write_output();
    {
      unique_lock<mutex> lk(queue_mtx);
      ++closed_outputs;
      --running_threads;
      if (running_threads == 0 || running_threads == n_threads - 1) {
        lk.unlock();
        queue_cv.notify_one();
      }
    }
  };

  int first_result_idx_not_done_with_output = 0;
  int last_result_idx_done_with_input = -1;

  constexpr uint64_t PROGRESS_UPDATE_INTERVAL = 1000 * 1000;
  uint64_t total_reads_last_update = 0;

  auto task_dispatch_loop = [&]() {
    bool all_done = false;
    do {
      unique_lock<mutex> lk(queue_mtx);
      queue_cv.wait(lk, [&] {
        // Progress update.
        if ((total_reads - total_reads_last_update) >= PROGRESS_UPDATE_INTERVAL) {
          unique_lock<mutex> lk(print_lock);
          cerr << chrono_time() << ":  [Progress] " << (total_reads / 10000) / 100.0 << " million reads scanned after "
               << (chrono_time() - s_start) / 1000 << " seconds";
          if (o_name) {
            cerr << ", and " << closed_outputs << " files output.";
          } else {
            cerr << ".";
          }
          cerr << endl;
          total_reads_last_update = total_reads;
        }
        // Can we kick off any output writer threads?
        while (running_threads < n_threads) {
          int pending_output_result_idx = -1;
          for (int i = first_result_idx_not_done_with_output; i <= last_result_idx_done_with_input; ++i) {
            if (results[i]->pending_output()) {
              pending_output_result_idx = i;
              break;
            }
          }
          if (pending_output_result_idx == -1) {
            break;
          }
          auto &r = *results[pending_output_result_idx];
          r.done_with_output = true;
          if (!(r.skip)) {
            ++running_threads;
            thread(write_output_func, pending_output_result_idx).detach();
          }
          while (first_result_idx_not_done_with_output < n_inputs &&
                 results[first_result_idx_not_done_with_output]->done_with_output) {
            first_result_idx_not_done_with_output += 1;
          }
        }
        // Can we kick off any query threads?
        while (running_threads < n_threads && !(query_tasks.empty())) {
          ++running_threads;
          auto task = query_tasks.front();
          // cerr << "Dispatching query task for " << get<0>(task) << " " << get<1>(task) << " " << get<2>(task) << " " <<
          // get<3>(task) << endl;
          thread(query_task_func, task).detach();
          query_tasks.pop();
        }
        all_done = (running_threads == 0) && query_tasks.empty() && all_inputs_scanned &&
                   (first_result_idx_not_done_with_output == n_inputs);
        // cerr << running_threads << " " << query_tasks.size() << " " << all_inputs_scanned << " " <<
        // first_result_idx_not_done_with_output << endl;
        return all_done;
      });
    } while (!(all_done));
  };

  ReadersContext rc;

  auto done_with_input = [&](const int channel) {
    unique_lock<mutex> lk(queue_mtx);
    results[channel]->finished_reading = true;
    if (channel > last_result_idx_done_with_input) {
      last_result_idx_done_with_input = channel;
    }
    lk.unlock();
    queue_cv.notify_one();
  };

  auto scan_input = [&](const int channel) {
    ReadersContextRelease rcr(rc);
    Segment segment(sc);
    auto r = results[channel];
    if (r->error) {
      return;
    }
    uint64_t offset_in_file = 0;
    r->open_input();
    while (!(r->error)) {
      segment.advance(r->input_file, channel);
      if (ferror(r->input_file)) {
        r->note_io_error();
        break;
      }
      if (segment.size == 0) {
        break;
      }
      if (segment.size == SEGMENT_SIZE) {
        // If we've filled the segment's entire buffer, it's likely that the segment
        // ends in the middle of a fastq read.  Reverse to the start of that read.
        segment.end_addr = last_read(segment.start_addr, segment.size);
        if (segment.end_addr == NULL || segment.end_addr[0] != '@') {
          r->data_format_error();
          break;
        }
        // Re-establish segment data invariant
        segment.bytes_leftover = segment.size - (segment.end_addr - segment.start_addr);
        segment.assert_invariant();
      }
      // transfer 1 reservation to the task, to be released when the task completes
      --segment.tokens;
      r->n_input_chunks++;
      enqueue_query_task(QueryTask(channel, segment.idx, segment.end_addr - segment.start_addr, offset_in_file));
      offset_in_file +=
          segment.end_addr - segment.start_addr; // only used for error reporting: number of bytes preceding segment
    }
    r->close_input();
    done_with_input(channel);
  };

  auto input_scan_loop = [&]() {
    // Dispatch scanner threads for all inputs in order
    // Pause when necessary to fit under MAX_PARALLEL_READERS threads
    for (int channel = 0; channel < n_inputs; ++channel) {
      if (results[channel]->skip) {
        done_with_input(channel);
      } else {
        rc.acquire_reader();
        thread(scan_input, channel).detach();
      }
    }
    // Wait for all reader threads to complete.
    {
      unique_lock<mutex> lk(print_lock);
      cerr << chrono_time() << ":  [Info] Waiting for all readers to quiesce" << endl;
    }
    rc.acquire_all();
    {
      unique_lock<mutex> lk(queue_mtx);
      all_inputs_scanned = true;
      lk.unlock();
      queue_cv.notify_one();
    }
  };

  thread(input_scan_loop).detach();
  task_dispatch_loop();

  cerr << chrono_time() << ":  " << (total_reads / 10000) / 100.0 << " million reads were scanned after "
       << (chrono_time() - s_start) / 1000 << " seconds" << endl;
  int files_with_errors = 0;
  int files_without_errors = 0;
  int skipped_files = 0;
  uint64_t reads_covered = 0;
  for (int i = 0; i < n_inputs; ++i) {
    if (results[i]->skip) {
      ++skipped_files;
    } else if (results[i]->error || results[i]->output_error) {
      ++files_with_errors;
    } else {
      ++files_without_errors;
      reads_covered += results[i]->n_reads;
    }
    delete results[i];
  }
  if (files_without_errors) {
    cerr << chrono_time() << ":  "
         << "Successfully processed " << files_without_errors << " input files containing " << reads_covered << " reads."
         << endl;
  }
  if (skipped_files) {
    cerr << chrono_time() << ":  "
         << "Skipped " << skipped_files << " input files due to pre-existing results." << endl;
  }
  if (files_with_errors) {
    cerr << "*** Failed for " << (files_without_errors == 0 ? "ALL " : "") << files_with_errors << " input files. ***" << endl;
  }
  return (files_with_errors > 0);
}

void display_usage(char *fname) {
  cerr << "GT-Pro version 1.0.1\n"
       << "For copyright and licensing information, please see\n"
       << "https://github.com/zjshi/gt-pro/blob/master/LICENSE\n"
       << "\n"
       << "ARGUMENTS: \n"
       << "  -d <sckmerdb_path: string> \n"
       << "  -t <n_threads; int; default CPU_count>\n"
       << "  -o <out_prefix; string; default: cur_dir/%{in}__gtpro__%{db}>\n"
       << "  -l <number of index address bits; int 28..32; default: depends on machine RAM>\n"
       << "  -m <bloom filter address bits; int 30..36; default: depends on machine RAM>\n"
       << "  -h <display this usage info>\n"
       << "  -f <force overwrite of pre-existing outputs>\n"
       << "  -C <in_prefix; string; default: none>\n"
       << "  [input0, input1, ...]\n"
       << "\n"
       << "WHERE\n"
       << "\n"
       << "  input1, input2, ... are files in FASTQ format, optionally compressed,\n"
       << "  and optionally in the dir specified by -C, which may be an s3 bucket\n"
       << "\n"
       << "  when no inputs are specified, gt_pro consumes fastq input from stdin\n"
       << "  until stdin reaches EOF, then emits all output to stdout at once\n"
       << "\n"
       << "  GT_Pro will refuse to work if pre-existing output files are detected.\n"
       << "  Consider using -f when any pre-existing output files should be overwritten\n"
       << "\n"
       << "USAGE EXAMPLES\n"
       << "\n"
       << "  Here are a few quick examples on how to use GT-Pro, which should cover most of it regular uses\n"
       << "\n"
       << "  The simplest use:\n"
       << "    GT_Pro genotype -d /path/to/dbname path/to/input.fastq.gz\n"
       << "    Note: " 
	   << "    By default the output file can be found in your current directory and named to path_to_input__gtpro__dbname.tsv.lz4\n"
       << "    A long name like path_to_input__gtpro__dbname.tsv.lz4 is a purposeful design to help avoid accidental file overwriting.\n"
       << "    The file type .tsv.lz4 reveals that the output is in the format of tab-separated values and compressed with lz4 algorithm.\n"
       << "\n"
       << "    Question: how to find the dbname?\n"
       << "    A: In a directory hosting a GT-Pro database, highly likily in your current directory, you may see two or all of four following files:\n"
       << "       [dbname]_optimized_db_kmer_index.bin\n"
       << "       [dbname]_optimized_db_snps.bin\n"
       << "       [dbname]_optimized_db_lmer_index_xx.bin, xx is a two-digit number, e.g. 30\n"
       << "       [dbname]_optimized_db_mmer_bloom_yy.bin, yy is also a two-digit number, e.g. 35\n"
       << "    dbname is all the characters in the brackets while brakets itself not included.\n"
       << "\n"
       << "    Question: how many CPUs does GT-Pro use by default?\n"
       << "    A: By defauly, GT-Pro automatically detects the number of available CPUs in a computing environment and then uses all of them\n"
       << "\n"
       << "    Question: why does GT-Pro mentioning 'Skipped x input files due to pre-existing results.'\n"
       << "    A: GT-Pro automatically detects whether there is a conflict on the location it is about to write an output file. There a conflict exists, GT-Pro will by default cautiously skip output writing. You may have a quick check and manually resolve the confict, or use the -f flag to overwrite pre-existing files.\n"
       << "\n"
       << "  The simplest use + use a certain number of CPUs:\n"
       << "    GT_Pro genotype -d /path/to/dbname -t 8 path/to/input.fastq.gz\n"
       << "    Note: You may consult this example if you do not want to let GT-Pro use all of CPUs.\n"
	   << "    The flag of -t can be used for designating a maximum number of CPUs GT-Pro uses.\n" 
	   << "    For performance reasons, we recommend never supplying -t with a number more than the total number of CPUs in a computing environment\n" 
       << "\n"
       << "  Genotype more than one sample/metagenome:\n"
       << "    GT_Pro genotype -d /path/to/dbname path/to/input1.fastq.gz path/to/input2.fastq.gz path/to/input3.fastq.gz ...\n"
       << "    Note: by default all the output files can be found in your current directory and each is named similiarly as the single input use.\n"
       << "\n"
       << "  Genotype more than one sample/metagenome with mixed file types (e.g. .gz, .lz4, .bz2):\n"
       << "    GT_Pro genotype -d /path/to/dbname path/to/input1.fastq.gz path/to/input2.fastq.lz4 path/to/input3.fq.bz2 ...\n"
       << "\n"
       << "  GT_Pro does not care if input files are mixed with the compressed or uncompressed as long as they are in FASTQ format:\n"
       << "    GT_Pro genotype -d /path/to/dbname path/to/input1.fastq.gz path/to/input2.fastq path/to/input3.fq ...\n"
       << "\n"
       << "  Genotype more than one sample/metagenome in the same directory:\n"
       << "    GT_Pro genotype -d /path/to/dbname -C /path/to/input/directory test576/r1.fastq.lz4 test576/r2.fq.bz2\n"
       << "    Note: you might want to use -C to avoid super long commandline\n"
       << "\n"
       << "  Genotype a lot of samples with the same file type (e.g. gzipped fastq) in the same directory:\n"
       << "    GT_Pro genotype -d /path/to/dbname -C /path/to/input/directory *.fastq.gz\n"
       << "\n"
       << "  Genotype a lot of samples with mixed file type (e.g. .gz, .lz4, .bz2) in the same directory:\n"
       << "    GT_Pro genotype -d /path/to/dbname -C /path/to/input/directory *.fastq.gz *.fastq.bz2 *.fq.lz4\n"
       << "\n"
       << "  Genotype everything in one directory:\n"
       << "    GT_Pro genotype -d /path/to/dbname -C /path/to/input/directory *\n"
       << "    Note: you might want to make sure all files in the directory are compatible with GT-Pro\n"
       << "\n"
       << "  Designate an full output path for a sample/metagenome :\n"
       << "    GT_Pro genotype -d /path/to/dbname -o /path/to/output/name path/to/input.fastq.gz\n"
       << "    Note: Please consult this example if you dislike the default way how GT-Pro writes an output file. In the example, the output can be found at /path/to/output/name.tsv.lz4\n"
       << "\n"
       << "  Designate a different output directory than the current directory for a sample/metagenome :\n"
       << "    GT_Pro genotype -d /path/to/dbname -o /path/to/output/directory/%{in}__gtpro__%{db} path/to/input.fastq.gz\n"
       << "    Note: Suppose that you are fine the way how GT-Pro names an output file but want to keep the current directory clean. In this example, the output can be found at in the designated output directory with a file name like path_to_input__gtpro__dbname.tsv.lz4\n"
	   << "    You may also realized that the example of the simplest use is equivalent as `GT_Pro genotype -d /path/to/dbname -o ./%{in}__gtpro__%{db} path/to/input.fastq.gz`\n"
       << "\n"
       << "  Designate a different output directory than the current directory when genotyping more than one sample/metagenome :\n"
       << "    GT_Pro genotype -d /path/to/dbname -o /path/to/output/directory/%{in}__gtpro__%{db} path/to/input1.fastq.gz "
	   << "path/to/input2.fastq.gz path/to/input3.fastq.gz\n"
       << "    Note: This is similiar as designating different output directory for a single input.\n"
	   << "    The output files can be found at in the designated output directory, with names like path_to_input1__gtpro__dbname.tsv.lz4, path_to_input2__gtpro__dbname.tsv.lz4 and path_to_input3__gtpro__dbname.tsv.lz4\n"
       << "\n"
       << "ADVANCED USAGE\n"
       << "\n"
       << "  Here are several examples of using GT-Pro in more than regular ways if you are an advanced user\n"
       << "\n"
       << "  Genotype a sample/metagenome from stdin:\n"
       << "    lz4 -dc /path/to/input/test576/r1.fastq.lz4 | GT_Pro genotype -d /path/to/db123 | lz4 -c > test576_r1__gtpro__db1234.tsv.lz4\n"
       << "    lbzip2 -dc /path/to/input/test576/r2.fq.bz2 | GT_Pro genotype -d /path/to/db123 | lbzip2 -c > "
          "test576_r2__gtpro__db1234.tsv.bz2\n"
       << "\n"
       << "  Designate a sophisticated output naming pattern when genotyping a larget number of samples/metagenomes:\n"
       << "    GT_Pro genotype -d /path/to/dbname -o /path/to/output/directory/%{in} path/to/input1.fastq.gz "
	   << "path/to/input2.fastq.gz path/to/input3.fastq.gz\n"
       << "    Note: naming output files with input path only\n"
       << "\n"
       << "    GT_Pro genotype -d /path/to/dbname -o /path/to/output/directory/out_%{n} path/to/input1.fastq.gz "
	   << "path/to/input2.fastq.gz path/to/input3.fastq.gz\n"
       << "    Note: naming output files with a prefix and a numerical label starting from 0, then 1, 2, ...\n"
       << "\n"
       << "    GT_Pro genotype -d /path/to/dbname -o /path/to/output/directory/%{db}_%{n} path/to/input1.fastq.gz "
	   << "path/to/input2.fastq.gz path/to/input3.fastq.gz\n"
       << "    Note: naming output files with a database name, a underscore symbol and a numerical label\n"
       << "\n"
       << "  Use a different L or M than the optimized values\n"
       << "    GT_Pro genotype -d /path/to/dbname -l 31 -m 36 path/to/input.fastq.gz\n"
       << "    Note: -l and -m can be used to specify the size of L-index and M-filter, which are two important data structures affecting the performance of GT-Pro\n"
       << "    Please consult the paper for more design details about these two data structures.\n"
       << "\n"
       << endl;
}

template <class ElementType> struct DBIndex {

  string filename;
  bool mmapped;

  DBIndex(const string &filename, const uint64_t expected_element_count = 0)
      : filename(filename), mmapped_data(NULL), mmapped(false), expected_element_count(expected_element_count), fd(-1),
        filesize(0) {}

  ElementType *address() {
    if (mmapped_data) {
      return mmapped_data;
    }
    assert(elements.size() > 0);
    return &(elements[0]);
  }

  uint64_t elementCount() {
    if (mmapped_data) {
      return filesize / sizeof(ElementType);
    } else {
      return elements.size();
    }
  }

  uint64_t dataSize() {
    return elementCount() * sizeof(ElementType);
  }

  vector<ElementType> *getElementsVector() { return &(elements); }

  // If file exists and nonempty, mmap it and return false;
  // If file is missing or empty, allocate space in elements array and return true.
  bool mmap(const bool source_available = true) {
    assert(!(mmapped));
    filesize = get_fsize(filename.c_str());
    if (filesize) {
      if (!(expected_element_count)) {
        expected_element_count = filesize / sizeof(ElementType);
      }
      assert(filesize == expected_element_count * sizeof(ElementType));
      MMAP_FOR_REAL();
    }
    if (mmapped) {
      // Does not need to be recomputed.
      return false;
    }
    if (source_available) {
      cerr << chrono_time() << ":  [ERROR] Failed to MMAP " << filename
           << ".  This is fine, but init will be slower as we recreate this file." << endl;
    } else {
      cerr << chrono_time() << ":  [ERROR] Failed to MMAP " << filename << " and lack the source to regenerate it." << endl;
      assert(false);
      exit(-1);
    }
    elements.resize(expected_element_count);
    // needs to be recomputed
    return true;
  }

  void save() {
    assert(!(mmapped));
    auto l_start = chrono_time();
    FILE *dbout = fopen(filename.c_str(), "wb");
    assert(dbout);
    const auto saved_element_count = fwrite(address(), sizeof(ElementType), elements.size(), dbout);
    fclose(dbout);
    assert(saved_element_count == elements.size());
    cerr << chrono_time() << ":  Done writing " << filename << ". That took " << (chrono_time() - l_start) / 1000
         << " more seconds." << endl;
  }

  ~DBIndex() {
    if (fd != -1) {
      assert(mmapped_data);
      assert(mmapped);
      int rc = munmap(mmapped_data, filesize);
      assert(rc == 0);
      close(fd);
    }
  }

private:
  vector<ElementType> elements;
  ElementType *mmapped_data;
  uint64_t expected_element_count;
  int fd;
  uint64_t filesize;

  void MMAP_FOR_REAL() {
    fd = open(filename.c_str(), O_RDONLY, 0);
    if (fd != -1) {
      cerr << chrono_time() << ":  [Info] MMAPPING " << filename << endl;
      auto mmappedData = (ElementType *)::mmap(NULL, filesize, PROT_READ, MMAP_FLAGS, fd, 0);
      if (mmappedData != MAP_FAILED) {
        mmapped_data = mmappedData;
        mmapped = true;
        // cerr << chrono_time() << ":  MMAPPED " << filename << endl;
      }
    }
  }
};

struct SNPSeq {
  uint64_t low_64;
  uint64_t high_64;
};

int main(int argc, char **argv) {

  errno = 0;

  extern char *optarg;
  extern int optind;

  bool dbflag = false;
  bool inflag = false;

  char *fname = argv[0];
  char *db_path = (char *)"";
  char *c_prefix = (char *)"";

  // Output name matches input name and includes DB tag.
  // TODO: Make it possible to deposit output right next to input,
  // in the same folder (for inputs that are folder-structured).
  char *oname = (char *)"%{in}__gtpro__%{db}";

  // Number of bits in the prefix part of the K-mer (also called L-index,
  // even though it might not correspond to an exact number of bases).
  // Override with command line -l parameter.
  //
  // This has a substantial effect on memory use.  Rule of thumb for
  // perf is L2 >= K2 - M3.  However, that rule may be broken in order
  // to reduce RAM use and eliminate I/O which is even worse for perf.
  auto L2 = 29;

  // Number of bits in the M-filter.  This has a substantial effect
  // on memory use.  Rule of thumb for perf is M3 >= 4 + log2(DB cardinality).
  // Override with command line -m parameter.
  auto M3 = 35;

  // The default L2 and M3 above are good for 32GB Apple MacBook Pro laptop.

  int n_threads = thread::hardware_concurrency(); // usually 2x the number of physical cores

  auto preload = false;
  auto force = false;

  auto explicit_l = false;
  auto explicit_m = false;

  int opt;
  while ((opt = getopt(argc, argv, "fl:m:d:C:t:o:h")) != -1) {
    switch (opt) {
    case 'd':
      dbflag = true;
      db_path = optarg;
      break;
    case 'C':
      c_prefix = optarg;
      break;
    case 't':
      n_threads = max(1, min(8 * n_threads, stoi(optarg)));
      break;
    case 'o':
      oname = optarg;
      break;
    case 'l':
      L2 = stoi(optarg);
      explicit_l = true;
      break;
    case 'm':
      M3 = stoi(optarg);
      explicit_m = true;
      break;
    case 'f':
      force = true;
      break;
    case 'h':
    case '?':
      display_usage(fname);
      exit(1);
    }
  }

  if (!dbflag) {
    cerr << "missing argument: -d <sckmerdb_path: string>\n";
    display_usage(fname);
    exit(1);
  }

  if (explicit_l != explicit_m) {
    cerr << chrono_time() << "please specify both or neither of -l and -m\n";
    display_usage(fname);
    exit(-1);
  }

  cerr << fname << '\t' << db_path << '\t' << n_threads << "\t" << (force ? "force_overwrite" : "no_overwrite") << endl;

  int in_pos = optind;

  auto l_start = chrono_time();
  cerr << chrono_time() << ":  "
       << "[Info] Starting to load DB: " << db_path << endl;

  uint64_t db_filesize = get_fsize(db_path);

  string dbbase = string(basename(db_path));
  string dbroot = "./";
  string db_path_str = db_path;
  if (dbbase != db_path_str) {
    dbroot = db_path_str.substr(0, db_path_str.size() - dbbase.size());
  }
  dbbase = regex_replace(dbbase, regex("\\.bin$"), "");
  dbbase = regex_replace(dbbase, regex("\\."), "_");

  // The input (un-optimized) DB is a sequence of 56-bit snp followed by 1-bit forward/rc indicator,
  // then 7 bit offset of SNP within kmer, then 64-bit kmer.  The 56-bit snp encodes the species id,
  // major/minor allele, and genomic position.  From that we build the "optimized" DBs
  // below.  The first one, db_snps, lists the unique SNPs in arbitrary order; and
  // for each SNP in addition to the 56-bits mentioned above it also shows the
  // sequence of 61bp centered on the SNP inferred from all kmers in the original DB.
  DBIndex<uint64_t> db_snps(dbroot + dbbase + "_optimized_db_snps.bin");
  const bool recompute_snps = db_snps.mmap(db_filesize > 0);

  // This encodes the list of all kmers, sorted in increasing order.  Each kmer is represented
  // not by the 62 bits of its 31-bp nucleotide sequence but rather by 27-bits that represent
  // an index into the db_snps table above, and 5 bits representing the SNP position within
  // the kmer;  possibly using the kmer's reverse complement instead of the kmer.
  DBIndex<uint32_t> db_kmer_index(dbroot + dbbase + "_optimized_db_kmer_index.bin");
  const bool recompute_kmer_index = db_kmer_index.mmap(db_filesize > 0);


  {
    int l2 = L2;
    int m3 = M3;
    const bool found_optimal_vals = choose_optimal_l_and_m(l2, m3, db_kmer_index.dataSize() + db_snps.dataSize(), explicit_l && explicit_m);
    // cerr << "Found optimal vals: " << found_optimal_vals << endl;
    if (explicit_l || explicit_m) {
      if (found_optimal_vals && (l2 != L2 || m3 != M3)) {
        cerr << chrono_time() << ":  [WARNING] Arguments -l " << L2 << " -m " << M3 << " override optimal values -l " << l2 << " -m " << m3 << endl;
      }
    } else {
      if (found_optimal_vals) {
        cerr << chrono_time() << ":  [Info] Using -l " << l2 << " -m " << m3 << " as optimal for system RAM" << endl;
      } else {
        cerr << chrono_time() << ":  [WARNING] Using -l " << l2 << " -m " << m3 << " parameter defaults.  Optimal values could not be determined on this system;  performance may suffer." << endl;
      }
      L2 = l2;
      M3 = m3;
    }
  }

  const auto M2 = K2 - L2;

  assert(L2 > 0 && "Unsupported value of -l");
  assert(L2 <= 32 && "Unsupported value of -l");
  assert(M2 > 0 && "Unsupported value of -m");
  assert(M2 < 64 && "Unsupported value of -m");
  assert(M3 > 0 && "Unsupported value of -m");
  assert(M3 < 64 && "Unsupported value of -m");

  const auto LMER_MASK = (LSB << L2) - LSB;
  const auto MMER_MASK = (LSB << M2) - LSB;
  const auto MAX_BLOOM = (LSB << M3) - LSB;

  // Bit vector with one presence/absence bit for every possible M3-bit kmer suffix (the M3
  // LSBs of a kmer's nucleotide sequence).
  DBIndex<uint64_t> db_mmer_bloom(dbroot + dbbase + "_optimized_db_mmer_bloom_" + to_string(M3) + ".bin", (1 + MAX_BLOOM) / 64);
  const bool recompute_mmer_bloom = db_mmer_bloom.mmap();

  // For every kmer in the original DB, the most-signifficant L2 bits of the kmer's nucleotide sequence
  // are called that kmer's lmer.  Kmers that share the same lmer occupy a range of consecutive
  // positions in the kmer_index, and that range is lmer_index[lmer].
  DBIndex<LmerRange> db_lmer_index(dbroot + dbbase + "_optimized_db_lmer_index_" + to_string(L2) + ".bin", 1 + LMER_MASK);
  const bool recompute_lmer_index = db_lmer_index.mmap();
  LmerRange *lmer_index = db_lmer_index.address();

  assert(recompute_kmer_index == recompute_snps &&
         "Please delete all of the optimized DB bin files before recomputing any of them.");

  const bool recompute_everything = recompute_kmer_index || recompute_snps || recompute_lmer_index || recompute_mmer_bloom;

  int fd = -1;
  uint64_t *db_data = NULL;

  auto t_last_progress_update = chrono_time();

  if (recompute_kmer_index || recompute_snps) {
    const auto MAX_SNPS = LSB << 27;               // sorry this is a hard constraint at the moment
    const auto MAX_KMERS = LSB << 40;              // ~1 trillon, up to 5.7 billion tested
    const uint64_t MAX_INPUT_DB_KMERS = MAX_KMERS; // this is a test/debug/developer-only feature
    fd = open(db_path, O_RDONLY, 0);
    assert(fd != -1);
    db_data = (uint64_t *)mmap(NULL, db_filesize, PROT_READ, MMAP_FLAGS, fd, 0);
    assert(db_data != MAP_FAILED);
    unordered_map<uint64_t, uint32_t> snps_map;
    set<uint64_t> virtual_snps;
    vector<tuple<uint64_t, uint64_t>> snps_known_bits; // not persisted, just for integrity checking during construction
    auto &kmer_index = *db_kmer_index.getElementsVector();
    auto &snps = *db_snps.getElementsVector();
    for (uint64_t end = 0; (end < min(MAX_INPUT_DB_KMERS, db_filesize / 8)) && (kmer_index.size() < MAX_KMERS); end += 2) {
      if (((end + 2) % (20 * 1000 * 1000)) == 0 && (chrono_time() - t_last_progress_update >= 10 * 1000)) {
        // print progress update every 10 million kmers / but not more often than every 10 seconds
        t_last_progress_update = chrono_time();
        const auto t_elapsed = t_last_progress_update - l_start;
        const auto fraction_complete = double(end + 2) / (db_filesize / 8);
        const auto t_remaining = (t_elapsed / fraction_complete) * (1.0 - fraction_complete);
        cerr << t_last_progress_update << ":  Loaded almost " << ((end + 2) / (2000 * 1000)) << " million input DB kmers ("
             << int(fraction_complete * 1000) / 10.0 << " percent).  Expect to complete processing in "
             << int(t_remaining / (60 * 100)) / 10.0 << " more minutes." << endl;
      }
      const auto snp_with_offset = db_data[end];
      const auto rc = snp_with_offset & 0x80; // 0 -> forward, 0x80 -> reverse complement
      const auto orig_snp = snp_with_offset >> 8;
      auto offset = snp_with_offset & 0x7f;
      const auto db_kmer = db_data[end + 1];
      const auto db_kmer_rc = reverse_complement(db_kmer);
      if (db_kmer > db_kmer_rc) {
        // We have already seen and encoded db_kmer_rc.
        continue;
      }
      // Encode the kmer that is flagged as "forward" in the input DB, to represent the pair.
      const auto kmer = rc ? db_kmer_rc : db_kmer;
      offset = rc ? 30 - offset : offset;
      assert(orig_snp <= SNP_MAX_REAL_ID); // this should leave 8 bits at the top for virtual SNP id
                                           // 96-97% of the time, vid=0 works
                                           // the rest of the time, we create virtual SNP ids to represent all conflicting kmers
      for (uint64_t vid = 0; vid <= SNP_MAX_VID; ++vid) {
        const auto snp = orig_snp | (vid << SNP_REAL_ID_BITS);
        const auto map_entry = snps_map.find(snp);
        uint32_t snp_id;
        if (map_entry == snps_map.end()) {
          snp_id = snps_map.size();
          snps_map.insert({snp, snp_id});
          if (snp_id == MAX_SNPS) {
            cerr << chrono_time() << ":  Too many SNPs in database.  Only up to " << MAX_SNPS
                 << " are supported, and it's best to leave 10% margin for 'virtual' SNPs." << endl;
          }
          if (snp_id >= MAX_SNPS) {
            break; // break out of vid loop
          }
          snps.push_back(0);
          snps.push_back(0);
          snps.push_back(snp);
          snps_known_bits.push_back(tuple<uint64_t, uint64_t>(0, 0));
        } else {
          snp_id = map_entry->second;
        }
        assert(0 <= offset && offset < K && offset <= 31);
        auto *snp_repr = &(snps[3 * snp_id]);
        auto &snp_known_bits_mask = snps_known_bits[snp_id];
        //
        // The SNP position "offset" divides the kmer binary representation into
        // low_bits and high_bits, as follows:
        //
        //     kmer nucleotide 0 --> kmer binary bits 0, 1                                  |
        //     kmer nucleotide 1 --> kmer binary bits 2, 3                                  |  "low bits"
        //     ...                                                                          |
        //     kmer nucleotide offset --> kmer binary bits 2 * offset, 2 * offset + 1      >*<  SNP position
        //     ...                                                                          |
        //     kmer nucleotide 29 --> kmer binary bits 59, 60                               |  "high bits"
        //     kmer nucleotide 30 --> kmer binary bits 61, 62                               |
        //
        // That is, the kmer nucleotides at positions offset, offset + 1, ... are
        // encoded by the kmer binary bits at 2 * offset, 2 * offset + 1, ..., 62.
        // These so called "high_bits" match the LSBs of snp_repr[1].
        //
        // Similarly, the "low_bits" of the kmer binary representation, bits
        // 0, 1, ..., 2 * offset, 2 * offset + 1, form the MSBs of snp_repr[0].
        //
        // Obligatory ASCII art diagram below.
        /*

                                  <---------- memory addreses increase right to left ------------+

                                            62-2*offset
                                               kmer
                                             high bits       2*offset+2
                                         +<-------------->+     kmer
                                         |                |   low_bits
                                         |             +<------------------>+
                                         |             |  |                 |
                                      MSB|             |  |                 |
                                      +-------------------------------------+
                                      |00|abcd  ... ijk|XY|uvw  ...      rst|   kmer
                 MSB                  +-------------------------------------+
                 +----------------------------------------+                 |
        snp[1]   |00|???  ...          ??|abcd  ... ijk|XY|                 |
                 +----------------------------------------+                 |                 LSB
                                                       +----------------------------------------+
                                                       |XY|uvw  ...      rst|????  ...     ??|00|   snp[0]
                                                       +----------------------------------------+
                                                       |  |                 |  62-2*offset bits |
                                                       +--+                 <------------------->
                                                     SNP bits
                                                       "XY"



                  snp[0]:            64 bits, 2 LSBs unused (00)

                  snp[1], kmer:      64 bits, 2 MSBs unused (00)

                  allele XY:         MSBs of snp[0] =

                                     LSBs of snp[1] =

                                     bits offset*2, offset*2+1 of kmer



        */
        // Note the nucleotide at the SNP position is redundantly represented,
        // being included in both the high_bits and the low_bits.  Thus,
        // the 2 LSBs of snp_repr[1] always equal the 2 MSBs of snp_repr[0].
        // This is intended as a correctnes check.
        //
        // The 2 LSBs of snp_repr[0] and the 2 MSBs of snp_repr[1] are unused,
        // and empty for future extensions.
        //
        // As we construct snp_repr from kmers, we note which of the snp_repr bits
        // have been initialized so far, because future kmers for the snp must
        // agree on those bits with past kmers.  This too is a correctness check.
        // We do not persist these coverage masks to file.
        //
        // Finally, after we have constructed the optimized DB, we use it to reconstruct
        // from that the original DB, and compare to the original.  That is the final
        // and most definitive correctness check.
        //
        static_assert(sizeof(kmer) * 8 == 64, "Use uint64_t for kmers, please.");
        const auto low_bits = kmer << (62 - (offset * BITS_PER_BASE));
        const auto high_bits = kmer >> (offset * BITS_PER_BASE);
        assert(((low_bits >> 62) == (high_bits & 0x3)) && "SNP position differs in two supposedly redundant representations.");
        auto &snp_mask_0 = get<0>(snp_known_bits_mask);
        auto &snp_mask_1 = get<1>(snp_known_bits_mask);
        // We've added information to the snp_repr.  Extend the coverage masks.
        const auto kmer_mask_0 = (FULL_KMER << (62 - (offset * BITS_PER_BASE)));
        const auto kmer_mask_1 = (FULL_KMER >> (offset * BITS_PER_BASE));
        const auto mask_0 = snp_mask_0 & kmer_mask_0;
        const auto mask_1 = snp_mask_1 & kmer_mask_1;
        if (((mask_0 & snp_repr[0]) != (mask_0 & low_bits)) || ((mask_1 & snp_repr[1]) != (mask_1 & high_bits))) {
          virtual_snps.insert(snp);
          if (vid == SNP_MAX_VID) {
            // This really shouldn't happen.
            cerr << "ERROR:  SNP " << snp << " covered by " << (SNP_MAX_VID + 1) << " conflicting kmers." << endl;
            assert(false);
          }
          // continue to attempt next vid
        } else {
          snp_repr[0] |= low_bits;
          snp_repr[1] |= high_bits;
          // We've added information to the snp_repr.  Extend the coverage masks.
          snp_mask_0 |= kmer_mask_0;
          snp_mask_1 |= kmer_mask_1;
          const auto kmer_repr = (snp_id << 5) | offset;
          kmer_index.push_back(kmer_repr);
          break; // break out of VID loop as we've successfully encoded the kmer into this virtual SNP id
        }
      }
    }
    if (snps_map.size() >= MAX_SNPS) {
      cerr << chrono_time() << ":  ERROR:  SNP count exceeds maximum by " << (snps_map.size() - MAX_SNPS) << " SNPs ("
           << int((snps_map.size() - MAX_SNPS) * 1000.0 / snps_map.size()) / 10.0
           << " percent).  A margin of 5-10% is recommended." << endl;
      // We cannot ensure DB integrity with SNP overflow.  It's too complicated with virtual SNPs.
      assert(false && "too many SNPs");
    }
    cerr << chrono_time() << ":  Compressed DB contains " << (snps.size() / 3) << " snps." << endl;
    cerr << chrono_time() << ":  There were " << virtual_snps.size() << " virtual snps." << endl;
    cerr << chrono_time() << ":  Validating optimized DB against original DB." << endl;
    assert((snps.size() / 3) == snps_map.size());
    for (uint64_t end = 0, last_kmi = 0; (end < min(MAX_INPUT_DB_KMERS, db_filesize / 8)) && (last_kmi < MAX_KMERS); end += 2) {
      const auto db_snp_with_offset = db_data[end];
      const auto db_snp = db_snp_with_offset >> 8;
      const auto db_offset = db_snp_with_offset & 0x1f;
      const auto rc = db_snp_with_offset & 0x80; // 0 -> forward, 0x80 -> reverse complement
      const auto map_entry = snps_map.find(db_snp);
      assert(map_entry != snps_map.end());
      const auto db_virtual_snp_id = map_entry->second;
      assert(db_virtual_snp_id < MAX_SNPS);
      const auto db_kmer = db_data[end + 1];
      const auto db_kmer_rc = reverse_complement(db_kmer);
      if (db_kmer_rc < db_kmer) {
        // We have already decoded db_kmer_rc.
        continue;
      }
      const auto kmi = kmer_index[last_kmi++];
      const auto offset = kmi & 0x1f;
      const auto virtual_snp_id = kmi >> 5;
      assert(0 <= offset && offset <= 31 && offset < K);
      assert(0 <= virtual_snp_id && virtual_snp_id <= (snps.size() / 3));
      const auto snp = snps[3 * virtual_snp_id + 2] & SNP_MAX_REAL_ID;
      assert(snp == db_snp);
      assert(offset == (rc ? 30 - db_offset : db_offset));
      const auto *snp_repr = &(snps[3 * virtual_snp_id]);
      const auto low_bits = snp_repr[0] >> (62 - (offset * BITS_PER_BASE));
      const auto high_bits = (snp_repr[1] << (offset * BITS_PER_BASE)) & FULL_KMER;
      assert(((snp_repr[0] >> 62) == (snp_repr[1] & 0x3)) &&
             "SNP position differs in two supposedly redundant representations.");
      const auto kmer = high_bits | low_bits;
      if (kmer != db_kmer && kmer != db_kmer_rc) {
        cerr << chrono_time() << ":  ERROR:  Mismatch between original and reconstructed kmer at input DB position " << end
             << endl;
        cerr << chrono_time() << ":  ERROR:       original " << bitset<64>(db_kmer) << endl;
        cerr << chrono_time() << ":  ERROR:    original_rc " << bitset<64>(db_kmer_rc) << endl;
        cerr << chrono_time() << ":  ERROR:  reconstructed " << bitset<64>(kmer) << endl;
        cerr << chrono_time() << ":  ERROR:  snp " << db_snp << " vid "
             << ((snps[3 * virtual_snp_id + 2] ^ snp) >> SNP_REAL_ID_BITS) << " offset " << offset << endl;
        assert(false);
      }
    }
  }

  if (recompute_lmer_index || recompute_mmer_bloom) {
    cerr << chrono_time() << ":  Recomputing bloom index and/or filter." << endl;
    uint64_t start = 0;
    uint64_t last_lmer;
    const auto kmer_index = db_kmer_index.address();
    const auto kmer_count = db_kmer_index.elementCount();
    const auto snps = db_snps.address();
    const auto snps_count = db_snps.elementCount() / 3;
    auto mmer_bloom = db_mmer_bloom.address();
    t_last_progress_update = chrono_time();
    for (uint64_t end = 0; end < kmer_count; ++end) {
      if (((end + 1) % (10 * 1000 * 1000)) == 0 && (chrono_time() - t_last_progress_update >= 10 * 1000)) {
        // print progress update every 10 million kmers / but not more often than every 10 seconds
        t_last_progress_update = chrono_time();
        const auto t_elapsed = t_last_progress_update - l_start;
        const auto fraction_complete = double(end + 1) / kmer_count;
        const auto t_remaining = (t_elapsed / fraction_complete) * (1.0 - fraction_complete);
        cerr << t_last_progress_update << ":  Processed almost " << ((end + 1) / (1000 * 1000)) << " million kmers ("
             << int(fraction_complete * 1000) / 10.0 << " percent).  Expect to complete processing in "
             << int(t_remaining / (60 * 100)) / 10.0 << " more minutes." << endl;
      }
      const auto kmi = kmer_index[end];
      const auto offset = kmi & 0x1f;
      const auto snp_id = kmi >> 5;
      assert(0 <= offset && offset <= 31 && offset < K);
      assert(0 <= snp_id && snp_id <= snps_count);
      const auto *snp_repr = &(snps[3 * snp_id]);
      const auto low_bits = snp_repr[0] >> (62 - (offset * BITS_PER_BASE));
      const auto high_bits = (snp_repr[1] << (offset * BITS_PER_BASE)) & FULL_KMER;
      assert(((snp_repr[0] >> 62) == (snp_repr[1] & 0x3)) &&
             "SNP position differs in two supposedly redundant representations.");
      auto kmer = high_bits | low_bits;
      const auto kmer_rc = reverse_complement(kmer);
      if (kmer_rc < kmer) {
        // remember only the smaller of the kmer pair is in the DB, index, and bloom filter
        kmer = kmer_rc;
      }
      const auto lmer = kmer >> M2;
      if (recompute_lmer_index) {
        if ((end > 0) && (lmer != last_lmer)) {
          start = end;
        }
        // Invariant:  The data loaded so far for lmer reside at kmer_index[start...]
        assert(start <= MAX_START);
        const auto len = end - start + 1;
        assert(len < MAX_LEN);
        assert(lmer <= LMER_MASK);
        lmer_index[lmer] = (start << LEN_BITS) | len;
        last_lmer = lmer;
      }
      if (recompute_mmer_bloom) {
        const uint64_t bloom_index = kmer & MAX_BLOOM;
        mmer_bloom[bloom_index / 64] |= ((uint64_t)1) << (bloom_index % 64);
      }
    }
  }

  if (recompute_kmer_index) {
    db_snps.save();
    db_kmer_index.save();
  }

  if (recompute_mmer_bloom) {
    db_mmer_bloom.save();
  }

  if (recompute_lmer_index) {
    db_lmer_index.save();
  }

  cerr << chrono_time() << ":  [Info] Done with init for optimized DB with " << db_kmer_index.elementCount() << " kmers.  That took "
       << (chrono_time() - l_start) / 1000 << " seconds." << endl;

  l_start = chrono_time();

  const auto errors =
      kmer_lookup(lmer_index, db_mmer_bloom.address(), db_kmer_index.address(), db_snps.address(), argc - optind,
                  (const char **)argv + optind, oname, M2, M3, n_threads, dbbase, force, c_prefix);

  if (fd != -1 && db_data != NULL) {
    int rc = munmap(db_data, db_filesize);
    assert(rc == 0);
    close(fd);
  }

  cerr << chrono_time() << ":  "
       << "Totally done: " << (chrono_time() - l_start) / 1000 << " seconds elapsed processing reads, after DB was loaded."
       << endl;

  return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
