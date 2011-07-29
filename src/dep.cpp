// CaboCha -- Yet Another Japanese Dependency Parser
//
//  $Id: dep.cpp 50 2009-05-03 08:25:36Z taku-ku $;
//
//  Copyright(C) 2001-2008 Taku Kudo <taku@chasen.org>
#include <vector>
#include <algorithm>
#include <fstream>
#include <functional>
#include <stack>
#include <iterator>
#include "param.h"
#include "dep.h"
#include "cabocha.h"
#include "timer.h"
#include "svm.h"
#include "utils.h"
#include "common.h"

namespace CaboCha {
static const size_t MAX_GAP_SIZE = 7;

DependencyParser::DependencyParser():  svm_(0) {}
DependencyParser::~DependencyParser() {}

bool DependencyParser::open(const Param &param) {
  close();
  if (action_mode() == PARSING_MODE) {
    const std::string filename = param.get<std::string>("parser-model");
    if (param.get<bool>("nonpke")) {
      svm_.reset(new SVMTest);
    } else {
      svm_.reset(new SVM);
    }
    CHECK_FALSE(svm_->open(filename.c_str()))
        << "no such file or directory: " << filename;
  }
  return true;
}

void DependencyParser::close() {
  svm_.reset(0);
}

size_t DependencyParser::build(Tree *tree) {
  const size_t size  = tree->chunk_size();

  static_feature_.clear();
  static_feature_.resize(size);

  dyn_b_feature_.clear();
  dyn_b_feature_.resize(size);
  dyn_b_.clear();
  dyn_b_.resize(size);

  dyn_a_feature_.clear();
  dyn_a_feature_.resize(size);
  dyn_a_.clear();
  dyn_a_.resize(size);

  gap_.clear();
  gap_list_.clear();
  gap_list_.resize(size);

  for (size_t i = 0; i < tree->chunk_size(); ++i) {
    Chunk *chunk = tree->mutable_chunk(i);
    for (size_t k = 0; k < chunk->feature_list_size; ++k) {
      char *feature = const_cast<char *>(chunk->feature_list[k]);
      switch (feature[0]) {
        case 'F': static_feature_[i].push_back(feature); break;
        case 'G': gap_list_[i].push_back(feature);       break;
        case 'A': dyn_a_feature_[i].push_back(feature);  break;
        case 'B': dyn_b_feature_[i].push_back(feature);  break;
        default:
          break;
      }
    }
  }

  // make gap features
  gap_.resize(size*(size+3)/2+1);

  for (size_t k = 0; k < size; k++)
    for (size_t i = 0; i < k; i++)
      for (size_t j = k + 1; j < size; j++)
        std::copy(gap_list_[k].begin(), gap_list_[k].end(),
                  std::back_inserter(gap_[j*(j+1)/2+i]));

  return size;
}

bool DependencyParser::estimate(const Tree *tree,
                                int src, int dst,
                                double *score) {
  const bool is_end = ((static_cast<int>(tree->chunk_size()) - 1) == dst);
  if (action_mode() == PARSING_MODE && is_end) {
    *score = 0.0;
    return true;
  }

  fpset_.clear();
  const int dist = dst - src;
  if (dist == 1)                   fpset_.insert("DIST:1");
  else if (dist >= 2 && dist <= 5) fpset_.insert("DIST:2-5");
  else                             fpset_.insert("DIST:6-");

  // static feature
  for (size_t i = 0; i < static_feature_[src].size(); i++) {
    static_feature_[src][i][0] = 'f';
    fpset_.insert(static_feature_[src][i]);
  }

  for (size_t i = 0; i < static_feature_[dst].size(); i++) {
    static_feature_[dst][i][0] = 'F';
    fpset_.insert(static_feature_[dst][i]);
  }

  size_t max_gap = 0;

  // gap feature
  int k = dst * (dst + 1) / 2 + src;
  max_gap = _min(gap_[k].size(), MAX_GAP_SIZE);
  for (size_t i = 0; i < max_gap; ++i)
    fpset_.insert(gap_[k][i]);

  // dynamic features
  max_gap = _min(dyn_a_[dst].size(), MAX_GAP_SIZE);
  for (size_t i = 0; i < max_gap; ++i) {
    dyn_a_[dst][i][0] = 'A';
    fpset_.insert(dyn_a_[dst][i]);
  }

  max_gap = _min(dyn_a_[src].size(), MAX_GAP_SIZE);
  for (size_t i = 0; i < max_gap; ++i) {
    dyn_a_[src][i][0] = 'a';
    fpset_.insert(dyn_a_[src][i]);
  }

  max_gap = _min(dyn_b_[dst].size(), MAX_GAP_SIZE);
  for (size_t i = 0; i < max_gap; ++i) {
    dyn_b_[dst][i][0] = 'B';
    fpset_.insert(dyn_b_[dst][i]);
  }

  const size_t fsize = fpset_.size();
  std::copy(fpset_.begin(), fpset_.end(), fp_);

  if (action_mode() == PARSING_MODE) {
    *score = svm_->classify(fsize, const_cast<char **>(fp_));
#if 0
    for (size_t i = 0; i < fsize; ++i) {
      std::cout << "[" << fp_[i] << "]" << std::endl;
    }
    std::cout << "SRC=" << src << " DST="
              << dst << " score=" << *score << std::endl;
#endif
    return (*score > 0);
  } else {
    const bool isdep = (is_end || tree->chunk(src)->link == dst);
    *stream() << (isdep ? "+1" : "-1");
    for (size_t i = 0; i < fsize; ++i) {
      *stream() << ' ' << fp_[i];
    }
    *stream() << std::endl;
    return isdep;
  }

  return false;
}

bool DependencyParser::parse(Tree *tree) {
#define MYPOP(agenda, n) do {                   \
    if (agenda.empty()) {                       \
      n = -1;                                   \
    } else {                                    \
      n = agenda.top();                         \
      agenda.pop();                             \
    }                                           \
  } while (0)

  const int size = static_cast<int>(tree->chunk_size());
  build(tree);

  std::stack<int> agenda;
  double score = 0.0;
  agenda.push(0);
  for (int dst = 1; dst < size; ++dst) {
    int src = 0;
    MYPOP(agenda, src);
    while (src != -1 && estimate(tree, src, dst, &score)) {
      Chunk *chunk = tree->mutable_chunk(src);
      chunk->link = dst;
      chunk->score = score;

      // copy dynamic feature
      std::copy(dyn_b_feature_[dst].begin(),
                dyn_b_feature_[dst].end(),
                std::back_inserter(dyn_b_[src]));
      std::copy(dyn_a_feature_[src].begin(),
                dyn_a_feature_[src].end(),
                std::back_inserter(dyn_a_[dst]));

      MYPOP(agenda, src);
    }
    if (src != -1) agenda.push(src);
    agenda.push(dst);
  }

  tree->set_output_layer(OUTPUT_DEP);
  return true;
#undef MYPOP
}
}